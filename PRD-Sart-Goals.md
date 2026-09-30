# Sart parity contract for singularity-labwc

This document is the behavioral contract for bringing sartwc's intended window-placement experience into this LabWC fork. Read it before `sart-parity-TASKBOARD.md`. The source decisions are `~/repos/sartwc/GOAL.md`, `KEYMAP.md`, and the accepted human smoke notes; this document is the fork-local handoff, not a request to transplant River client code.

## Outcome and boundaries

Keep LabWC's existing appearance, theme parser, decorations, `rc.xml` bindings, and ordinary stacking behavior. Add the smallest in-compositor placement and input state needed to send one focused window to an exact zone of the **next existing workspace**. A command may move without following or move and follow. If placement is invalid, the window, workspace state, register occupation, geometry, and focus stay as they were.

The user's observed idle memory is about 35–50 MB for LabWC versus about 200 MB for River plus sartwc. Preserve the fork's single-compositor advantage: no persistent companion daemon is required for the core behavior. An optional short-lived CLI or protocol client is acceptable for IPC. Do not introduce a second theme/keybinding config; the existing LabWC theme and `rc.xml` remain authoritative.

The current fork has `SendToDesktop` with `follow`, `SnapToRegion`, per-output workspace support, and `zsingularity_tiling_manager_v1`. None is an atomic, collision-aware placement operation. In particular, a client issuing separate `move_to_workspace` and `set_geometry` requests does **not** satisfy this contract. Implement one shared placement decision inside the compositor, reachable from keyboard actions and external commands.

## Placement model

- Destination is the workspace immediately after the window's current workspace in configured order. It must already exist. Never create, skip, overflow, or wrap to another workspace. The current workspace and focused window are the anchors, not whichever output happens to hold the pointer.
- V1 targets LabWC's normal shared/global workspace ordering (`<desktops><perOutput>no</perOutput>`). If `perOutput=yes`, reject placement explicitly until its destination/output semantics are defined and tested; do not silently choose a different workspace group.
- A destination may hold any combination of halves, thirds, and quarters whose actual rectangles do not overlap. Placement never locks a workspace to a single shape. A newly empty workspace has no residual placement restriction.
- Each occupied register belongs to a live window on that workspace. Replacing/moving/closing that window updates occupation; stale slots must not remain. Register geometry is derived from the usable output area, not the full monitor rectangle. Divide with integer boundaries so same-shape regions cover the area without gaps or overlaps, including odd sizes; shared edges are legal.
- Preflight the requested destination, occupied register, actual rectangle overlap, and known client size constraints before mutating membership, geometry, occupation, or focus. A missing workspace, occupied slot, overlapping slot, or unfit dimensions rejects **at that destination**. No eviction, stacking into an occupied slot, fallback slot, or partial move.
- Wayland clients may report a constrained size asynchronously. Treat that response as part of placement completion: a size that cannot fit the registered zone must not leave a falsely successful occupation or a window stranded in the destination. The first implementation must prove an achievable rollback or delayed-commit path. Do not promise pixel-atomic rendering across client round trips.
- Following is a separate boolean (`false` by default), never encoded in the register. On success, no-follow keeps the original visible workspace and repairs focus there; follow switches to the destination and focuses the moved window. On failure, neither mode changes focus. A visible error is optional; diagnostic logs should record the rejection reason.

## Canonical registers

Every keyboard sequence and external command resolves to one of these registers and calls the same validation path. Shape is inferred from the register for an atomic command.

