#!/usr/bin/env bash
set -euo pipefail

binary=${SARTWC_BINARY:-/tmp/singularity-labwc-full-build/labwc}
if [[ ! -x $binary ]]; then
	printf 'Built compositor missing: %s\n' "$binary" >&2
	exit 1
fi
if [[ -z ${WAYLAND_DISPLAY:-} || -z ${XDG_RUNTIME_DIR:-} ]]; then
	printf 'Launch this from the existing Wayland session.\n' >&2
	exit 1
fi
parent_socket=$WAYLAND_DISPLAY
if [[ $parent_socket != /* ]]; then
	parent_socket=$XDG_RUNTIME_DIR/$parent_socket
fi
run_dir=$(mktemp -d /tmp/sart-parity-smoke.XXXXXX)
mkdir -p "$run_dir/runtime" "$run_dir/config"
chmod 700 "$run_dir/runtime"
cat > "$run_dir/config/rc.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<labwc_config>
  <desktops number="4" popupTime="500"><perOutput>no</perOutput></desktops>
  <keyboard>
    <keybind key="C-A-Return"><action name="Execute" command="kitty" /></keybind>
    <keybind key="C-A-n"><action name="GoToDesktop" to="right" wrap="no" /></keybind>
    <keybind key="C-A-b"><action name="GoToDesktop" to="left" wrap="no" /></keybind>
    <keybind key="C-A-Escape"><action name="Exit" /></keybind>
XML
placement_keys=(u i h j k o p l ';')
for key in "${placement_keys[@]}"; do
	cat >> "$run_dir/config/rc.xml" <<XML
    <keybind key="C-A-$key"><action name="PlaceNextRegister" register="$key" follow="no" /></keybind>
    <keybind key="C-W-$key"><action name="PlaceNextRegister" register="$key" follow="yes" /></keybind>
    <keybind key="A-$key"><action name="PlaceCurrentRegister" register="$key" /></keybind>
XML
done
for reg in {1..9}; do
	cat >> "$run_dir/config/rc.xml" <<XML
    <keybind key="C-A-$reg"><action name="PlaceCurrentRegister" register="$reg" /></keybind>
XML
done
cat >> "$run_dir/config/rc.xml" <<'XML'
  </keyboard>
</labwc_config>
XML
cat > "$run_dir/startup" <<'SH'
#!/usr/bin/env bash
kitty --title 'Sart letter-map A' &
kitty --title 'Sart letter-map B' &
xterm -fa monospace -fs 10 -title 'Sart letter-map Xwayland' &
wait
SH
chmod +x "$run_dir/startup"
printf 'Nested compositor: %s\nConfig: %s/config/rc.xml\nLogs: %s/compositor.log\n' "$binary" "$run_dir" "$run_dir"
printf 'Divisions: U/I halves; H/J/K thirds; O/P/L/; quarters.\n'
printf 'Ctrl+Alt+letter = next workspace; Ctrl+Mod4+letter = next and follow; Alt+letter = current workspace.\n'
printf 'Test helper: Ctrl+Alt+1..9 places on the current workspace for collision checks.\n'
printf 'Ctrl+Alt+N/B switches workspaces; Ctrl+Alt+Return opens Kitty; Ctrl+Alt+Escape exits.\n'
XDG_RUNTIME_DIR="$run_dir/runtime" WAYLAND_DISPLAY="$parent_socket" \
	WLR_BACKENDS=wayland WLR_WL_OUTPUTS=1 \
	"$binary" -C "$run_dir/config" -s "$run_dir/startup" -d \
	2> "$run_dir/compositor.log"
