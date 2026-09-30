{
  description = "SartWC: LabWC stacking compositor with explicit placement";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system:
        f (import nixpkgs { inherit system; }));
    in {
      packages = forAllSystems (pkgs:
        let
          sartwc = pkgs.labwc.overrideAttrs (old: {
            pname = "sartwc";
            version = "0.9.5";
            src = self;
            passthru = (old.passthru or { }) // { providedSessions = [ ]; };
            meta = old.meta // {
              description = "LabWC-based compositor with explicit workspace placement";
              homepage = "https://github.com/jaycee1285/sartwc";
              mainProgram = "labwc";
            };
          });
        in {
          default = sartwc;
          sartwc = sartwc;
        });
    };
}
