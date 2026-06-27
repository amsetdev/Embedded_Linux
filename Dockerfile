# ============================================================
#  STM32MP1 Cross-Compilation Docker Environment
#  Target : arm-linux-gnueabihf (Cortex-A7)
#  Base   : Ubuntu 22.04 (pinned for reproducibility)
# ============================================================

FROM ubuntu:22.04

# Avoid interactive prompts during package install
ENV DEBIAN_FRONTEND=noninteractive

# --- Add armhf architecture and its package source ---
# The default mirrors only carry amd64/i386.
# ARM packages live on ports.ubuntu.com.
RUN dpkg --add-architecture armhf && \
    echo "deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy main restricted universe multiverse" > /etc/apt/sources.list.d/armhf.list && \
    echo "deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy-updates main restricted universe multiverse" >> /etc/apt/sources.list.d/armhf.list && \
    echo "deb [arch=armhf] http://ports.ubuntu.com/ubuntu-ports jammy-security main restricted universe multiverse" >> /etc/apt/sources.list.d/armhf.list && \
    sed -i 's/^deb /deb [arch=amd64] /' /etc/apt/sources.list && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
        gcc-arm-linux-gnueabihf \
        g++-arm-linux-gnueabihf \
        make \
        bc \
        kmod \
        libmodbus-dev:armhf \
        libmosquitto-dev:armhf \
        libsqlite3-dev:armhf \
        libssl-dev:armhf \
        libcurl4-openssl-dev:armhf \
        zlib1g-dev:armhf \
        sshpass \
        openssh-client \
        file \
    && rm -rf /var/lib/apt/lists/*

# --- Kernel headers mount point ---
# Mount your STM32MP1 kernel headers here at runtime:
#   docker run -v /path/to/kernel/headers:/kernel ...
ENV KDIR=/kernel

WORKDIR /project

# Default: build the project
CMD ["make", "clean", "all"]
