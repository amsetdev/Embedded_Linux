# ============================================================
#  STM32MP1 Cross-Compilation Docker Environment
#  Target : arm-linux-gnueabihf (Cortex-A7)
#  Base   : Ubuntu 22.04 (pinned for reproducibility)
# ============================================================

FROM ubuntu:22.04

# Avoid interactive prompts during package install
ENV DEBIAN_FRONTEND=noninteractive

# --- armhf cross toolchain + target libraries ---
# The package list lives in tools/ci/install_toolchain.sh, shared with the
# CI build job (.gitlab/ci/build.yml), so local and CI builds use the same set.
COPY tools/ci/install_toolchain.sh /tmp/install_toolchain.sh
RUN bash /tmp/install_toolchain.sh && rm /tmp/install_toolchain.sh

# --- Kernel headers mount point ---
# Mount your STM32MP1 kernel headers here at runtime:
#   docker run -v /path/to/kernel/headers:/kernel ...
ENV KDIR=/kernel

WORKDIR /project

# Default: build the project
CMD ["make", "clean", "all"]
