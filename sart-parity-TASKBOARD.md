# Sart parity taskboard

Read `PRD-Sart-Goals.md` first. This board tracks the **remaining work in this fork**, not a port of sartwc's Rust implementation. Check items only after the stated proof and human feedback; source inspection alone is not completion.

## Feature-slice delivery rule

John does **not** read this board as the handoff. At the end of **each** numbered feature group below, the agent must build the full compositor binary on this NixOS system, verify that the exact build can start nested from LabWC, and send John one copy-pasteable command that launches it. The message must say what changed, what to try on screen, how to exit, and where that run's logs are. Do not hand off a compile-only result, an unbuilt source command, or a multi-command setup recipe. Wait for John's report; his direct interaction is the acceptance gate, even when internal tests pass. Record his precise feedback here and fix in-scope failures before advancing. If a group is internal-only, demonstrate its behavior through the nested build or keep it explicitly unaccepted.

## 0. Prove the fork is the viable target

- [x] Record the fork revision, NixOS dependency resolution (especially `wlroots-0.20`), a full build, and the exact binary launched. Do not replace the live LabWC session.
- [x] Make a minimal nested launch with one or two test windows, an exit path, compositor stderr, and a single copy-pasteable command for John. Keep child display and runtime sockets isolated.
- [ ] Measure comparable idle resident memory for the fork and the existing River+sartwc setup using the same client/background conditions. John's observed 35–50 MB versus ~200 MB is the decision baseline, not a benchmark this board has reproduced.
- [x] **First feature proof:** from a focused window, request next-existing-workspace register 1 with no-follow. Verify that a missing next workspace fails in place and that a successful move leaves focus/visible workspace correct. This proof may use a temporary binding, but must use an internal single placement operation rather than two external protocol requests.
- [x] Show John the built candidate and ask whether the fork route should continue before expanding the placement grammar.

Human gate: John runs the one-command nested build, tries a successful register-1 no-follow placement and a missing-next-workspace rejection, and confirms the visible workspace/focus behavior. His answer decides whether group 1 begins.

### Group 0 evidence, awaiting John smoke

- Fork revision before this slice: `6fe073cb94dde9eefa2039671671139839ea5c91`. NixOS shell derived from `nixpkgs.labwc` plus Meson, Ninja, pkg-config and `wlroots_0_20`; resolved `wlroots-0.20` version `0.20.2`.
- Full build: `/tmp/singularity-labwc-build/labwc` (Meson debugoptimized, Xwayland disabled for this nested proof). The final binary started with the Wayland backend and mapped two Kitty windows; log: `/tmp/sart-parity-smoke.7tcyO7/compositor.log`.
- Nested command: `./scripts/sart-parity-smoke.sh`. It creates an isolated runtime socket and config, logs each run to the printed `/tmp/sart-parity-smoke.*/compositor.log`, and binds Ctrl+Alt+P to the internal register-1 operation. The first implementation keeps source membership during the XDG resize round trip, commits only if the response fits, and restores the original geometry on rejection.
- One sample with two Kitty windows: nested fork RSS 99,476 KiB, existing outer LabWC RSS 48,300 KiB. River+sartwc were not running, so the required comparable idle baseline is still open. Nested rendering and debug logging also differ from the outer session.
- Human placement/focus result: John reported "smoke clears" on 2026-09-27. Group 0's human gate passed; proceed to group 1. The comparable River+sartwc memory measurement remains open.

## 1. One placement engine, all entry points

- [ ] Define live register ownership; choose lifecycle hooks for window close, ordinary workspace move, and workspace removal. Keep state coherent with existing `src/workspaces.c` and `src/view.c`. Do not retain a workspace shape lock.
- [ ] Normalize registers 1–9 and letter routes into one internal request type with separate `follow`. Derive geometry from the destination's usable output area, including odd dimensions, frame extents, and layer-shell reservations.
- [ ] Validate destination existence, occupied register, **actual** rectangle overlap, and known size constraints before mutation. Record rejection reasons without requiring a popup.
- [ ] Resolve asynchronous client size constraints with tested delayed commit or rollback. Failed placement must preserve membership, geometry, lock, occupation, and focus.
- [ ] Apply move, zone, occupation and follow behavior together through one compositor operation. No-follow repairs focus in the source; follow switches only after success.
- [ ] Exercise each shape's coexistence cases, especially `6/8`, `7/9`, all four quarters, and `4` versus `3/5`. Test every failure invariant and no-wrap behavior.

