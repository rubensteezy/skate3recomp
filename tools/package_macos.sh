#!/bin/sh
# Package a macos-release build as Skate3Recomp-macOS.zip.
#
# Run from the repository root after building the macos-release preset:
#   tools/package_macos.sh [build-dir] [version]
#
# build-dir defaults to out/build/macos-release. version defaults to the
# exact v* tag on HEAD. The bundle is skate3recomp.app with everything the
# build stages next to the executable (runtime and GPU plugin dylibs and the
# vulkan folder) in Contents/MacOS, ad-hoc signed. The zip is written to
# out/release.

set -eu

build_dir=${1:-out/build/macos-release}
version=${2:-$(git describe --tags --exact-match --match 'v[0-9]*' 2>/dev/null | sed 's/^v//')}

if [ -z "$version" ]; then
    echo "package_macos: HEAD has no v* tag; pass the version as the second argument" >&2
    exit 1
fi
for f in skate3 librexruntime.dylib librexgpu-xenos.dylib vulkan; do
    if [ ! -e "$build_dir/$f" ]; then
        echo "package_macos: $build_dir/$f is missing; build the macos-release preset first" >&2
        exit 1
    fi
done

stage=out/release/Skate3Recomp-macOS
app=$stage/skate3recomp.app
rm -rf "$stage" out/release/Skate3Recomp-macOS.zip
mkdir -p "$app/Contents/MacOS"

cp "$build_dir/skate3" "$build_dir/librexruntime.dylib" "$build_dir/librexgpu-xenos.dylib" \
    "$app/Contents/MacOS/"
cp -R "$build_dir/vulkan" "$app/Contents/MacOS/"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>skate3recomp</string>
  <key>CFBundleDisplayName</key><string>skate3recomp</string>
  <key>CFBundleIdentifier</key><string>io.github.rubensteezy.skate3recomp</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>skate3</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>GCSupportsGameMode</key><true/>
</dict>
</plist>
EOF

# Sign the nested dylibs first, then the bundle, so the seal covers them.
find "$app/Contents/MacOS" -name '*.dylib' -type f -exec codesign --force --sign - {} \;
codesign --force --sign - "$app"
codesign --verify --strict --verbose=2 "$app"

# ditto keeps the bundle's symlinks, permissions and signature intact.
(cd out/release && ditto -c -k --sequesterRsrc --keepParent Skate3Recomp-macOS Skate3Recomp-macOS.zip)
echo "package_macos: wrote out/release/Skate3Recomp-macOS.zip ($version)"
