#!/usr/bin/env bash
set -euo pipefail

# ── Configuration ──────────────────────────────────────────────────────────
MUMBLE_HOST="${MUMBLE_HOST:-murmur.middleearth.samlockart.com}"
MUMBLE_PORT="${MUMBLE_PORT:-64738}"
MUMBLE_USER="${MUMBLE_USER:-moshi-bot}"

MODEL_DIR=".models"
MODEL_NAME="Codes4Fun/moshika-q4_k-GGUF"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARIA2_FILES=(
    "${SCRIPT_DIR}/tools/Codes4Fun_moshika-q4_k-GGUF.txt"
)

# ── Step 1: Build ──────────────────────────────────────────────────────────
echo "▸ Building moshi.cpp..."
nix build .#default

# ── Step 2: Download models if needed ──────────────────────────────────────
MODEL_PATH="${MODEL_DIR}/${MODEL_NAME}"
if [ -d "${MODEL_PATH}" ] && [ "$(find "${MODEL_PATH}" -name '*.gguf' 2>/dev/null | wc -l)" -gt 0 ]; then
    echo "▸ Models already present in ${MODEL_PATH}, skipping download."
else
    echo "▸ Downloading models to ${MODEL_DIR}/..."
    mkdir -p "${MODEL_DIR}"
    for f in "${ARIA2_FILES[@]}"; do
        echo "  ↳ $(basename "$f")"
        aria2c --disable-ipv6 \
               --dir="${MODEL_DIR}" \
               --input-file="$f" \
               --console-log-level=warn \
               --summary-interval=0
    done
    echo "▸ Download complete."
fi

# ── Step 3: Run moshi-mumble ───────────────────────────────────────────────
echo ""
echo "▸ Connecting to ${MUMBLE_HOST}:${MUMBLE_PORT} as ${MUMBLE_USER}..."
echo ""

exec ./result/bin/moshi-mumble \
    --host "${MUMBLE_HOST}" \
    --port "${MUMBLE_PORT}" \
    --username "${MUMBLE_USER}" \
    --model-root "${MODEL_DIR}/" \
    --model "${MODEL_NAME}" \
    "$@"
