#!/bin/bash
# ============================================================
#  OTA Package Builder
#  Usage: ./package_ota.sh <version> <github_repo_dir>
#  Example: ./package_ota.sh 1.0.2 ~/OTA_test
# ============================================================

set -e

VERSION="$1"
REPO_DIR="$2"

if [ -z "$VERSION" ] || [ -z "$REPO_DIR" ]; then
    echo "Usage: $0 <version> <path_to_OTA_test_repo>"
    echo "Example: $0 1.0.2 ~/OTA_test"
    exit 1
fi

if [ ! -f "build/main" ]; then
    echo "ERROR: build/main not found. Run 'make' first."
    exit 1
fi

echo "=== Packaging OTA release v$VERSION ==="

PKG_DIR=$(mktemp -d)
cp build/main "$PKG_DIR/main"
echo "$VERSION" > "$PKG_DIR/version.txt"

cd "$PKG_DIR"
tar -czf gateway.tar.gz main version.txt
sha256sum gateway.tar.gz | awk '{print $1}' > gateway.sha256

PKG_SIZE=$(stat -c%s gateway.tar.gz)
PKG_HASH=$(cat gateway.sha256)

echo "Package size : $PKG_SIZE bytes"
echo "SHA256       : $PKG_HASH"

# --- Copy into repo firmware/sha256 folders ---
mkdir -p "$REPO_DIR/firmware" "$REPO_DIR/sha256"
cp gateway.tar.gz "$REPO_DIR/firmware/gateway.tar.gz"
cp gateway.sha256 "$REPO_DIR/sha256/gateway.sha256"

# --- Generate latest.json ---
cat > "$REPO_DIR/latest.json" <<EOF
{
  "latest_version": "$VERSION",
  "package_name": "gateway.tar.gz",
  "package_url": "https://raw.githubusercontent.com/Aishwarya20042002/OTA_test/stm32_ota/firmware/gateway.tar.gz",
  "sha256_url": "https://raw.githubusercontent.com/Aishwarya20042002/OTA_test/stm32_ota/sha256/gateway.sha256",
  "package_size": $PKG_SIZE
}
EOF

echo "=== latest.json written to $REPO_DIR/latest.json ==="
cat "$REPO_DIR/latest.json"

# --- Commit and push ---
cd "$REPO_DIR"
git add firmware/gateway.tar.gz sha256/gateway.sha256 latest.json
git commit -m "OTA release v$VERSION"
git push origin stm32_ota

rm -rf "$PKG_DIR"

echo "=== Done. Board will pick up v$VERSION on its next check ==="