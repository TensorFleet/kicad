#!/usr/bin/env bash
# Build dependencies for the headless kicad-cli on Ubuntu 24.04 (the nightly runner).
# The list mirrors packages/kicad-patches/docker/Dockerfile in fab_pcb (Debian trixie) and
# install-deps.sh at the repository root, plus patchelf for the relocatable bundle.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config git ca-certificates ccache patchelf gettext \
  libwxgtk3.2-dev libwxgtk-webview3.2-dev libgtk-3-dev \
  libboost-all-dev \
  libcairo2-dev libpixman-1-dev libharfbuzz-dev libfontconfig1-dev libfreetype-dev libglib2.0-dev \
  libcurl4-openssl-dev libgit2-dev libsecret-1-dev libsqlite3-dev unixodbc-dev \
  libprotobuf-dev protobuf-compiler libnng-dev \
  libngspice0-dev ngspice \
  libocct-data-exchange-dev libocct-draw-dev libocct-foundation-dev libocct-modeling-algorithms-dev \
  libocct-modeling-data-dev libocct-ocaf-dev libocct-visualization-dev \
  nlohmann-json3-dev libglm-dev libzint-dev libpoppler-glib-dev \
  libgl-dev libglu1-mesa-dev libegl-dev libgles-dev libx11-dev \
  libwayland-dev wayland-protocols libspnav-dev \
  libzstd-dev zlib1g-dev libbz2-dev libssl-dev python3-dev
