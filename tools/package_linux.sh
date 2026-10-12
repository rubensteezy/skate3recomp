#!/bin/sh
# Package a linux-release build as Skate3Recomp-Linux.zip.
#
# Run from the repository root after building the linux-release preset:
#   tools/package_linux.sh [build-dir]
#
# build-dir defaults to out/build/linux-release. The zip holds a
# Skate3Recomp-Linux folder with the executable, the runtime library and the
# GPU plugin library, and is written to out/release.

set -eu

build_dir=${1:-out/build/linux-release}
files="skate3 librexruntime.so librexgpu-xenos.so"

for f in $files; do
    if [ ! -f "$build_dir/$f" ]; then
        echo "package_linux: $build_dir/$f is missing; build the linux-release preset first" >&2
        exit 1
    fi
done

stage=out/release/Skate3Recomp-Linux
rm -rf "$stage" out/release/Skate3Recomp-Linux.zip
mkdir -p "$stage"

for f in $files; do
    cp "$build_dir/$f" "$stage/"
    chmod 755 "$stage/$f"
done

(cd out/release && zip -q -r -X Skate3Recomp-Linux.zip Skate3Recomp-Linux)
echo "package_linux: wrote out/release/Skate3Recomp-Linux.zip"
