{
  description = "RVVM HAL — bare-metal RISC-V drivers for the RVVM emulator's emulated devices";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Runtime deps RVVM dlopens. Mirrors LekKit/RVVM:flake.nix
        # so consumers running `rvvm` from this devshell get audio.
        rvvmRuntimeDeps = with pkgs; [ alsa-lib ];
        alsaPluginDir   = "${pkgs.pipewire}/lib/alsa-lib";

      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            zig                       # cc + linker + cross targets
            llvmPackages.bintools     # llvm-ar / llvm-objcopy / llvm-readelf
          ];

          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath rvvmRuntimeDeps;
          ALSA_PLUGIN_DIR = alsaPluginDir;

          shellHook = ''
            echo "rvvm-hal: zig $(zig version)"
            echo "target: riscv64-freestanding-none, libhal.a static archive"
            echo "audio:  libasound at $LD_LIBRARY_PATH"
          '';
        };
      });
}
