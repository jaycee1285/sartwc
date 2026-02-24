{
  description = "sartwc - Stacking and reasonable tiling Wayland compositor (labwc fork with IPC)";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          sartwcPkg = pkgs.stdenv.mkDerivation {
            pname = "sartwc";
            version = "0.9.3-ipc";

            src = self;

            outputs = [ "out" "man" ];

            nativeBuildInputs = with pkgs; [
              gettext
              meson
              ninja
              pkg-config
              scdoc
              wayland-scanner
            ];

            buildInputs = with pkgs; [
              cairo
              glib
              libdrm
              libinput
              libpng
              librsvg
              libsfdo
              libxcb
              libxkbcommon
              libxml2
              pango
              wayland
              wayland-protocols
              wlroots_0_19
              xcbutilwm
              xwayland
            ];

            mesonFlags = [
              (pkgs.lib.mesonEnable "xwayland" true)
            ];

            # Older source revisions still install `labwc`; provide a stable `sartwc`
            # entrypoint so downstream consumers can reference `${pkg}/bin/sartwc`.
            postInstall = ''
              if [ -x "$out/bin/labwc" ] && [ ! -e "$out/bin/sartwc" ]; then
                ln -s labwc "$out/bin/sartwc"
              fi

              if [ -f "$out/share/wayland-sessions/labwc.desktop" ] && [ ! -f "$out/share/wayland-sessions/sartwc.desktop" ]; then
                cp "$out/share/wayland-sessions/labwc.desktop" "$out/share/wayland-sessions/sartwc.desktop"
              fi

              if [ -f "$out/share/wayland-sessions/sartwc.desktop" ]; then
                substituteInPlace "$out/share/wayland-sessions/sartwc.desktop" \
                  --replace-warn 'Exec=labwc' 'Exec=sartwc' \
                  --replace-warn 'Name=labwc' 'Name=sartwc' \
                  --replace-warn 'DesktopNames=labwc;wlroots' 'DesktopNames=sartwc;wlroots'
              fi
            '';

            strictDeps = true;

            meta = {
              description = "Stacking and reasonable tiling Wayland compositor (labwc fork with IPC)";
              license = [ pkgs.lib.licenses.gpl2Plus ];
              mainProgram = "sartwc";
              platforms = pkgs.wayland.meta.platforms;
            };
          };
        in
        {
          sartwc = sartwcPkg;
          default = sartwcPkg;
        });

      apps = forAllSystems (system:
        let
          pkg = self.packages.${system}.default;
        in
        {
          sartwc = {
            type = "app";
            program = "${pkg}/bin/sartwc";
          };
          default = self.apps.${system}.sartwc;
        });

      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.default ];
            packages = with pkgs; [
              gdb
              valgrind
              wayland-utils
              wlr-randr
            ];
          };
        });
    };
}
