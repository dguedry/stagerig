#!/bin/sh
# Writes stagerig-VERSION.tar.xz: the tree at HEAD with its submodules, which
# GitHub's own "Source code" archives leave out, minus what a Linux build never
# uses and a distribution would have to strip anyway -- JUCE's examples and
# tools, and the AAX and ASIO SDKs, which carry proprietary licence options;
# Link's examples and the tests and docs of the Asio it brings.
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
# --recursive: Link carries Asio as a submodule of its own; $displaypath is from the top.
git submodule foreach --quiet --recursive 'git archive --format=tar --prefix="'"$NAME"'/$displaypath/" HEAD | tar -x -C "'"$WORK"'"'

J="${WORK:?}/${NAME:?}/external/JUCE"
for d in examples docs \
         extras/AudioPerformanceTest extras/AudioPluginHost extras/BinaryBuilder \
         extras/NetworkGraphicsDemo extras/Projucer extras/UnitTestRunner extras/WindowsDLL \
         modules/juce_audio_plugin_client/AAX/SDK \
         modules/juce_audio_devices/native/asio; do
    rm -rf "${J:?}/${d:?}"
done
rm -rf "${WORK:?}/${NAME:?}/external/clap/.github"
# Link: the headers and the Asio it uses; not its examples, CI, assets or Asio's tests and docs.
L="${WORK:?}/${NAME:?}/external/link"
for d in examples ci assets "Ableton Link Guidelines.pdf" \
         modules/asio-standalone/src/tests modules/asio-standalone/src/examples modules/asio-standalone/src/doc; do
    rm -rf "${L:?}/${d:?}"
done

tar -C "$WORK" --sort=name --owner=0 --group=0 --numeric-owner \
    --mtime="@$(git log -1 --format=%ct)" -cJf "$OUT/$NAME.tar.xz" "$NAME"
echo "$OUT/$NAME.tar.xz"
