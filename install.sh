#!/usr/bin/env sh
set -eu

REPO="gregoryz2013/SiftLog"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
ARCH="$(uname -m)"

case "$OS" in
  linux)
    case "$ARCH" in
      x86_64|amd64) ASSET="siftlog-linux-x86_64" ;;
      *) echo "Unsupported Linux architecture: $ARCH (build from source via CMake)"; exit 1 ;;
    esac
    ;;
  darwin)
    case "$ARCH" in
      arm64|aarch64) ASSET="siftlog-macos-arm64" ;;
      *) echo "Unsupported macOS architecture: $ARCH (build from source via CMake)"; exit 1 ;;
    esac
    ;;
  *)
    echo "Unsupported OS: $OS. Download Windows binary from GitHub Releases."
    exit 1
    ;;
esac

URL="https://github.com/${REPO}/releases/latest/download/${ASSET}"
INSTALL_DIR="/usr/local/bin"
if [ ! -w "$INSTALL_DIR" ]; then
  INSTALL_DIR="${HOME}/.local/bin"
  mkdir -p "$INSTALL_DIR"
fi

TMP_FILE="$(mktemp)"
echo "Downloading ${ASSET} from ${URL}..."
curl -fsSL "$URL" -o "$TMP_FILE"
chmod +x "$TMP_FILE"
mv "$TMP_FILE" "${INSTALL_DIR}/siftlog"

echo "Installed siftlog to ${INSTALL_DIR}/siftlog"
"${INSTALL_DIR}/siftlog" --help | head -n 3 || true
