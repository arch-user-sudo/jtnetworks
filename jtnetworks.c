/*
 * jtnetworks — a small, zen-styled network control panel.
 *
 * Wayland + cairo + Pango. No X11, no GTK, no Qt.
 *
 *   make
 *   ./jtnetworks
 *   ./jtnetworks '#748382'   (optional accent override)
 *   ./jtnetworksctl accent '#748382'   (change accent while running)
 *
 * Controls:
 *   Tab / Left / Right   switch between Wi-Fi and Bluetooth
 *   1 / 2                jump to Wi-Fi / Bluetooth
 *   Space / Enter        toggle the active radio
 *   S                    scan for Wi-Fi networks
 *   Esc / q              quit
 *   Drag header          move the window
 *
 *   Click a Wi-Fi row    connect, or activate a saved profile
 *   Type / Backspace     edit the password in that dialog
 *   Enter / Esc          connect / cancel in that dialog
 *   Row buttons          disconnect or forget saved networks/devices
 *
 * The Wi-Fi side uses libnm, NetworkManager's client library. Bluetooth
 * still uses BlueZ's bluetoothctl CLI when available. State is read back
 * after a change so the panel reflects the real radios. Saved Wi-Fi
 * profiles stay in the list even when out of range, and connected
 * Bluetooth devices are trusted automatically.
 *
 * Palette from ~/JTLab/config.h:
 *   bg #030303, fg #FFFFFF, borders #404040,
 *   accent #E8A03C (overridable), success #66B366, font Liberation.
 */

#define _GNU_SOURCE

#include <cairo/cairo.h>
#include <pango/pangocairo.h>
#include <NetworkManager.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-client-protocol.h"
#include "xdg-toplevel-icon-client-protocol.h"
#include "cursor-shape-client-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  Look & feel                                                        */
/* ------------------------------------------------------------------ */

#define WIN_W 880.0
#define WIN_H 600.0

/* Idle decorative motion.  The static UI is cached, so this is cheap. */
#define AMBIENT_FPS 30.0
/* Tab crossfades should feel fluid rather than stepped. */
#define TAB_FPS 60.0

#define SIDEBAR_X 14.0
#define SIDEBAR_Y 14.0
#define SIDEBAR_W 200.0
#define SIDEBAR_H (WIN_H - 28.0)

#define CONTENT_X 240.0
#define CONTENT_W (WIN_W - CONTENT_X - 24.0)

#define HERO_Y 108.0
#define HERO_H 158.0
#define SECTION_Y 278.0
#define LIST_Y 306.0
#define ROW_H 54.0
#define ROW_GAP 8.0

/* Compact row actions for saved / connected Wi-Fi networks. */
#define ROW_ACTION_H 20.0
#define ROW_ACTION_Y_OFF (ROW_H - 28.0)
#define ROW_ACTION_RIGHT_PAD 14.0
#define ROW_ACTION_GAP 6.0
#define ROW_ACTION_FORGET_W 56.0
#define ROW_ACTION_DISCONNECT_W 82.0

#define TOGGLE_X (WIN_W - 120.0)
#define TOGGLE_Y 32.0
#define TOGGLE_W 62.0
#define TOGGLE_H 32.0

#define CLOSE_X (WIN_W - 32.0)
#define CLOSE_Y 34.0
#define CLOSE_R 14.0

/* Wi-Fi connection dialog */
#define MODAL_W 380.0
#define MODAL_H 236.0
#define MODAL_X ((WIN_W - MODAL_W) / 2.0)
#define MODAL_Y ((WIN_H - MODAL_H) / 2.0)
#define MODAL_PAD 24.0
#define MODAL_FIELD_Y (MODAL_Y + 96.0)
#define MODAL_FIELD_H 44.0
#define MODAL_BTN_Y (MODAL_Y + 174.0)
#define MODAL_BTN_H 38.0
#define MODAL_BTN_W 120.0

#define FONT_FAMILY "Liberation"

#define C_BG        0x030303
#define C_BG_TOP    0x090A0B
#define C_FG        0xFFFFFF
#define C_DIM       0x9A9A9A
#define C_MUTED     0x707070
#define C_FAINT     0x5C5C5C
#define C_BORDER    0x404040
#define C_CARD      0x0D0E0F
#define C_CARD_HI   0x131416
#define C_TRACK     0x1A1B1D
#define C_GREEN     0x66B366
#define C_RED       0xD06B6B

/* The accent can be overridden at launch.  These aliases keep the drawing
 * code unchanged while allowing the values to be replaced at runtime. */
static unsigned accent_rgb = 0xE8A03C;
static unsigned accent_hi_rgb = 0xF2B25C;
static unsigned accent_lo_rgb = 0xCE8A2F;
static unsigned on_accent_rgb = 0x14100A;
#define C_ACCENT    accent_rgb
#define C_ACCENT_HI accent_hi_rgb
#define C_ACCENT_LO accent_lo_rgb
#define C_ON_ACCENT on_accent_rgb

#define PI 3.14159265358979323846

#define MAX_NETS 16
#define MAX_SAVED 16
#define MAX_CANDIDATES 96
#define MAX_BT   4

#define R_(h) ((((h) >> 16) & 0xFF) / 255.0)
#define G_(h) ((((h) >> 8) & 0xFF) / 255.0)
#define B_(h) (((h) & 0xFF) / 255.0)

static void rgba(cairo_t *cr, unsigned hex, double a)
{
    cairo_set_source_rgba(cr, R_(hex), G_(hex), B_(hex), a);
}

static void rounded_rect(cairo_t *cr, double x, double y,
                         double w, double h, double r)
{
    double d = 2.0 * r;
    if (d > w) d = w;
    if (d > h) d = h;
    r = d / 2.0;

    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, PI / 2.0, PI);
    cairo_arc(cr, x + r, y + r, r, PI, 3.0 * PI / 2.0);
    cairo_close_path(cr);
}

static double mono_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static int accent_hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int accent_parse(const char *s, unsigned *out)
{
    if (!s)
        return 0;
    if (*s == '#')
        s++;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s += 2;
    if (strlen(s) != 6)
        return 0;

    unsigned value = 0;
    for (int i = 0; i < 6; i++) {
        int nibble = accent_hex_nibble(s[i]);
        if (nibble < 0)
            return 0;
        value = (value << 4) | (unsigned)nibble;
    }
    *out = value;
    return 1;
}

static double accent_linear_channel(unsigned channel)
{
    double v = (double)channel / 255.0;
    return v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

static double accent_luminance(unsigned rgb)
{
    return 0.2126 * accent_linear_channel((rgb >> 16) & 0xFF) +
           0.7152 * accent_linear_channel((rgb >> 8) & 0xFF) +
           0.0722 * accent_linear_channel(rgb & 0xFF);
}

static double accent_contrast(double a, double b)
{
    if (a < b) {
        double t = a;
        a = b;
        b = t;
    }
    return (a + 0.05) / (b + 0.05);
}

static unsigned accent_ink(unsigned rgb)
{
    const unsigned dark = 0x14100A;
    const unsigned light = 0xF7F5F0;
    double background = accent_luminance(rgb);
    double dark_contrast = accent_contrast(background,
                                           accent_luminance(dark));
    double light_contrast = accent_contrast(background,
                                            accent_luminance(light));
    return light_contrast > dark_contrast ? light : dark;
}

static unsigned accent_mix(unsigned rgb, unsigned toward, double amount)
{
    int r1 = (int)((rgb >> 16) & 0xFF);
    int g1 = (int)((rgb >> 8) & 0xFF);
    int b1 = (int)(rgb & 0xFF);
    int r2 = (int)((toward >> 16) & 0xFF);
    int g2 = (int)((toward >> 8) & 0xFF);
    int b2 = (int)(toward & 0xFF);
    int r = (int)lround(r1 + (r2 - r1) * amount);
    int g = (int)lround(g1 + (g2 - g1) * amount);
    int b = (int)lround(b1 + (b2 - b1) * amount);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

static void accent_set(unsigned rgb)
{
    accent_rgb = rgb;
    accent_hi_rgb = accent_mix(rgb, 0xFFFFFF, 0.16);
    accent_lo_rgb = accent_mix(rgb, 0x000000, 0.14);
    on_accent_rgb = accent_ink(rgb);
}

/* Remove a leading '#RRGGBB' (or '#RRGGBB' with quotes removed by the shell),
 * a bare leading RRGGBB, or an explicit --accent COLOR from argv. */
static int accent_cli(int *argc, char **argv)
{
    for (int i = 1; i < *argc; i++) {
        const char *value = NULL;
        int consumed = 0;

        if (!strcmp(argv[i], "--accent")) {
            if (i + 1 >= *argc) {
                fprintf(stderr, "%s: --accent requires a color\n", argv[0]);
                return 0;
            }
            value = argv[i + 1];
            consumed = 2;
        } else if (!strncmp(argv[i], "--accent=", 9)) {
            value = argv[i] + 9;
            consumed = 1;
        } else if (argv[i][0] == '#') {
            value = argv[i];
            consumed = 1;
        } else if (i == 1) {
            unsigned candidate;
            if (accent_parse(argv[i], &candidate)) {
                value = argv[i];
                consumed = 1;
            }
        }

        if (!value)
            continue;

        unsigned rgb;
        if (!accent_parse(value, &rgb)) {
            fprintf(stderr,
                    "%s: invalid accent color '%s' (expected #RRGGBB)\n",
                    argv[0], value);
            return 0;
        }
        accent_set(rgb);

        for (int j = i; j + consumed < *argc; j++)
            argv[j] = argv[j + consumed];
        *argc -= consumed;
        i--;
    }
    return 1;
}

static double approach(double cur, double target, double dt, double speed)
{
    return cur + (target - cur) * (1.0 - exp(-dt * speed));
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* ------------------------------------------------------------------ */
/*  Text                                                               */
/* ------------------------------------------------------------------ */

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

typedef struct {
    int key;
    PangoFontDescription *fd;
} FontEnt;

#define FONT_CACHE 256
static FontEnt g_fonts[FONT_CACHE];
static int g_font_count;

static PangoFontDescription *font_get(double size, int weight)
{
    int key = (int)lround(size * 10.0) * 10000 + weight;

    for (int i = 0; i < g_font_count; i++)
        if (g_fonts[i].key == key)
            return g_fonts[i].fd;

    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, FONT_FAMILY);
    pango_font_description_set_absolute_size(fd, size * PANGO_SCALE);
    pango_font_description_set_weight(fd, (PangoWeight)weight);

    if (g_font_count < FONT_CACHE) {
        g_fonts[g_font_count].key = key;
        g_fonts[g_font_count].fd = fd;
        g_font_count++;
    }
    return fd;
}

static PangoLayout *layout_make(cairo_t *cr, const char *text,
                                double size, int weight, double spacing)
{
    PangoLayout *l = pango_cairo_create_layout(cr);
    pango_layout_set_font_description(l, font_get(size, weight));
    pango_layout_set_text(l, text, -1);

    if (spacing > 0.0) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs,
            pango_attr_letter_spacing_new((int)lround(spacing * PANGO_SCALE)));
        pango_layout_set_attributes(l, attrs);
        pango_attr_list_unref(attrs);
    }
    return l;
}

static double text_width(cairo_t *cr, const char *text, double size,
                         int weight, double spacing)
{
    if (!text || !*text)
        return 0.0;
    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);
    double w = logical.width;
    g_object_unref(l);
    return w;
}

static double text_draw(cairo_t *cr, const char *text, double size, int weight,
                        unsigned hex, double alpha, double x, double y,
                        int align, double spacing)
{
    if (!text || !*text)
        return 0.0;

    PangoLayout *l = layout_make(cr, text, size, weight, spacing);
    PangoRectangle logical;
    pango_layout_get_pixel_extents(l, NULL, &logical);

    double w = logical.width;
    if (align == ALIGN_CENTER)
        x -= w / 2.0;
    else if (align == ALIGN_RIGHT)
        x -= w;

    cairo_move_to(cr, x, y);
    rgba(cr, hex, alpha);
    pango_cairo_show_layout(cr, l);
    g_object_unref(l);
    return w;
}

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

typedef enum { TAB_WIFI = 0, TAB_BLUETOOTH = 1 } Tab;

enum {
    WIFI_ACTION_NONE = 0,
    WIFI_ACTION_DISCONNECT,
    WIFI_ACTION_FORGET,
    WIFI_ACTION_ACTIVATE,

    BT_ACTION_NONE = 0,
    BT_ACTION_DISCONNECT = 11,
    BT_ACTION_FORGET,
    BT_ACTION_CONNECT
};

typedef enum {
    DEV_GENERIC = 0,
    DEV_HEADPHONES,
    DEV_EARBUDS,
    DEV_MOUSE,
    DEV_KEYBOARD,
    DEV_GAMEPAD
} DevKind;

typedef struct {
    char ssid[96];
    int signal;
    char security[32];
    int connected;
    int saved;
    char profile[96];
    char uuid[64];
    char ap_path[128];
} WifiNet;

typedef struct {
    char name[96];
    char ssid[96];
    char uuid[64];
    int active;
} SavedWifi;

typedef struct {
    char name[96];
    char mac[24];
    DevKind kind;
    int connected;
    int trusted;
} BtDev;

typedef struct {
    double t;       /* wall clock, used by timed UI actions */
    double deco_t;  /* decorative animation clock */

    int ambient_skip; /* omit animated decoration while caching the base */

    int wifi_on;
    int bt_on;
    double wifi_k;
    double bt_k;
    double tab_mix;
    int tab;

    int wifi_connected;
    char ssid[96];
    char ip[64];
    int signal;
    char security[32];
    char wifi_profile[96];
    char wifi_uuid[64];
    WifiNet nets[MAX_NETS];
    int net_count;
    SavedWifi saved[MAX_SAVED];
    int saved_count;

    int wifi_action_kind;   /* 0 none, 1 disconnect, 2 forget, 3 activate */
    int wifi_action_row;
    char wifi_action_ssid[96];
    char wifi_msg[160];
    double wifi_msg_t;
    int wifi_msg_ok;

    char bt_name[96];
    int bt_discoverable;
    BtDev bt[MAX_BT];
    int bt_count;
    int bt_paired_total;
    int bt_action_kind;
    int bt_action_row;
    char bt_action_name[96];
    char bt_action_mac[24];
    char bt_msg[160];
    double bt_msg_t;
    int bt_msg_ok;

    /* Set by the interactive client; screenshot mode leaves it off so it
     * never changes the live Bluetooth state. */
    int allow_mutation;

    int hover_nav;       /* -1 none, 0 Wi-Fi, 1 Bluetooth */
    int hover_toggle;
    int hover_close;
    int hover_scan;
    int hover_pair;
    int hover_enable;
    int hover_row;       /* -1 none, otherwise visible row */
    int hover_row_action;     /* 0 none, 1 disconnect, 2 forget */
    int hover_row_action_idx; /* row containing the hovered action */
    int press_toggle;

    /* Wi-Fi connection dialog */
    int connect_open;
    int connect_idx;
    int connect_secure;
    int connect_busy;
    int connect_focus;
    char connect_ssid[96];
    char connect_pass[128];
    char connect_security[32];
    char connect_msg[160];
    double connect_caret_t;
    int hover_connect_field;
    int hover_connect_cancel;
    int hover_connect_submit;

    double scan_t;
    double pair_t;
    double ptr_x;
    double ptr_y;
} Ui;

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

static void soft_glow(cairo_t *cr, double cx, double cy, double radius,
                      unsigned hex, double alpha)
{
    if (alpha <= 0.0)
        return;
    cairo_pattern_t *p = cairo_pattern_create_radial(cx, cy, 0,
                                                     cx, cy, radius);
    cairo_pattern_add_color_stop_rgba(p, 0.0, R_(hex), G_(hex), B_(hex), alpha);
    cairo_pattern_add_color_stop_rgba(p, 0.55, R_(hex), G_(hex), B_(hex),
                                      alpha * 0.32);
    cairo_pattern_add_color_stop_rgba(p, 1.0, R_(hex), G_(hex), B_(hex), 0.0);
    cairo_set_source(cr, p);
    cairo_paint(cr);
    cairo_pattern_destroy(p);
}

static void line(cairo_t *cr, double x1, double y1, double x2, double y2,
                 double width, unsigned hex, double alpha)
{
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, hex, alpha);
    cairo_move_to(cr, x1, y1);
    cairo_line_to(cr, x2, y2);
    cairo_stroke(cr);
}

static void dot(cairo_t *cr, double cx, double cy, double r,
                unsigned hex, double alpha)
{
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * PI);
    rgba(cr, hex, alpha);
    cairo_fill(cr);
}

static void ring(cairo_t *cr, double cx, double cy, double r,
                 double width, unsigned hex, double alpha)
{
    cairo_new_path(cr);
    cairo_arc(cr, cx, cy, r, 0, 2.0 * PI);
    cairo_set_line_width(cr, width);
    rgba(cr, hex, alpha);
    cairo_stroke(cr);
}

static void wifi_glyph(cairo_t *cr, double cx, double cy, double sz,
                       unsigned hex, double alpha, double t, int pulse)
{
    double dot_y = cy + sz * 0.27;
    double lw = fmax(1.3, sz * 0.055);
    double radii[3] = { sz * 0.20, sz * 0.36, sz * 0.52 };

    cairo_new_path(cr);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, hex, alpha);
    cairo_set_line_width(cr, lw);
    for (int i = 0; i < 3; i++) {
        cairo_arc(cr, cx, dot_y, radii[i], -PI * 0.75, -PI * 0.25);
        cairo_stroke(cr);
    }
    dot(cr, cx, dot_y, fmax(1.2, sz * 0.047), hex, alpha);

    if (pulse && alpha > 0.05) {
        double ph = fmod(t * 0.52, 1.0);
        cairo_arc(cr, cx, dot_y, sz * 0.52 + ph * sz * 0.18,
                  -PI * 0.75, -PI * 0.25);
        cairo_set_line_width(cr, lw * 0.75);
        rgba(cr, hex, alpha * 0.25 * (1.0 - ph));
        cairo_stroke(cr);
    }
}

static void bt_glyph(cairo_t *cr, double cx, double cy, double sz,
                     unsigned hex, double alpha, double t, int pulse)
{
    double h = sz * 0.92;
    double hw = sz * 0.30;
    double top = cy - h / 2.0;
    double bot = cy + h / 2.0;
    double lw = fmax(1.3, sz * 0.085);

    cairo_set_line_width(cr, lw);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    rgba(cr, hex, alpha);

    cairo_move_to(cr, cx, top);
    cairo_line_to(cr, cx, bot);

    cairo_move_to(cr, cx, top);
    cairo_line_to(cr, cx + hw, top + h * 0.25);
    cairo_line_to(cr, cx - hw, top + h * 0.50);
    cairo_line_to(cr, cx, bot);

    cairo_move_to(cr, cx, bot);
    cairo_line_to(cr, cx + hw, bot - h * 0.25);
    cairo_line_to(cr, cx - hw, top + h * 0.50);
    cairo_line_to(cr, cx, top);
    cairo_stroke(cr);

    if (pulse && alpha > 0.05) {
        double ph = fmod(t * 0.42, 1.0);
        ring(cr, cx, cy, h * 0.56 + ph * sz * 0.15, lw * 0.45,
             hex, alpha * 0.16 * (1.0 - ph));
    }
}

static void signal_bars(cairo_t *cr, double x, double base_y, int level,
                        unsigned hex, double alpha)
{
    double bw = 3.0;
    double gap = 2.6;
    double hs[4] = { 5.0, 9.0, 13.0, 17.0 };

    for (int i = 0; i < 4; i++) {
        double by = base_y - hs[i];
        rounded_rect(cr, x + i * (bw + gap), by, bw, hs[i], 1.2);
        if (i < level)
            rgba(cr, hex, alpha);
        else
            rgba(cr, C_TRACK, 0.95);
        cairo_fill(cr);
    }
}

static int signal_level(int pct)
{
    if (pct >= 75) return 4;
    if (pct >= 50) return 3;
    if (pct >= 25) return 2;
    if (pct > 0)   return 1;
    return 0;
}

static void lock_glyph(cairo_t *cr, double cx, double cy, double s,
                       unsigned hex, double alpha)
{
    cairo_new_path(cr);
    cairo_set_line_width(cr, fmax(1.1, s * 0.11));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    rgba(cr, hex, alpha);
    cairo_arc(cr, cx, cy - s * 0.10, s * 0.25, PI, 2.0 * PI);
    cairo_stroke(cr);
    rounded_rect(cr, cx - s * 0.35, cy - s * 0.02,
                 s * 0.70, s * 0.50, s * 0.10);
    cairo_fill(cr);
}

static void plus_icon(cairo_t *cr, double cx, double cy, double s,
                      unsigned hex, double alpha)
{
    line(cr, cx - s / 2.0, cy, cx + s / 2.0, cy, 1.4, hex, alpha);
    line(cr, cx, cy - s / 2.0, cx, cy + s / 2.0, 1.4, hex, alpha);
}

static void radar_icon(cairo_t *cr, double cx, double cy, double s,
                       unsigned hex, double alpha, double t)
{
    cairo_new_path(cr);
    cairo_set_line_width(cr, 1.3);
    rgba(cr, hex, alpha * 0.85);
    cairo_arc(cr, cx, cy, s * 0.72, 0, 2.0 * PI);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, s * 0.36, 0, 2.0 * PI);
    cairo_stroke(cr);
    dot(cr, cx, cy, 1.4, hex, alpha);
    double a = t * 2.2;
    line(cr, cx, cy, cx + cos(a) * s * 0.70, cy + sin(a) * s * 0.70,
         1.2, C_ACCENT, alpha * 0.85);
}

