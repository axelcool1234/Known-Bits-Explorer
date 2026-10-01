{
  description = "A small explorer for LLVM KnownBits transfer functions";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs, ... }:
    let
      systems = [
        "aarch64-darwin"
        "aarch64-linux"
        "x86_64-darwin"
        "x86_64-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_23;
        in
        rec {
          known-bits-explorer = pkgs.stdenv.mkDerivation {
            pname = "known-bits-explorer";
            version = "0.1.0";
            src = nixpkgs.lib.cleanSource ./.;

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.ninja
            ];
            buildInputs = [ llvm.llvm ];
            cmakeFlags = [ "-DLLVM_DIR=${llvm.llvm.dev}/lib/cmake/llvm" ];

            doCheck = true;
            checkPhase = ''
              runHook preCheck
              ctest --output-on-failure
              runHook postCheck
            '';
          };

          default = known-bits-explorer;
        }
      );

      apps = forAllSystems (system: {
        default = {
          type = "app";
          program = "${self.packages.${system}.default}/bin/known-bits-explorer";
          meta.description = "Explore LLVM KnownBits transfer functions";
        };
      });

      checks = forAllSystems (system: {
        build-and-test = self.packages.${system}.default;
      });

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs { inherit system; };
          llvm = pkgs.llvmPackages_23;
        in
        {
          default = pkgs.mkShell {
            packages = [
              pkgs.cmake
              pkgs.ninja
              llvm.clang
              llvm.llvm
            ];
            LLVM_DIR = "${llvm.llvm.dev}/lib/cmake/llvm";
          };
        }
      );
    };
}
