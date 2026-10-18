#!/bin/sh
# Writes stagerig-VERSION.tar.xz: the tree at HEAD with its submodules, which
# GitHub's own "Source code" archives leave out, minus what a Linux build never
# uses and a distribution would have to strip anyway -- JUCE's examples and
# tools, and the AAX and ASIO SDKs, which carry proprietary licence options.
#
#   scripts/make-source-tarball.sh [VERSION] [OUTDIR]
set -eu
cd "$(dirname "$0")/.."
VERSION="${1:-$(sed -n 's/^set(STAGERIG_VERSION "\([^"]*\)".*/\1/p' CMakeLists.txt)}"
OUT="$(cd "${2:-.}" && pwd)"
NAME="stagerig-${VERSION:?no version}"
WORK="$(mktemp -d)"
trap 'rm -rf "${WORK:?}"' EXIT

git archive --format=tar --prefix="$NAME/" HEAD | tar -x -C "$WORK"
git submodule foreach --quiet 'git archive --format=tar --prefix="'"$NAME"'/$sm_path/" HEAD | tar -x -C "'"$WORK"'"'

J="${WORK:?}/${NAME:?}/external/JUCE"
for d in examples docs \
         extras/AudioPerformanceTest extras/AudioPluginHost extras/BinaryBuilder \
         extras/NetworkGraphicsDemo extras/Projucer extras/UnitTestRunner extras/WindowsDLL \
         modules/juce_audio_plugin_client/AAX/SDK \
         modules/juce_audio_devices/native/asio; do
    rm -rf "${J:?}/${d:?}"
done
rm -rf "${WORK:?}/${NAME:?}/external/clap/.github"

tar -C "$WORK" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git log -1 --format=%ct)" -cJf "$OUT/$NAME.tar.xz" "$NAME"
echo "$OUT/$NAME.tar.xz"