static void device_glyph(cairo_t *cr, DevKind kind, double cx, double cy,
                         double s, unsigned hex, double alpha)
{
    cairo_new_path(cr);
    cairo_set_line_width(cr, fmax(1.2, s * 0.085));
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    rgba(cr, hex, alpha);

    switch (kind) {
    case DEV_HEADPHONES:
        cairo_arc(cr, cx, cy + s * 0.06, s * 0.40, PI, 2.0 * PI);
        cairo_stroke(cr);
        rounded_rect(cr, cx - s * 0.48, cy + s * 0.02,
                     s * 0.20, s * 0.38, s * 0.09);
        cairo_fill(cr);
        rounded_rect(cr, cx + s * 0.28, cy + s * 0.02,
                     s * 0.20, s * 0.38, s * 0.09);
        cairo_fill(cr);
        break;

    case DEV_EARBUDS:
        ring(cr, cx - s * 0.24, cy - s * 0.18, s * 0.16, s * 0.10,
             hex, alpha);
        ring(cr, cx + s * 0.24, cy - s * 0.18, s * 0.16, s * 0.10,
             hex, alpha);
        line(cr, cx - s * 0.24, cy + s * 0.00,
             cx - s * 0.18, cy + s * 0.40, s * 0.12, hex, alpha);
        line(cr, cx + s * 0.24, cy + s * 0.00,
             cx + s * 0.18, cy + s * 0.40, s * 0.12, hex, alpha);
        break;

    case DEV_MOUSE:
        rounded_rect(cr, cx - s * 0.26, cy - s * 0.44,
                     s * 0.52, s * 0.88, s * 0.26);
        rgba(cr, hex, alpha);
        cairo_stroke(cr);
        line(cr, cx, cy - s * 0.30, cx, cy + s * 0.02,
             fmax(1.0, s * 0.07), hex, alpha * 0.8);
        break;

    case DEV_KEYBOARD:
        rounded_rect(cr, cx - s * 0.48, cy - s * 0.26,
                     s * 0.96, s * 0.54, s * 0.09);
        rgba(cr, hex, alpha);
        cairo_stroke(cr);
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 4; c++)
                dot(cr, cx - s * 0.31 + c * s * 0.20,
                    cy - s * 0.11 + r * s * 0.20,
                    s * 0.035, hex, alpha * 0.85);
        break;

    case DEV_GAMEPAD:
        rounded_rect(cr, cx - s * 0.48, cy - s * 0.28,
                     s * 0.96, s * 0.56, s * 0.24);
        rgba(cr, hex, alpha);
        cairo_stroke(cr);
        line(cr, cx - s * 0.30, cy, cx - s * 0.14, cy,
             fmax(1.0, s * 0.07), hex, alpha);
        line(cr, cx - s * 0.22, cy - s * 0.08, cx - s * 0.22, cy + s * 0.08,
             fmax(1.0, s * 0.07), hex, alpha);
        dot(cr, cx + s * 0.24, cy - s * 0.06, s * 0.055, hex, alpha);
        dot(cr, cx + s * 0.34, cy + s * 0.08, s * 0.055, hex, alpha);
        break;

    default:
        bt_glyph(cr, cx, cy, s * 0.82, hex, alpha, 0.0, 0);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Window chrome and sidebar                                          */
/* ------------------------------------------------------------------ */

static void paint_dust(cairo_t *cr, Ui *u)
{
    for (int i = 0; i < 28; i++) {
        double x = fmod(i * 73.0 + 17.0 + u->deco_t * (2.0 + (i % 3) * 0.4) +
                        sin(u->deco_t * 0.11 + i) * 10.0, WIN_W);
        double y = fmod(i * 131.0 + 31.0 + WIN_H * 2.0 -
                        u->deco_t * (1.1 + (i % 4) * 0.3) +
                        cos(u->deco_t * 0.08 + i * 1.3) * 8.0, WIN_H);
        double a = 0.016 + 0.022 * ((i % 5) / 4.0);
        dot(cr, x, y, 0.6 + (i % 3) * 0.35, C_FG, a);
    }
}

static void paint_window_base(cairo_t *cr, Ui *u)
{
    rounded_rect(cr, 0.5, 0.5, WIN_W - 1.0, WIN_H - 1.0, 16.0);
    cairo_pattern_t *bg = cairo_pattern_create_linear(0, 0, 0, WIN_H);
    cairo_pattern_add_color_stop_rgba(bg, 0.0, R_(C_BG_TOP), G_(C_BG_TOP),
                                      B_(C_BG_TOP), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 0.55, R_(C_BG), G_(C_BG),
                                      B_(C_BG), 1.0);
    cairo_pattern_add_color_stop_rgba(bg, 1.0, 0.012, 0.012, 0.014, 1.0);
    cairo_set_source(cr, bg);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(bg);

    rgba(cr, C_BORDER, 0.85);
    cairo_set_line_width(cr, 1.0);
    cairo_stroke(cr);

    rounded_rect(cr, 1.5, 1.5, WIN_W - 3.0, WIN_H - 3.0, 15.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_FG, 0.035);
    cairo_stroke(cr);

    cairo_save(cr);
    rounded_rect(cr, 1.0, 1.0, WIN_W - 2.0, WIN_H - 2.0, 15.0);
    cairo_clip(cr);

    double px = (u->ptr_x / WIN_W - 0.5) * 28.0;
    double on = (u->wifi_k * (1.0 - u->tab_mix) + u->bt_k * u->tab_mix);
    soft_glow(cr, CONTENT_X + 118.0 + px, HERO_Y + 82.0, 300.0,
              C_ACCENT, 0.028 + on * 0.024);
    soft_glow(cr, WIN_W - 130.0 - px * 0.5, WIN_H - 40.0, 260.0,
              C_FG, 0.018);
    soft_glow(cr, SIDEBAR_X + SIDEBAR_W / 2.0, 490.0, 170.0,
              C_ACCENT, 0.018);
    if (!u->ambient_skip)
        paint_dust(cr, u);
}

static void keycap(cairo_t *cr, double x, double y, double w,
                   const char *label)
{
    rounded_rect(cr, x, y, w, 19.0, 5.0);
    rgba(cr, C_CARD_HI, 0.95);
    cairo_fill(cr);
    rounded_rect(cr, x, y, w, 19.0, 5.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.55);
    cairo_stroke(cr);
    text_draw(cr, label, 7.4, 600, C_DIM, 0.9, x + w / 2.0, y + 5.2,
              ALIGN_CENTER, 0.5);
}

static void nav_item(cairo_t *cr, Ui *u, int tab, double y,
                     const char *label, int on)
{
    const double x = 26.0;
    const double w = 176.0;
    const double h = 46.0;
    int active = (u->tab == tab);
    int hover = (u->hover_nav == tab);

    rounded_rect(cr, x, y, w, h, 9.0);
    if (active) {
        rgba(cr, C_ACCENT, 0.085);
        cairo_fill_preserve(cr);
        rounded_rect(cr, x, y, w, h, 9.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_ACCENT, 0.28);
        cairo_stroke(cr);
        rounded_rect(cr, x, y + 9.0, 2.5, 28.0, 1.2);
        rgba(cr, C_ACCENT, 0.9);
        cairo_fill(cr);
    } else {
        rgba(cr, hover ? C_FG : C_FG, hover ? 0.045 : 0.012);
        cairo_fill(cr);
        if (hover) {
            rounded_rect(cr, x, y, w, h, 9.0);
            cairo_set_line_width(cr, 1.0);
            rgba(cr, C_BORDER, 0.45);
            cairo_stroke(cr);
        }
    }

    unsigned icon_col = active ? C_ACCENT : C_DIM;
    double icon_a = active ? 0.95 : (hover ? 0.8 : 0.6);
    if (tab == TAB_WIFI)
        wifi_glyph(cr, x + 26.0, y + 21.0, 30.0, icon_col, icon_a,
                   u->deco_t, 0);
    else
        bt_glyph(cr, x + 26.0, y + 23.0, 23.0, icon_col, icon_a,
                 u->deco_t, 0);

    text_draw(cr, label, 11.5, active ? 600 : 500,
              active ? C_FG : C_DIM, active ? 0.97 : 0.82,
              x + 48.0, y + 13.0, ALIGN_LEFT, 0.15);

    dot(cr, x + w - 18.0, y + h / 2.0, 3.0,
        on ? C_GREEN : C_FAINT, on ? 0.9 : 0.55);
}

static void paint_brand_bars(cairo_t *cr, Ui *u)
{
    double base = 49.0;
    for (int i = 0; i < 4; i++) {
        double hs = 9.0 + 9.0 * fabs(sin(u->deco_t * 1.15 + i * 0.82));
        if (i == 1) hs += 3.0;
        if (i == 3) hs += 2.0;
        rounded_rect(cr, 32.0 + i * 6.0, base - hs, 3.2, hs, 1.6);
        rgba(cr, C_ACCENT, 0.72 + i * 0.065);
        cairo_fill(cr);
    }
}

static void paint_ornament_dots(cairo_t *cr, Ui *u)
{
    const double ox = SIDEBAR_X + SIDEBAR_W / 2.0;
    const double oy = 495.0;
    double a1 = u->deco_t * 0.32;
    double a2 = a1 + PI;
    dot(cr, ox + cos(a1) * 38.0, oy + sin(a1) * 38.0, 2.0, C_FG, 0.45);
    dot(cr, ox + cos(a2) * 29.0, oy + sin(a2) * 29.0, 1.5, C_ACCENT, 0.55);
}

static void paint_sidebar(cairo_t *cr, Ui *u)
{
    rounded_rect(cr, SIDEBAR_X, SIDEBAR_Y, SIDEBAR_W, SIDEBAR_H, 13.0);
    cairo_pattern_t *p = cairo_pattern_create_linear(
        SIDEBAR_X, SIDEBAR_Y, SIDEBAR_X, SIDEBAR_Y + SIDEBAR_H);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.052, 0.055, 0.060, 0.96);
    cairo_pattern_add_color_stop_rgba(p, 0.55, 0.031, 0.033, 0.036, 0.94);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.018, 0.019, 0.021, 0.94);
    cairo_set_source(cr, p);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(p);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.55);
    cairo_stroke(cr);

    rounded_rect(cr, SIDEBAR_X + 1.0, SIDEBAR_Y + 1.0,
                 SIDEBAR_W - 2.0, SIDEBAR_H - 2.0, 12.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_FG, 0.025);
    cairo_stroke(cr);

    /* brand ---------------------------------------------------------- */
    if (!u->ambient_skip)
        paint_brand_bars(cr, u);
    text_draw(cr, "JTLab", 15.0, 700, C_FG, 0.98,
              60.0, 25.0, ALIGN_LEFT, 0.0);
    text_draw(cr, "NETWORKS", 8.0, 600, C_ACCENT, 0.75,
              60.0, 45.0, ALIGN_LEFT, 1.65);

    line(cr, 28.0, 78.0, 200.0, 78.0, 1.0, C_BORDER, 0.42);

    text_draw(cr, "RADIO", 8.1, 600, C_FAINT, 0.9,
              32.0, 96.0, ALIGN_LEFT, 1.5);
    nav_item(cr, u, TAB_WIFI, 114.0, "Wi-Fi", u->wifi_on);
    nav_item(cr, u, TAB_BLUETOOTH, 168.0, "Bluetooth", u->bt_on);

    /* radio status --------------------------------------------------- */
    text_draw(cr, "STATUS", 8.1, 600, C_FAINT, 0.9,
              32.0, 230.0, ALIGN_LEFT, 1.5);

    rounded_rect(cr, 26.0, 248.0, 176.0, 86.0, 10.0);
    cairo_set_source_rgba(cr, 0.020, 0.022, 0.025, 0.9);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.38);
    cairo_stroke(cr);

    dot(cr, 40.0, 269.0, 3.2, u->wifi_on ? C_GREEN : C_FAINT,
        u->wifi_on ? 0.9 : 0.55);
    text_draw(cr, "Wi-Fi", 9.6, 500, C_DIM, 0.9,
              52.0, 261.0, ALIGN_LEFT, 0.1);
    text_draw(cr, u->wifi_on ? "ON" : "OFF", 8.4, 600,
              u->wifi_on ? C_GREEN : C_FAINT, u->wifi_on ? 0.9 : 0.72,
              190.0, 262.0, ALIGN_RIGHT, 1.2);

    line(cr, 38.0, 285.0, 190.0, 285.0, 1.0, C_BORDER, 0.28);

    dot(cr, 40.0, 301.0, 3.2, u->bt_on ? C_GREEN : C_FAINT,
        u->bt_on ? 0.9 : 0.55);
    text_draw(cr, "Bluetooth", 9.6, 500, C_DIM, 0.9,
              52.0, 293.0, ALIGN_LEFT, 0.1);
    text_draw(cr, u->bt_on ? "ON" : "OFF", 8.4, 600,
              u->bt_on ? C_GREEN : C_FAINT, u->bt_on ? 0.9 : 0.72,
              190.0, 294.0, ALIGN_RIGHT, 1.2);

    /* shortcuts ------------------------------------------------------ */
    text_draw(cr, "SHORTCUTS", 8.1, 600, C_FAINT, 0.9,
              32.0, 350.0, ALIGN_LEFT, 1.5);

    keycap(cr, 32.0, 370.0, 38.0, "TAB");
    text_draw(cr, "Switch tab", 9.1, 400, C_DIM, 0.82,
              80.0, 373.0, ALIGN_LEFT, 0.0);

    keycap(cr, 32.0, 398.0, 56.0, "SPACE");
    text_draw(cr, "Toggle radio", 9.1, 400, C_DIM, 0.82,
              98.0, 401.0, ALIGN_LEFT, 0.0);

    keycap(cr, 32.0, 426.0, 32.0, "ESC");
    text_draw(cr, "Close", 9.1, 400, C_DIM, 0.82,
              74.0, 429.0, ALIGN_LEFT, 0.0);

    /* quiet ornament ------------------------------------------------ */
    const double ox = SIDEBAR_X + SIDEBAR_W / 2.0;
    const double oy = 495.0;
    ring(cr, ox, oy, 38.0, 1.0, C_ACCENT, 0.065);
    ring(cr, ox, oy, 29.0, 1.0, C_ACCENT, 0.075);
    ring(cr, ox, oy, 20.0, 1.0, C_ACCENT, 0.085);
    ring(cr, ox, oy, 11.0, 1.0, C_ACCENT, 0.10);
    dot(cr, ox, oy, 2.6, C_ACCENT, 0.65);
    if (!u->ambient_skip)
        paint_ornament_dots(cr, u);

    text_draw(cr, "JTLab Networks", 8.0, 500, C_FAINT, 0.82,
              ox, 550.0, ALIGN_CENTER, 0.9);
    text_draw(cr, "v1.0", 7.2, 500, C_FAINT, 0.55,
              ox, 565.0, ALIGN_CENTER, 0.6);
}

/* ------------------------------------------------------------------ */
/*  Header and controls                                                */
/* ------------------------------------------------------------------ */

static void paint_toggle(cairo_t *cr, Ui *u, double k)
{
    k = clampd(k, 0.0, 1.0);
    int hover = u->hover_toggle;
    double x = TOGGLE_X;
    double y = TOGGLE_Y;
    double w = TOGGLE_W;
    double h = TOGGLE_H;

    if (k > 0.01) {
        soft_glow(cr, x + w / 2.0, y + h / 2.0, 58.0,
                  C_ACCENT, 0.13 * k);
    }

    rounded_rect(cr, x, y, w, h, h / 2.0);
    rgba(cr, C_TRACK, 1.0);
    cairo_fill(cr);

    if (k > 0.005) {
        cairo_save(cr);
        rounded_rect(cr, x, y, w, h, h / 2.0);
        cairo_clip(cr);
        cairo_pattern_t *p = cairo_pattern_create_linear(
            x, y, x + w, y + h);
        cairo_pattern_add_color_stop_rgba(p, 0.0, R_(C_ACCENT_LO),
                                          G_(C_ACCENT_LO), B_(C_ACCENT_LO), k);
        cairo_pattern_add_color_stop_rgba(p, 0.55, R_(C_ACCENT),
                                          G_(C_ACCENT), B_(C_ACCENT), k);
        cairo_pattern_add_color_stop_rgba(p, 1.0, R_(C_ACCENT_HI),
                                          G_(C_ACCENT_HI), B_(C_ACCENT_HI), k);
        cairo_set_source(cr, p);
        cairo_paint(cr);
        cairo_pattern_destroy(p);
        cairo_restore(cr);
    }

    rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, h / 2.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.85);
    cairo_stroke(cr);
    if (k > 0.01) {
        rounded_rect(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0, h / 2.0);
        rgba(cr, C_ACCENT_HI, 0.35 * k);
        cairo_stroke(cr);
    }
    if (hover) {
        rounded_rect(cr, x - 1.0, y - 1.0, w + 2.0, h + 2.0, h / 2.0 + 1.0);
        rgba(cr, C_FG, 0.08);
        cairo_stroke(cr);
    }

    double kr = h / 2.0 - 4.0;
    if (u->press_toggle)
        kr *= 0.90;
    double kx = x + h / 2.0 + (w - h) * k;
    double ky = y + h / 2.0;

    cairo_arc(cr, kx, ky + 1.0, kr, 0, 2.0 * PI);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0,
                          0.18 * (1.0 - k) + 0.10);
    cairo_fill(cr);

    cairo_arc(cr, kx, ky, kr, 0, 2.0 * PI);
    rgba(cr, 0xD8D8D8, 1.0 - k * 0.96);
    cairo_fill(cr);
    if (k > 0.001) {
        cairo_arc(cr, kx, ky, kr, 0, 2.0 * PI);
        rgba(cr, C_ON_ACCENT, k);
        cairo_fill(cr);
        dot(cr, kx - kr * 0.25, ky - kr * 0.30, kr * 0.30,
            C_ACCENT_HI, 0.22 * k);
    }
}

static void paint_close(cairo_t *cr, Ui *u)
{
    if (u->hover_close) {
        cairo_arc(cr, CLOSE_X, CLOSE_Y, 15.0, 0, 2.0 * PI);
        rgba(cr, C_FG, 0.07);
        cairo_fill(cr);
    }
    unsigned col = u->hover_close ? C_FG : C_DIM;
    double a = u->hover_close ? 0.95 : 0.62;
    line(cr, CLOSE_X - 4.5, CLOSE_Y - 4.5,
         CLOSE_X + 4.5, CLOSE_Y + 4.5, 1.5, col, a);
    line(cr, CLOSE_X + 4.5, CLOSE_Y - 4.5,
         CLOSE_X - 4.5, CLOSE_Y + 4.5, 1.5, col, a);
}

static void paint_header(cairo_t *cr, Ui *u)
{
    double mix = u->tab_mix;

    line(cr, CONTENT_X, 96.0, WIN_W - 24.0, 96.0,
         1.0, C_BORDER, 0.32);

    if (mix < 0.999) {
        text_draw(cr, "Wi-Fi", 26.0, 700, C_FG, 1.0 - mix,
                  CONTENT_X, 26.0 - mix * 5.0, ALIGN_LEFT, 0.0);
        text_draw(cr, "Wireless network control", 9.8, 400, C_DIM,
                  (1.0 - mix) * 0.82, CONTENT_X, 66.0 - mix * 3.0,
                  ALIGN_LEFT, 0.2);
    }
    if (mix > 0.001) {
        text_draw(cr, "Bluetooth", 26.0, 700, C_FG, mix,
                  CONTENT_X, 26.0 + (1.0 - mix) * 5.0, ALIGN_LEFT, 0.0);
        text_draw(cr, "Bluetooth device control", 9.8, 400, C_DIM,
                  mix * 0.82, CONTENT_X, 66.0 + (1.0 - mix) * 3.0,
                  ALIGN_LEFT, 0.2);
    }

    double k = u->wifi_k * (1.0 - mix) + u->bt_k * mix;
    int wifi_side = mix < 0.5;
    int on = wifi_side ? u->wifi_on : u->bt_on;
    if (mix < 0.999)
        text_draw(cr, u->wifi_on ? "RADIO ON" : "RADIO OFF", 8.2, 600,
                  u->wifi_on ? C_ACCENT : C_FAINT,
                  (1.0 - mix) * (u->wifi_on ? 0.92 : 0.8),
                  TOGGLE_X - 12.0, 44.0, ALIGN_RIGHT, 1.25);
    if (mix > 0.001)
        text_draw(cr, u->bt_on ? "RADIO ON" : "RADIO OFF", 8.2, 600,
                  u->bt_on ? C_ACCENT : C_FAINT,
                  mix * (u->bt_on ? 0.92 : 0.8),
                  TOGGLE_X - 12.0, 44.0, ALIGN_RIGHT, 1.25);

    paint_toggle(cr, u, k);
    (void)on;
    paint_close(cr, u);
}

/* ------------------------------------------------------------------ */
/*  Hero card                                                          */
/* ------------------------------------------------------------------ */

static void hero_begin(cairo_t *cr, Ui *u, int on)
{
    rounded_rect(cr, CONTENT_X, HERO_Y, CONTENT_W, HERO_H, 14.0);
    cairo_pattern_t *p = cairo_pattern_create_linear(
        CONTENT_X, HERO_Y, CONTENT_X, HERO_Y + HERO_H);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.075, 0.080, 0.088, 0.95);
    cairo_pattern_add_color_stop_rgba(p, 0.52, 0.045, 0.048, 0.053, 0.96);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.026, 0.028, 0.031, 0.96);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);

    cairo_save(cr);
    rounded_rect(cr, CONTENT_X, HERO_Y, CONTENT_W, HERO_H, 14.0);
    cairo_clip(cr);

    double icx = CONTENT_X + 74.0;
    double icy = HERO_Y + 78.0;
    soft_glow(cr, icx, icy, 132.0, C_ACCENT, on ? 0.105 : 0.025);
    if (on && !u->ambient_skip) {
        for (int i = 0; i < 2; i++) {
            double ph = fmod(u->deco_t * 0.30 + i * 0.5, 1.0);
            ring(cr, icx, icy, 48.0 + ph * 24.0, 1.1,
                 C_ACCENT, 0.085 * (1.0 - ph));
        }
    }
}

static void hero_end(cairo_t *cr, int on)
{
    cairo_restore(cr);
    rounded_rect(cr, CONTENT_X + 0.5, HERO_Y + 0.5,
                 CONTENT_W - 1.0, HERO_H - 1.0, 13.5);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.62);
    cairo_stroke(cr);
    if (on) {
        rounded_rect(cr, CONTENT_X + 1.5, HERO_Y + 1.5,
                     CONTENT_W - 3.0, HERO_H - 3.0, 13.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_ACCENT, 0.075);
        cairo_stroke(cr);
    }
}

static void hero_icon_plate(cairo_t *cr, Ui *u, int on, int is_wifi)
{
    if (u->ambient_skip)
        return;

    double x = CONTENT_X + 28.0;
    double y = HERO_Y + 32.0;
    double w = 92.0;
    double h = 92.0;
    double cx = x + w / 2.0;
    double cy = y + h / 2.0;

    rounded_rect(cr, x, y, w, h, 24.0);
    cairo_pattern_t *p = cairo_pattern_create_linear(x, y, x, y + h);
    cairo_pattern_add_color_stop_rgba(p, 0.0, 0.075, 0.080, 0.088, 0.94);
    cairo_pattern_add_color_stop_rgba(p, 1.0, 0.022, 0.024, 0.027, 0.98);
    cairo_set_source(cr, p);
    cairo_fill(cr);
    cairo_pattern_destroy(p);

    rounded_rect(cr, x, y, w, h, 24.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.62);
    cairo_stroke(cr);
    if (on) {
        rounded_rect(cr, x + 1.0, y + 1.0, w - 2.0, h - 2.0, 23.0);
        rgba(cr, C_ACCENT, 0.18);
        cairo_stroke(cr);
    }

    /* Keep the hero glyph itself still; the rings behind the plate carry
     * the ambient motion. */
    if (is_wifi)
        wifi_glyph(cr, cx, cy - 3.0, 100.0, C_ACCENT,
                   on ? 0.96 : 0.30, u->deco_t, 0);
    else
        bt_glyph(cr, cx, cy, 55.0, C_ACCENT,
                 on ? 0.96 : 0.30, u->deco_t, 0);

    if (on)
        dot(cr, x + w - 13.0, y + h - 13.0, 2.4, C_GREEN, 0.85);
}

