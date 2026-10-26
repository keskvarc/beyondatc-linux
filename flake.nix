{
  description = "Unofficial native Linux X-Plane connector for BeyondATC Experimental";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/55bfb4d7437bd4e09ab7cba58a43bd68eb310044";

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
      package = pkgs.callPackage ./nix/package.nix { };
    in
    {
      packages.${system} = {
        default = package;
        beyondatc-native = package;
        wine = package.wineRuntime;
      };
      checks.${system}.connector = package;
      formatter.${system} = pkgs.nixfmt;
      devShells.${system}.default = pkgs.mkShell {
        packages = [
          pkgs.cmake
          pkgs.ninja
          pkgs.python3
          pkgs.git
        ];
        XPMP2_SOURCE_DIR = package.xpmp2Source;
        PYTHONPATH = "${self}/python";
        PYTHONDONTWRITEBYTECODE = "1";
      };
    };
}
