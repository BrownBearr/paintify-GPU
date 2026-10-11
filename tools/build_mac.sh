#!/bin/bash
# Build Brushkit.app on macOS with the Xcode command line tools, CMake and vcpkg.
#
#   tools/build_mac.sh             build build/Brushkit.app
#   tools/build_mac.sh --install   ...and copy it to /Applications
#   tools/build_mac.sh --dmg       ...and package dist/Brushkit-<version>-macOS-<arch>.dmg
set -euo pipefail
cd "$(dirname "$0")/.."

if ! xcode-select -p >/dev/null 2>&1; then
    echo "The Xcode command line tools are required: xcode-select --install"
    exit 1
fi
command -v cmake >/dev/null || { echo "CMake is required: brew install cmake"; exit 1; }
VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
if [ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]; then
    echo "Set VCPKG_ROOT to your vcpkg checkout, or clone it to ~/vcpkg:"
    echo "  git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh"
    exit 1
fi
BUILD="${BRUSHKIT_BUILD_DIR:-build}"
GEN=()
command -v ninja >/dev/null && GEN=(-G Ninja)

cmake -S . -B "$BUILD" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build "$BUILD"
echo "Built $BUILD/Brushkit.app"

case "${1:-}" in
    --install)
        rm -rf /Applications/Brushkit.app
        ditto "$BUILD/Brushkit.app" /Applications/Brushkit.app
        echo "Installed /Applications/Brushkit.app"
        ;;
    --dmg)
        cpack --config "$BUILD/CPackConfig.cmake" -B dist
        ;;
esac