Human gate: hand over the rebuilt nested compositor and ask John to place halves, thirds, and quarters; try legal mixed-shape pairs, an occupied slot, an overlap, an unfit window, and a missing next workspace; then compare move-only with move-and-follow. Nothing should visibly move on rejection.

### Group 1 evidence and feedback

- Internal `PlaceNextRegister` action accepts registers 1–9 and separate `follow`; the group 0 register-1 action remains an alias. Live owners are updated on successful placement and cleared on ordinary moves, geometry changes, unmap, and destroy. The original workspace shape lock was removed after John's mixed-shape smoke feedback.
- Integer register geometry and symmetric overlap table pass `test_placement_registers`, including odd usable areas and legal shared edges. XDG clients are resized on the source workspace and committed only after a fitting response; client minimum hints are checked before and after configure. Xwayland resize commits now use the same validation path with a bounded timeout and rollback; visual placement still needs John's smoke.
- Full Meson build with Xwayland enabled: `/tmp/singularity-labwc-full-build/labwc`; Nix shell resolved wlroots 0.20.2. A pre-existing `strtok_r` feature-macro compile failure in `src/xwayland-scale.c` was fixed to make this build reachable. Exact binary started nested and mapped two Kitty windows plus an Xterm through Xwayland; latest launcher log: `/tmp/sart-parity-smoke.64xXU4/compositor.log`.
- John reported: "1/2s and 1/3s pass, all 6-9 fail." The run log at `/tmp/sart-parity-smoke.zorzu1/compositor.log` shows register 1 into Workspace 2 and registers 3/4 into Workspace 3 committed. Every logged quarter request (6 or 9) targeted Workspace 3 from Workspace 2, then rejected because Workspace 3 was already locked to shape 3. No quarter geometry request reached Workspace 4. The nested launcher added Ctrl+Alt+G/H direct navigation to Workspaces 3/4 and prints the Workspace 3 → 4 quarter route; rejection logs include source, destination, and lock.
- John then reported "Smokes clear" on 2026-09-27. The original group 1 human gate passed. His follow-up request is a same-workspace placement route for fast collision checks. Later feedback extended the mixed-shape rule to next-workspace placement.

### Group 1 addition: direct current-workspace keys

- [x] `Ctrl+Alt+A/S/D/F/G/H/J/K/L` directly map to registers 1–9 and call the same placement engine with the current workspace as destination. Keep ordinary typing and direct next-workspace bindings intact.
- [x] Mixed shapes on this route are accepted only when their zones do not overlap. Geometry tests cover `3+7+9`, `1+5`, and overlapping `1+4`. The same rule now applies to next-workspace placement.
- [x] Full build and nested launch passed. John reported the Space prefix disrupted other shortcuts and did not work; it was removed. The replacement full Xwayland build and nested launch passed; `/tmp/sart-parity-smoke.AzGLTM/compositor.log` shows both Kitty windows and Xterm mapped with no invalid action. John reported "smoke clears" for the direct home-row keys on 2026-09-27; this addition is accepted.
- John then reported `Ctrl+Alt+G/H` still navigated workspaces and same-workspace placement did not work. Those navigation bindings exist in earlier smoke configs, not in the replacement config; the live compositor process is stock LabWC 0.20.2, so it cannot run the new action. The launcher now names its windows `Sart home-row` and prints its config path to distinguish the current child session from earlier runs. A fresh 5-second nested launch mapped all three renamed windows, and `/tmp/sart-parity-smoke.FYEujh/compositor.log` recorded `PlaceCurrentRegister` register 5 committed on Workspace 1 during that run. John subsequently reported the smoke clear.
- John revised the direct selector keys to `u/i`, `h/j/k`, and `o/p/l/;` for halves, thirds, and quarters. The first draft reused `l` for right third and lower-left quarter; John corrected thirds to `h/j/k`. All nine letters are now unique. He chose `Ctrl+Alt+letter` for next workspace, `Ctrl+Mod4+letter` for next and follow, and `Alt+letter` for current workspace. The action parser accepts letters as register arguments for all three routes. `Ctrl+Alt+1…9` remains in the nested config as a test-only current-workspace collision helper. Follow and Alt routes still need specific human confirmation.
- The outer `~/.config/labwc/rc.xml` binds `Alt+K/U/O/P/L/;`, so those current-workspace child bindings can be intercepted before the nested compositor sees them. The nested launcher keeps them for the intended production syntax; its numeric helper exercises current-workspace collision handling without those outer conflicts. Full Xwayland build, focused register test, and nested boot passed; `/tmp/sart-parity-smoke.STZacz/compositor.log` shows both Kitty windows and Xterm mapped. Alt route remains untested by nested keyboard smoke.
- John's later smoke showed next-workspace register 9 committed on Workspace 2, followed by register 3 and register 1 rejections with `destination shape lock differs` in `/tmp/sart-parity-smoke.elRBKy/compositor.log`. He explicitly wants left half `u` to coexist with right third `k`, upper-right quarter `p`, and lower-right quarter `;`, plus symmetric right-half/left-side combinations. The destination shape lock was removed; only occupied-register and rectangle-overlap checks remain. Full Xwayland rebuild and focused geometry test passed, and `/tmp/sart-parity-smoke.nnOkWu/compositor.log` shows the exact new binary started nested and mapped two Kitty windows plus Xterm. John reported "smoke clears" after this rebuild; the mixed-shape next-workspace behavior is accepted.

