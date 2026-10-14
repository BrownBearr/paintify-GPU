#!/bin/bash
# Installs Paintify on macOS:
#
#   curl -fsSL https://raw.githubusercontent.com/BrownBearr/paintify-GPU/main/install.sh | bash
#
# Why a script instead of just the DMG: a browser marks everything it downloads
# as "from the internet", and macOS then refuses to open an app that is not
# notarized by Apple. Files fetched by curl carry no such mark, so the app
# opens like any other. Run it again to update.
set -euo pipefail

repo="BrownBearr/paintify-GPU"
arch="$(uname -m)"
[ "$arch" = "arm64" ] || arch="x86_64"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if [ -n "${PAINTIFY_ZIP:-}" ]; then
    cp "$PAINTIFY_ZIP" "$tmp/Paintify.zip"          # a local build, for testing
else
    url="$(curl -fsSL "https://api.github.com/repos/$repo/releases/latest" |
           grep -o "https://[^\"]*-macOS-$arch\.zip" | head -1)"
    if [ -z "$url" ]; then
        echo "Could not find a Paintify download for this Mac ($arch)." >&2
        exit 1
    fi
    echo "Downloading $(basename "$url")..."
    curl -fL --progress-bar "$url" -o "$tmp/Paintify.zip"
fi

ditto -x -k "$tmp/Paintify.zip" "$tmp"

dest="/Applications"
[ -w "$dest" ] || dest="$HOME/Applications"
mkdir -p "$dest"
pkill -x Paintify 2>/dev/null || true
rm -rf "$dest/Paintify.app"
ditto "$tmp/Paintify.app" "$dest/Paintify.app"
# Belt and braces: clear the mark in case the zip came through a browser.
xattr -dr com.apple.quarantine "$dest/Paintify.app" 2>/dev/null || true

echo "Installed Paintify in $dest."
if ! command -v ffmpeg >/dev/null 2>&1 && [ ! -x /opt/homebrew/bin/ffmpeg ] && [ ! -x /usr/local/bin/ffmpeg ]; then
    echo "For video, also install ffmpeg:  brew install ffmpeg"
fi
[ -n "${PAINTIFY_NO_OPEN:-}" ] || open "$dest/Paintify.app"
