SUMMARY = "Lightweight AMSET image for STM32MP157F-DK2"
DESCRIPTION = "Minimal image with Modbus, MQTT, SQLite and build tools"

require recipes-core/images/core-image-minimal.bb

IMAGE_FEATURES += "ssh-server-openssh"

IMAGE_INSTALL:append = " \
    packagegroup-core-boot \
    packagegroup-base \
    openssh \
    openssh-scp \
    ca-certificates \
    gcc \
    gcc-symlinks \
    g++ \
    g++-symlinks \
    make \
    binutils \
    libmodbus \
    libmodbus-dev \
    mosquitto \
    mosquitto-dev \
    libmosquitto1 \
    sqlite3 \
    libsqlite3 \
    libsqlite3-dev \
    openssl \
    libssl-dev \
    curl \
    wget \
    nano \
    htop \
    util-linux \
    procps \
    net-tools \
    iproute2 \
    iputils \
    ntpdate \
"

# Remove heavy packages to keep image light
IMAGE_FEATURES:remove = "x11 wayland"

# Set root password to empty (allow passwordless login)
EXTRA_USERS_PARAMS = "usermod -P '' root;"

# Image size limit (512MB)
IMAGE_ROOTFS_SIZE ?= "524288"
IMAGE_OVERHEAD_FACTOR ?= "1.3"
