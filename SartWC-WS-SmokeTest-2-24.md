# SartWC Workspace Smoke Test (2-24)

Post-login smoke test for:
- `sartwc` live workspaces (add/remove/rename)
- `ferritebar` workspace display/update behavior
- `intentile` workspace + placement flow
- startup reset policy (`start at 1`)

## Assumptions

- You are logged into a `sartwc` session.
- `WAYLAND_DISPLAY` and `XDG_RUNTIME_DIR` are set (normal in-session).
- `intentile` and `ferritebar` are installed/built and runnable from your shell.

## 0. One-time helper setup (terminal)

```bash
export SARTWC_IPC_SOCKET="${SARTWC_IPC_SOCKET:-$XDG_RUNTIME_DIR/sartwc-$WAYLAND_DISPLAY.sock}"
echo "$SARTWC_IPC_SOCKET"
test -S "$SARTWC_IPC_SOCKET" && echo "SartWC IPC socket OK" || echo "Missing SartWC IPC socket"
```

Pick one IPC helper (use whichever exists):

```bash
command -v socat && alias sartwc-ipc='socat - UNIX-CONNECT:$SARTWC_IPC_SOCKET'
```

```bash
command -v nc && alias sartwc-ipc='nc -U $SARTWC_IPC_SOCKET'
```

Sanity check:

```bash
printf 'ping\n' | sartwc-ipc
```

Expected:
- `OK`

## 1. Startup policy check (most important)

Immediately after login, confirm SartWC starts at exactly 1 workspace.

```bash
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- JSON response with exactly one workspace
- `current_workspace` should be `1`
- workspace list should contain only index `1`

Optional text view:

```bash
printf 'list-workspaces\n' | sartwc-ipc
```

Expected:
- one `workspace ...` line only

## 2. Live workspace mutation via SartWC IPC (no intentile daemon required)

### Add workspaces

```bash
printf 'workspace-add\n' | sartwc-ipc
printf 'workspace-add name=code\n' | sartwc-ipc
printf 'workspace-add name=web%20docs\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- each `workspace-add` returns `OK`
- workspace list grows to 4 total
- names include `1`, auto-generated next index name, `code`, `web docs`

### Rename a workspace

```bash
printf 'workspace-rename index=2 name=term\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- `OK`
- workspace `2` name is now `term`

### Remove a workspace

```bash
printf 'workspace-remove index=4\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- `OK`
- workspace count drops by 1

### Guardrail: cannot remove last remaining workspace

Run this only after trimming back to one workspace.

```bash
printf 'workspace-remove index=1\n' | sartwc-ipc
```

Expected:
- `ERROR ...`

## 3. Ferritebar workspace behavior (prefer ext-workspace, fallback to SartWC IPC)

Start ferritebar (or restart it) in the same session.

```bash
ferritebar
```

If launching from a terminal and you want logs:

```bash
RUST_LOG=info ferritebar
```

Then, in another terminal, mutate workspaces:

```bash
printf 'workspace-add name=mail\n' | sartwc-ipc
printf 'workspace-rename index=2 name=dev\n' | sartwc-ipc
printf 'GoToDesktop to=2\n' | sartwc-ipc
printf 'workspace-remove index=3\n' | sartwc-ipc
```

Expected in bar UI:
- workspace buttons appear/disappear as count changes
- label updates on rename
- active workspace highlight follows `GoToDesktop`
- no “stuck” workspace list after add/remove

## 4. Intentile daemon basics (optional for workspace mutation, required for tiling flow)

You do not need the intentile daemon for direct SartWC workspace IPC, but you do need it for `intentile` tiling features.

Check status (auto-start behavior may vary by command):

```bash
intentile status
```

Start daemon explicitly if desired:

```bash
intentile daemon
```

(Run that in a dedicated terminal, or background it your usual way.)

## 5. Intentile workspace commands (live SartWC IPC through intentile)

These now go through the intentile daemon -> SartWC IPC (not `rc.xml` editing).

```bash
intentile workspace add code
intentile workspace add "web docs"
intentile workspace rename 2 term
intentile workspace remove 3
intentile status
```

Confirm in compositor:

```bash
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- changes are reflected immediately
- ferritebar updates live

## 6. Intentile reconcile + event updates

```bash
intentile reconcile
intentile status
```

Expected:
- no error
- occupancy state repopulates from current tiled views (if any)

## 7. Intentile atomic placement overflow auto-add

Goal: verify overflow can create new workspace(s) automatically when needed.

Prepare:
- Have one or more windows to tile
- Be on workspace 1

Example sequence (manual):

```bash
intentile 1
intentile 2
intentile 3
intentile 4
intentile 5
```

If placement overflows past current max workspace count, intentile should create new workspaces via SartWC IPC automatically.

Confirm:

```bash
printf 'list-workspaces-json\n' | sartwc-ipc
intentile status
```

Expected:
- workspace count may increase
- no `rc.xml` edits/reconfigure needed

## 8. Reconfigure behavior (in-session)

This tests that a live `sartwc -r` does not force workspaces back to `rc.xml` if persisted state exists.

Mutate workspaces first:

```bash
printf 'workspace-add name=testreconf\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Then reconfigure:

```bash
sartwc -r
```

Recheck:

```bash
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- workspace list remains what it was before reconfigure (session persistence honored)

## 9. Restart/login reset behavior (startup should return to 1)

This is the policy check you care about.

Before logout/restart compositor, create several workspaces:

```bash
printf 'workspace-add name=tmp1\n' | sartwc-ipc
printf 'workspace-add name=tmp2\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Then:
- log out, or
- restart the compositor/session

After logging back into `sartwc`, run:

```bash
export SARTWC_IPC_SOCKET="${SARTWC_IPC_SOCKET:-$XDG_RUNTIME_DIR/sartwc-$WAYLAND_DISPLAY.sock}"
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- exactly 1 workspace again (`index=1`)
- previous session's expanded list should not survive startup

## 10. Failure cases worth checking

### Bad rename input

```bash
printf 'workspace-rename index=999 name=nope\n' | sartwc-ipc
```

Expected:
- `ERROR ...`

### Bad remove input

```bash
printf 'workspace-remove index=999\n' | sartwc-ipc
```

Expected:
- `ERROR ...`

### Percent-encoding handling

```bash
printf 'workspace-add name=foo%20bar%20baz\n' | sartwc-ipc
printf 'list-workspaces-json\n' | sartwc-ipc
```

Expected:
- workspace name appears as `foo bar baz`

## Notes

- Direct SartWC IPC testing isolates compositor behavior from intentile daemon behavior.
- `intentile` workspace commands are now a convenience/control layer over SartWC live IPC.
- `rc.xml` no longer needs live editing for workspace count/name iteration.
- Startup policy is intentionally: fresh session starts with exactly one workspace.