static void hero_stat(cairo_t *cr, double x, const char *label,
                      const char *value, double alpha)
{
    text_draw(cr, label, 7.2, 600, C_FAINT, alpha * 0.86,
              x, HERO_Y + 106.0, ALIGN_LEFT, 1.2);
    text_draw(cr, value, 10.4, 600, C_FG, alpha * 0.92,
              x, HERO_Y + 118.0, ALIGN_LEFT, 0.1);
}

static void hero_separator(cairo_t *cr, double x)
{
    line(cr, x, HERO_Y + 108.0, x, HERO_Y + 142.0,
         1.0, C_BORDER, 0.30);
}

static void paint_hero_wifi(cairo_t *cr, Ui *u)
{
    int on = u->wifi_on;
    double a = on ? 1.0 : 0.42;
    double tx = CONTENT_X + 142.0;

    hero_begin(cr, u, on);
    hero_icon_plate(cr, u, on, 1);

    dot(cr, tx + 3.0, HERO_Y + 29.0, 3.0,
        (on && u->wifi_connected) ? C_GREEN : C_FAINT,
        (on && u->wifi_connected) ? 0.9 : 0.55);
    text_draw(cr, !on ? "RADIO OFF" :
              (u->wifi_connected ? "CONNECTED" : "NOT CONNECTED"),
              8.5, 600,
              !on ? C_FAINT : (u->wifi_connected ? C_GREEN : C_DIM),
              a * 0.95, tx + 14.0, HERO_Y + 22.0, ALIGN_LEFT, 1.45);

    const char *title = !on ? "Wi-Fi is off" :
                        (u->wifi_connected ? u->ssid : "Not connected");
    text_draw(cr, title, 23.5, 700, C_FG, on ? 0.97 : 0.40,
              tx, HERO_Y + 38.0, ALIGN_LEFT, 0.0);

    char sub[192];
    if (!on)
        snprintf(sub, sizeof sub, "Enable the radio to scan for networks.");
    else if (u->wifi_connected)
        snprintf(sub, sizeof sub, "%s · NetworkManager",
                 u->security[0] ? u->security : "Wireless network");
    else
        snprintf(sub, sizeof sub, "No active connection");
    text_draw(cr, sub, 9.2, 400, C_DIM, a * 0.80,
              tx, HERO_Y + 76.0, ALIGN_LEFT, 0.15);

    char sig[32];
    snprintf(sig, sizeof sig, "%d%%", u->signal);
    hero_stat(cr, tx, "IP ADDRESS",
              (on && u->wifi_connected && u->ip[0]) ? u->ip : "—", a);
    hero_stat(cr, tx + 148.0, "SIGNAL",
              (on && u->wifi_connected) ? sig : "—", a);
    hero_stat(cr, tx + 294.0, "SECURITY",
              (on && u->wifi_connected && u->security[0]) ?
              u->security : "—", a);
    hero_separator(cr, tx + 132.0);
    hero_separator(cr, tx + 278.0);

    hero_end(cr, on);
}

static void paint_hero_bt(cairo_t *cr, Ui *u)
{
    int on = u->bt_on;
    double a = on ? 1.0 : 0.42;
    double tx = CONTENT_X + 142.0;

    hero_begin(cr, u, on);
    hero_icon_plate(cr, u, on, 0);

    dot(cr, tx + 3.0, HERO_Y + 29.0, 3.0,
        on ? C_GREEN : C_FAINT, on ? 0.9 : 0.55);
    text_draw(cr, on ? "POWERED ON" : "RADIO OFF", 8.5, 600,
              on ? C_GREEN : C_FAINT, a * 0.95,
              tx + 14.0, HERO_Y + 22.0, ALIGN_LEFT, 1.45);

    text_draw(cr, "Bluetooth", 23.5, 700, C_FG, on ? 0.97 : 0.40,
              tx, HERO_Y + 38.0, ALIGN_LEFT, 0.0);

    char sub[192];
    if (!on)
        snprintf(sub, sizeof sub, "Enable the radio to manage devices.");
    else if (u->bt_name[0])
        snprintf(sub, sizeof sub, "%s · adapter hci0", u->bt_name);
    else
        snprintf(sub, sizeof sub, "BlueZ · adapter hci0");
    text_draw(cr, sub, 9.2, 400, C_DIM, a * 0.80,
              tx, HERO_Y + 76.0, ALIGN_LEFT, 0.15);

    char paired[32];
    snprintf(paired, sizeof paired, "%d %s", u->bt_paired_total,
             u->bt_paired_total == 1 ? "device" : "devices");
    hero_stat(cr, tx, "ADAPTER", "hci0", a);
    hero_stat(cr, tx + 148.0, "PAIRED",
              on ? paired : "—", a);
    hero_stat(cr, tx + 294.0, "DISCOVERABLE",
              on ? (u->bt_discoverable ? "Yes" : "No") : "—", a);
    hero_separator(cr, tx + 132.0);
    hero_separator(cr, tx + 278.0);

    hero_end(cr, on);
}

/* ------------------------------------------------------------------ */
/*  Lists                                                              */
/* ------------------------------------------------------------------ */

static void section_action_ambient(cairo_t *cr, Ui *u, int action_kind)
{
    double bw = action_kind == 0 ? 78.0 : 92.0;
    double bx = CONTENT_X + CONTENT_W - bw;
    double by = SECTION_Y - 6.0;
    int hover = action_kind == 0 ? u->hover_scan : u->hover_pair;
    int enabled = action_kind == 0 ? u->wifi_on : u->bt_on;
    unsigned ic = enabled ? (hover ? C_ACCENT_HI : C_ACCENT) : C_FAINT;
    double ia = enabled ? (hover ? 1.0 : 0.82) : 0.35;

    if (action_kind == 0)
        radar_icon(cr, bx + 18.0, by + 14.0, 6.2, ic, ia, u->deco_t);

    double at = action_kind == 0 ? u->scan_t : u->pair_t;
    double age = u->t - at;
    if (enabled && age >= 0.0 && age < 2.0) {
        double a = age * 7.0;
        cairo_set_line_width(cr, 1.4);
        cairo_new_path(cr);
        cairo_arc(cr, bx + 18.0, by + 14.0, 8.5,
                  a, a + PI * 1.25);
        rgba(cr, C_ACCENT, 0.9);
        cairo_stroke(cr);
    }
}

static void section_header(cairo_t *cr, Ui *u, const char *label,
                           const char *action, int action_kind)
{
    text_draw(cr, label, 8.9, 600, C_FAINT, 0.95,
              CONTENT_X, SECTION_Y, ALIGN_LEFT, 1.65);

    double bw = action_kind == 0 ? 78.0 : 92.0;
    double bx = CONTENT_X + CONTENT_W - bw;
    double by = SECTION_Y - 6.0;
    int hover = action_kind == 0 ? u->hover_scan : u->hover_pair;
    int enabled = action_kind == 0 ? u->wifi_on : u->bt_on;

    rounded_rect(cr, bx, by, bw, 28.0, 8.0);
    rgba(cr, hover && enabled ? C_CARD_HI : C_CARD, 0.90);
    cairo_fill_preserve(cr);
    rounded_rect(cr, bx, by, bw, 28.0, 8.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, hover && enabled ? C_ACCENT : C_BORDER,
         hover && enabled ? 0.34 : 0.45);
    cairo_stroke(cr);

    unsigned ic = enabled ? (hover ? C_ACCENT_HI : C_ACCENT) : C_FAINT;
    double ia = enabled ? (hover ? 1.0 : 0.82) : 0.35;
    if (action_kind == 0) {
        text_draw(cr, action, 8.0, 600, ic, ia,
                  bx + 33.0, by + 8.0, ALIGN_LEFT, 1.1);
    } else {
        plus_icon(cr, bx + 17.0, by + 14.0, 8.0, ic, ia);
        text_draw(cr, action, 8.0, 600, ic, ia,
                  bx + 29.0, by + 8.0, ALIGN_LEFT, 1.1);
    }

    if (!u->ambient_skip)
        section_action_ambient(cr, u, action_kind);
}

static void row_shell(cairo_t *cr, Ui *u, double y, int connected,
                      int hover)
{
    rounded_rect(cr, CONTENT_X, y, CONTENT_W, ROW_H, 10.0);
    if (connected) {
        rgba(cr, C_ACCENT, 0.045 + (hover ? 0.018 : 0.0));
        cairo_fill_preserve(cr);
        rounded_rect(cr, CONTENT_X, y, CONTENT_W, ROW_H, 10.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_ACCENT, hover ? 0.38 : 0.26);
        cairo_stroke(cr);
    } else {
        rgba(cr, hover ? C_CARD_HI : C_CARD, hover ? 0.96 : 0.72);
        cairo_fill_preserve(cr);
        rounded_rect(cr, CONTENT_X, y, CONTENT_W, ROW_H, 10.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_BORDER, hover ? 0.62 : 0.34);
        cairo_stroke(cr);
    }

    if (connected) {
        rounded_rect(cr, CONTENT_X + 0.5, y + 12.0, 2.5, 30.0, 1.2);
        rgba(cr, C_ACCENT, 0.85);
        cairo_fill(cr);
    }
}

static int action_is_forget(int kind)
{
    return kind == WIFI_ACTION_FORGET || kind == BT_ACTION_FORGET;
}

static int action_is_connect(int kind)
{
    return kind == BT_ACTION_CONNECT;
}

static void row_action_rect(int kind, double right, double y,
                            double *x, double *w)
{
    if (action_is_forget(kind)) {
        *w = ROW_ACTION_FORGET_W;
        *x = right - ROW_ACTION_FORGET_W;
    } else {
        *w = ROW_ACTION_DISCONNECT_W;
        *x = right - ROW_ACTION_FORGET_W - ROW_ACTION_GAP -
             ROW_ACTION_DISCONNECT_W;
    }
    (void)y;
}

static void paint_row_action(cairo_t *cr, Ui *u, int row, int kind,
                             double x, double y, double w)
{
    int hovered = (u->hover_row_action_idx == row &&
                   u->hover_row_action == kind);
    int forget = action_is_forget(kind);
    int connect = action_is_connect(kind);
    unsigned fg = forget ? C_DIM : C_ACCENT;
    unsigned border = forget ? C_RED : C_ACCENT;
    if (hovered)
        fg = forget ? C_RED : C_ACCENT_HI;

    rounded_rect(cr, x, y, w, ROW_ACTION_H, 5.0);
    rgba(cr, hovered ? C_CARD_HI : C_TRACK, hovered ? 0.96 : 0.82);
    cairo_fill_preserve(cr);
    rounded_rect(cr, x, y, w, ROW_ACTION_H, 5.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, border, hovered ? 0.54 : (forget ? 0.22 : 0.32));
    cairo_stroke(cr);

    text_draw(cr, forget ? "FORGET" : (connect ? "CONNECT" : "DISCONNECT"),
              7.2, 600, fg,
              hovered ? 0.95 : (forget ? 0.66 : 0.82),
              x + w / 2.0, y + 5.5, ALIGN_CENTER, 0.8);
}

static void paint_wifi_list(cairo_t *cr, Ui *u)
{
    for (int i = 0; i < u->net_count && i < 4; i++) {
        WifiNet *n = &u->nets[i];
        double y = LIST_Y + i * (ROW_H + ROW_GAP);
        int hover = (u->hover_row == i);
        row_shell(cr, u, y, n->connected, hover);

        unsigned bar_col = n->connected ? C_ACCENT : C_DIM;
        signal_bars(cr, CONTENT_X + 22.0, y + 35.0,
                    signal_level(n->signal), bar_col,
                    n->connected ? 0.95 : 0.58);

        text_draw(cr, n->ssid, 11.8, n->connected ? 600 : 500,
                  C_FG, n->connected ? 0.96 :
                  (n->saved && n->signal < 0 ? 0.68 : 0.78),
                  CONTENT_X + 52.0, y + 11.0, ALIGN_LEFT, 0.0);

        const char *sec;
        if (n->security[0]) {
            sec = n->security;
        } else if (n->saved) {
            sec = n->signal < 0 ? "Not in range" : "Saved network";
        } else {
            sec = "Open network";
        }
        text_draw(cr, sec, 8.4, 400, C_DIM, 0.62,
                  CONTENT_X + 52.0, y + 29.0, ALIGN_LEFT, 0.15);
        if (n->saved && !n->connected) {
            double sw = text_width(cr, sec, 8.4, 400, 0.15);
            text_draw(cr, "·  CONNECTED BEFORE", 7.2, 600, C_ACCENT, 0.80,
                      CONTENT_X + 52.0 + sw + 5.0, y + 29.0,
                      ALIGN_LEFT, 0.7);
        }

        int locked = n->security[0] && strstr(n->security, "WPA");
        if (!locked && n->security[0] && strstr(n->security, "WEP"))
            locked = 1;
        if (locked && !n->connected && !n->saved)
            lock_glyph(cr, CONTENT_X + CONTENT_W - 80.0, y + 27.0,
                       12.0, C_FAINT, 0.62);

        char pct[16];
        if (n->signal >= 0) {
            snprintf(pct, sizeof pct, "%d%%", n->signal);
            text_draw(cr, pct, 10.0, 600,
                      n->connected ? C_ACCENT : C_DIM,
                      n->connected ? 0.9 : 0.62,
                      CONTENT_X + CONTENT_W - 20.0, y + 10.0,
                      ALIGN_RIGHT, 0.0);
        } else {
            text_draw(cr, "SAVED", 7.4, 600, C_FAINT, 0.62,
                      CONTENT_X + CONTENT_W - 20.0, y + 10.0,
                      ALIGN_RIGHT, 0.9);
        }

        int busy = (u->wifi_action_kind != WIFI_ACTION_NONE &&
                    u->wifi_action_row == i);
        if (busy) {
            const char *label = u->wifi_action_kind == WIFI_ACTION_DISCONNECT ?
                                "DISCONNECTING…" :
                                (u->wifi_action_kind == WIFI_ACTION_FORGET ?
                                 "FORGETTING…" : "CONNECTING…");
            text_draw(cr, label, 7.4, 600, C_ACCENT, 0.86,
                      CONTENT_X + CONTENT_W - 20.0, y + 29.0,
                      ALIGN_RIGHT, 0.8);
        } else if (n->connected || n->saved) {
            double right = CONTENT_X + CONTENT_W - ROW_ACTION_RIGHT_PAD;
            double ay = y + ROW_ACTION_Y_OFF;
            if (n->connected) {
                double ax, aw;
                row_action_rect(WIFI_ACTION_DISCONNECT, right, ay,
                                &ax, &aw);
                paint_row_action(cr, u, i, WIFI_ACTION_DISCONNECT,
                                 ax, ay, aw);
            }
            if (n->saved || n->connected) {
                double ax, aw;
                row_action_rect(WIFI_ACTION_FORGET, right, ay, &ax, &aw);
                paint_row_action(cr, u, i, WIFI_ACTION_FORGET, ax, ay, aw);
            }
        } else {
            text_draw(cr, hover ? "CONNECT" : "Available",
                      8.2, 500,
                      hover ? C_ACCENT : C_FAINT,
                      hover ? 0.90 : 0.62,
                      CONTENT_X + CONTENT_W - 20.0, y + 29.0,
                      ALIGN_RIGHT, 0.1);
        }
    }
}

static const char *dev_kind_name(DevKind k)
{
    switch (k) {
    case DEV_HEADPHONES: return "Headphones";
    case DEV_EARBUDS:    return "Earbuds";
    case DEV_MOUSE:      return "Mouse";
    case DEV_KEYBOARD:   return "Keyboard";
    case DEV_GAMEPAD:    return "Game controller";
    default:             return "Bluetooth device";
    }
}

static void paint_bt_list(cairo_t *cr, Ui *u)
{
    for (int i = 0; i < u->bt_count && i < 4; i++) {
        BtDev *d = &u->bt[i];
        double y = LIST_Y + i * (ROW_H + ROW_GAP);
        int hover = (u->hover_row == i);
        row_shell(cr, u, y, d->connected, hover);

        rounded_rect(cr, CONTENT_X + 22.0, y + 12.0, 30.0, 30.0, 8.0);
        rgba(cr, C_TRACK, 0.92);
        cairo_fill_preserve(cr);
        rounded_rect(cr, CONTENT_X + 22.0, y + 12.0, 30.0, 30.0, 8.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_BORDER, 0.38);
        cairo_stroke(cr);
        device_glyph(cr, d->kind, CONTENT_X + 37.0, y + 27.0,
                     17.0, d->connected ? C_ACCENT : C_DIM,
                     d->connected ? 0.95 : 0.75);

        text_draw(cr, d->name, 11.8, d->connected ? 600 : 500,
                  C_FG, d->connected ? 0.96 : 0.80,
                  CONTENT_X + 62.0, y + 11.0, ALIGN_LEFT, 0.0);
        text_draw(cr, dev_kind_name(d->kind), 8.4, 400, C_DIM, 0.62,
                  CONTENT_X + 62.0, y + 29.0, ALIGN_LEFT, 0.15);

        const char *status = d->connected ? "Connected" : "Paired";
        unsigned sc = d->connected ? C_GREEN : C_FAINT;
        double sa = d->connected ? 0.88 : 0.62;
        text_draw(cr, status, 8.8, 500, sc, sa,
                  CONTENT_X + CONTENT_W - 20.0, y + 10.0,
                  ALIGN_RIGHT, 0.1);

        int busy = (u->bt_action_kind != BT_ACTION_NONE &&
                    u->bt_action_row == i);
        if (busy) {
            const char *label = u->bt_action_kind == BT_ACTION_DISCONNECT ?
                                "DISCONNECTING…" :
                                (u->bt_action_kind == BT_ACTION_FORGET ?
                                 "FORGETTING…" : "CONNECTING…");
            text_draw(cr, label, 7.4, 600, C_ACCENT, 0.86,
                      CONTENT_X + CONTENT_W - 20.0, y + 29.0,
                      ALIGN_RIGHT, 0.8);
        } else {
            double right = CONTENT_X + CONTENT_W - ROW_ACTION_RIGHT_PAD;
            double ay = y + ROW_ACTION_Y_OFF;
            int main_kind = d->connected ? BT_ACTION_DISCONNECT :
                                          BT_ACTION_CONNECT;
            double ax, aw;
            row_action_rect(main_kind, right, ay, &ax, &aw);
            paint_row_action(cr, u, i, main_kind, ax, ay, aw);
            row_action_rect(BT_ACTION_FORGET, right, ay, &ax, &aw);
            paint_row_action(cr, u, i, BT_ACTION_FORGET, ax, ay, aw);
        }
    }
}

static void paint_bt_adapter_card(cairo_t *cr, Ui *u)
{
    if (!u->bt_on || u->bt_count == 0 || u->bt_count >= 4)
        return;

    double y = LIST_Y + u->bt_count * (ROW_H + ROW_GAP) + 2.0;
    if (y + 46.0 > 546.0)
        return;

    rounded_rect(cr, CONTENT_X, y, CONTENT_W, 46.0, 10.0);
    rgba(cr, C_CARD, 0.48);
    cairo_fill_preserve(cr);
    rounded_rect(cr, CONTENT_X, y, CONTENT_W, 46.0, 10.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.30);
    cairo_stroke(cr);

    double tx = CONTENT_X + 18.0;
    text_draw(cr, "ADAPTER", 7.0, 600, C_FAINT, 0.8,
              tx, y + 10.0, ALIGN_LEFT, 1.2);
    text_draw(cr, "hci0", 9.5, 600, C_DIM, 0.85,
              tx, y + 24.0, ALIGN_LEFT, 0.1);

    tx = CONTENT_X + 170.0;
    text_draw(cr, "CONTROLLER", 7.0, 600, C_FAINT, 0.8,
              tx, y + 10.0, ALIGN_LEFT, 1.2);
    text_draw(cr, u->bt_name[0] ? u->bt_name : "BlueZ",
              9.5, 500, C_DIM, 0.85, tx, y + 24.0, ALIGN_LEFT, 0.1);

    tx = CONTENT_X + CONTENT_W - 18.0;
    text_draw(cr, "DISCOVERABLE", 7.0, 600, C_FAINT, 0.8,
              tx, y + 10.0, ALIGN_RIGHT, 1.2);
    text_draw(cr, u->bt_discoverable ? "Yes" : "No", 9.5, 600,
              u->bt_discoverable ? C_GREEN : C_DIM,
              u->bt_discoverable ? 0.88 : 0.75,
              tx, y + 24.0, ALIGN_RIGHT, 0.1);
}

/* ------------------------------------------------------------------ */
/*  Empty states and tab bodies                                        */
/* ------------------------------------------------------------------ */

static void paint_empty_state(cairo_t *cr, Ui *u, int is_wifi, int radio_off)
{
    double cx = CONTENT_X + CONTENT_W / 2.0;
    double card_y = LIST_Y;
    double card_h = 240.0;

    rounded_rect(cr, CONTENT_X, card_y, CONTENT_W, card_h, 12.0);
    rgba(cr, C_CARD, 0.38);
    cairo_fill_preserve(cr);
    rounded_rect(cr, CONTENT_X, card_y, CONTENT_W, card_h, 12.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.28);
    cairo_stroke(cr);

    double icon_y = card_y + 54.0;
    if (is_wifi)
        wifi_glyph(cr, cx, icon_y - 6.0, 72.0, C_ACCENT,
                   radio_off ? 0.20 : 0.16, u->deco_t, 0);
    else
        bt_glyph(cr, cx, icon_y, 44.0, C_ACCENT,
                 radio_off ? 0.20 : 0.16, u->deco_t, 0);

    if (radio_off) {
        line(cr, cx - 24.0, icon_y - 20.0,
             cx + 24.0, icon_y + 24.0, 2.2, C_FAINT, 0.52);
    }

    const char *title;
    const char *sub;
    if (radio_off) {
        title = is_wifi ? "Wireless radio is off" : "Bluetooth is off";
        sub = is_wifi ? "Turn the radio on to see nearby networks."
                      : "Turn the radio on to manage devices.";
    } else {
        title = is_wifi ? "No networks found" : "No paired devices";
        sub = is_wifi ? "Use SCAN to look again."
                      : "Use PAIR NEW to add a device.";
    }

    text_draw(cr, title, 17.0, 600, C_FG, 0.82,
              cx, card_y + 102.0, ALIGN_CENTER, 0.0);
    text_draw(cr, sub, 9.2, 400, C_DIM, 0.68,
              cx, card_y + 130.0, ALIGN_CENTER, 0.15);

    if (radio_off) {
        double bw = is_wifi ? 158.0 : 190.0;
        double bh = 38.0;
        double bx = cx - bw / 2.0;
        double by = card_y + 162.0;
        int hov = u->hover_enable;
        rounded_rect(cr, bx, by, bw, bh, bh / 2.0);
        rgba(cr, C_ACCENT, hov ? 0.17 : 0.10);
        cairo_fill_preserve(cr);
        rounded_rect(cr, bx, by, bw, bh, bh / 2.0);
        cairo_set_line_width(cr, 1.0);
        rgba(cr, C_ACCENT, hov ? 0.62 : 0.40);
        cairo_stroke(cr);
        text_draw(cr, is_wifi ? "ENABLE WI-FI" : "ENABLE BLUETOOTH",
                  8.8, 600, C_ACCENT, hov ? 1.0 : 0.88,
                  cx, by + 13.0, ALIGN_CENTER, 1.45);
    }
}

