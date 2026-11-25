#!/usr/bin/env bash
set -euo pipefail

BASE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
cd "$BASE"

SFL_VERSION="0.3.0"
SFL_TARBALL="silverfast-linux-${SFL_VERSION}-x86_64.tar.gz"
SFL_URL="https://github.com/sepgh/silverfast-linux/releases/download/v${SFL_VERSION}/${SFL_TARBALL}"

echo "=== Film Cockpit Setup ==="

# 1. Check required host tools & libraries
missing=()
for cmd in c++ pkg-config python3 Xvfb xdotool wine curl tar; do
    command -v "$cmd" >/dev/null 2>&1 || missing+=("$cmd")
done
if ! pkg-config --exists libtiff-4 2>/dev/null; then
    missing+=("libtiff-4 (pkg-config)")
fi
if ! python3 -c "import numpy, PIL" 2>/dev/null; then
    missing+=("python3 numpy & Pillow")
fi

if [ "${#missing[@]}" -gt 0 ]; then
    echo "Missing prerequisites:"
    for m in "${missing[@]}"; do echo "  - $m"; done
    echo ""
    echo "Install on Arch / EndeavourOS:"
    echo "  sudo pacman -S --needed base-devel libtiff pkgconf python python-numpy python-pillow xorg-server-xvfb xdotool wine curl"
    echo "Install on Debian / Ubuntu:"
    echo "  sudo apt install build-essential libtiff-dev pkg-config python3 python3-numpy python3-pillow xvfb xdotool wine curl"
    echo "Install on Fedora:"
    echo "  sudo dnf install gcc-c++ libtiff-devel pkgconf-pkg-config python3 python3-numpy python3-pillow xorg-x11-server-Xvfb xdotool wine curl"
    exit 1
fi
echo "[ok] Host tools, libtiff, numpy, Pillow, Xvfb, xdotool, wine"

# 2. Build C++20 imaging core via nobcpp
if [ ! -x ./nobcpp ]; then
    echo "[build] Bootstrapping nobcpp..."
    c++ -std=c++20 -o nobcpp nobcpp.cpp
fi
echo "[build] Building libfilm_core.so (release)..."
./nobcpp release

# 3. Install sepgh/silverfast-linux release into ./silverfast-bin
if [ ! -x "$BASE/silverfast-bin/bin/silverfast-linux" ]; then
    echo "[fetch] Downloading silverfast-linux v${SFL_VERSION}..."
    mkdir -p "$BASE/silverfast-release" "$BASE/silverfast-bin"
    curl -fsSL "$SFL_URL" -o "$BASE/silverfast-release/$SFL_TARBALL"
    tar -xzf "$BASE/silverfast-release/$SFL_TARBALL" --strip-components=1 -C "$BASE/silverfast-bin"
fi

# 4. Install bundled Epson V700/V750 model definition
MODELS_DST="$BASE/silverfast-bin/lib/silverfast-linux/models"
mkdir -p "$MODELS_DST"
cp -f "$BASE/models/epson-perfection-v700-v750.json" "$MODELS_DST/epson-perfection-v700-v750.json"
echo "[ok] Installed Epson V700/V750 model definition into silverfast-bin"

# 5. Create local data folders
mkdir -p "$BASE/scans" "$BASE/exports"

# 6. Check USB udev access for Epson V700/V750 (04b8:012c)
if lsusb -d 04b8:012c >/dev/null 2>&1; then
    echo "[ok] Epson V700/V750 detected on USB"
else
    echo "[info] Epson V700/V750 (04b8:012c) not currently plugged in / powered on"
fi

echo ""
echo "=== Setup Complete ==="
if [ ! -f "$BASE/silverfast-data/prefix/drive_c/Program Files/SilverFast Application/SilverFast 9/SilverFast 9.exe" ]; then
    echo "Next (one-time) step to enable the EUR 0 SilverFast 9 backend:"
    echo "  1. Download the official SilverFast 9 Windows installer (.exe) from silverfast.com"
    echo "  2. Run:  ./sfl.sh install /path/to/SilverFast-9.x.exe --model epson-perfection-v700-v750"
    echo "  3. Run:  ./sfl.sh run   (sign in to MySilverFast once, then close it)"
    echo "  4. Verify: ./sfl.sh doctor"
fi
echo ""
echo "Start Film Cockpit:"
echo "  ./start_cockpit.sh"
echo "Then open http://127.0.0.1:8765 (or http://<this-host-ip>:8765 from another laptop)."
