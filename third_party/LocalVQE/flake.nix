{
  description = "LocalVQE — CPU inference build environment (cmake + gcc + libsndfile)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = import nixpkgs { inherit system; };
    in
    {
      devShells.${system} = {
        default = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
            pkgs.libsndfile
            # Optional: used when building with -DLOCALVQE_VULKAN=ON
            pkgs.vulkan-loader
            pkgs.vulkan-headers
            pkgs.shaderc
          ];
        };

        # Fuzzing shell: clang with libFuzzer + ASan/UBSan. Use with
        #   nix develop .#fuzz
        # then configure with -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
        # -DLOCALVQE_FUZZ=ON. See ggml/fuzz/README.md for the full recipe.
        fuzz = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.clang
            pkgs.llvm
            pkgs.pkg-config
            pkgs.libsndfile
          ];
        };

        # WASM shell: Emscripten toolchain for the in-browser demo. Use with
        #   nix develop .#wasm
        # then run web/build-wasm.sh (emcmake cmake + emmake). Emscripten's
        # Nix package ships a read-only cache; EM_CACHE must point at a
        # writable copy or the first sysroot/port build fails.
        wasm = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.emscripten
            pkgs.python3
          ];
          shellHook = ''
            export EM_CACHE="''${EM_CACHE:-$PWD/.em-cache}"
            if [ ! -d "$EM_CACHE" ]; then
              mkdir -p "$EM_CACHE"
              cp -r ${pkgs.emscripten}/share/emscripten/cache/. "$EM_CACHE/" 2>/dev/null || true
              chmod -R u+w "$EM_CACHE"
            fi
          '';
        };

        # OBS plugin shell: libobs headers + CMake config alongside the
        # parent project's build deps (the plugin links libvqe.so, so you
        # typically build both in the same tree). Use with
        #   nix develop .#obs-plugin
        # then configure obs-plugin/ as a normal CMake project.
        obs-plugin = pkgs.mkShell {
          packages = [
            pkgs.cmake
            pkgs.gcc
            pkgs.pkg-config
            pkgs.libsndfile
            pkgs.obs-studio
          ];
        };
      };
    };
}
