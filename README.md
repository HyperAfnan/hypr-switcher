# hyprswitcher

A minimal Alt+Tab style window switcher overlay for the Hyprland Wayland compositor.

---

## Features
- Layer-shell overlay (wlr-layer-shell)
- Dynamic height based on number of windows
- Keyboard handling via Wayland + xkbcommon (Alt, Tab, Escape)
- Robust Hyprland IPC JSON parsing with incremental json-c tokener

- Graceful shutdown on focus loss or Escape

---

## Dependencies

You need development packages for (names vary by distro):

- wayland-client
- wayland-protocols
- wayland-scanner (runtime tool)
- cairo
- pangocairo
- json-c
- xkbcommon
- meson

Example (Arch Linux):
```sh
pacman -S wayland wayland-protocols cairo pango json-c xkbcommon meson
```

Example (Debian/Ubuntu):
```sh
apt install libwayland-dev wayland-protocols \
            libcairo2-dev libpango1.0-dev libjson-c-dev \
            libxkbcommon-dev meson ninja-build
```

Hyprland itself sets:
- `HYPRLAND_INSTANCE_SIGNATURE`
- `XDG_RUNTIME_DIR`
(Required for IPC socket path construction)

---

## Build & Install

```sh
git clone <your fork or repo> hyprswitcher
cd hyprswitcher
meson setup build
meson compile -C build
sudo meson install -C build   # installs the 'hyprswitcher' binary into prefix (default /usr/local)
```

To rebuild after changes:
```sh
meson compile -C build
```

Clean (optional):
```sh
rm -rf build
meson setup build
```

---

## Running

From a Hyprland session:
```sh
hyprswitcher
```
It should open a top overlay with the window list. Press:
- Alt+Tab: advance selection
- Keep Alt held while tapping Tab to cycle
- Release Alt: focus selected window and exit
- Escape: abort without changing focus


If you see no overlay, ensure:
- Running inside Hyprland
- `HYPRLAND_INSTANCE_SIGNATURE` is present
- Dependencies installed

---

## Hyprland Key Binding

Add to your Hyprland config (e.g. `~/.config/hypr/hyprland.conf`):

```
bind = ALT, TAB, exec, hyprswitcher
```

Explanation:
- First Alt+Tab press spawns `hyprswitcher`.
- Subsequent Tab presses (while holding Alt) are handled inside the program (not additional Hypr binds).
- Releasing Alt finalizes focus.

Optional (to avoid accidental repeats if you hold Tab):
```
bindr = ALT, TAB, exec, hyprswitcher
```
(Use `bindr` only if you want repeat behavior controlled externally—normally `bind` is sufficient.)


## Roadmap

- [ ] Add support for alt + shift + tab
- [ ] Add support for a preview for all the client
- [ ] Improve UI
