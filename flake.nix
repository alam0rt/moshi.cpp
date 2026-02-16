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
            sentencepiece

            # GGML (user must supply or build separately — paths set below)
            # ggml

            # Mumble client deps
            openssl
            protobuf
            libopus
          ];

          shellHook = ''
            echo "moshi.cpp dev shell"
            echo ""
            echo "New deps available: openssl, protobuf, libopus"
            echo ""
            echo "You still need to set GGML paths if building from source:"
            echo "  export GGML_INCLUDE_DIR=~/repos/ggml/include"
            echo "  export GGML_LIBRARY_DIR=~/repos/ggml/build/src"
            echo ""
            echo "Then configure with:"
            echo "  cmake -B build -G Ninja \\"
            echo "    -DGGML_INCLUDE_DIR=\$GGML_INCLUDE_DIR \\"
            echo "    -DGGML_LIBRARY_DIR=\$GGML_LIBRARY_DIR \\"
            echo "    -DSentencePiece_INCLUDE_DIR=${pkgs.sentencepiece}/include \\"
            echo "    -DSentencePiece_LIBRARY_DIR=${pkgs.sentencepiece}/lib"
            echo ""

            # Make SentencePiece discoverable by the custom Find module
            export SentencePiece_INCLUDE_DIR="${pkgs.sentencepiece}/include"
            export SentencePiece_LIBRARY_DIR="${pkgs.sentencepiece}/lib"
          '';
        };
      });
}
