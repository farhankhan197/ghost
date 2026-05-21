#!/usr/bin/env bash
set -e

GHOST_REPO="farhankhan197/ghost"
GHOST_BIN_DIR="$HOME/.ghost/bin"
GITHUB_API="https://api.github.com/repos/$GHOST_REPO"

echo "▖ installing ghost-ai..."

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

# For linux arm64, fall back to x86_64 if no binary exists
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

# Create bin directory
mkdir -p "$GHOST_BIN_DIR"

# Download binaries
echo "  downloading ghost..."
curl -sL -o "$GHOST_BIN_DIR/ghost" "$DOWNLOAD_URL/$GHOST_BINARY"
chmod +x "$GHOST_BIN_DIR/ghost"

echo "  downloading ghost-checkpoint..."
curl -sL -o "$GHOST_BIN_DIR/ghost-checkpoint" "$DOWNLOAD_URL/$CHECKPOINT_BINARY"
chmod +x "$GHOST_BIN_DIR/ghost-checkpoint"

# Verify download
if [[ ! -x "$GHOST_BIN_DIR/ghost" ]]; then
  echo "  ERROR: failed to download ghost binary"
  exit 1
fi

# Add to PATH hint
if [[ ":$PATH:" != *":$GHOST_BIN_DIR:"* ]]; then
  echo ""
  echo "  Add to your PATH:"
  echo "    export PATH=\"\$HOME/.ghost/bin:\$PATH\""
  echo ""
  echo "  Add this to your ~/.bashrc, ~/.zshrc, or ~/.profile to make it permanent."
fi

echo ""
echo "  installed: $GHOST_BIN_DIR/ghost ($LATEST)"
echo ""
echo "  Next: cd to your repo and run 'ghost install'"