static void paint_footer(cairo_t *cr, Ui *u)
{
    line(cr, CONTENT_X, 558.0, WIN_W - 24.0, 558.0,
         1.0, C_BORDER, 0.22);

    double mix = u->tab_mix;
    if (mix < 0.999) {
        if (u->wifi_msg[0]) {
            unsigned mc = u->wifi_msg_ok ? C_GREEN : C_RED;
            dot(cr, CONTENT_X + 3.0, 571.0, 2.5, mc,
                (1.0 - mix) * 0.85);
            text_draw(cr, u->wifi_msg, 8.2, 500, mc,
                      (1.0 - mix) * 0.82, CONTENT_X + 12.0, 565.0,
                      ALIGN_LEFT, 0.2);
        } else {
            dot(cr, CONTENT_X + 3.0, 571.0, 2.5,
                u->wifi_on ? C_GREEN : C_FAINT,
                (1.0 - mix) * (u->wifi_on ? 0.85 : 0.50));
            text_draw(cr, u->wifi_on ? "Wi-Fi radio enabled" :
                      "Wi-Fi radio off",
                      8.2, 500, u->wifi_on ? C_DIM : C_FAINT,
                      (1.0 - mix) * 0.72, CONTENT_X + 12.0, 565.0,
                      ALIGN_LEFT, 0.35);
        }
    }
    if (mix > 0.001) {
        if (u->bt_msg[0]) {
            unsigned mc = u->bt_msg_ok ? C_GREEN : C_RED;
            dot(cr, CONTENT_X + 3.0, 571.0, 2.5, mc, mix * 0.85);
            text_draw(cr, u->bt_msg, 8.2, 500, mc,
                      mix * 0.82, CONTENT_X + 12.0, 565.0,
                      ALIGN_LEFT, 0.2);
        } else {
            dot(cr, CONTENT_X + 3.0, 571.0, 2.5,
                u->bt_on ? C_GREEN : C_FAINT,
                mix * (u->bt_on ? 0.85 : 0.50));
            text_draw(cr, u->bt_on ? "Bluetooth powered on" :
                      "Bluetooth powered off",
                      8.2, 500, u->bt_on ? C_DIM : C_FAINT,
                      mix * 0.72, CONTENT_X + 12.0, 565.0,
                      ALIGN_LEFT, 0.35);
        }
    }

    text_draw(cr, "WAYLAND · CAIRO + PANGO", 7.6, 500,
              C_FAINT, 0.55, WIN_W - 24.0, 566.0,
              ALIGN_RIGHT, 1.25);
}

static void connect_password_mask(const char *pass, char *out, size_t cap)
{
    size_t m = 0;
    if (cap == 0)
        return;
    for (const char *p = pass; *p && m + 4 < cap; p++) {
        if ((*p & 0xC0) != 0x80) {
            out[m++] = (char)0xE2;
            out[m++] = (char)0x80;
            out[m++] = (char)0xA2;
        }
    }
    out[m] = 0;
}

static void paint_connect_modal(cairo_t *cr, Ui *u)
{
    if (!u->connect_open)
        return;

    /* Dim the panel so the dialog reads as a modal layer. */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.62);
    cairo_paint(cr);

    double x = MODAL_X;
    double y = MODAL_Y;
    double w = MODAL_W;
    double h = MODAL_H;

    /* Soft drop shadow. */
    rounded_rect(cr, x + 4.0, y + 10.0, w, h, 15.0);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
    cairo_fill(cr);

    rounded_rect(cr, x, y, w, h, 15.0);
    rgba(cr, C_CARD_HI, 0.99);
    cairo_fill_preserve(cr);
    rounded_rect(cr, x, y, w, h, 15.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.72);
    cairo_stroke(cr);

    /* Accent hairline along the top edge. */
    line(cr, x + 22.0, y + 0.5, x + w - 22.0, y + 0.5,
         1.2, C_ACCENT, 0.55);

    text_draw(cr, "CONNECT TO WI-FI", 7.8, 600, C_ACCENT, 0.86,
              x + MODAL_PAD, y + 20.0, ALIGN_LEFT, 1.8);

    char title[96];
    snprintf(title, sizeof title, "%s", u->connect_ssid);
    cairo_save(cr);
    cairo_rectangle(cr, x + MODAL_PAD, y + 34.0,
                    w - MODAL_PAD * 2.0, 24.0);
    cairo_clip(cr);
    text_draw(cr, title, 15.0, 600, C_FG, 0.97,
              x + MODAL_PAD, y + 37.0, ALIGN_LEFT, 0.0);
    cairo_restore(cr);

    const char *sec = u->connect_security[0] ?
                      u->connect_security : "Open network";
    text_draw(cr, sec, 8.3, 400, C_DIM, 0.62,
              x + MODAL_PAD, y + 63.0, ALIGN_LEFT, 0.2);

    double fx = x + MODAL_PAD;
    double fy = MODAL_FIELD_Y;
    double fw = w - MODAL_PAD * 2.0;
    double fh = MODAL_FIELD_H;

    rounded_rect(cr, fx, fy, fw, fh, 9.0);
    rgba(cr, C_TRACK, 0.92);
    cairo_fill_preserve(cr);
    rounded_rect(cr, fx, fy, fw, fh, 9.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, u->hover_connect_field || u->connect_secure ?
         C_ACCENT : C_BORDER,
         u->hover_connect_field ? 0.62 : 0.46);
    cairo_stroke(cr);

    cairo_save(cr);
    cairo_rectangle(cr, fx + 2.0, fy + 2.0, fw - 4.0, fh - 4.0);
    cairo_clip(cr);
    if (!u->connect_secure) {
        text_draw(cr, "Open network — no password required",
                  9.6, 400, C_FAINT, 0.70,
                  fx + 14.0, fy + 13.0, ALIGN_LEFT, 0.15);
    } else if (u->connect_pass[0]) {
        char masked[256];
        connect_password_mask(u->connect_pass, masked, sizeof masked);
        text_draw(cr, masked, 10.5, 500, C_FG, 0.88,
                  fx + 14.0, fy + 12.0, ALIGN_LEFT, 0.35);
    } else {
        text_draw(cr, "Network password", 9.6, 400, C_FAINT, 0.58,
                  fx + 18.0, fy + 13.0, ALIGN_LEFT, 0.12);
    }
    cairo_restore(cr);

    if (u->connect_msg[0]) {
        unsigned col = u->connect_busy ? C_ACCENT : C_DIM;
        double alpha = u->connect_busy ? 0.90 : 0.68;
        text_draw(cr, u->connect_msg, 7.9, 500, col, alpha,
                  x + MODAL_PAD, y + 151.0, ALIGN_LEFT, 0.15);
    }

    double by = MODAL_BTN_Y;
    double bh = MODAL_BTN_H;
    double bw = MODAL_BTN_W;
    double bx2 = x + w - MODAL_PAD - bw;

    rounded_rect(cr, x + MODAL_PAD, by, bw, bh, 8.0);
    rgba(cr, u->hover_connect_cancel && !u->connect_busy ?
         C_CARD_HI : C_CARD, 0.95);
    cairo_fill_preserve(cr);
    rounded_rect(cr, x + MODAL_PAD, by, bw, bh, 8.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_BORDER, 0.52);
    cairo_stroke(cr);
    text_draw(cr, "CANCEL", 8.2, 600,
              u->connect_busy ? C_FAINT : C_DIM,
              u->connect_busy ? 0.40 : 0.86,
              x + MODAL_PAD + bw / 2.0, by + 13.0,
              ALIGN_CENTER, 1.3);

    rounded_rect(cr, bx2, by, bw, bh, 8.0);
    if (u->connect_busy)
        rgba(cr, C_ACCENT, 0.20);
    else
        rgba(cr, u->hover_connect_submit ? C_ACCENT_HI : C_ACCENT, 0.94);
    cairo_fill_preserve(cr);
    rounded_rect(cr, bx2, by, bw, bh, 8.0);
    cairo_set_line_width(cr, 1.0);
    rgba(cr, C_ACCENT, u->connect_busy ? 0.30 : 0.72);
    cairo_stroke(cr);
    text_draw(cr, u->connect_busy ? "CONNECTING…" : "CONNECT",
              8.2, 600,
              u->connect_busy ? C_ACCENT : C_ON_ACCENT,
              u->connect_busy ? 0.72 : 0.96,
              bx2 + bw / 2.0, by + 13.0,
              ALIGN_CENTER, 1.3);

    text_draw(cr, "ENTER connects · ESC cancels", 7.3, 500,
              C_FAINT, 0.48,
              x + w / 2.0, y + h - 19.0, ALIGN_CENTER, 0.35);
}

/* The caret is painted above the cached frame so blinking only redraws
 * this small overlay instead of rebuilding the whole window. */
static void paint_connect_caret(cairo_t *cr, Ui *u)
{
    if (!u->connect_open || !u->connect_secure || !u->connect_focus ||
        u->connect_busy)
        return;

    double phase = fmod(u->t - u->connect_caret_t, 1.06);
    if (phase < 0.0)
        phase += 1.06;
    if (phase >= 0.53)
        return;

    double fx = MODAL_X + MODAL_PAD;
    double fy = MODAL_FIELD_Y;
    double fw = MODAL_W - MODAL_PAD * 2.0;
    double fh = MODAL_FIELD_H;
    double text_x = fx + 14.0;

    double w = 0.0;
    if (u->connect_pass[0]) {
        char masked[256];
        connect_password_mask(u->connect_pass, masked, sizeof masked);
        w = text_width(cr, masked, 10.5, 500, 0.35);
    }

    double cx = text_x + (w > 0.0 ? w + 2.0 : 0.0);
    double max_x = fx + fw - 10.0;
    if (cx > max_x)
        cx = max_x;
    if (cx < text_x)
        cx = text_x;

    cairo_save(cr);
    cairo_rectangle(cr, fx + 2.0, fy + 2.0, fw - 4.0, fh - 4.0);
    cairo_clip(cr);
    line(cr, cx, fy + 13.0, cx, fy + 31.0, 1.4, C_ACCENT_HI, 0.95);
    cairo_restore(cr);
}

static void paint_wifi_tab(cairo_t *cr, Ui *u)
{
    paint_hero_wifi(cr, u);
    section_header(cr, u, "NETWORKS", "SCAN", 0);

    if (!u->wifi_on)
        paint_empty_state(cr, u, 1, 1);
    else if (u->net_count == 0)
        paint_empty_state(cr, u, 1, 0);
    else
        paint_wifi_list(cr, u);
}

static void paint_bt_tab(cairo_t *cr, Ui *u)
{
    paint_hero_bt(cr, u);
    section_header(cr, u, "PAIRED DEVICES", "PAIR NEW", 1);

    if (!u->bt_on)
        paint_empty_state(cr, u, 0, 1);
    else if (u->bt_count == 0)
        paint_empty_state(cr, u, 0, 0);
    else {
        paint_bt_list(cr, u);
        paint_bt_adapter_card(cr, u);
    }
}

static void paint_content(cairo_t *cr, Ui *u)
{
    paint_window_base(cr, u);
    paint_sidebar(cr, u);
    paint_header(cr, u);

    double mix = clampd(u->tab_mix, 0.0, 1.0);
    cairo_save(cr);
    cairo_rectangle(cr, CONTENT_X - 10.0, HERO_Y - 10.0,
                    CONTENT_W + 20.0, WIN_H - HERO_Y - 36.0);
    cairo_clip(cr);

    if (mix < 0.999) {
        cairo_push_group(cr);
        cairo_translate(cr, -10.0 * mix, 0.0);
        paint_wifi_tab(cr, u);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, 1.0 - mix);
    }
    if (mix > 0.001) {
        cairo_push_group(cr);
        cairo_translate(cr, 10.0 * (1.0 - mix), 0.0);
        paint_bt_tab(cr, u);
        cairo_pop_group_to_source(cr);
        cairo_paint_with_alpha(cr, mix);
    }
    cairo_restore(cr);

    paint_footer(cr, u);
    paint_connect_modal(cr, u);
    cairo_restore(cr); /* window base clip */
}

/* Animated decoration that lives on top of the cached base frame. */
static void paint_ambient(cairo_t *cr, Ui *u)
{
    paint_dust(cr, u);
    paint_brand_bars(cr, u);
    paint_ornament_dots(cr, u);

    int is_wifi = (u->tab == TAB_WIFI);
    int on = is_wifi ? u->wifi_on : u->bt_on;

    cairo_save(cr);
    rounded_rect(cr, CONTENT_X, HERO_Y, CONTENT_W, HERO_H, 14.0);
    cairo_clip(cr);
    if (on) {
        double icx = CONTENT_X + 74.0;
        double icy = HERO_Y + 78.0;
        for (int i = 0; i < 2; i++) {
            double ph = fmod(u->deco_t * 0.30 + i * 0.5, 1.0);
            ring(cr, icx, icy, 48.0 + ph * 24.0, 1.1,
                 C_ACCENT, 0.085 * (1.0 - ph));
        }
    }
    hero_icon_plate(cr, u, on, is_wifi);
    cairo_restore(cr);

    section_action_ambient(cr, u, is_wifi ? 0 : 1);
}

/* ------------------------------------------------------------------ */
/*  System state                                                       */
/* ------------------------------------------------------------------ */

static void run_async(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        pid_t child = fork();
        if (child == 0) {
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0)
        waitpid(pid, NULL, 0);
}

static int read_cmd(const char *cmd, char *out, size_t cap)
{
    if (cap == 0)
        return 0;
    out[0] = 0;
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    int rc = pclose(f);
    char *s = trim(out);
    if (s != out)
        memmove(out, s, strlen(s) + 1);
    return rc == 0;
}

static int net_cmp(const void *a, const void *b)
{
    const WifiNet *x = a;
    const WifiNet *y = b;
    if (x->connected != y->connected)
        return y->connected - x->connected;
    if (x->saved != y->saved)
        return y->saved - x->saved;
    if (x->signal != y->signal)
        return y->signal - x->signal;
    return strcasecmp(x->ssid, y->ssid);
}

static NMDevice *wifi_device(NMClient *nm)
{
    if (!nm)
        return NULL;
    const GPtrArray *devices = nm_client_get_devices(nm);
    if (!devices)
        return NULL;
    for (guint i = 0; i < devices->len; i++) {
        NMDevice *dev = g_ptr_array_index(devices, i);
        if (NM_IS_DEVICE_WIFI(dev))
            return dev;
    }
    return NULL;
}

static NMActiveConnection *wifi_active_connection(NMClient *nm)
{
    if (!nm)
        return NULL;
    const GPtrArray *acs = nm_client_get_active_connections(nm);
    if (!acs)
        return NULL;
    for (guint i = 0; i < acs->len; i++) {
        NMActiveConnection *ac = g_ptr_array_index(acs, i);
        if (!g_strcmp0(nm_active_connection_get_connection_type(ac),
                       NM_SETTING_WIRELESS_SETTING_NAME))
            return ac;
    }
    return NULL;
}

static char *ssid_to_utf8(GBytes *bytes)
{
    if (!bytes)
        return NULL;
    gsize len = 0;
    const guint8 *data = g_bytes_get_data(bytes, &len);
    if (!data || len == 0)
        return NULL;
    char *ssid = nm_utils_ssid_to_utf8(data, len);
    if (ssid && !ssid[0]) {
        g_free(ssid);
        return NULL;
    }
    return ssid;
}

static void ap_security_string(NMAccessPoint *ap, char *out, size_t cap)
{
    out[0] = 0;
    if (!ap)
        return;

    NM80211ApFlags flags = nm_access_point_get_flags(ap);
    NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
    NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
    NM80211ApSecurityFlags key = wpa | rsn;

    int wpa1 = (wpa & (NM_802_11_AP_SEC_KEY_MGMT_PSK |
                       NM_802_11_AP_SEC_KEY_MGMT_802_1X)) != 0;
    int wpa2 = (rsn & (NM_802_11_AP_SEC_KEY_MGMT_PSK |
                       NM_802_11_AP_SEC_KEY_MGMT_802_1X |
                       NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)) != 0;
    int sae = (key & NM_802_11_AP_SEC_KEY_MGMT_SAE) != 0;
    int owe = (key & (NM_802_11_AP_SEC_KEY_MGMT_OWE |
                      NM_802_11_AP_SEC_KEY_MGMT_OWE_TM)) != 0;
    int eap = (key & (NM_802_11_AP_SEC_KEY_MGMT_802_1X |
                      NM_802_11_AP_SEC_KEY_MGMT_EAP_SUITE_B_192)) != 0;

    char text[40] = "";
    if (sae && wpa2)
        snprintf(text, sizeof text, "WPA2 WPA3");
    else if (sae)
        snprintf(text, sizeof text, "WPA3");
    else if (wpa1 && wpa2)
        snprintf(text, sizeof text, "WPA1 WPA2");
    else if (wpa2)
        snprintf(text, sizeof text, "WPA2");
    else if (wpa1)
        snprintf(text, sizeof text, "WPA1");
    else if (owe)
        snprintf(text, sizeof text, "OWE");
    else if (flags & NM_802_11_AP_FLAGS_PRIVACY)
        snprintf(text, sizeof text, "WEP");

    if (eap) {
        size_t len = strlen(text);
        if (len < sizeof text - 8)
            snprintf(text + len, sizeof text - len, " 802.1X");
    }
    snprintf(out, cap, "%s", text);
}

static int ap_is_sae_only(NMAccessPoint *ap)
{
    if (!ap)
        return 0;
    NM80211ApSecurityFlags key = nm_access_point_get_wpa_flags(ap) |
                                 nm_access_point_get_rsn_flags(ap);
    return (key & NM_802_11_AP_SEC_KEY_MGMT_SAE) &&
           !(key & NM_802_11_AP_SEC_KEY_MGMT_PSK);
}

static NMAccessPoint *wifi_ap_by_ssid(NMDevice *dev, const char *ssid)
{
    if (!dev || !NM_IS_DEVICE_WIFI(dev) || !ssid)
        return NULL;
    const GPtrArray *aps = nm_device_wifi_get_access_points(
        NM_DEVICE_WIFI(dev));
    if (!aps)
        return NULL;
    NMAccessPoint *best = NULL;
    int best_signal = -1;
    for (guint i = 0; i < aps->len; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        char *cur = ssid_to_utf8(nm_access_point_get_ssid(ap));
        int match = cur && !strcmp(cur, ssid);
        g_free(cur);
        if (!match)
            continue;
        int strength = nm_access_point_get_strength(ap);
        if (strength > best_signal) {
            best = ap;
            best_signal = strength;
        }
    }
    return best;
}

static void read_saved_wifi(Ui *u, NMClient *nm)
{
    u->saved_count = 0;
    u->wifi_profile[0] = 0;
    u->wifi_uuid[0] = 0;

    if (!nm)
        return;

    NMActiveConnection *wac = wifi_active_connection(nm);
    if (wac) {
        const char *id = nm_active_connection_get_id(wac);
        const char *uuid = nm_active_connection_get_uuid(wac);
        if (id)
            snprintf(u->wifi_profile, sizeof u->wifi_profile,
                     "%.95s", id);
        if (uuid)
            snprintf(u->wifi_uuid, sizeof u->wifi_uuid, "%.63s", uuid);
    }

    const GPtrArray *conns = nm_client_get_connections(nm);
    if (!conns)
        return;

    for (guint i = 0; i < conns->len && u->saved_count < MAX_SAVED; i++) {
        NMConnection *conn = g_ptr_array_index(conns, i);
        NMSettingWireless *sw = nm_connection_get_setting_wireless(conn);
        if (!sw || g_strcmp0(nm_connection_get_connection_type(conn),
                             NM_SETTING_WIRELESS_SETTING_NAME))
            continue;

        char *ssid = ssid_to_utf8(nm_setting_wireless_get_ssid(sw));
        if (!ssid)
            continue;

        const char *id = nm_connection_get_id(conn);
        const char *uuid = nm_connection_get_uuid(conn);
        if (!id)
            id = ssid;
        if (!uuid)
            uuid = "";

        SavedWifi *s = &u->saved[u->saved_count++];
        snprintf(s->name, sizeof s->name, "%.95s", id);
        snprintf(s->ssid, sizeof s->ssid, "%.95s", ssid);
        snprintf(s->uuid, sizeof s->uuid, "%.63s", uuid);
        s->active = u->wifi_uuid[0] && !strcmp(uuid, u->wifi_uuid);
        g_free(ssid);
    }
}

static int saved_wifi_index(const Ui *u, const char *ssid)
{
    int found = -1;
    for (int i = 0; i < u->saved_count; i++) {
        if (strcmp(u->saved[i].ssid, ssid))
            continue;
        if (u->saved[i].active)
            return i;
        if (found < 0)
            found = i;
    }
    return found;
}

static void apply_saved_to_net(Ui *u, WifiNet *n)
{
    int si = saved_wifi_index(u, n->ssid);
    if (si < 0)
        return;
    n->saved = 1;
    if (u->saved[si].active)
        n->connected = 1;
    snprintf(n->profile, sizeof n->profile, "%s", u->saved[si].name);
    snprintf(n->uuid, sizeof n->uuid, "%s", u->saved[si].uuid);
}

