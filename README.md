# SartWC

SartWC is John's LabWC-based Wayland compositor with explicit window placement.
It keeps LabWC's stacking behavior, `rc.xml`, Openbox-compatible theming, and
normal window management. A placement command assigns the focused window to a
half, third, or quarter of the current or next existing workspace. It rejects
occupied or overlapping rectangles without rearranging other windows.

This repository replaces an earlier River-based SartWC prototype. The old
prototype remains in local archive `~/repos/sartwc-river-2026-09-27`; it is not
part of this compositor.

## Upstream and license

SartWC is derived from two successive upstreams:

1. [LabWC](https://github.com/labwc/labwc) supplies the compositor, stacking
   model, configuration and Openbox-compatible themes. LabWC's license and
   source notices are retained.
2. [SingularityOS Lab's LabWC fork](https://github.com/singularityos-lab/labwc)
   supplies the tiling-capable compositor base used for this version. The
   source snapshot came from commit
   `6fe073cb94dde9eefa2039671671139839ea5c91` plus the SartWC placement
   changes described below. The Singularity fork's notices are retained.

SartWC's added code uses the same GPL-2.0-only license as this source tree;
see [LICENSE](LICENSE). This project is independently maintained and is not an
official release of either upstream.

## Placement

| Region | Keys |
| --- | --- |
| Halves | `u` left, `i` right |
| Thirds | `h` left, `j` middle, `k` right |
| Quarters | `o` upper-left, `p` upper-right, `l` lower-left, `;` lower-right |

The SartWC session's `rc.xml` uses these modifiers with every division key:

| Modifier | Operation |
| --- | --- |
| Ctrl+Alt | Place on the next existing workspace, stay here |
| Ctrl+Super | Place on the next existing workspace and follow |
| Alt | Place on the current workspace |

Mixed divisions are allowed wherever their rectangles do not overlap. For
example, a left half can coexist with a right third or either right quarter.
The placement code checks a client-reported size before committing its move.

The compositor actions are `PlaceNextRegister` with `register` and optional
`follow`, and `PlaceCurrentRegister` with `register`. Each accepts the letters
above; numeric registers 1–9 remain available for testing. SartWC currently
requires a next workspace to exist and does not create one automatically. The
initial NixOS session config creates four workspaces. The earlier `intentile`
workspace add/remove shortcuts are not active in this session.

## NixOS

The flake packages the compositor as `packages.x86_64-linux.default`. It uses
the same nixpkgs LabWC build inputs and wlroots version as the caller's pinned
nixpkgs; it does not pin a separate dependency set.

```sh
nix build path:/home/john/repos/sartwc
```

John's `~/repos/config` flake provides a separate **SartWC** Wayland session
using this package and `~/.config/sartwc`. The stock **LabWC** session remains
available at login. To try the packaged binary nested with three sample
windows after building, run:

```sh
SARTWC_BINARY="$(nix eval --raw path:/home/john/repos/sartwc#packages.x86_64-linux.default.outPath)/bin/labwc" ./scripts/sart-parity-smoke.sh
```

See [PRD-Sart-Goals.md](PRD-Sart-Goals.md) and
[sart-parity-TASKBOARD.md](sart-parity-TASKBOARD.md) for the placement contract
and its human smoke history.
