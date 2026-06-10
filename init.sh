#!/usr/bin/env bash
set -euo pipefail

# One-shot init for Ghost.
# Usage:
#   curl -fsSL https://raw.githubusercontent.com/farhankhan197/ghost/main/init.sh | bash

GHOST_REPO="farhankhan197/ghost"
GHOST_API="https://api.github.com/repos/$GHOST_REPO"

echo "  initializing Ghost..."

OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
  linux*) OS_NAME="linux" ;;
  mingw*|cygwin*|msys*) OS_NAME="windows" ;;
  darwin*) echo "  ERROR: macOS binaries are not currently published. Ghost supports Linux x86_64 and Windows x86_64."; exit 1 ;;
  *) echo "  ERROR: unsupported OS: $OS"; exit 1 ;;
esac

case "$ARCH" in
  x86_64|amd64) ARCH_NAME="x86_64" ;;
  arm64|aarch64) echo "  ERROR: ARM binaries are not currently published. Ghost supports Linux x86_64 and Windows x86_64."; exit 1 ;;
  *) echo "  ERROR: unsupported architecture: $ARCH"; exit 1 ;;
esac

command -v curl >/dev/null 2>&1 || { echo "  ERROR: curl is required"; exit 1; }

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "  fetching latest release..."
LATEST="$(curl -fsSL "$GHOST_API/releases/latest" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')"
if [[ -z "$LATEST" ]]; then
  echo "  ERROR: could not find latest release"
  exit 1
fi

echo "  latest: $LATEST"

EXT=""
if [[ "$OS_NAME" == "windows" ]]; then
  EXT=".exe"
fi

BASE_URL="https://github.com/$GHOST_REPO/releases/download/$LATEST"
GHOST_BINARY="ghost-${OS_NAME}-${ARCH_NAME}${EXT}"
CHECKPOINT_BINARY="ghost-checkpoint-${OS_NAME}-${ARCH_NAME}${EXT}"
GHOST_OUT="$TMP_DIR/ghost${EXT}"
CHECKPOINT_OUT="$TMP_DIR/ghost-checkpoint${EXT}"

echo "  downloading Ghost..."
curl -fsSL -o "$GHOST_OUT" "$BASE_URL/$GHOST_BINARY"
chmod +x "$GHOST_OUT"

echo "  downloading Ghost checkpoint..."
curl -fsSL -o "$CHECKPOINT_OUT" "$BASE_URL/$CHECKPOINT_BINARY"
chmod +x "$CHECKPOINT_OUT"

if [[ ! -x "$GHOST_OUT" ]]; then
  echo "  ERROR: failed to download Ghost binary"
  exit 1
fi

echo ""
echo "  running ghost init --yes..."
echo ""

GHOST_BIN="$TMP_DIR" "$GHOST_OUT" init --yes

echo ""
echo "  Ghost is initialized."
echo ""
echo "  To keep Ghost permanently installed:"
echo "    curl -fsSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash"
echo ""
