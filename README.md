# JTLab Networks

A small native Wayland network-control panel, drawn with cairo + Pango.
It follows the JTLab / `jtspeedtest` visual language: near-black surfaces,
thin grey borders, a configurable accent (amber by default), and a calm,
low-contrast layout.

It is a Wayland client only. There is no GTK, Qt, X11, or web view.

![Wi-Fi tab](preview.png)

![Bluetooth tab](preview-bt.png)

## Features

- Two sidebar tabs: **Wi-Fi** and **Bluetooth**.
- Animated radio toggle in the header, plus per-tab off/empty states.
- Live Wi-Fi state (SSID, IP, signal, security) from NetworkManager.
- Click-to-connect Wi-Fi rows, with a password dialog for secured
  networks. The connect action runs asynchronously and reports failures
  in the dialog.
- Saved Wi-Fi profiles stay visible even when out of range, marked
  **CONNECTED BEFORE**, with per-row **DISCONNECT** and **FORGET**
  actions. Clicking a saved row activates the stored profile without
  asking for the password again.
- Live Bluetooth adapter and paired-device list from BlueZ, with
  **CONNECT**, **DISCONNECT**, and **FORGET** actions. Connected devices
  are trusted automatically.
- Asynchronous Wi-Fi scanning through libnm: the window stays responsive
  while NetworkManager scans.
- Real radio toggles through libnm and `bluetoothctl`, with an `rfkill`
  fallback for Bluetooth.
- Keyboard controls and a draggable header.
- Pointer-shape feedback on interactive controls via
  `wp_cursor_shape_manager_v1`.
- Screenshot mode for previews and visual regression checks.

## Requirements

Build dependencies:

- `cc`, `make`, `pkg-config`
- `wayland-client`
- `wayland-protocols` (for the XML protocol definitions)
- `wayland-scanner`
- `xkbcommon`
- `cairo`
- `pangocairo`
- `libnm` / NetworkManager development headers

Runtime dependencies are optional:

- NetworkManager for Wi-Fi state and toggles (libnm talks to it over D-Bus)
- `bluetoothctl` / BlueZ for Bluetooth state and toggles
- `rfkill` as a fallback for the Bluetooth radio toggle

Without NetworkManager or BlueZ the panel still renders, but the affected
tab will show its empty/off state. The Wi-Fi backend does not shell out to
`nmcli`; it uses libnm directly.

## Build and run

```sh
cd ~/jtnetworks
make
./jtnetworks
./jtnetworks '#748382'   # override the amber accent
```

The accent can be given as a positional `#RRGGBB` or `RRGGBB` value, or with
`--accent COLOR`. Quote the `#` in a shell. Without an accent argument the
original amber `#E8A03C` is used. The hover and pressed shades are derived from
the chosen color automatically.

The accent can also be changed while the window is running:

```sh
jtnetworksctl accent '#748382'
jtnetworksctl '#748382'      # shorthand
jtnetworksctl quit
```

JTLab uses this control socket to update the accent live when the wallpaper
changes, so a running network panel does not need to restart.

Install the binary and hicolor icon into `/usr/local`:

```sh
sudo make install
```

That installs:

- `/usr/local/bin/jtnetworks`
- `/usr/local/bin/jtnetworksctl`
- `/usr/local/share/icons/hicolor/scalable/apps/jtnetworks.svg`
- `/usr/local/share/icons/hicolor/256x256/apps/jtnetworks.png`

Uninstall with `sudo make uninstall`. Use `make install PREFIX=/usr` for a
distribution-style prefix, or set `DESTDIR` when packaging.

The client requests an `xdg-toplevel-icon-v1` icon when the compositor
supports it, and also uses the app ID `jtnetworks`. Installing the hicolor
icon gives taskbars and launchers a theme lookup fallback.

## Controls

| Input | Action |
| --- | --- |
| Click a Wi-Fi row | Open the connect dialog, or activate a saved profile |
| Click a Bluetooth row | Connect a paired device |
| Row `DISCONNECT` / `FORGET` | Disconnect or remove the saved network/device |
| Type / `Backspace` | Edit the password while the dialog is open |
| `Enter` | Connect (dialog), otherwise toggle the active radio |
| `Esc` | Cancel the dialog (dialog), otherwise quit |
| `Tab`, `Left`, `Right`, `h`, `l` | Switch tab |
| `1` / `2` | Jump to Wi-Fi / Bluetooth |
| `Space` | Toggle the active radio |
| `S` | Scan for Wi-Fi networks / animate the pair action |
| `q` | Quit |
| Drag the header | Move the window |