| Shape | Input | Register | Region | Conflicts with occupied registers |
| --- | --- | ---: | --- | --- |
| 2 | `u` | 1 | left half | 1, 3, 4, 6, 8 |
| 2 | `i` | 2 | right half | 2, 4, 5, 7, 9 |
| 3 | `h` | 3 | left third | 1, 3, 6, 8 |
| 3 | `j` | 4 | middle third | 1, 2, 4, 6, 7, 8, 9 |
| 3 | `k` | 5 | right third | 2, 5, 7, 9 |
| 4 | `o` | 6 | upper-left quarter | 1, 3, 4, 6 |
| 4 | `p` | 7 | upper-right quarter | 2, 4, 5, 7 |
| 4 | `l` | 8 | lower-left quarter | 1, 3, 4, 8 |
| 4 | `;` | 9 | lower-right quarter | 2, 4, 5, 9 |

The table expresses **actual overlap**, not a blanket prohibition on the same side. Register 4 conflicts with everything except 3 and 5. Distinct quarters may coexist (`6` with `8`, `7` with `9`, and all four together). Cross-shape combinations are legal when their rectangles do not overlap: `u` can coexist with `k`, `p`, or `;`, and `i` with `h`, `o`, or `l`. An occupied register always conflicts with itself.

## Command and key behavior

- External operations must cover atomic placement by letter on the next or current workspace, with an independent `follow` flag for next-workspace placement. A read-only status command should expose workspace locks/occupants, focus, and last rejection. Exact CLI executable name and wire encoding may be chosen during implementation; retain a thin adapter for documented `intentile` calls in the user's existing `rc.xml` where necessary. Numeric registers remain internal identifiers; the nested smoke config may bind `Ctrl+Alt+1…9` to current-workspace placement as a collision-test helper.
- Every division has a unique one-key selector: `u/i` for halves, `h/j/k` for thirds, and `o/p/l/;` for quarters. The three operations—place on the next workspace, place there and follow, and place on the current workspace—share this register map. `arm`, if retained for compatibility with existing `intentile` calls, persists until successful placement, rearm, or clear; failed placement does not consume it.
- Bind `Ctrl+Alt+letter` for next-workspace placement, `Ctrl+Mod4+letter` for next-workspace placement with follow, and `Alt+letter` for current-workspace placement. Preserve LabWC's `A` and `W` modifier aliases. Do not steal unconditional letter keys from applications. Surface conflicts with the user's existing `rc.xml`; do not silently replace bindings.
- The unique letter routes supersede the earlier Alt-M-P and two-stroke quarter proposals. No letter capture or timing window is needed for register selection.
- Prefer direct `rc.xml` actions and in-compositor dispatch for bindings. External clients may invoke the same operation via an extension of the fork's protocol or a small socket, but command acceptance must have a distinguishable success/rejection result. Existing standalone `SendToDesktop` and `SnapToRegion` remain intact; chaining them is not the implementation.

## Acceptance and non-goals

Automated checks cover all nine register mappings, symmetric collision cases including legal cross-shape pairs, failed-destination invariants, odd-sized usable areas, client-size rejection/rollback, and follow/no-follow focus. Run a full NixOS-reachable build and a nested LabWC smoke; compilation alone is not acceptance. Give the human one command to launch the candidate, record logs for input/placement decisions, and wait for visual/input feedback.

V1 does **not** require automatic workspace creation, wraparound, overflow routing, per-output placement, a persistent shell service, new theme formats, or River compatibility. Those are separate choices, not hidden defaults.

## Follow-up: same-workspace placement

After accepting the group 1 smoke, John requested a same-workspace placement route. The first `Ctrl+Alt+Space`, then number, key capture disrupted other shortcuts and did not work in his smoke. Direct `Ctrl+Alt+A/S/D/F/G/H/J/K/L` was accepted in human smoke; John then revised the selector layout to `U/I`, `H/J/K`, `O/P/L/;` for halves, thirds, and quarters, and assigned `Alt+letter` to this route. This route uses the existing nine register geometries and collision check. It permits mixed shapes when their actual rectangles do not overlap, including `3+7+9` and `1+5`; `1+4` is rejected for overlap. A later smoke showed that the original next-workspace shape lock also rejected nonoverlapping mixed pairs; John requested the same rectangle-only rule for next-workspace placement.