static void apply_wifi_list(Ui *u, NMClient *nm)
{
    WifiNet tmp[MAX_CANDIDATES];
    int tc = 0;
    NMDevice *dev = wifi_device(nm);
    NMAccessPoint *active = NULL;
    const GPtrArray *aps = NULL;

    if (dev) {
        NMDeviceWifi *wdev = NM_DEVICE_WIFI(dev);
        active = nm_device_wifi_get_active_access_point(wdev);
        aps = nm_device_wifi_get_access_points(wdev);
    }

    for (guint i = 0; aps && i < aps->len && tc < MAX_CANDIDATES; i++) {
        NMAccessPoint *ap = g_ptr_array_index(aps, i);
        char *ssid = ssid_to_utf8(nm_access_point_get_ssid(ap));
        if (!ssid)
            continue;

        int connected = ap == active;
        int signal = nm_access_point_get_strength(ap);
        const char *path = nm_object_get_path(NM_OBJECT(ap));

        int found = -1;
        for (int j = 0; j < tc; j++) {
            if (!strcmp(tmp[j].ssid, ssid)) {
                found = j;
                break;
            }
        }
        if (found >= 0) {
            WifiNet *n = &tmp[found];
            if (connected) {
                /* Prefer the BSSID we are actually associated with. */
                n->connected = 1;
                n->signal = signal;
                ap_security_string(ap, n->security, sizeof n->security);
                if (path)
                    snprintf(n->ap_path, sizeof n->ap_path, "%s", path);
            } else if (!n->connected && signal > n->signal) {
                n->signal = signal;
                ap_security_string(ap, n->security, sizeof n->security);
                if (path)
                    snprintf(n->ap_path, sizeof n->ap_path, "%s", path);
            }
            g_free(ssid);
            continue;
        }

        WifiNet *n = &tmp[tc++];
        memset(n, 0, sizeof *n);
        snprintf(n->ssid, sizeof n->ssid, "%.95s", ssid);
        n->signal = signal;
        n->connected = connected;
        ap_security_string(ap, n->security, sizeof n->security);
        if (path)
            snprintf(n->ap_path, sizeof n->ap_path, "%s", path);
        g_free(ssid);
    }

    /* Keep remembered profiles visible even when they are currently out
     * of range, so they can still be activated or forgotten. */
    for (int i = 0; i < u->saved_count && tc < MAX_CANDIDATES; i++) {
        int present = 0;
        for (int j = 0; j < tc; j++) {
            if (!strcmp(tmp[j].ssid, u->saved[i].ssid)) {
                present = 1;
                break;
            }
        }
        if (present)
            continue;

        WifiNet *n = &tmp[tc++];
        memset(n, 0, sizeof *n);
        snprintf(n->ssid, sizeof n->ssid, "%s", u->saved[i].ssid);
        snprintf(n->profile, sizeof n->profile, "%s", u->saved[i].name);
        snprintf(n->uuid, sizeof n->uuid, "%s", u->saved[i].uuid);
        n->signal = -1;
        n->saved = 1;
        n->connected = u->saved[i].active;
    }

    for (int i = 0; i < tc; i++)
        apply_saved_to_net(u, &tmp[i]);

    qsort(tmp, tc, sizeof(WifiNet), net_cmp);
    int keep = tc < MAX_NETS ? tc : MAX_NETS;
    memcpy(u->nets, tmp, sizeof(WifiNet) * (size_t)keep);
    u->net_count = keep;
    u->wifi_connected = 0;
    u->signal = 0;
    u->ssid[0] = 0;
    u->security[0] = 0;
    for (int i = 0; i < keep; i++) {
        if (tmp[i].connected) {
            u->wifi_connected = 1;
            u->signal = tmp[i].signal > 0 ? tmp[i].signal : 0;
            snprintf(u->ssid, sizeof u->ssid, "%s", tmp[i].ssid);
            snprintf(u->security, sizeof u->security, "%s",
                     tmp[i].security);
            break;
        }
    }
}

static void read_wifi(Ui *u, NMClient *nm)
{
    u->wifi_on = nm ? nm_client_wireless_get_enabled(nm) : 0;
    read_saved_wifi(u, nm);
    apply_wifi_list(u, nm);
}

static void read_ip(Ui *u, NMClient *nm)
{
    u->ip[0] = 0;
    NMDevice *dev = wifi_device(nm);
    if (!dev)
        return;
    NMIPConfig *cfg = nm_device_get_ip4_config(dev);
    if (!cfg)
        return;
    GPtrArray *addrs = nm_ip_config_get_addresses(cfg);
    if (!addrs || addrs->len == 0)
        return;
    NMIPAddress *addr = g_ptr_array_index(addrs, 0);
    const char *text = nm_ip_address_get_address(addr);
    if (text)
        snprintf(u->ip, sizeof u->ip, "%.63s", text);
}

static DevKind guess_kind(const char *name)
{
    if (strcasestr(name, "controller") || strcasestr(name, "gamepad"))
        return DEV_GAMEPAD;
    if (strcasestr(name, "keyboard"))
        return DEV_KEYBOARD;
    if (strcasestr(name, "mouse"))
        return DEV_MOUSE;
    if (strcasestr(name, "buds") || strcasestr(name, "pods") ||
        strcasestr(name, "earbud"))
        return DEV_EARBUDS;
    if (strcasestr(name, "head") || strcasestr(name, "wh-") ||
        strcasestr(name, "xm"))
        return DEV_HEADPHONES;
    return DEV_GENERIC;
}

static int parse_device_line(char *line, char *mac, size_t macsz,
                             char *name, size_t namesz)
{
    char *dev = strstr(line, "Device ");
    if (!dev)
        return 0;
    dev += 7;
    char *sp = strchr(dev, ' ');
    if (!sp)
        return 0;

    size_t n = (size_t)(sp - dev);
    if (n >= macsz)
        n = macsz - 1;
    if (macsz > 0) {
        memcpy(mac, dev, n);
        mac[n] = 0;
    }

    if (name && namesz > 0) {
        char *nm = trim(sp + 1);
        snprintf(name, namesz, "%s", nm);
    }
    return 1;
}

static int mac_connected(char cmacs[][24], int n, const char *mac)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(cmacs[i], mac))
            return 1;
    return 0;
}

static int bt_mac_list(const char *filter, char macs[][24], int cap)
{
    char cmd[128];
    snprintf(cmd, sizeof cmd, "bluetoothctl devices %s 2>/dev/null", filter);
    int count = 0;
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    char line[256];
    while (fgets(line, sizeof line, f) && count < cap) {
        char mac[24];
        if (parse_device_line(line, mac, sizeof mac, NULL, 0))
            snprintf(macs[count++], 24, "%s", mac);
    }
    pclose(f);
    return count;
}

static void read_bt(Ui *u)
{
    char out[8192];
    if (read_cmd("bluetoothctl show 2>/dev/null", out, sizeof out)) {
        if (strstr(out, "Powered: yes"))
            u->bt_on = 1;
        else if (strstr(out, "Powered: no"))
            u->bt_on = 0;

        const char *p = strstr(out, "Alias:");
        if (p) {
            p += 6;
            while (*p == ' ' || *p == '\t')
                p++;
            const char *e = p;
            while (*e && *e != '\n' && *e != '\r')
                e++;
            size_t n = (size_t)(e - p);
            if (n >= sizeof u->bt_name)
                n = sizeof u->bt_name - 1;
            memcpy(u->bt_name, p, n);
            u->bt_name[n] = 0;
        }
        u->bt_discoverable = strstr(out, "Discoverable: yes") != NULL;
    }

    char cmacs[32][24];
    int cmc = bt_mac_list("Connected", cmacs, 32);
    char tmacs[32][24];
    int tmc = bt_mac_list("Trusted", tmacs, 32);

    BtDev tmp[MAX_BT];
    int tc = 0;
    int total = 0;
    FILE *f = popen("bluetoothctl devices Paired 2>/dev/null", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char mac[24];
            char name[96];
            if (!parse_device_line(line, mac, sizeof mac,
                                   name, sizeof name))
                continue;
            total++;
            if (tc < MAX_BT) {
                BtDev *d = &tmp[tc++];
                snprintf(d->name, sizeof d->name, "%s", name);
                snprintf(d->mac, sizeof d->mac, "%s", mac);
                d->kind = guess_kind(name);
                d->connected = mac_connected(cmacs, cmc, mac);
                d->trusted = mac_connected(tmacs, tmc, mac);
            }
        }
        pclose(f);

        /* A device that connects without going through this panel should
         * still become trusted. Screenshot mode never mutates state. */
        if (u->allow_mutation && u->bt_on) {
            for (int i = 0; i < tc; i++) {
                if (!tmp[i].connected || tmp[i].trusted)
                    continue;
                char cmd[160];
                snprintf(cmd, sizeof cmd,
                         "bluetoothctl trust %s >/dev/null 2>&1",
                         tmp[i].mac);
                run_async(cmd);
                tmp[i].trusted = 1;
            }
        }

        memcpy(u->bt, tmp, sizeof(BtDev) * (size_t)tc);
        u->bt_count = tc;
        u->bt_paired_total = total;
    }
}

static void sys_refresh(Ui *u, NMClient *nm)
{
    read_wifi(u, nm);
    read_ip(u, nm);
    read_bt(u);
}

static void set_bt_radio(Ui *u, int on)
{
    u->bt_on = on;
    run_async(on ? "bluetoothctl power on >/dev/null 2>&1 || "
                   "rfkill unblock bluetooth >/dev/null 2>&1"
                 : "bluetoothctl power off >/dev/null 2>&1 || "
                   "rfkill block bluetooth >/dev/null 2>&1");
}

/* ------------------------------------------------------------------ */
/*  Wayland backend                                                    */
/* ------------------------------------------------------------------ */

#define NBUFS 4
#define MAX_OUTPUTS 8
#define BTN_LEFT 0x110

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width;
    int height;
    int busy;
    int retired;
} ShmBuf;

typedef struct App App;

typedef struct {
    struct wl_output *output;
    int32_t scale;
    int entered;
    App *app;
} Output;

struct App {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct zxdg_decoration_manager_v1 *deco_manager;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    uint32_t pointer_serial;
    struct wl_keyboard *keyboard;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct xdg_toplevel_icon_manager_v1 *icon_manager;
    struct wp_cursor_shape_manager_v1 *cursor_shape_manager;
    struct wp_cursor_shape_device_v1 *cursor_shape_device;
    uint32_t cursor_shape;

    int configured;
    int scale;
    int fixed_scale;
    int has_argb;
    ShmBuf bufs[NBUFS];

    struct xkb_context *xkb_ctx;
    struct xkb_keymap *xkb_keymap;
    struct xkb_state *xkb_state;
    int keyboard_focused;

    Output outputs[MAX_OUTPUTS];
    int output_count;

    Ui ui;

    NMClient *nm;
    GCancellable *net_cancel;
    int net_pending;
    int scan_pending;

    double t0;
    double last_draw;
    double pointer_x;
    double pointer_y;
    double resync_at;
    int dirty;

    /* Cached static frame.  Ambient animation only redraws the dynamic
     * decoration on top of this surface, keeping idle CPU low. */
    cairo_surface_t *base;
    int base_valid;
    int base_w;
    int base_h;

    int action_fd;
    pid_t action_pid;
    char action_buf[2048];
    size_t action_len;

    int ctl_fd;
    int ctl_client_fd;
    char ctl_buf[256];
    size_t ctl_len;

    int quit;
};

static int g_signal_quit;

static void mark_dirty(App *a)
{
    a->dirty = 1;
    a->base_valid = 0;
}

static int update_hover(App *a);
static void update_cursor(App *a);

static void nm_ui_refresh(App *a)
{
    read_wifi(&a->ui, a->nm);
    read_ip(&a->ui, a->nm);
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void nm_changed_cb(NMClient *client, gpointer object,
                          gpointer user_data)
{
    (void)client;
    (void)object;
    nm_ui_refresh(user_data);
}

static void nm_prop_cb(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    (void)object;
    (void)pspec;
    nm_ui_refresh(user_data);
}

static int create_anon_file(size_t size)
{
    int fd = memfd_create("jtnetworks", MFD_CLOEXEC);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0)
            return fd;
        close(fd);
    }

    const char *dir = getenv("XDG_RUNTIME_DIR");
    char path[512];
    snprintf(path, sizeof path, "%s/jtnetworks-XXXXXX",
             (dir && *dir) ? dir : "/tmp");
    fd = mkstemp(path);
    if (fd < 0)
        return -1;
    unlink(path);
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void shm_buffer_destroy(ShmBuf *b)
{
    if (b->buffer) {
        wl_buffer_destroy(b->buffer);
        b->buffer = NULL;
    }
    if (b->data) {
        munmap(b->data, b->size);
        b->data = NULL;
    }
    b->size = 0;
    b->width = b->height = 0;
    b->busy = b->retired = 0;
}

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    ShmBuf *b = data;
    b->busy = 0;
    if (b->retired)
        shm_buffer_destroy(b);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static int shm_buffer_create(App *a, ShmBuf *b, int w, int h)
{
    int stride = w * 4;
    size_t size = (size_t)stride * (size_t)h;
    int fd = create_anon_file(size);
    if (fd < 0)
        return -1;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, (int32_t)size);
    if (!pool) {
        munmap(data, size);
        close(fd);
        return -1;
    }

    b->buffer = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!b->buffer) {
        munmap(data, size);
        return -1;
    }

    b->data = data;
    b->size = size;
    b->width = w;
    b->height = h;
    b->busy = 0;
    b->retired = 0;
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    return 0;
}

static ShmBuf *next_buffer(App *a, int w, int h)
{
    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired &&
            b->width == w && b->height == h)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer && shm_buffer_create(a, b, w, h) == 0)
            return b;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->buffer && !b->busy && !b->retired) {
            shm_buffer_destroy(b);
            if (shm_buffer_create(a, b, w, h) == 0)
                return b;
        }
    }

    return NULL;
}

static int build_base_cache(App *a, int w, int h)
{
    if (a->base && (a->base_w != w || a->base_h != h)) {
        cairo_surface_destroy(a->base);
        a->base = NULL;
        a->base_w = a->base_h = 0;
    }
    if (!a->base) {
        a->base = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
        if (cairo_surface_status(a->base) != CAIRO_STATUS_SUCCESS) {
            cairo_surface_destroy(a->base);
            a->base = NULL;
            return 0;
        }
        a->base_w = w;
        a->base_h = h;
    }

    cairo_t *cr = cairo_create(a->base);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_scale(cr, a->scale, a->scale);

    a->ui.ambient_skip = 1;
    paint_content(cr, &a->ui);
    a->ui.ambient_skip = 0;

    cairo_destroy(cr);
    cairo_surface_flush(a->base);
    a->base_valid = 1;
    return 1;
}

static int app_draw(App *a, int use_cache)
{
    if (!a->configured || !a->surface || !a->shm)
        return 0;

    int w = (int)lround(WIN_W * a->scale);
    int h = (int)lround(WIN_H * a->scale);
    if (w < 1 || h < 1)
        return 0;

    ShmBuf *b = next_buffer(a, w, h);
    if (!b)
        return 0;

    cairo_surface_t *surf = cairo_image_surface_create_for_data(
        b->data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(surf);
        return 0;
    }

    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    if (use_cache) {
        if (!a->base_valid || a->base_w != w || a->base_h != h) {
            if (!build_base_cache(a, w, h)) {
                cairo_destroy(cr);
                cairo_surface_destroy(surf);
                return 0;
            }
        }

        /* The cache is already at device resolution. */
        cairo_set_source_surface(cr, a->base, 0, 0);
        cairo_paint(cr);
        cairo_scale(cr, a->scale, a->scale);

        cairo_save(cr);
        rounded_rect(cr, 1.0, 1.0, WIN_W - 2.0, WIN_H - 2.0, 15.0);
        cairo_clip(cr);
        if (!a->ui.connect_open)
            paint_ambient(cr, &a->ui);
        cairo_restore(cr);
    } else {
        a->base_valid = 0;
        cairo_scale(cr, a->scale, a->scale);
        a->ui.ambient_skip = 0;
        paint_content(cr, &a->ui);
    }

    if (a->keyboard_focused) {
        cairo_save(cr);
        rounded_rect(cr, 1.0, 1.0, WIN_W - 2.0, WIN_H - 2.0, 15.0);
        cairo_clip(cr);
        paint_connect_caret(cr, &a->ui);
        cairo_restore(cr);
    }

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_attach(a->surface, b->buffer, 0, 0);
    wl_surface_damage(a->surface, 0, 0, (int)WIN_W, (int)WIN_H);
    wl_surface_commit(a->surface);
    b->busy = 1;
    return 1;
}

static void recalc_scale(App *a)
{
    if (a->fixed_scale)
        return;

    int s = 0;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].entered && a->outputs[i].scale > s)
            s = a->outputs[i].scale;
    }
    if (s < 1)
        s = 1;
    if (s > 3)
        s = 3;
    if (s == a->scale)
        return;

    a->scale = s;
    mark_dirty(a);
    if (a->surface && wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, s);

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (!b->buffer)
            continue;
        if (b->busy)
            b->retired = 1;
        else
            shm_buffer_destroy(b);
    }
}

/* ------------------------------------------------------------------ */
/*  Background Wi-Fi scan                                              */
/* ------------------------------------------------------------------ */

static void wifi_scan_cb(GObject *source, GAsyncResult *result,
                         gpointer user_data)
{
    App *a = user_data;
    GError *err = NULL;

    nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(source), result, &err);
    if (err)
        g_error_free(err);

    a->scan_pending = 0;
    if (a->net_pending > 0)
        a->net_pending--;
    read_wifi(&a->ui, a->nm);
    read_ip(&a->ui, a->nm);
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void request_wifi_scan(App *a)
{
    if (!a->nm || !a->ui.wifi_on || a->scan_pending)
        return;
    NMDevice *dev = wifi_device(a->nm);
    if (!dev)
        return;

    a->scan_pending = 1;
    a->net_pending++;
    nm_device_wifi_request_scan_async(
        NM_DEVICE_WIFI(dev), a->net_cancel, wifi_scan_cb, a);
}

static void wifi_radio_cb(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    App *a = user_data;
    GError *err = NULL;

    if (!nm_client_dbus_set_property_finish(
            NM_CLIENT(source), result, &err)) {
        if (err && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            a->ui.wifi_msg_ok = 0;
            snprintf(a->ui.wifi_msg, sizeof a->ui.wifi_msg,
                     "Failed: %.145s", err->message);
            a->ui.wifi_msg_t = a->ui.t;
        }
        g_clear_error(&err);
    }

    if (a->net_pending > 0)
        a->net_pending--;
    if (a->ui.wifi_on)
        request_wifi_scan(a);
    a->resync_at = mono_sec() + 0.5;
    mark_dirty(a);
}

static void set_wifi_radio(App *a, int on)
{
    a->ui.wifi_on = on;
    if (!a->nm) {
        a->ui.wifi_on = 0;
        a->ui.wifi_msg_ok = 0;
        snprintf(a->ui.wifi_msg, sizeof a->ui.wifi_msg,
                 "Failed: NetworkManager is not available.");
        a->ui.wifi_msg_t = a->ui.t;
        a->resync_at = mono_sec() + 0.5;
    } else {
        a->net_pending++;
        GVariant *value = g_variant_ref_sink(g_variant_new_boolean(on));
        nm_client_dbus_set_property(
            a->nm, NM_DBUS_PATH, NM_DBUS_INTERFACE, "WirelessEnabled",
            value, -1, a->net_cancel, wifi_radio_cb, a);
        g_variant_unref(value);
    }
    mark_dirty(a);
}

/* ------------------------------------------------------------------ */
/*  Wi-Fi connection                                                   */
/* ------------------------------------------------------------------ */

static int wifi_is_secure(const WifiNet *n)
{
    if (!n->security[0])
        return 0;
    if (!strcasecmp(n->security, "open") ||
        !strcasecmp(n->security, "none") ||
        !strcasecmp(n->security, "owe") ||
        !strcmp(n->security, "--"))
        return 0;
    return 1;
}

typedef struct {
    App *a;
    NMConnection *conn;
    NMActiveConnection *ac;
    guint state_id;
    guint timeout_id;
    char uuid[64];
} WifiConnectOp;

static void wifi_connect_delete_cb(GObject *source, GAsyncResult *result,
                                   gpointer user_data)
{
    (void)user_data;
    GError *err = NULL;
    nm_remote_connection_delete_finish(
        NM_REMOTE_CONNECTION(source), result, &err);
    g_clear_error(&err);
}

static void wifi_connect_drop_profile(WifiConnectOp *op)
{
    if (!op->uuid[0] || !op->a->nm)
        return;
    NMRemoteConnection *rc =
        nm_client_get_connection_by_uuid(op->a->nm, op->uuid);
    if (rc)
        nm_remote_connection_delete_async(
            rc, op->a->net_cancel, wifi_connect_delete_cb, NULL);
}

static void wifi_connect_op_free(WifiConnectOp *op)
{
    if (op->ac && op->state_id)
        g_signal_handler_disconnect(op->ac, op->state_id);
    if (op->timeout_id)
        g_source_remove(op->timeout_id);

    App *a = op->a;
    g_clear_object(&op->ac);
    g_clear_object(&op->conn);
    if (a->net_pending > 0)
        a->net_pending--;
    g_free(op);
}

static void wifi_connect_finish(WifiConnectOp *op, const char *reason)
{
    App *a = op->a;
    Ui *u = &a->ui;

    u->connect_busy = 0;
    if (!reason) {
        u->connect_open = 0;
        u->connect_idx = -1;
        u->connect_secure = 0;
        u->connect_focus = 0;
        u->connect_msg[0] = 0;
        memset(u->connect_pass, 0, sizeof u->connect_pass);
        a->resync_at = mono_sec() + 0.35;
    } else {
        u->connect_caret_t = u->t;
        snprintf(u->connect_msg, sizeof u->connect_msg,
                 "Failed: %.145s", reason);
        wifi_connect_drop_profile(op);
    }

    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
    wifi_connect_op_free(op);
}

static gboolean wifi_connect_timeout_cb(gpointer user_data)
{
    WifiConnectOp *op = user_data;
    op->timeout_id = 0;
    wifi_connect_finish(op, "Connection timed out.");
    return G_SOURCE_REMOVE;
}

static void wifi_connect_state_cb(NMActiveConnection *ac, GParamSpec *pspec,
                                  gpointer user_data)
{
    (void)pspec;
    WifiConnectOp *op = user_data;
    NMActiveConnectionState state = nm_active_connection_get_state(ac);

    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
        wifi_connect_finish(op, NULL);
    } else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
        wifi_connect_finish(op, "Could not connect to the network.");
    }
}

