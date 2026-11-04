#!/bin/bash
# Usage: ./package-macos.sh [version] [build-directory]
set -euo pipefail
cd "$(dirname "$0")"
version="${1:-dev}"
build_dir="${2:-build}"
case "$version" in
  *[!a-zA-Z0-9._-]*|'') echo "Invalid version: $version" >&2; exit 1 ;;
esac
app="$build_dir/record-align.app"
cli="$build_dir/record-align-cli"
test -x "$app/Contents/MacOS/record-align"
test -x "$cli"
codesign --verify --strict "$app"
codesign --verify --strict "$cli"
architectures="$(lipo -archs "$app/Contents/MacOS/record-align")"
case "$architectures" in
  *arm64*x86_64*|*x86_64*arm64*) architecture=universal ;;
  arm64|x86_64) architecture="$architectures" ;;
  *) echo "Unexpected architectures: $architectures" >&2; exit 1 ;;
esac
name="record-align-$version-macos-$architecture"
stage="dist/$name"
rm -rf "$stage"
mkdir -p "$stage"
ditto "$app" "$stage/record-align.app"
cp "$cli" README.md "$stage/"
# Preserve the build's signing identity and microphone permission identity.
# In particular, don't replace a configured development signature with ad-hoc signing.
codesign --verify --strict "$stage/record-align-cli"
codesign --verify --strict "$stage/record-align.app"
plutil -lint "$stage/record-align.app/Contents/Info.plist"
rm -f "dist/$name.zip"
ditto -c -k --sequesterRsrc --keepParent "$stage" "dist/$name.zip"
(cd dist && shasum -a 256 "$name.zip" > "$name.zip.sha256")
echo "dist/$name.zip"
