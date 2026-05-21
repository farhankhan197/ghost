#!/usr/bin/env bash
set -e

# npx-style one-liner init for ghost
# Usage: curl -fsSL https://raw.githubusercontent.com/farhankhan197/ghost/main/init.sh | bash

GHOST_REPO="farhankhan197/ghost"
GITHUB_API="https://api.github.com/repos/$GHOST_REPO"

TMP_DIR=$(mktemp -d)
trap "rm -rf $TMP_DIR" EXIT

echo "▖ initializing ghost..."

# Detect OS and arch
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
  linux*)  OS_NAME="linux" ;;
  darwin*) OS_NAME="macos" ;;
  mingw*|cygwin*|msys*) OS_NAME="windows" ;;
  *) echo "Unsupported OS: $OS"; exit 1 ;;
esac

case "$ARCH" in
  x86_64|amd64)  ARCH_NAME="x86_64" ;;
  arm64|aarch64) ARCH_NAME="arm64" ;;
  *) echo "Unsupported architecture: $ARCH"; exit 1 ;;
esac

# For linux arm64, fall back to x86_64
if [[ "$OS_NAME" == "linux" && "$ARCH_NAME" == "arm64" ]]; then
  echo "  Note: Linux arm64 not yet available, using x86_64"
  ARCH_NAME="x86_64"
fi

# Get latest release
echo "  fetching latest release..."
LATEST=$(curl -sL "$GITHUB_API/releases/latest" | grep '"tag_name"' | head -1 | sed 's/.*"tag_name": *"\([^"]*\)".*/\1/')

if [[ -z "$LATEST" ]]; then
  echo "  ERROR: could not find latest release"
  exit 1
fi

echo "  latest: $LATEST"

# Determine binary names
if [[ "$OS_NAME" == "windows" ]]; then
  GHOST_BINARY="ghost-${OS_NAME}-${ARCH_NAME}.exe"
  CHECKPOINT_BINARY="ghost-checkpoint-${OS_NAME}-${ARCH_NAME}.exe"
else
  GHOST_BINARY="ghost-${OS_NAME}-${ARCH_NAME}"
  CHECKPOINT_BINARY="ghost-checkpoint-${OS_NAME}-${ARCH_NAME}"
fi

DOWNLOAD_URL="https://github.com/$GHOST_REPO/releases/download/$LATEST"

# Download to temp
echo "  downloading ghost to temp directory..."
curl -sL -o "$TMP_DIR/ghost" "$DOWNLOAD_URL/$GHOST_BINARY"
chmod +x "$TMP_DIR/ghost"

curl -sL -o "$TMP_DIR/ghost-checkpoint" "$DOWNLOAD_URL/$CHECKPOINT_BINARY"
chmod +x "$TMP_DIR/ghost-checkpoint"

if [[ ! -x "$TMP_DIR/ghost" ]]; then
  echo "  ERROR: failed to download ghost binary"
  exit 1
fi

echo ""
echo "  Running ghost init --yes..."
echo ""

# Run init from temp
GHOST_BIN="$TMP_DIR" "$TMP_DIR/ghost" init --yes

echo ""
echo "  Ghost is initialized!"
echo ""
echo "  To keep ghost permanently installed:"
echo "    curl -fsSL https://raw.githubusercontent.com/farhankhan197/ghost/main/install.sh | bash"
echo ""
echo "  Or add this to your PATH for this session only:"
echo "    export PATH=\"$TMP_DIR:\$PATH\""
echo ""
