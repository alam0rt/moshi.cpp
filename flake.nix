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
      in
      {
        devShells.default = pkgs.mkShell {
          name = "moshi-cpp";

          nativeBuildInputs = with pkgs; [
            cmake
            pkg-config
            protobuf
            ninja
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
