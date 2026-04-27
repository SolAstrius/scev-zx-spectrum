{
  description = "ZX Spectrum 48K core for RVVM bare-metal — riscv64 freestanding, zig-cc toolchain";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { nixpkgs, flake-utils, ... }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Runtime libraries RVVM dlopens when launched from `make run`.
        # Mirrors RVVM's own flake — without these on LD_LIBRARY_PATH,
        # libasound.so isn't found and the HDA backend falls back to
        # silent. ALSA_PLUGIN_DIR points at PipeWire's alsa-lib bridge,
        # since on NixOS /etc/alsa/conf.d routes default → pcm.pipewire
        # but alsa-lib only finds that plugin shared object via this
        # env var.
        rvvmRuntimeDeps = with pkgs; [ alsa-lib ];
        alsaPluginDir   = "${pkgs.pipewire}/lib/alsa-lib";

      in {
        devShells.default = pkgs.mkShell {
          packages = with pkgs; [
            zig                    # cc + linker + cross targets
            qemu                   # quick smoke-test against qemu virt before RVVM
            llvmPackages.bintools  # llvm-objdump / llvm-objcopy / llvm-readelf
          ];

          LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath rvvmRuntimeDeps;
          ALSA_PLUGIN_DIR = alsaPluginDir;

          shellHook = ''
            echo "zx-spectrum core: zig $(zig version)"
            echo "target: riscv64-freestanding-none, attached as RVVM mtd-physmap firmware"
            echo "audio:  libasound at $LD_LIBRARY_PATH"
          '';
        };
      });
}