static void wifi_connect_cb(GObject *source, GAsyncResult *result,
                            gpointer user_data)
{
    WifiConnectOp *op = user_data;
    App *a = op->a;
    GError *err = NULL;

    NMActiveConnection *ac =
        nm_client_add_and_activate_connection_finish(
            NM_CLIENT(source), result, &err);

    if (!ac) {
        const char *reason = err && err->message ? err->message :
                             "Could not connect to the network.";
        wifi_connect_finish(op, reason);
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);

    const char *ac_uuid = nm_active_connection_get_uuid(ac);
    if (ac_uuid && ac_uuid[0])
        snprintf(op->uuid, sizeof op->uuid, "%s", ac_uuid);

    op->ac = g_object_ref(ac);
    op->state_id = g_signal_connect(ac, "notify::state",
                                    G_CALLBACK(wifi_connect_state_cb), op);
    op->timeout_id = g_timeout_add_seconds(30, wifi_connect_timeout_cb, op);

    a->ui.connect_msg[0] = 0;
    snprintf(a->ui.connect_msg, sizeof a->ui.connect_msg,
             "Connecting to network…");
    mark_dirty(a);

    NMActiveConnectionState state = nm_active_connection_get_state(ac);
    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED)
        wifi_connect_finish(op, NULL);
    else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED)
        wifi_connect_finish(op, "Could not connect to the network.");
}

static void start_wifi_connect(App *a)
{
    Ui *u = &a->ui;
    if (!u->connect_open || u->connect_busy)
        return;
    if (!a->nm || !a->ui.wifi_on) {
        snprintf(u->connect_msg, sizeof u->connect_msg,
                 "Failed: NetworkManager is not available.");
        mark_dirty(a);
        return;
    }

    NMDevice *dev = wifi_device(a->nm);
    if (!dev) {
        snprintf(u->connect_msg, sizeof u->connect_msg,
                 "Failed: no Wi-Fi device is available.");
        mark_dirty(a);
        return;
    }

    if (u->connect_secure && !u->connect_pass[0]) {
        snprintf(u->connect_msg, sizeof u->connect_msg,
                 "Enter the network password.");
        mark_dirty(a);
        return;
    }

    int owe = strstr(u->connect_security, "OWE") != NULL;

    char ssid[96];
    char pass[128];
    snprintf(ssid, sizeof ssid, "%s", u->connect_ssid);
    snprintf(pass, sizeof pass, "%s", u->connect_pass);

    NMAccessPoint *ap = wifi_ap_by_ssid(dev, ssid);
    char ap_path[128] = "";
    if (ap) {
        const char *path = nm_object_get_path(NM_OBJECT(ap));
        if (path)
            snprintf(ap_path, sizeof ap_path, "%s", path);
    }

    NMConnection *conn = nm_simple_connection_new();
    NMSettingConnection *sc =
        NM_SETTING_CONNECTION(nm_setting_connection_new());
    char *uuid = g_uuid_string_random();
    g_object_set(sc,
                 NM_SETTING_CONNECTION_ID, ssid,
                 NM_SETTING_CONNECTION_UUID, uuid,
                 NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
                 NM_SETTING_CONNECTION_AUTOCONNECT, TRUE,
                 NULL);
    nm_connection_add_setting(conn, NM_SETTING(sc));

    NMSettingWireless *sw =
        NM_SETTING_WIRELESS(nm_setting_wireless_new());
    GBytes *ssid_bytes = g_bytes_new(ssid, strlen(ssid));
    g_object_set(sw, NM_SETTING_WIRELESS_SSID, ssid_bytes, NULL);
    g_bytes_unref(ssid_bytes);
    nm_connection_add_setting(conn, NM_SETTING(sw));

    if (u->connect_secure || owe) {
        NMSettingWirelessSecurity *ssec =
            NM_SETTING_WIRELESS_SECURITY(nm_setting_wireless_security_new());
        if (owe) {
            g_object_set(ssec,
                         NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "owe",
                         NULL);
        } else if (strstr(u->connect_security, "WEP")) {
            g_object_set(ssec,
                         NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "none",
                         NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, pass,
                         NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
                         NM_WEP_KEY_TYPE_PASSPHRASE,
                         NULL);
        } else {
            const char *key_mgmt =
                ap_is_sae_only(ap) ? "sae" : "wpa-psk";
            g_object_set(ssec,
                         NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, key_mgmt,
                         NM_SETTING_WIRELESS_SECURITY_PSK, pass,
                         NULL);
        }
        nm_connection_add_setting(conn, NM_SETTING(ssec));
    }

    WifiConnectOp *op = g_new0(WifiConnectOp, 1);
    op->a = a;
    op->conn = conn;
    snprintf(op->uuid, sizeof op->uuid, "%s", uuid);
    g_free(uuid);

    u->connect_busy = 1;
    snprintf(u->connect_msg, sizeof u->connect_msg,
             "Connecting to network…");
    a->net_pending++;
    nm_client_add_and_activate_connection_async(
        a->nm, conn, dev, ap_path[0] ? ap_path : NULL,
        a->net_cancel, wifi_connect_cb, op);
    mark_dirty(a);
}

static void cancel_action(App *a)
{
    if (a->action_fd >= 0) {
        close(a->action_fd);
        a->action_fd = -1;
    }
    if (a->action_pid > 0) {
        kill(a->action_pid, SIGTERM);
        waitpid(a->action_pid, NULL, 0);
        a->action_pid = -1;
    }
    a->action_len = 0;
    a->ui.wifi_action_kind = WIFI_ACTION_NONE;
    a->ui.wifi_action_row = -1;
    a->ui.bt_action_kind = BT_ACTION_NONE;
    a->ui.bt_action_row = -1;
}

static void finish_action(App *a)
{
    if (a->action_fd < 0 && a->action_pid <= 0)
        return;

    if (a->action_fd >= 0) {
        close(a->action_fd);
        a->action_fd = -1;
    }

    int status = 0;
    if (a->action_pid > 0) {
        waitpid(a->action_pid, &status, 0);
        a->action_pid = -1;
    }

    a->action_buf[a->action_len] = 0;
    a->action_len = 0;

    Ui *u = &a->ui;
    int kind = u->bt_action_kind;
    char name[96];
    char mac[24];
    snprintf(name, sizeof name, "%s", u->bt_action_name);
    snprintf(mac, sizeof mac, "%s", u->bt_action_mac);

    u->bt_action_kind = BT_ACTION_NONE;
    u->bt_action_row = -1;

    int output_failed =
        strstr(a->action_buf, "Failed") != NULL ||
        strstr(a->action_buf, "not available") != NULL;
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0 &&
             !output_failed;
    if (ok) {
        if (kind == BT_ACTION_CONNECT && mac[0]) {
            char cmd[160];
            snprintf(cmd, sizeof cmd,
                     "bluetoothctl trust %s >/dev/null 2>&1", mac);
            run_async(cmd);
        }
        u->bt_msg_ok = 1;
        if (kind == BT_ACTION_DISCONNECT)
            snprintf(u->bt_msg, sizeof u->bt_msg,
                     "Disconnected from %s", name);
        else if (kind == BT_ACTION_FORGET)
            snprintf(u->bt_msg, sizeof u->bt_msg,
                     "Forgot %s", name);
        else
            snprintf(u->bt_msg, sizeof u->bt_msg,
                     "Connected to %s · trusted", name);
    } else {
        char *msg = a->action_buf;
        char *err = strstr(msg, "Error:");
        char *failed = strstr(msg, "Failed");
        if (err)
            msg = err + 6;
        else if (failed)
            msg = failed;
        while (*msg == ' ' || *msg == '\t')
            msg++;
        char *nl = strpbrk(msg, "\r\n");
        if (nl)
            *nl = 0;
        if (!strncmp(msg, "Failed: ", 8))
            msg += 8;
        else if (!strncmp(msg, "Failed to ", 10))
            msg += 10;
        if (!msg[0]) {
            if (kind == BT_ACTION_CONNECT)
                msg = "Could not connect to the device.";
            else if (kind == BT_ACTION_DISCONNECT)
                msg = "Could not disconnect the device.";
            else if (kind == BT_ACTION_FORGET)
                msg = "Could not forget the device.";
            else
                msg = "Could not update the device.";
        }
        u->bt_msg_ok = 0;
        snprintf(u->bt_msg, sizeof u->bt_msg,
                 "Failed: %.145s", msg);
    }

    u->bt_msg_t = u->t;
    a->resync_at = mono_sec() + (ok ? 0.35 : 1.0);

    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

typedef struct {
    App *a;
    int kind;
    int row;
    int pending;
    NMActiveConnection *ac;
    guint state_id;
    guint timeout_id;
    char name[96];
} WifiActionOp;

static void wifi_action_op_free(WifiActionOp *op)
{
    if (op->ac && op->state_id)
        g_signal_handler_disconnect(op->ac, op->state_id);
    if (op->timeout_id)
        g_source_remove(op->timeout_id);
    g_clear_object(&op->ac);

    App *a = op->a;
    if (op->pending && a->net_pending > 0)
        a->net_pending--;
    g_free(op);
}

static void wifi_action_done(WifiActionOp *op, GError *err)
{
    App *a = op->a;
    Ui *u = &a->ui;

    if (!err) {
        u->wifi_msg_ok = 1;
        if (op->kind == WIFI_ACTION_DISCONNECT)
            snprintf(u->wifi_msg, sizeof u->wifi_msg,
                     "Disconnected from %s", op->name);
        else if (op->kind == WIFI_ACTION_FORGET)
            snprintf(u->wifi_msg, sizeof u->wifi_msg,
                     "Forgot %s", op->name);
        else
            snprintf(u->wifi_msg, sizeof u->wifi_msg,
                     "Connected to %s", op->name);
    } else {
        u->wifi_msg_ok = 0;
        const char *msg = err->message && err->message[0] ?
                          err->message : "NetworkManager operation failed.";
        snprintf(u->wifi_msg, sizeof u->wifi_msg,
                 "Failed: %.145s", msg);
        g_error_free(err);
    }

    u->wifi_action_kind = WIFI_ACTION_NONE;
    u->wifi_action_row = -1;
    u->wifi_msg_t = u->t;
    a->resync_at = mono_sec() + (u->wifi_msg_ok ? 0.35 : 1.0);

    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
    wifi_action_op_free(op);
}

static gboolean wifi_action_timeout_cb(gpointer user_data)
{
    WifiActionOp *op = user_data;
    op->timeout_id = 0;
    if (op->ac && op->a->nm)
        nm_client_deactivate_connection_async(
            op->a->nm, op->ac, op->a->net_cancel, NULL, NULL);

    GError *err = g_error_new_literal(NM_DEVICE_ERROR,
                                      NM_DEVICE_ERROR_NOT_ACTIVE,
                                      "Activation timed out.");
    wifi_action_done(op, err);
    return G_SOURCE_REMOVE;
}

static void wifi_action_state_cb(NMActiveConnection *ac, GParamSpec *pspec,
                                 gpointer user_data)
{
    (void)pspec;
    WifiActionOp *op = user_data;
    NMActiveConnectionState state = nm_active_connection_get_state(ac);

    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
        wifi_action_done(op, NULL);
    } else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
        GError *err = g_error_new_literal(
            NM_DEVICE_ERROR, NM_DEVICE_ERROR_NOT_ACTIVE,
            "Could not activate the saved network.");
        wifi_action_done(op, err);
    }
}

static void wifi_disconnect_cb(GObject *source, GAsyncResult *result,
                               gpointer user_data)
{
    WifiActionOp *op = user_data;
    GError *err = NULL;
    gboolean ok = nm_client_deactivate_connection_finish(
        NM_CLIENT(source), result, &err);
    if (!ok && !err)
        err = g_error_new_literal(NM_DEVICE_ERROR, NM_DEVICE_ERROR_FAILED,
                                  "Could not disconnect the network.");
    wifi_action_done(op, ok ? NULL : err);
}

static void wifi_forget_cb(GObject *source, GAsyncResult *result,
                           gpointer user_data)
{
    WifiActionOp *op = user_data;
    GError *err = NULL;
    gboolean ok = nm_remote_connection_delete_finish(
        NM_REMOTE_CONNECTION(source), result, &err);
    if (!ok && !err)
        err = g_error_new_literal(NM_CONNECTION_ERROR,
                                  NM_CONNECTION_ERROR_FAILED,
                                  "Could not delete the saved profile.");
    wifi_action_done(op, ok ? NULL : err);
}

static void wifi_activate_cb(GObject *source, GAsyncResult *result,
                             gpointer user_data)
{
    WifiActionOp *op = user_data;
    GError *err = NULL;
    NMActiveConnection *ac = nm_client_activate_connection_finish(
        NM_CLIENT(source), result, &err);
    if (!ac) {
        if (!err)
            err = g_error_new_literal(
                NM_DEVICE_ERROR, NM_DEVICE_ERROR_FAILED,
                "Could not activate the saved network.");
        wifi_action_done(op, err);
        return;
    }
    g_clear_error(&err);

    op->ac = g_object_ref(ac);
    op->state_id = g_signal_connect(ac, "notify::state",
                                    G_CALLBACK(wifi_action_state_cb), op);
    op->timeout_id = g_timeout_add_seconds(30, wifi_action_timeout_cb, op);

    NMActiveConnectionState state = nm_active_connection_get_state(ac);
    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
        wifi_action_done(op, NULL);
    } else if (state == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
        GError *e = g_error_new_literal(
            NM_DEVICE_ERROR, NM_DEVICE_ERROR_NOT_ACTIVE,
            "Could not activate the saved network.");
        wifi_action_done(op, e);
    } else {
        App *a = op->a;
        a->ui.wifi_msg_ok = 1;
        snprintf(a->ui.wifi_msg, sizeof a->ui.wifi_msg,
                 "Connecting to %s…", op->name);
        a->ui.wifi_msg_t = a->ui.t;
        mark_dirty(a);
    }
}

static void start_wifi_action(App *a, int kind, int row)
{
    Ui *u = &a->ui;
    if (!u->wifi_on || !a->nm || u->connect_open || u->connect_busy ||
        u->wifi_action_kind != WIFI_ACTION_NONE ||
        a->action_fd >= 0 || a->action_pid > 0 ||
        row < 0 || row >= u->net_count)
        return;

    WifiNet *n = &u->nets[row];
    NMDevice *dev = wifi_device(a->nm);

    WifiActionOp *op = g_new0(WifiActionOp, 1);
    op->a = a;
    op->kind = kind;
    op->row = row;
    snprintf(op->name, sizeof op->name, "%s", n->ssid);

    const char *uuid = n->uuid[0] ? n->uuid :
                       (n->connected ? u->wifi_uuid : "");

    if (kind == WIFI_ACTION_DISCONNECT) {
        NMActiveConnection *ac = wifi_active_connection(a->nm);
        if (!ac || !dev) {
            GError *err = g_error_new_literal(
                NM_DEVICE_ERROR, NM_DEVICE_ERROR_NOT_ACTIVE,
                "No active Wi-Fi connection to disconnect.");
            wifi_action_done(op, err);
            return;
        }
        u->wifi_action_kind = kind;
        u->wifi_action_row = row;
        snprintf(u->wifi_action_ssid, sizeof u->wifi_action_ssid,
                 "%s", n->ssid);
        u->wifi_msg[0] = 0;
        op->pending = 1;
        a->net_pending++;
        mark_dirty(a);
        nm_client_deactivate_connection_async(
            a->nm, ac, a->net_cancel, wifi_disconnect_cb, op);
        return;
    }

    NMRemoteConnection *rc = uuid[0] ?
        nm_client_get_connection_by_uuid(a->nm, uuid) : NULL;
    if (!rc) {
        GError *err = g_error_new_literal(
            NM_CONNECTION_ERROR, NM_CONNECTION_ERROR_FAILED,
            "The saved network profile is no longer available.");
        wifi_action_done(op, err);
        return;
    }

    if (kind == WIFI_ACTION_FORGET) {
        u->wifi_action_kind = kind;
        u->wifi_action_row = row;
        snprintf(u->wifi_action_ssid, sizeof u->wifi_action_ssid,
                 "%s", n->ssid);
        u->wifi_msg[0] = 0;
        op->pending = 1;
        a->net_pending++;
        mark_dirty(a);
        nm_remote_connection_delete_async(
            rc, a->net_cancel, wifi_forget_cb, op);
        return;
    }

    if (!dev) {
        GError *err = g_error_new_literal(
            NM_DEVICE_ERROR, NM_DEVICE_ERROR_FAILED,
            "No Wi-Fi device is available.");
        wifi_action_done(op, err);
        return;
    }

    NMAccessPoint *ap = NULL;
    if (n->ap_path[0])
        ap = nm_device_wifi_get_access_point_by_path(
            NM_DEVICE_WIFI(dev), n->ap_path);
    if (!ap)
        ap = wifi_ap_by_ssid(dev, n->ssid);
    const char *specific = ap ? nm_object_get_path(NM_OBJECT(ap)) : NULL;

    u->wifi_action_kind = kind;
    u->wifi_action_row = row;
    snprintf(u->wifi_action_ssid, sizeof u->wifi_action_ssid,
             "%s", n->ssid);
    u->wifi_msg[0] = 0;
    op->pending = 1;
    a->net_pending++;
    mark_dirty(a);
    nm_client_activate_connection_async(
        a->nm, NM_CONNECTION(rc), dev, specific,
        a->net_cancel, wifi_activate_cb, op);
}

static void start_bt_action(App *a, int kind, int row)
{
    Ui *u = &a->ui;
    if (!u->bt_on || u->connect_open || u->connect_busy ||
        u->wifi_action_kind != WIFI_ACTION_NONE ||
        a->action_fd >= 0 || a->action_pid > 0 ||
        row < 0 || row >= u->bt_count)
        return;

    BtDev *d = &u->bt[row];
    const char *verb;
    if (kind == BT_ACTION_DISCONNECT)
        verb = "disconnect";
    else if (kind == BT_ACTION_FORGET)
        verb = "remove";
    else
        verb = "connect";

    int p[2];
    if (pipe(p) < 0) {
        u->bt_msg_ok = 0;
        snprintf(u->bt_msg, sizeof u->bt_msg,
                 "Failed: cannot start bluetoothctl.");
        u->bt_msg_t = u->t;
        mark_dirty(a);
        return;
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        dup2(p[1], STDERR_FILENO);
        close(p[1]);

        char *args[4];
        args[0] = "bluetoothctl";
        args[1] = (char *)verb;
        args[2] = d->mac;
        args[3] = NULL;
        execvp("bluetoothctl", args);
        _exit(127);
    }

    close(p[1]);
    if (pid < 0) {
        close(p[0]);
        u->bt_msg_ok = 0;
        snprintf(u->bt_msg, sizeof u->bt_msg,
                 "Failed: cannot start bluetoothctl.");
        u->bt_msg_t = u->t;
        mark_dirty(a);
        return;
    }

    a->action_fd = p[0];
    a->action_pid = pid;
    a->action_len = 0;
    u->bt_action_kind = kind;
    u->bt_action_row = row;
    snprintf(u->bt_action_name, sizeof u->bt_action_name,
             "%s", d->name);
    snprintf(u->bt_action_mac, sizeof u->bt_action_mac,
             "%s", d->mac);
    u->bt_msg[0] = 0;
    mark_dirty(a);
}

/* ---- protocol listeners ------------------------------------------ */

static void shm_format(void *data, struct wl_shm *shm, uint32_t format)
{
    (void)shm;
    App *a = data;
    if (format == WL_SHM_FORMAT_ARGB8888)
        a->has_argb = 1;
}

static const struct wl_shm_listener shm_listener = {
    .format = shm_format,
};

static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

static void xdg_surface_configure(void *data, struct xdg_surface *xs,
                                  uint32_t serial)
{
    App *a = data;
    xdg_surface_ack_configure(xs, serial);
    a->configured = 1;
    mark_dirty(a);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void toplevel_configure(void *data, struct xdg_toplevel *t,
                               int32_t width, int32_t height,
                               struct wl_array *states)
{
    (void)data; (void)t; (void)width; (void)height; (void)states;
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    App *a = data;
    a->quit = 1;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
};

static void decoration_configure(void *data,
                                 struct zxdg_toplevel_decoration_v1 *dec,
                                 uint32_t mode)
{
    (void)data; (void)dec; (void)mode;
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    .configure = decoration_configure,
};

static void output_geometry(void *data, struct wl_output *output,
                            int32_t x, int32_t y, int32_t pw, int32_t ph,
                            int32_t subpixel, const char *make,
                            const char *model, int32_t transform)
{
    (void)data; (void)output; (void)x; (void)y; (void)pw; (void)ph;
    (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_mode(void *data, struct wl_output *output, uint32_t flags,
                        int32_t width, int32_t height, int32_t refresh)
{
    (void)data; (void)output; (void)flags;
    (void)width; (void)height; (void)refresh;
}

static void output_done(void *data, struct wl_output *output)
{
    (void)data; (void)output;
}

static void output_scale(void *data, struct wl_output *output, int32_t factor)
{
    (void)output;
    Output *o = data;
    if (factor < 1)
        factor = 1;
    o->scale = factor;
    if (o->entered)
        recalc_scale(o->app);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_geometry,
    .mode = output_mode,
    .done = output_done,
    .scale = output_scale,
};

static void surface_enter(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 1;
            break;
        }
    }
    recalc_scale(a);
}

static void surface_leave(void *data, struct wl_surface *surface,
                          struct wl_output *output)
{
    (void)surface;
    App *a = data;
    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output == output) {
            a->outputs[i].entered = 0;
            break;
        }
    }
    recalc_scale(a);
}

static const struct wl_surface_listener surface_listener = {
    .enter = surface_enter,
    .leave = surface_leave,
};

/* ---- hit testing -------------------------------------------------- */

static int hit_close(double x, double y)
{
    double dx = x - CLOSE_X;
    double dy = y - CLOSE_Y;
    return dx * dx + dy * dy <= 16.0 * 16.0;
}

static int hit_toggle(double x, double y)
{
    return x >= TOGGLE_X - 4.0 && x <= TOGGLE_X + TOGGLE_W + 4.0 &&
           y >= TOGGLE_Y - 4.0 && y <= TOGGLE_Y + TOGGLE_H + 4.0;
}

static int hit_nav(double x, double y)
{
    if (x < 26.0 || x > 202.0)
        return -1;
    if (y >= 114.0 && y <= 160.0)
        return TAB_WIFI;
    if (y >= 168.0 && y <= 214.0)
        return TAB_BLUETOOTH;
    return -1;
}

static int hit_scan(App *a, double x, double y)
{
    return a->ui.tab == TAB_WIFI && a->ui.wifi_on &&
           x >= CONTENT_X + CONTENT_W - 78.0 &&
           x <= CONTENT_X + CONTENT_W &&
           y >= SECTION_Y - 6.0 && y <= SECTION_Y + 22.0;
}

static int hit_pair(App *a, double x, double y)
{
    return a->ui.tab == TAB_BLUETOOTH && a->ui.bt_on &&
           x >= CONTENT_X + CONTENT_W - 92.0 &&
           x <= CONTENT_X + CONTENT_W &&
           y >= SECTION_Y - 6.0 && y <= SECTION_Y + 22.0;
}

