# SartWC

**Stacking And Reasonable Tiling Window Compositor**

A fork of [labwc](https://github.com/labwc/labwc) with a Unix socket IPC interface for programmatic window control.

Also a play on Sartre — because other people's window managers are hell.

## What This Is

labwc is a wlroots-based Wayland stacking compositor inspired by Openbox. It's lightweight (~57 MB runtime), fast, and deliberately boring. The upstream project's philosophy explicitly rejects non-Wayland-protocol IPC.

SartWC adds one thing: a text-based IPC socket that lets external tools send actions and query compositor state. This enables tools like [intentile](https://github.com/your/intentile) to place windows, manage workspaces, and read window geometry without simulating keyboard input through wtype.

## What Changed From Upstream

**New files:**
- `src/ipc.c` — ~290 lines. Unix socket server on the Wayland event loop.
- `include/ipc.h` — Header.

**Modified files:**
- `include/labwc.h` — Two fields added to `struct server` (socket fd + event source).
- `src/server.c` — Three lines: include, `ipc_init()`, `ipc_finish()`.
- `src/meson.build` — One line: `ipc.c` added to source list.
- `src/main.c` — Env vars renamed to `SARTWC_*`.
- `meson.build` — Project name and version.
- `data/labwc.desktop` — Session entry renamed.

Everything else is unmodified labwc 0.9.3.

## IPC Protocol

Socket at `$XDG_RUNTIME_DIR/sartwc-$WAYLAND_DISPLAY.sock`. Text protocol, newline-delimited.

**Actions** — same names as rc.xml, case-insensitive:
```
echo "MoveTo x=0 y=0" | socat - UNIX:$SARTWC_IPC_SOCKET
echo "ResizeTo width=50% height=100%" | socat - UNIX:$SARTWC_IPC_SOCKET
echo "GoToDesktop to=right wrap=yes" | socat - UNIX:$SARTWC_IPC_SOCKET
echo "SendToDesktop to=3 follow=no" | socat - UNIX:$SARTWC_IPC_SOCKET
echo "Close" | socat - UNIX:$SARTWC_IPC_SOCKET
```

**Queries:**
```
echo "list-views" | socat - UNIX:$SARTWC_IPC_SOCKET
# Returns: app_id, title, workspace, x/y/w/h, state flags per window

echo "list-workspaces" | socat - UNIX:$SARTWC_IPC_SOCKET
# Returns: workspace list with index, name, active flag
```

Full protocol docs: see `intentile/docs/SARTWC-IPC.md` in the intentile repo.

## Building

```bash
nix build   # 7 seconds from clean
```

Or with meson directly:
```bash
meson setup build
ninja -C build
```

## Configuration

SartWC reads the same `~/.config/labwc/` config files as labwc (rc.xml, menu.xml, autostart, etc.). It is a drop-in replacement.

## Environment Variables

| Variable | Description |
|----------|-------------|
| `SARTWC_IPC_SOCKET` | Path to IPC socket |
| `SARTWC_PID` | Compositor PID |
| `SARTWC_VER` | Version string |

## License

GPL-2.0-only, same as labwc.

## Upstream

All credit for the compositor itself goes to the [labwc project](https://github.com/labwc/labwc). The IPC patch is the only addition.
