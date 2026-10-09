#!/bin/bash
# install_toolchain.sh - installs the armhf cross toolchain and target libraries
# on Ubuntu 22.04. Single source of truth for the package list: used by the
# Dockerfile (local builds) and by the CI build job (.gitlab/ci/build.yml).
#
# Usage (as root):  bash tools/ci/install_toolchain.sh
#
# APT_CACHE_DIR (optional): keep downloaded .deb files there instead of
# deleting them, so CI can cache them between jobs.

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

# The default mirrors only carry amd64/i386; armhf packages are on ports.ubuntu.com.
if ! dpkg --print-foreign-architectures | grep -qx armhf; then
    dpkg --add-architecture armhf
fi
cat > /etc/apt/sources.list.d/armhf.list <<'EOF'
deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse
deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse
deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse
EOF
sed -i 's/^deb http/deb [arch=amd64] http/' /etc/apt/sources.list

APT_OPTS=()
if [ -n "${APT_CACHE_DIR:-}" ]; then
    mkdir -p "$APT_CACHE_DIR/partial"
    rm -f /etc/apt/apt.conf.d/docker-clean   # the image's hook deletes downloaded .debs
    APT_OPTS=(-o "Dir::Cache::Archives=$APT_CACHE_DIR")
fi

apt-get update -qq
apt-get install -y -qq --no-install-recommends "${APT_OPTS[@]}" \
    gcc-arm-linux-gnueabihf \
    g++-arm-linux-gnueabihf \
    make \
    bc \
    kmod \
    file \
    git \
    python3 \
    sshpass \
    openssh-client \
    libmodbus-dev:armhf \
    libmosquitto-dev:armhf \
    libsqlite3-dev:armhf \
    libssl-dev:armhf \
    libcurl4-openssl-dev:armhf \
    zlib1g-dev:armhf \
    > /dev/null

if [ -z "${APT_CACHE_DIR:-}" ]; then
    rm -rf /var/lib/apt/lists/*
fi

arm-linux-gnueabihf-gcc --version | head -1