static int hit_enable(App *a, double x, double y)
{
    int off = (a->ui.tab == TAB_WIFI) ? !a->ui.wifi_on : !a->ui.bt_on;
    if (!off)
        return 0;
    double bw = (a->ui.tab == TAB_WIFI) ? 158.0 : 190.0;
    double cx = CONTENT_X + CONTENT_W / 2.0;
    return x >= cx - bw / 2.0 && x <= cx + bw / 2.0 &&
           y >= LIST_Y + 162.0 && y <= LIST_Y + 200.0;
}

static int hit_row(App *a, double x, double y)
{
    int on = (a->ui.tab == TAB_WIFI) ? a->ui.wifi_on : a->ui.bt_on;
    int count = (a->ui.tab == TAB_WIFI) ? a->ui.net_count : a->ui.bt_count;
    if (!on || count <= 0)
        return -1;
    if (x < CONTENT_X || x > CONTENT_X + CONTENT_W)
        return -1;
    if (y < LIST_Y || y >= LIST_Y + 4.0 * (ROW_H + ROW_GAP))
        return -1;
    int idx = (int)((y - LIST_Y) / (ROW_H + ROW_GAP));
    double local = y - (LIST_Y + idx * (ROW_H + ROW_GAP));
    if (local > ROW_H || idx >= count)
        return -1;
    return idx;
}

static int hit_row_action(const Ui *u, int row, double x, double y)
{
    if (row < 0 || row >= 4)
        return WIFI_ACTION_NONE;

    double ay = LIST_Y + row * (ROW_H + ROW_GAP) + ROW_ACTION_Y_OFF;
    if (y < ay || y > ay + ROW_ACTION_H)
        return WIFI_ACTION_NONE;

    double right = CONTENT_X + CONTENT_W - ROW_ACTION_RIGHT_PAD;
    double ax, aw;

    if (u->tab == TAB_WIFI) {
        if (row >= u->net_count)
            return WIFI_ACTION_NONE;
        if (u->wifi_action_kind != WIFI_ACTION_NONE &&
            u->wifi_action_row == row)
            return WIFI_ACTION_NONE;

        const WifiNet *n = &u->nets[row];
        if (!n->connected && !n->saved)
            return WIFI_ACTION_NONE;

        if (n->connected) {
            row_action_rect(WIFI_ACTION_DISCONNECT, right, ay, &ax, &aw);
            if (x >= ax && x <= ax + aw)
                return WIFI_ACTION_DISCONNECT;
        }
        if (n->saved || n->connected) {
            row_action_rect(WIFI_ACTION_FORGET, right, ay, &ax, &aw);
            if (x >= ax && x <= ax + aw)
                return WIFI_ACTION_FORGET;
        }
    } else {
        if (row >= u->bt_count)
            return WIFI_ACTION_NONE;
        if (u->bt_action_kind != BT_ACTION_NONE &&
            u->bt_action_row == row)
            return WIFI_ACTION_NONE;

        const BtDev *d = &u->bt[row];
        int main_kind = d->connected ? BT_ACTION_DISCONNECT :
                                       BT_ACTION_CONNECT;
        row_action_rect(main_kind, right, ay, &ax, &aw);
        if (x >= ax && x <= ax + aw)
            return main_kind;
        row_action_rect(BT_ACTION_FORGET, right, ay, &ax, &aw);
        if (x >= ax && x <= ax + aw)
            return BT_ACTION_FORGET;
    }
    return WIFI_ACTION_NONE;
}

static int hit_connect_card(double x, double y)
{
    return x >= MODAL_X && x <= MODAL_X + MODAL_W &&
           y >= MODAL_Y && y <= MODAL_Y + MODAL_H;
}

static int hit_connect_field(App *a, double x, double y)
{
    return a->ui.connect_open &&
           x >= MODAL_X + MODAL_PAD &&
           x <= MODAL_X + MODAL_W - MODAL_PAD &&
           y >= MODAL_FIELD_Y && y <= MODAL_FIELD_Y + MODAL_FIELD_H;
}

static int hit_connect_cancel(App *a, double x, double y)
{
    return a->ui.connect_open &&
           x >= MODAL_X + MODAL_PAD &&
           x <= MODAL_X + MODAL_PAD + MODAL_BTN_W &&
           y >= MODAL_BTN_Y && y <= MODAL_BTN_Y + MODAL_BTN_H;
}

static int hit_connect_submit(App *a, double x, double y)
{
    double bx = MODAL_X + MODAL_W - MODAL_PAD - MODAL_BTN_W;
    return a->ui.connect_open &&
           x >= bx && x <= bx + MODAL_BTN_W &&
           y >= MODAL_BTN_Y && y <= MODAL_BTN_Y + MODAL_BTN_H;
}

static int update_hover(App *a)
{
    double x = a->pointer_x;
    double y = a->pointer_y;
    int changed = 0;

#define SET_HOVER(field, value)                    \
    do {                                           \
        int v_ = (value);                          \
        if (a->ui.field != v_) {                   \
            a->ui.field = v_;                      \
            changed = 1;                           \
        }                                          \
    } while (0)

    if (a->ui.connect_open) {
        SET_HOVER(hover_nav, -1);
        SET_HOVER(hover_toggle, 0);
        SET_HOVER(hover_close, 0);
        SET_HOVER(hover_scan, 0);
        SET_HOVER(hover_pair, 0);
        SET_HOVER(hover_enable, 0);
        SET_HOVER(hover_row, -1);
        SET_HOVER(hover_row_action, WIFI_ACTION_NONE);
        SET_HOVER(hover_row_action_idx, -1);
        SET_HOVER(hover_connect_field, hit_connect_field(a, x, y));
        SET_HOVER(hover_connect_cancel, hit_connect_cancel(a, x, y));
        SET_HOVER(hover_connect_submit, hit_connect_submit(a, x, y));
        return changed;
    }

    SET_HOVER(hover_nav, hit_nav(x, y));
    SET_HOVER(hover_toggle, hit_toggle(x, y));
    SET_HOVER(hover_close, hit_close(x, y));
    SET_HOVER(hover_scan, hit_scan(a, x, y));
    SET_HOVER(hover_pair, hit_pair(a, x, y));
    SET_HOVER(hover_enable, hit_enable(a, x, y));
    int row = hit_row(a, x, y);
    int row_action = hit_row_action(&a->ui, row, x, y);
    SET_HOVER(hover_row, row);
    SET_HOVER(hover_row_action, row_action);
    SET_HOVER(hover_row_action_idx,
              row_action != WIFI_ACTION_NONE ? row : -1);
    SET_HOVER(hover_connect_field, hit_connect_field(a, x, y));
    SET_HOVER(hover_connect_cancel, hit_connect_cancel(a, x, y));
    SET_HOVER(hover_connect_submit, hit_connect_submit(a, x, y));

#undef SET_HOVER
    return changed;
}

static void update_cursor(App *a)
{
    if (!a->cursor_shape_manager || !a->pointer)
        return;
    if (!a->cursor_shape_device) {
        a->cursor_shape_device = wp_cursor_shape_manager_v1_get_pointer(
            a->cursor_shape_manager, a->pointer);
        a->cursor_shape = 0;
    }

    uint32_t shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT;
    if (a->ui.connect_open) {
        if (a->ui.hover_connect_field)
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT;
        else if (a->ui.hover_connect_cancel || a->ui.hover_connect_submit)
            shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    } else if (a->ui.hover_close || a->ui.hover_toggle ||
        a->ui.hover_scan || a->ui.hover_pair || a->ui.hover_enable ||
        a->ui.hover_row >= 0 || a->ui.hover_nav >= 0) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER;
    } else if (a->pointer_x >= 0.0 && a->pointer_y >= 0.0 &&
               a->pointer_y < 96.0) {
        shape = WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_GRAB;
    }

    if (shape != a->cursor_shape) {
        wp_cursor_shape_device_v1_set_shape(a->cursor_shape_device,
                                            a->pointer_serial, shape);
        a->cursor_shape = shape;
    }
}

static void connect_modal_close(App *a)
{
    Ui *u = &a->ui;
    if (!u->connect_open || u->connect_busy)
        return;

    u->connect_open = 0;
    u->connect_idx = -1;
    u->connect_secure = 0;
    u->connect_focus = 0;
    u->connect_msg[0] = 0;
    memset(u->connect_pass, 0, sizeof u->connect_pass);
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void connect_modal_open(App *a, int idx)
{
    Ui *u = &a->ui;
    if (u->connect_open || u->connect_busy ||
        u->wifi_action_kind != WIFI_ACTION_NONE ||
        a->action_fd >= 0 || a->action_pid > 0 ||
        idx < 0 || idx >= u->net_count)
        return;

    WifiNet *n = &u->nets[idx];
    if (!n->ssid[0] || n->connected)
        return;

    u->connect_open = 1;
    u->connect_idx = idx;
    u->connect_secure = wifi_is_secure(n);
    snprintf(u->connect_ssid, sizeof u->connect_ssid, "%s", n->ssid);
    snprintf(u->connect_security, sizeof u->connect_security,
             "%s", n->security);
    u->connect_pass[0] = 0;
    u->connect_msg[0] = 0;
    u->connect_busy = 0;
    u->connect_focus = 1;
    u->connect_caret_t = u->t;
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void switch_tab(App *a, int tab)
{
    if (a->ui.tab == tab)
        return;
    a->ui.tab = tab;
    a->ui.hover_row = -1;
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void toggle_active(App *a)
{
    if (a->ui.tab == TAB_WIFI) {
        set_wifi_radio(a, !a->ui.wifi_on);
    } else {
        set_bt_radio(&a->ui, !a->ui.bt_on);
    }
    a->resync_at = mono_sec() + 1.2;
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    a->pointer_serial = serial;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    a->cursor_shape = 0;
    update_hover(a);
    update_cursor(a);
    mark_dirty(a);
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface)
{
    (void)pointer; (void)serial; (void)surface;
    App *a = data;
    a->pointer_x = -1;
    a->pointer_y = -1;
    a->ui.hover_nav = -1;
    a->ui.hover_toggle = 0;
    a->ui.hover_close = 0;
    a->ui.hover_scan = 0;
    a->ui.hover_pair = 0;
    a->ui.hover_enable = 0;
    a->ui.hover_row = -1;
    a->ui.hover_connect_field = 0;
    a->ui.hover_connect_cancel = 0;
    a->ui.hover_connect_submit = 0;
    a->ui.press_toggle = 0;
    update_cursor(a);
    mark_dirty(a);
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)pointer; (void)time;
    App *a = data;
    a->pointer_x = wl_fixed_to_double(sx);
    a->pointer_y = wl_fixed_to_double(sy);
    if (update_hover(a))
        mark_dirty(a);
    update_cursor(a);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
    (void)pointer; (void)time;
    App *a = data;

    if (button != BTN_LEFT)
        return;

    mark_dirty(a);
    double x = a->pointer_x;
    double y = a->pointer_y;

    if (a->ui.connect_open) {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
            update_hover(a);
            update_cursor(a);
            if (hit_connect_field(a, x, y)) {
                /* The password field is the only text target, so a click
                 * keeps focus there and restarts the caret blink. */
                a->ui.connect_focus = 1;
                a->ui.connect_caret_t = a->ui.t;
                mark_dirty(a);
            } else if (hit_connect_cancel(a, x, y)) {
                connect_modal_close(a);
            } else if (hit_connect_submit(a, x, y)) {
                start_wifi_connect(a);
            } else if (!hit_connect_card(x, y)) {
                connect_modal_close(a);
            }
        }
        return;
    }

    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        update_hover(a);
        update_cursor(a);
        int nav = hit_nav(x, y);
        int row = hit_row(a, x, y);
        int row_action = hit_row_action(&a->ui, row, x, y);
        if (hit_close(x, y)) {
            a->quit = 1;
        } else if (hit_toggle(x, y)) {
            a->ui.press_toggle = 1;
        } else if (nav >= 0) {
            switch_tab(a, nav);
        } else if (hit_scan(a, x, y)) {
            if (a->ui.wifi_on) {
                a->ui.scan_t = mono_sec() - a->t0;
                request_wifi_scan(a);
            }
        } else if (hit_pair(a, x, y)) {
            a->ui.pair_t = mono_sec() - a->t0;
        } else if (hit_enable(a, x, y)) {
            if (a->ui.tab == TAB_WIFI)
                set_wifi_radio(a, 1);
            else
                set_bt_radio(&a->ui, 1);
            a->resync_at = mono_sec() + 1.2;
        } else if (a->ui.tab == TAB_BLUETOOTH && row >= 0 &&
                   row_action != WIFI_ACTION_NONE) {
            start_bt_action(a, row_action, row);
        } else if (a->ui.tab == TAB_BLUETOOTH && row >= 0) {
            BtDev *d = &a->ui.bt[row];
            if (!d->connected)
                start_bt_action(a, BT_ACTION_CONNECT, row);
        } else if (a->ui.tab == TAB_WIFI && row >= 0 &&
                   row_action != WIFI_ACTION_NONE) {
            start_wifi_action(a, row_action, row);
        } else if (a->ui.tab == TAB_WIFI && row >= 0) {
            WifiNet *n = &a->ui.nets[row];
            if (n->saved && !n->connected)
                start_wifi_action(a, WIFI_ACTION_ACTIVATE, row);
            else if (!n->connected)
                connect_modal_open(a, row);
        } else if (y < 96.0) {
            if (a->toplevel && a->seat)
                xdg_toplevel_move(a->toplevel, a->seat, serial);
        }
    } else if (a->ui.press_toggle) {
        a->ui.press_toggle = 0;
        if (hit_toggle(x, y))
            toggle_active(a);
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer)
{
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source)
{
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis)
{
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete)
{
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
                            uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    App *a = data;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;

    struct xkb_keymap *km = xkb_keymap_new_from_string(
        a->xkb_ctx, map, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!km)
        return;

    struct xkb_state *st = xkb_state_new(km);
    if (!st) {
        xkb_keymap_unref(km);
        return;
    }

    xkb_state_unref(a->xkb_state);
    xkb_keymap_unref(a->xkb_keymap);
    a->xkb_keymap = km;
    a->xkb_state = st;
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)keyboard; (void)serial; (void)surface; (void)keys;
    App *a = data;
    a->keyboard_focused = 1;
    a->ui.connect_caret_t = a->ui.t;
    mark_dirty(a);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
                           uint32_t serial, struct wl_surface *surface)
{
    (void)keyboard; (void)serial; (void)surface;
    App *a = data;
    a->keyboard_focused = 0;
    mark_dirty(a);
}

static void connect_modal_key(App *a, uint32_t key)
{
    Ui *u = &a->ui;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);

    if (u->connect_busy)
        return;
    if (sym == XKB_KEY_Escape) {
        connect_modal_close(a);
        return;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        start_wifi_connect(a);
        return;
    }
    if (!u->connect_secure)
        return;

    if (sym == XKB_KEY_BackSpace) {
        size_t len = strlen(u->connect_pass);
        if (len > 0) {
            size_t n = len - 1;
            while (n > 0 && ((unsigned char)u->connect_pass[n] & 0xC0) == 0x80)
                n--;
            u->connect_pass[n] = 0;
            u->connect_msg[0] = 0;
            u->connect_caret_t = u->t;
            mark_dirty(a);
        }
        return;
    }

    char buf[16];
    int n = xkb_state_key_get_utf8(a->xkb_state, key + 8, buf, sizeof buf);
    if (n <= 0)
        return;

    size_t len = strlen(u->connect_pass);
    size_t room = sizeof u->connect_pass - len - 1;
    if (room == 0)
        return;

    size_t add = 0;
    for (int i = 0; i < n && add < room; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x20 || c == 0x7f)
            continue;
        u->connect_pass[len + add++] = (char)c;
    }
    if (add > 0) {
        u->connect_pass[len + add] = 0;
        u->connect_msg[0] = 0;
        u->connect_caret_t = u->t;
        mark_dirty(a);
    }
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
                         uint32_t serial, uint32_t time,
                         uint32_t key, uint32_t state)
{
    (void)keyboard; (void)serial; (void)time;
    App *a = data;

    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !a->xkb_state)
        return;

    if (a->ui.connect_open) {
        connect_modal_key(a, key);
        return;
    }

    mark_dirty(a);
    xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xkb_state, key + 8);
    if (sym == XKB_KEY_Escape || sym == XKB_KEY_q || sym == XKB_KEY_Q) {
        a->quit = 1;
    } else if (sym == XKB_KEY_Tab || sym == XKB_KEY_Left ||
               sym == XKB_KEY_Right || sym == XKB_KEY_h || sym == XKB_KEY_l) {
        switch_tab(a, a->ui.tab == TAB_WIFI ? TAB_BLUETOOTH : TAB_WIFI);
    } else if (sym == XKB_KEY_1) {
        switch_tab(a, TAB_WIFI);
    } else if (sym == XKB_KEY_2) {
        switch_tab(a, TAB_BLUETOOTH);
    } else if (sym == XKB_KEY_space || sym == XKB_KEY_Return ||
               sym == XKB_KEY_KP_Enter) {
        toggle_active(a);
    } else if (sym == XKB_KEY_s || sym == XKB_KEY_S) {
        if (a->ui.tab == TAB_WIFI) {
            if (a->ui.wifi_on) {
                a->ui.scan_t = mono_sec() - a->t0;
                request_wifi_scan(a);
            }
        } else if (a->ui.bt_on) {
            a->ui.pair_t = mono_sec() - a->t0;
        }
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
                               uint32_t serial, uint32_t dep,
                               uint32_t latched, uint32_t locked,
                               uint32_t group)
{
    (void)keyboard; (void)serial;
    App *a = data;
    if (a->xkb_state)
        xkb_state_update_mask(a->xkb_state, dep, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
                                 int32_t rate, int32_t delay)
{
    (void)data; (void)keyboard; (void)rate; (void)delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    App *a = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->pointer) {
        a->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(a->pointer, &pointer_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && a->pointer) {
        if (a->cursor_shape_device) {
            wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
            a->cursor_shape_device = NULL;
            a->cursor_shape = 0;
        }
        wl_pointer_destroy(a->pointer);
        a->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->keyboard) {
        a->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(a->keyboard, &keyboard_listener, a);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && a->keyboard) {
        wl_keyboard_destroy(a->keyboard);
        a->keyboard = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

static void registry_global(void *data, struct wl_registry *registry,
                            uint32_t name, const char *interface,
                            uint32_t version)
{
    App *a = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        a->compositor = wl_registry_bind(registry, name,
                                         &wl_compositor_interface,
                                         version < 4 ? version : 4);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        a->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
        wl_shm_add_listener(a->shm, &shm_listener, a);
    } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        a->wm_base = wl_registry_bind(registry, name,
                                      &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(a->wm_base, &wm_base_listener, a);
    } else if (!strcmp(interface,
                       zxdg_decoration_manager_v1_interface.name)) {
        a->deco_manager = wl_registry_bind(
            registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       xdg_toplevel_icon_manager_v1_interface.name)) {
        a->icon_manager = wl_registry_bind(
            registry, name, &xdg_toplevel_icon_manager_v1_interface, 1);
    } else if (!strcmp(interface,
                       wp_cursor_shape_manager_v1_interface.name)) {
        a->cursor_shape_manager = wl_registry_bind(
            registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        a->seat = wl_registry_bind(registry, name, &wl_seat_interface,
                                   version < 5 ? version : 5);
        wl_seat_add_listener(a->seat, &seat_listener, a);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (a->output_count < MAX_OUTPUTS) {
            Output *o = &a->outputs[a->output_count];
            o->output = wl_registry_bind(registry, name,
                                         &wl_output_interface,
                                         version < 2 ? version : 2);
            o->scale = 1;
            o->entered = 0;
            o->app = a;
            wl_output_add_listener(o->output, &output_listener, o);
            a->output_count++;
        }
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static int wayland_start(App *a)
{
    a->display = wl_display_connect(NULL);
    if (!a->display) {
        fprintf(stderr, "jtnetworks: cannot connect to a Wayland display\n");
        return 0;
    }

    a->registry = wl_display_get_registry(a->display);
    wl_registry_add_listener(a->registry, &registry_listener, a);
    if (wl_display_roundtrip(a->display) < 0 ||
        wl_display_roundtrip(a->display) < 0) {
        fprintf(stderr, "jtnetworks: Wayland roundtrip failed\n");
        return 0;
    }

    if (!a->compositor || !a->shm || !a->wm_base) {
        fprintf(stderr, "jtnetworks: missing wl_compositor, wl_shm "
                        "or xdg_wm_base\n");
        return 0;
    }
    if (!a->has_argb) {
        fprintf(stderr, "jtnetworks: compositor has no ARGB8888 shm format\n");
        return 0;
    }

    a->surface = wl_compositor_create_surface(a->compositor);
    if (!a->surface) {
        fprintf(stderr, "jtnetworks: cannot create wl_surface\n");
        return 0;
    }
    wl_surface_add_listener(a->surface, &surface_listener, a);

    if (wl_surface_get_version(a->surface) < 3) {
        a->scale = 1;
        a->fixed_scale = 1;
    }

    a->xdg_surface = xdg_wm_base_get_xdg_surface(a->wm_base, a->surface);
    xdg_surface_add_listener(a->xdg_surface, &xdg_surface_listener, a);
    a->toplevel = xdg_surface_get_toplevel(a->xdg_surface);
    xdg_toplevel_add_listener(a->toplevel, &toplevel_listener, a);
    xdg_toplevel_set_title(a->toplevel, "JTLab Networks");
    xdg_toplevel_set_app_id(a->toplevel, "jtnetworks");
    xdg_toplevel_set_min_size(a->toplevel, (int)WIN_W, (int)WIN_H);
    xdg_toplevel_set_max_size(a->toplevel, (int)WIN_W, (int)WIN_H);

    if (a->deco_manager) {
        a->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
            a->deco_manager, a->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(
            a->decoration, &decoration_listener, a);
        zxdg_toplevel_decoration_v1_set_mode(
            a->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }

    if (a->icon_manager) {
        struct xdg_toplevel_icon_v1 *icon =
            xdg_toplevel_icon_manager_v1_create_icon(a->icon_manager);
        if (icon) {
            xdg_toplevel_icon_v1_set_name(icon, "jtnetworks");
            xdg_toplevel_icon_manager_v1_set_icon(a->icon_manager,
                                                  a->toplevel, icon);
            xdg_toplevel_icon_v1_destroy(icon);
        }
    }

    if (wl_surface_get_version(a->surface) >= 3)
        wl_surface_set_buffer_scale(a->surface, a->scale);
    wl_surface_commit(a->surface);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Live control socket                                                */
/* ------------------------------------------------------------------ */

static void
ctl_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !rt[0]) rt = "/tmp";
    snprintf(buf, n, "%s/jtnetworks-ctl.sock", rt);
}

static void
ctl_handle_line(App *a, char *line)
{
    while (*line == ' ' || *line == '\t')
        line++;

    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t' ||
                     line[n - 1] == '\r' || line[n - 1] == '\n'))
        line[--n] = '\0';

    if (!strcmp(line, "quit")) {
        a->quit = 1;
        return;
    }

    if (strncmp(line, "accent", 6) == 0 &&
        (line[6] == ' ' || line[6] == '\t')) {
        const char *arg = line + 6;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        unsigned rgb;
        if (accent_parse(arg, &rgb)) {
            accent_set(rgb);
            mark_dirty(a);
        }
    }
}

static void
ctl_close_client(App *a)
{
    if (a->ctl_client_fd >= 0) {
        close(a->ctl_client_fd);
        a->ctl_client_fd = -1;
    }
    a->ctl_len = 0;
}

static void
ctl_process(App *a, int flush)
{
    size_t start = 0;
    for (size_t i = 0; i < a->ctl_len; i++) {
        if (a->ctl_buf[i] == '\n') {
            a->ctl_buf[i] = '\0';
            ctl_handle_line(a, a->ctl_buf + start);
            start = i + 1;
        }
    }
    if (start > 0) {
        memmove(a->ctl_buf, a->ctl_buf + start, a->ctl_len - start);
        a->ctl_len -= start;
    }
    if (flush && a->ctl_len > 0) {
        a->ctl_buf[a->ctl_len] = '\0';
        ctl_handle_line(a, a->ctl_buf);
        a->ctl_len = 0;
    }
}

static void
ctl_read_client(App *a)
{
    char buf[128];
    for (;;) {
        ssize_t n = read(a->ctl_client_fd, buf, sizeof(buf));
        if (n > 0) {
            size_t room = sizeof(a->ctl_buf) - 1 - a->ctl_len;
            if ((size_t)n > room) {
                ctl_close_client(a);
                return;
            }
            memcpy(a->ctl_buf + a->ctl_len, buf, (size_t)n);
            a->ctl_len += (size_t)n;
            ctl_process(a, 0);
            continue;
        }
        if (n == 0) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ctl_process(a, 1);
            ctl_close_client(a);
            return;
        }
        return;
    }
}

static void
ctl_accept(App *a)
{
    for (;;) {
        int fd = accept4(a->ctl_fd, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (a->ctl_client_fd >= 0) {
            close(fd);
            continue;
        }
        a->ctl_client_fd = fd;
        a->ctl_len = 0;
    }
}

static int
ctl_start(App *a)
{
    char path[256];
    ctl_path(path, sizeof(path));

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "jtnetworks: control socket path too long: %s\n",
                path);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        perror("jtnetworks: control socket");
        return 0;
    }
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "jtnetworks: cannot bind %s: %s\n",
                path, strerror(errno));
        close(fd);
        return 0;
    }
    if (listen(fd, 4) < 0) {
        fprintf(stderr, "jtnetworks: cannot listen on %s: %s\n",
                path, strerror(errno));
        close(fd);
        unlink(path);
        return 0;
    }

    a->ctl_fd = fd;
    return 1;
}

