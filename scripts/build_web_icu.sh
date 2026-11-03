#!/usr/bin/env bash
# Build the Web reader's ICU 77.1 policy on macOS without a Homebrew formula.
set -euo pipefail

mkdir -p "${1:?usage: build_web_icu.sh BUILD_DIRECTORY [PARALLEL]}"
icu_build_root="$(cd "$1" && pwd)"
icu_archive="$icu_build_root/icu4c-77_1-src.tgz"
curl --fail --location --max-time 600 \
  --proto '=https' --proto-redir '=https' \
  --output "$icu_archive" \
  https://github.com/unicode-org/icu/releases/download/release-77-1/icu4c-77_1-src.tgz
printf '%s  %s\n' \
  588e431f77327c39031ffbb8843c0e3bc122c211374485fa87dc5f3faff24061 \
  "$icu_archive" | shasum -a 256 -c -
tar -xzf "$icu_archive" -C "$icu_build_root"
mkdir -p "$icu_build_root/build"
cd "$icu_build_root/build"
"$icu_build_root/icu/source/runConfigureICU" MacOSX \
  --prefix="$icu_build_root/install" \
  --enable-shared --disable-static --enable-rpath \
  --disable-samples --disable-tests \
  --with-library-bits=64
make -j "${2:-3}"
make install
