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
        in
        {
          default = pkgs.stdenv.mkDerivation {
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

            strictDeps = true;

            meta = {
              description = "Stacking and reasonable tiling Wayland compositor (labwc fork with IPC)";
              license = [ pkgs.lib.licenses.gpl2Plus ];
              mainProgram = "sartwc";
              platforms = pkgs.wayland.meta.platforms;
            };
          };
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