static void
ctl_cleanup(App *a)
{
    ctl_close_client(a);
    if (a->ctl_fd >= 0) {
        close(a->ctl_fd);
        a->ctl_fd = -1;
        char path[256];
        ctl_path(path, sizeof(path));
        unlink(path);
    }
}

/* Returns < 0 on a fatal display error, 0 otherwise. */
static int pump_events(App *a, int timeout_ms)
{
    GMainContext *def_ctx = g_main_context_default();
    GMainContext *nm_ctx = a->nm ? nm_client_get_main_context(a->nm) : NULL;

    for (;;) {
        int did = 0;
        while (g_main_context_pending(def_ctx)) {
            g_main_context_iteration(def_ctx, FALSE);
            did = 1;
        }
        if (nm_ctx && nm_ctx != def_ctx) {
            while (g_main_context_pending(nm_ctx)) {
                g_main_context_iteration(nm_ctx, FALSE);
                did = 1;
            }
        }
        if (!did)
            break;
    }

    if (wl_display_dispatch_pending(a->display) < 0)
        return -1;

    int want_write = 0;
    if (wl_display_flush(a->display) < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        want_write = 1;
    }
    if (a->dirty && timeout_ms > 16)
        timeout_ms = 16;
    if (a->net_pending > 0 && timeout_ms > 16)
        timeout_ms = 16;

    struct pollfd pfd[4];
    pfd[0].fd = wl_display_get_fd(a->display);
    pfd[0].events = POLLIN | (want_write ? POLLOUT : 0);
    pfd[0].revents = 0;

    int nfds = 1;
    int action_idx = -1;
    if (a->action_fd >= 0) {
        action_idx = nfds;
        pfd[nfds].fd = a->action_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }

    int ctl_idx = -1;
    int ctl_client_idx = -1;
    if (a->ctl_fd >= 0) {
        ctl_idx = nfds;
        pfd[nfds].fd = a->ctl_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }
    if (a->ctl_client_fd >= 0) {
        ctl_client_idx = nfds;
        pfd[nfds].fd = a->ctl_client_fd;
        pfd[nfds].events = POLLIN;
        pfd[nfds].revents = 0;
        nfds++;
    }

    int pr = poll(pfd, (nfds_t)nfds, timeout_ms);
    if (pr < 0)
        return (errno == EINTR) ? 0 : -1;
    if (pr == 0)
        return 0;

    if (pfd[0].revents & POLLOUT) {
        if (wl_display_flush(a->display) < 0 && errno != EAGAIN)
            return -1;
    }
    if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL))
        return -1;
    if (pfd[0].revents & POLLIN) {
        if (wl_display_dispatch(a->display) < 0)
            return -1;
    }

    if (action_idx >= 0 && pfd[action_idx].revents) {
        if (pfd[action_idx].revents & (POLLERR | POLLNVAL)) {
            finish_action(a);
        } else {
            for (;;) {
                char tmp[2048];
                ssize_t n = read(a->action_fd, tmp, sizeof tmp);
                if (n > 0) {
                    size_t room = sizeof(a->action_buf) - a->action_len - 1;
                    size_t take = (size_t)n < room ? (size_t)n : room;
                    if (take > 0) {
                        memcpy(a->action_buf + a->action_len, tmp, take);
                        a->action_len += take;
                    }
                    continue;
                }
                if (n == 0) {
                    finish_action(a);
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                finish_action(a);
                break;
            }
            if (a->action_fd >= 0 &&
                (pfd[action_idx].revents & POLLHUP))
                finish_action(a);
        }
    }

    if (ctl_idx >= 0 && (pfd[ctl_idx].revents & POLLIN))
        ctl_accept(a);
    if (ctl_client_idx >= 0 && pfd[ctl_client_idx].revents)
        ctl_read_client(a);
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_signal_quit = 1;
}

static void app_cleanup(App *a)
{
    ctl_cleanup(a);
    cancel_action(a);
    if (a->net_cancel) {
        g_cancellable_cancel(a->net_cancel);
        g_clear_object(&a->net_cancel);
    }
    if (a->nm) {
        g_signal_handlers_disconnect_by_data(a->nm, a);
        g_clear_object(&a->nm);
    }
    if (a->base) {
        cairo_surface_destroy(a->base);
        a->base = NULL;
        a->base_valid = 0;
    }
    if (a->keyboard)
        wl_keyboard_destroy(a->keyboard);
    if (a->cursor_shape_device) {
        wp_cursor_shape_device_v1_destroy(a->cursor_shape_device);
        a->cursor_shape_device = NULL;
    }
    if (a->pointer)
        wl_pointer_destroy(a->pointer);
    if (a->cursor_shape_manager)
        wp_cursor_shape_manager_v1_destroy(a->cursor_shape_manager);
    if (a->decoration)
        zxdg_toplevel_decoration_v1_destroy(a->decoration);
    if (a->toplevel)
        xdg_toplevel_destroy(a->toplevel);
    if (a->xdg_surface)
        xdg_surface_destroy(a->xdg_surface);
    if (a->surface)
        wl_surface_destroy(a->surface);
    if (a->seat)
        wl_seat_destroy(a->seat);
    if (a->deco_manager)
        zxdg_decoration_manager_v1_destroy(a->deco_manager);
    if (a->icon_manager)
        xdg_toplevel_icon_manager_v1_destroy(a->icon_manager);
    if (a->wm_base)
        xdg_wm_base_destroy(a->wm_base);
    if (a->shm)
        wl_shm_destroy(a->shm);
    if (a->compositor)
        wl_compositor_destroy(a->compositor);

    for (int i = 0; i < a->output_count; i++) {
        if (a->outputs[i].output)
            wl_output_destroy(a->outputs[i].output);
    }
    if (a->registry)
        wl_registry_destroy(a->registry);

    if (a->xkb_state)
        xkb_state_unref(a->xkb_state);
    if (a->xkb_keymap)
        xkb_keymap_unref(a->xkb_keymap);
    if (a->xkb_ctx)
        xkb_context_unref(a->xkb_ctx);

    if (a->display) {
        for (int i = 0; i < NBUFS; i++) {
            ShmBuf *b = &a->bufs[i];
            if (b->buffer && !b->busy) {
                wl_buffer_destroy(b->buffer);
                b->buffer = NULL;
            }
        }
        wl_display_flush(a->display);
        wl_display_disconnect(a->display);
        a->display = NULL;
    }

    for (int i = 0; i < NBUFS; i++) {
        ShmBuf *b = &a->bufs[i];
        if (b->data) {
            munmap(b->data, b->size);
            b->data = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Preview / screenshot mode                                          */
/* ------------------------------------------------------------------ */

static void seed_demo(Ui *u)
{
    static const WifiNet demo_nets[4] = {
        { .ssid = "JTLab-5G", .signal = 92, .security = "WPA3",
          .connected = 1 },
        { .ssid = "JTLab", .signal = 78, .security = "WPA2",
          .connected = 0 },
        { .ssid = "CoffeeHouse", .signal = 54, .security = "Open",
          .connected = 0 },
        { .ssid = "Neighbour", .signal = 31, .security = "WPA2",
          .connected = 0 },
    };
    static const BtDev demo_bt[3] = {
        { .name = "WH-1000XM4", .kind = DEV_HEADPHONES, .connected = 1 },
        { .name = "MX Master 3", .kind = DEV_MOUSE, .connected = 0 },
        { .name = "Pixel Buds Pro", .kind = DEV_EARBUDS, .connected = 0 },
    };

    if (u->wifi_on) {
        for (int i = 0; i < 4 && u->net_count < 4; i++) {
            int dup = 0;
            for (int j = 0; j < u->net_count; j++)
                if (!strcmp(u->nets[j].ssid, demo_nets[i].ssid))
                    dup = 1;
            if (!dup)
                u->nets[u->net_count++] = demo_nets[i];
        }
        if (u->net_count > 0 && !u->wifi_connected) {
            u->wifi_connected = 1;
            u->signal = u->nets[0].signal;
            snprintf(u->ssid, sizeof u->ssid, "%s", u->nets[0].ssid);
            snprintf(u->security, sizeof u->security, "%s",
                     u->nets[0].security);
        }
    }
    if (u->wifi_on && u->wifi_connected && !u->ip[0])
        snprintf(u->ip, sizeof u->ip, "192.168.1.42");

    if (u->bt_on && u->bt_count == 0) {
        memcpy(u->bt, demo_bt, sizeof demo_bt);
        u->bt_count = 3;
        u->bt_paired_total = 3;
    }
    if (u->bt_on && u->bt_count > 0 && u->bt_paired_total < u->bt_count)
        u->bt_paired_total = u->bt_count;
    if (u->bt_on && !u->bt_name[0])
        snprintf(u->bt_name, sizeof u->bt_name, "BlueZ 5.87");
}

static int write_screenshot(const char *path, const char *tab_name, int off)
{
    Ui u;
    memset(&u, 0, sizeof u);
    u.tab = (tab_name && (!strcmp(tab_name, "bluetooth") ||
                          !strcmp(tab_name, "bt"))) ? TAB_BLUETOOTH : TAB_WIFI;
    u.tab_mix = u.tab == TAB_BLUETOOTH ? 1.0 : 0.0;
    u.wifi_on = 1;
    u.bt_on = 1;
    u.wifi_k = 1.0;
    u.bt_k = 1.0;
    u.hover_nav = -1;
    u.hover_row = -1;
    u.scan_t = -10.0;
    u.pair_t = -10.0;
    u.t = 2.35;
    u.deco_t = 2.35;

    NMClient *nm = nm_client_new(NULL, NULL);
    sys_refresh(&u, nm);
    if (nm)
        g_object_unref(nm);
    u.wifi_on = 1;
    u.bt_on = 1;
    u.wifi_k = 1.0;
    u.bt_k = 1.0;
    if (off) {
        if (u.tab == TAB_WIFI) {
            u.wifi_on = 0;
            u.wifi_k = 0.0;
        } else {
            u.bt_on = 0;
            u.bt_k = 0.0;
        }
    } else {
        seed_demo(&u);
    }

    cairo_surface_t *surf = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, (int)WIN_W, (int)WIN_H);
    cairo_t *cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    rgba(cr, 0x1A1A1C, 1.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    paint_content(cr, &u);
    cairo_destroy(cr);

    cairo_status_t st = cairo_surface_write_to_png(surf, path);
    cairo_surface_destroy(surf);
    if (st != CAIRO_STATUS_SUCCESS) {
        fprintf(stderr, "jtnetworks: could not write %s: %s\n",
                path, cairo_status_to_string(st));
        return 1;
    }
    printf("wrote %s\n", path);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main loop                                                          */
/* ------------------------------------------------------------------ */

static void ui_step(Ui *u, double dt)
{
    u->wifi_k = approach(u->wifi_k, u->wifi_on ? 1.0 : 0.0, dt, 14.0);
    u->bt_k = approach(u->bt_k, u->bt_on ? 1.0 : 0.0, dt, 14.0);
    u->tab_mix = approach(u->tab_mix,
                          u->tab == TAB_BLUETOOTH ? 1.0 : 0.0, dt, 16.0);
    u->wifi_k = clampd(u->wifi_k, 0.0, 1.0);
    u->bt_k = clampd(u->bt_k, 0.0, 1.0);
    u->tab_mix = clampd(u->tab_mix, 0.0, 1.0);
    if (fabs(u->wifi_k - (u->wifi_on ? 1.0 : 0.0)) < 0.001)
        u->wifi_k = u->wifi_on ? 1.0 : 0.0;
    if (fabs(u->bt_k - (u->bt_on ? 1.0 : 0.0)) < 0.001)
        u->bt_k = u->bt_on ? 1.0 : 0.0;
    if (fabs(u->tab_mix - (u->tab == TAB_BLUETOOTH ? 1.0 : 0.0)) < 0.001)
        u->tab_mix = u->tab == TAB_BLUETOOTH ? 1.0 : 0.0;
}

int main(int argc, char **argv)
{
    if (!accent_cli(&argc, argv))
        return 2;

    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: %s [ACCENT] [--screenshot PATH [wifi|bluetooth] "
               "[on|off]]\n\n"
               "  ACCENT              #RRGGBB or RRGGBB, for example '#748382'\n"
               "  --accent COLOR      explicit form of ACCENT\n"
               "  --screenshot PATH   render a preview frame to PATH and exit\n"
               "                     optional tab: wifi (default) or bluetooth\n"
               "                     optional state: on (default) or off\n\n"
               "the default accent is #E8A03C (amber)\n\n"
               "controls: Tab/1/2 switch, Space/Enter toggle, S scan, Esc/q quit\n"
               "          click a Wi-Fi row to connect; type password, Enter connects\n"
               "          saved rows: click to activate, buttons disconnect/forget\n"
               "          Bluetooth rows: connect, disconnect, forget\n"
               "live accent: jtnetworksctl accent '#RRGGBB'\n"
               "requires a Wayland compositor supporting xdg-shell\n", argv[0]);
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "--screenshot")) {
        if (argc < 3) {
            fprintf(stderr, "usage: %s --screenshot PATH [wifi|bluetooth] "
                            "[on|off]\n", argv[0]);
            return 2;
        }
        const char *tab = argc >= 4 ? argv[3] : NULL;
        int off = argc >= 5 && !strcmp(argv[4], "off");
        return write_screenshot(argv[2], tab, off);
    }

    App a;
    memset(&a, 0, sizeof a);
    a.scale = 1;
    a.action_fd = -1;
    a.action_pid = -1;
    a.ctl_fd = -1;
    a.ctl_client_fd = -1;
    a.net_cancel = g_cancellable_new();
    a.ui.tab = TAB_WIFI;
    a.ui.wifi_on = 1;
    a.ui.bt_on = 1;
    a.ui.wifi_k = 1.0;
    a.ui.bt_k = 1.0;
    a.ui.tab_mix = 0.0;
    a.ui.deco_t = 2.35;
    a.ui.hover_nav = -1;
    a.ui.hover_row = -1;
    a.ui.hover_row_action_idx = -1;
    a.ui.wifi_action_row = -1;
    a.ui.bt_action_row = -1;
    a.ui.allow_mutation = 1;
    a.ui.connect_idx = -1;
    a.ui.scan_t = -10.0;
    a.ui.pair_t = -10.0;

    const char *scale_env = getenv("JTNETWORKS_SCALE");
    if (scale_env && *scale_env) {
        int s = (int)lround(atof(scale_env));
        if (s < 1) s = 1;
        if (s > 3) s = 3;
        a.scale = s;
        a.fixed_scale = 1;
    }

    a.xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!a.xkb_ctx) {
        fprintf(stderr, "jtnetworks: cannot create xkb context\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    GError *nm_err = NULL;
    a.nm = nm_client_new(NULL, &nm_err);
    if (!a.nm) {
        fprintf(stderr, "jtnetworks: NetworkManager unavailable: %s\n",
                nm_err ? nm_err->message : "unknown error");
        g_clear_error(&nm_err);
    } else {
        g_signal_connect(a.nm, "notify::wireless-enabled",
                         G_CALLBACK(nm_prop_cb), &a);
        g_signal_connect(a.nm, "notify::active-connections",
                         G_CALLBACK(nm_prop_cb), &a);
        g_signal_connect(a.nm, "notify::devices",
                         G_CALLBACK(nm_prop_cb), &a);
        g_signal_connect(a.nm, "connection-added",
                         G_CALLBACK(nm_changed_cb), &a);
        g_signal_connect(a.nm, "connection-removed",
                         G_CALLBACK(nm_changed_cb), &a);
        g_signal_connect(a.nm, "device-added",
                         G_CALLBACK(nm_changed_cb), &a);
        g_signal_connect(a.nm, "device-removed",
                         G_CALLBACK(nm_changed_cb), &a);
        g_signal_connect(a.nm, "active-connection-added",
                         G_CALLBACK(nm_changed_cb), &a);
        g_signal_connect(a.nm, "active-connection-removed",
                         G_CALLBACK(nm_changed_cb), &a);
    }

    sys_refresh(&a.ui, a.nm);
    a.ui.wifi_k = a.ui.wifi_on ? 1.0 : 0.0;
    a.ui.bt_k = a.ui.bt_on ? 1.0 : 0.0;
    request_wifi_scan(&a);

    if (!wayland_start(&a)) {
        app_cleanup(&a);
        return 1;
    }

    ctl_start(&a);

    a.t0 = mono_sec();
    a.last_draw = 0.0;
    mark_dirty(&a);
    double last = a.t0;
    int prev_animating = 0;

    while (!a.quit && !g_signal_quit) {
        double now = mono_sec();
        double dt = now - last;
        if (dt > 0.12) dt = 0.12;
        if (dt < 0.0) dt = 0.0;
        last = now;

        a.ui.t = now - a.t0;
        ui_step(&a.ui, dt);

        if (a.ui.wifi_msg[0] && a.ui.t - a.ui.wifi_msg_t >= 4.0) {
            a.ui.wifi_msg[0] = 0;
            mark_dirty(&a);
        }
        if (a.ui.bt_msg[0] && a.ui.t - a.ui.bt_msg_t >= 4.0) {
            a.ui.bt_msg[0] = 0;
            mark_dirty(&a);
        }

        if (a.resync_at > 0.0 && now >= a.resync_at) {
            sys_refresh(&a.ui, a.nm);
            update_hover(&a);
            update_cursor(&a);
            if (a.ui.wifi_on)
                request_wifi_scan(&a);
            a.resync_at = 0.0;
            mark_dirty(&a);
        }

        int k_animating =
            fabs(a.ui.wifi_k - (a.ui.wifi_on ? 1.0 : 0.0)) > 0.003 ||
            fabs(a.ui.bt_k - (a.ui.bt_on ? 1.0 : 0.0)) > 0.003;
        int tab_animating =
            fabs(a.ui.tab_mix -
                 (a.ui.tab == TAB_BLUETOOTH ? 1.0 : 0.0)) > 0.003;
        int spinner_animating =
            (a.ui.t - a.ui.scan_t >= 0.0 && a.ui.t - a.ui.scan_t < 2.0) ||
            (a.ui.t - a.ui.pair_t >= 0.0 && a.ui.t - a.ui.pair_t < 2.0);
        int caret_animating =
            a.ui.connect_open && a.ui.connect_focus &&
            a.ui.connect_secure && !a.ui.connect_busy &&
            a.keyboard_focused;
        int animating = k_animating || tab_animating || spinner_animating;
        int use_cache = !(k_animating || tab_animating);

        if (prev_animating && !animating)
            mark_dirty(&a);
        prev_animating = animating;

        /* Ambient decoration always moves.  Tab crossfades run at
         * TAB_FPS; other transitions and idle ambient motion are slower. */
        a.ui.deco_t += dt;

        double interval;
        if (tab_animating)
            interval = 1.0 / TAB_FPS;
        else if (k_animating || caret_animating)
            interval = 1.0 / 30.0;
        else if (spinner_animating)
            interval = 1.0 / 15.0;
        else
            interval = 1.0 / AMBIENT_FPS;

        int ambient = a.configured && !a.ui.connect_open;
        int need_draw = a.dirty || animating || ambient || caret_animating;
        if (need_draw &&
            (a.last_draw == 0.0 || now - a.last_draw >= interval)) {
            if (app_draw(&a, use_cache)) {
                a.dirty = 0;
                a.last_draw = now;
            }
        }

        int timeout = 200;
        if (!a.configured) {
            timeout = 100;
        } else if (need_draw) {
            double wait = interval - (mono_sec() - a.last_draw);
            if (wait < 0.0) wait = 0.0;
            timeout = (int)(wait * 1000.0) + 1;
        }
        if (a.resync_at > 0.0) {
            double wait = a.resync_at - mono_sec();
            if (wait < 0.0) wait = 0.0;
            int rt = (int)(wait * 1000.0) + 1;
            if (rt < timeout) timeout = rt;
        }

        if (pump_events(&a, timeout) < 0)
            break;
    }

    app_cleanup(&a);
    return 0;
}

