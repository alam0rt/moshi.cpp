{
  description = "moshi.cpp – C++/GGML port of Kyutai's Moshi with Mumble bot support";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
        pkgs-cuda = import nixpkgs {
          inherit system;
          config = {
            allowUnfree = true;
            cudaSupport = true;
          };
        };

        # Shared derivation logic — builds with or without CUDA
        mkMoshi = { cudaEnabled ? false }:
          let
            p = if cudaEnabled then pkgs-cuda else pkgs;
          in p.stdenv.mkDerivation {
          pname = "moshi-cpp" + p.lib.optionalString cudaEnabled "-cuda";
          version = "0.0.1";

          src = self;

          nativeBuildInputs = with p; [
            cmake
            ninja
            pkg-config
            protobuf
            makeWrapper
          ] ++ p.lib.optionals cudaEnabled [
            p.cudaPackages.cuda_nvcc
          ];

          buildInputs = with p; [
            SDL2
            ffmpeg
            sentencepiece.dev
            sentencepiece.out
            openssl
            protobuf
            libopus
          ] ++ p.lib.optionals cudaEnabled [
            p.cudaPackages.cuda_cudart
            p.cudaPackages.libcublas
            p.cudaPackages.cuda_cccl
          ];

          cmakeFlags = [
            "-DSentencePiece_INCLUDE_DIR=${p.sentencepiece.dev}/include"
            "-DSentencePiece_LIBRARY_DIR=${p.sentencepiece.out}/lib"
            "-DCMAKE_INSTALL_RPATH=${placeholder "out"}/lib"
            "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON"
          ] ++ p.lib.optionals cudaEnabled [
            "-DGGML_CUDA=ON"
          ];

          # The project has no install() rules for its own targets, so manually
          # copy the binaries and shared lib from the Ninja build output dir.
          installPhase = ''
            runHook preInstall

            mkdir -p $out/bin $out/lib

            # Shared libraries (moshi + ggml)
            for f in bin/*.so bin/*.so.*; do
              [ -f "$f" ] && cp -v "$f" $out/lib/
            done

            # Executables
            for f in bin/mimi-* bin/moshi-*; do
              [ -f "$f" ] && [ -x "$f" ] && cp -v "$f" $out/bin/
            done

            # Config/aria2 helper files
            for f in bin/*.json bin/*.txt; do
              [ -f "$f" ] && cp -v "$f" $out/bin/
            done

            runHook postInstall
          '';

          # Patch RPATH so binaries find libmoshi.so, libggml*.so at runtime
          postFixup = ''
            for f in $out/lib/*.so $out/lib/*.so.*; do
              [ -f "$f" ] && patchelf --set-rpath "$out/lib" "$f"
            done
            for f in $out/bin/mimi-* $out/bin/moshi-*; do
              [ -x "$f" ] && wrapProgram "$f" \
                --prefix LD_LIBRARY_PATH : "$out/lib:${p.lib.getLib p.stdenv.cc.cc}/lib"
            done
          '';

          meta = with p.lib; {
            description = "C++/GGML port of Kyutai's Moshi with Mumble bot support";
            license = licenses.mit;
            platforms = platforms.unix;
          };
        };
      in
      {
        packages.default = mkMoshi { };
        packages.cuda = mkMoshi { cudaEnabled = true; };

        devShells.default = pkgs.mkShell {
          name = "moshi-cpp";

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            protobuf
            ninja
            aria2
          ];

          buildInputs = with pkgs; [
            # Existing deps
            SDL2
            ffmpeg
            sentencepiece.dev
            sentencepiece.out

            # GGML is vendored in vendor/ggml and built as a CMake subdirectory

            # Mumble client deps
            openssl
            protobuf
            libopus
          ];

          shellHook = ''
            echo "moshi.cpp dev shell"
            echo ""
            echo "GGML is vendored in vendor/ggml (no external paths needed)."
            echo ""
            echo "Configure with:"
            echo "  cmake -B build -G Ninja \\"
            echo "    -DSentencePiece_INCLUDE_DIR=${pkgs.sentencepiece.dev}/include \\"
            echo "    -DSentencePiece_LIBRARY_DIR=${pkgs.sentencepiece.out}/lib"
            echo ""
            echo "Optional GGML backends (pass to cmake):"
            echo "  -DGGML_CUDA=ON  -DGGML_VULKAN=ON  -DGGML_BACKEND_DL=ON"
            echo ""

            # Make SentencePiece discoverable by the custom Find module
            # Headers live in the "dev" output, libraries in the "out" output
            export SentencePiece_INCLUDE_DIR="${pkgs.sentencepiece.dev}/include"
            export SentencePiece_LIBRARY_DIR="${pkgs.sentencepiece.out}/lib"
          '';
        };
      });
}