Click the sidebar entries to switch tabs, the header toggle to change the
active radio, the section action (`SCAN` / `PAIR NEW`), a row to connect,
the row buttons to disconnect or forget, or the `TURN ON` button in an
off state.

For a secured network, the dialog starts with an empty password field.
Enter the password and press `Enter` or click `CONNECT`. Networks that
NetworkManager remembers remain listed as saved after a restart, even if
they are currently out of range. Clicking one activates its stored profile
without asking for the password again; **FORGET** deletes that profile.

Paired Bluetooth devices can be connected from the row. A successful
connection is followed automatically by `bluetoothctl trust <MAC>`, and
any already-connected untrusted device is trusted on the next state
refresh. **FORGET** removes the pairing with `bluetoothctl remove`.

The `PAIR NEW` action is currently a visual placeholder. A Bluetooth
pairing flow is not implemented; existing paired devices are read and
displayed.

## Screenshot mode

Screenshot mode renders one frame to a PNG without opening a window. It
still reads the real system state when that state is available, and fills
in demo rows for a complete preview when the real list is sparse.

```sh
./jtnetworks --screenshot preview.png
./jtnetworks --screenshot preview.png wifi
./jtnetworks --screenshot preview-bt.png bluetooth
./jtnetworks --screenshot preview-off.png wifi off
./jtnetworks --screenshot preview-bt-off.png bluetooth off
```

The Makefile has shortcuts for the two main previews:

```sh
make preview
make preview-bt
```

## How the backend works

- Wi-Fi state comes from libnm. The list merges NetworkManager's Wi-Fi
  access points with saved Wi-Fi connections, matches them by SSID, and
  keeps saved profiles as rows even when they are out of range.
- Scans are requested with `nm_device_wifi_request_scan_async()`. The
  Wayland event loop also pumps the GLib main context, so libnm callbacks
  and D-Bus signals never block rendering.
- Connecting builds an `NMConnection` in memory and calls
  `nm_client_add_and_activate_connection_async()`. The dialog stays open
  with the error text if NetworkManager rejects the attempt.
- Saved-profile actions use libnm:
  `nm_client_activate_connection_async()`,
  `nm_client_deactivate_connection_async()`, and
  `nm_remote_connection_delete_async()`. The footer reports success or
  the NetworkManager error.
- NetworkManager signals refresh the visible state when an external
  client connects, disconnects, adds a profile, or toggles the radio.
- Idle ambient motion (dust, brand bars, orbit dots, hero pulse rings,
  and the scan radar) runs continuously at up to 30 fps. The static UI
  is rendered once into a cached surface; each frame blits that cache
  and redraws only the animated overlay. In testing this keeps idle
  cost around 10% of one core instead of repainting the whole window.
  Tab/radio transitions and changes to the static UI rebuild the cache.
- Bluetooth state, paired devices, and trust state come from
  `bluetoothctl`. Row actions run `connect`, `disconnect`, or `remove`
  directly through `execvp`.
- A successful Bluetooth connection runs `bluetoothctl trust <MAC>`.
  During a state refresh, any connected-but-untrusted device is trusted
  as well. Screenshot mode never mutates Bluetooth state.
- The Wi-Fi radio toggle uses the NetworkManager D-Bus property through
  libnm; the Bluetooth toggle prefers `bluetoothctl` and falls back to
  `rfkill`.
- After an action or toggle the panel re-reads the real state shortly
  afterwards.
- The Wi-Fi list is sorted with the connected network first, then saved
  profiles, then by signal strength.

## Layout and palette

- Window: 880 x 600 logical pixels, fixed size.
- App ID: `jtnetworks`; title: `JTLab Networks`.
- Font family: Liberation.
- Palette: background `#030303`, foreground `#FFFFFF`, border `#404040`,
  default accent `#E8A03C` (overridable at launch), success `#66B366`.
- Decoration: client-side, with a rounded outer surface.

The source is intentionally contained in `jtnetworks.c`. Protocol
bindings are generated by `wayland-scanner` through the Makefile.
