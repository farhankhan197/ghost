#!/usr/bin/env bash
set -euo pipefail

REPO="farhankhan197/ghost"
GHOST_BIN_DIR="$HOME/.ghost/bin"

echo "▖ building ghost-ai from source..."

# ── Detect OS & arch ──────────────────────────────────────────────
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
  linux*)  OS_NAME="linux" ;;
  darwin*) OS_NAME="macos" ;;
  *)       echo "✖ Unsupported OS: $OS"; exit 1 ;;
esac

case "$ARCH" in
  x86_64|amd64)  ARCH_NAME="x86_64" ;;
  arm64|aarch64) ARCH_NAME="arm64" ;;
  *)             echo "✖ Unsupported architecture: $ARCH"; exit 1 ;;
esac

echo "  platform: ${OS_NAME}/${ARCH_NAME}"

# ── Check prerequisites ───────────────────────────────────────────
command -v git  >/dev/null 2>&1 || { echo "✖ git is required"; exit 1; }
command -v cmake >/dev/null 2>&1 || { echo "✖ cmake is required"; exit 1; }

if ! command -v c++ >/dev/null 2>&1 && ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
  echo "✖ A C++ compiler (c++/g++/clang++) is required"
  exit 1
fi

# ── Build in a temp dir ───────────────────────────────────────────
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "  cloning $REPO..."
git clone --depth 1 "https://github.com/$REPO.git" "$TMP_DIR/ghost"

BUILD_DIR="$TMP_DIR/ghost/build"
mkdir -p "$BUILD_DIR"

echo "  configuring..."
cmake -S "$TMP_DIR/ghost" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release

echo "  building..."
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
cmake --build "$BUILD_DIR" -j "$JOBS"

# ── Locate built binaries ─────────────────────────────────────────
GHOST_BIN="$BUILD_DIR/ghost${ext:-}"
CHECKPOINT_BIN="$BUILD_DIR/ghost-checkpoint${ext:-}"

if [ ! -f "$GHOST_BIN" ]; then
  # fallback: may be in bin/ subdir
  GHOST_BIN="$BUILD_DIR/bin/ghost${ext:-}"
  CHECKPOINT_BIN="$BUILD_DIR/bin/ghost-checkpoint${ext:-}"
fi

if [ ! -f "$GHOST_BIN" ]; then
  echo "✖ Build succeeded but ghost binary not found at $GHOST_BIN"
  exit 1
fi

# ── Install to ~/.ghost/bin ──────────────────────────────────────
echo "  installing to $GHOST_BIN_DIR..."
mkdir -p "$GHOST_BIN_DIR"

cp "$GHOST_BIN" "$GHOST_BIN_DIR/ghost"
if [ -f "$CHECKPOINT_BIN" ]; then
  cp "$CHECKPOINT_BIN" "$GHOST_BIN_DIR/ghost-checkpoint"
  chmod +x "$GHOST_BIN_DIR/ghost-checkpoint"
fi
chmod +x "$GHOST_BIN_DIR/ghost"

echo "  installed: $GHOST_BIN_DIR/ghost"

# ── PATH hint ─────────────────────────────────────────────────────
if [[ ":$PATH:" != *":$GHOST_BIN_DIR:"* ]]; then
  echo ""
  echo "  Add ghost to your PATH:"
  echo "    export PATH=\"\$HOME/.ghost/bin:\$PATH\""
  echo ""
  echo "  To make it permanent, add the line above to ~/.bashrc, ~/.zshrc, or ~/.profile."
fi

echo ""
echo "  Next steps:"
echo "    cd your-repo"
echo "    ghost install"
echo ""
echo "  Or try it right now:"
echo "    export PATH=\"\$HOME/.ghost/bin:\$PATH\""
echo "    cd \"$TMP_DIR/ghost\" && ghost init --yes"
