#!/bin/sh
# install.sh — build and install coding-agent for regular Linux
#
# Usage:  ./install.sh
#
# Compiles the project and installs the binary + system prompt to
# ~/.local/bin.  Adds this directory to PATH if not already there.

set -eu

INSTALL_DIR="${HOME}/.local/bin"

# ── 1. Ensure install directory exists ──────────────────────────────
mkdir -p "${INSTALL_DIR}"

# ── 2. Build ────────────────────────────────────────────────────────
echo "=== Building coding-agent ==="
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

BIN="build/coding-agent"
PROMPT="build/system_prompt.txt"

if [ ! -x "${BIN}" ]; then
    echo "ERROR: build failed — binary not found: ${BIN}" >&2
    exit 1
fi
if [ ! -f "${PROMPT}" ]; then
    echo "ERROR: build failed — prompt file not found: ${PROMPT}" >&2
    exit 1
fi

# ── 3. Install ──────────────────────────────────────────────────────
cp "${BIN}"      "${INSTALL_DIR}/coding-agent"
cp "${PROMPT}"   "${INSTALL_DIR}/system_prompt.txt"
echo "=== Installed to ${INSTALL_DIR}/ ==="
ls -l "${INSTALL_DIR}/coding-agent" "${INSTALL_DIR}/system_prompt.txt"

# ── 4. PATH check ───────────────────────────────────────────────────
# Detect current shell rc file to suggest adding PATH.
case "${SHELL:-}" in
    */bash) RC="${HOME}/.bashrc" ;;
    */zsh)  RC="${HOME}/.zshrc" ;;
    *)      RC="${HOME}/.profile" ;;
esac

if echo "${PATH:-}" | tr ':' '\n' | grep -qxF "${INSTALL_DIR}"; then
    echo "=== ${INSTALL_DIR} already in PATH ==="
else
    echo ""
    echo "NOTE: ${INSTALL_DIR} is not in your PATH."
    echo "Add this line to ${RC}:"
    echo ""
    echo "    export PATH=\"\${HOME}/.local/bin:\${PATH}\""
    echo ""
    echo "Or run:"
    echo "    echo 'export PATH=\"\${HOME}/.local/bin:\${PATH}\"' >> ${RC}"
    echo ""
fi

echo "=== Done. Run: coding-agent ==="