## 2. External contract and existing configuration

- [ ] Expose atomic placement, arm/slot/clear, separate follow, and read-only status through a compact command surface. Extend the existing custom Wayland protocol or add a small socket only as needed; do not create a resident helper daemon.
- [ ] Return machine-distinguishable success/rejection to external callers; log destination/register/reason. Keep direct keyboard actions and external commands on the same engine.
- [ ] Map supported existing `intentile` invocations from John's LabWC `rc.xml` through a narrow compatibility adapter. Keep that file and the existing theme authoritative; require no duplicate config.
- [ ] Document behavior when a window leaves a slot by an ordinary LabWC action and `perOutput=yes` rejection. Empty workspaces have no shape lock or residual placement restriction.

Human gate: hand over the rebuilt nested compositor with a session-specific command targeting only that child. John exercises atomic numbers and arm/slot/clear directly, checks status/rejection output, and confirms a failed request did not change the desktop. Do not require him to copy a socket path or run a setup script first.

## 3. Input grammar and conflict handling

- [ ] Expose the unique letter selectors `u/i`, `h/j/k`, `o/p/l/;` through all three operations: next workspace, next and follow, and current workspace. Keep one register normalization path. Retain `arm`/`clear` only where needed for compatibility with existing `intentile` calls.
- [ ] Bind direct letters in the nested smoke config: `Ctrl+Alt` for next, `Ctrl+Mod4` for next and follow, and `Alt` for current workspace. Audit existing `rc.xml` conflicts; John expects outer LabWC to consume some `Alt+letter` keys during nested smoke.
- [ ] Test modifier aliases, on-release bindings, key repeat, ordinary typing, and the fork's existing key dispatch so the new grammar does not regress LabWC behavior. No transient letter capture is planned.

Human gate: hand over the rebuilt nested compositor. John tries each shape's letters for current, next, and next-and-follow routes, plus ordinary typing. If outer LabWC consumes a binding, report that separately and let the child-session command prove placement independently.

## 4. Deliver and accept

- [ ] Full packaged Nix build, focused tests, and nested compositor smoke pass against the actual built binary. Capture compositor and placement logs; test IPC apart from key routing when outer LabWC swallows a chord.
- [ ] Hand John one launch command and a short human smoke: halves, thirds, quarters, collision rejection, size rejection, missing destination, move/follow, optional compatibility arm behavior, and unchanged normal typing.
- [ ] Record John's feedback and fix in-scope failures before checking v1 complete. Keep exceptions visible.

## Deferred

- [ ] Per-output placement semantics, optional error UI, additional theme polish, and cross-output policies. None is necessary for the v1 contract.
