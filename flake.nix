{
  description = "RVVM HAL — bare-metal RISC-V drivers for the RVVM emulator's emulated devices";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            zig                       # cc + linker + cross targets, the whole toolchain
            llvmPackages.bintools     # llvm-ar / llvm-objcopy / llvm-readelf
          ];

          shellHook = ''
            echo "rvvm-hal: zig $(zig version)"
            echo "target: riscv64-freestanding-none, libhal.a static archive"
          '';
        };
      });
}
