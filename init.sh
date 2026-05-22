if [[ -z "$LATEST" ]]; then
  echo "  ERROR: could not find latest release"
  exit 1
fi

echo "  latest: $LATESTDOWNLOAD_URL="https://github.com/$GHOST_REPO/releases/download/$LATEST"

"DOWNLOAD_URL="https://github.com/$GHOST_REPO/releases/download/$LATEST"

DOWNLOAD_URL="https://github.com/$GHOST_REPO/releases/download/$LATEST"

# Determine binary names
if [[ "$OS_NAME" == "windows" ]]; then
  GHOST_BINARY="ghost-${OS_NAME}-${ARCH_NAME}.exe"
  CHECKPOINT_BINARY="ghost-checkpoint-${OS_NAME}-${ARCH_NAME}.exe"
else
  GHOST_BINARY="ghost-${OS_NAME}-${ARCH_NAME}"
  CHECKPOINT_BINARY="ghost-checkpoint-${OS_NAME}-${ARCH_NAME}"
fi

# Download to tempdjsklajdklasjdakls:
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
