# STM32MP1 Secure Boot Chain — Implementation Guide (Developer Package)

**From zero to verified boot on STM32MP157F-DK2**

This guide uses ST's **Developer Package** approach — you download a pre-built SDK, get TF-A/OP-TEE/U-Boot sources with ST patches, and build only the components you need. This avoids the complexity of a full Yocto build while giving you full control over the boot chain.

> **Your current state (as of writing):**
> - Docker-based cross-compilation for userspace application only
> - No TF-A / OP-TEE / U-Boot source trees in your repository
> - Stock OpenSTLinux image on the DK2 board
> - No signing keys or secure boot infrastructure
> - OTA uses SHA256 integrity check only (no signature verification)

---

## Table of Contents

- [Part 1: Understanding What You Are Building](#part-1-understanding-what-you-are-building)
- [Part 2: STM32MP1 Packages — Which One and Why](#part-2-stm32mp1-packages--which-one-and-why)
- [Part 3: Setting Up the Developer Package](#part-3-setting-up-the-developer-package)
- [Part 4: Building TF-A, OP-TEE, and U-Boot from Source](#part-4-building-tf-a-op-tee-and-u-boot-from-source)
- [Part 5: Key Generation](#part-5-key-generation)
- [Part 6: Signing the Boot Images](#part-6-signing-the-boot-images)
- [Part 7: Flashing and Testing (Development Mode)](#part-7-flashing-and-testing-development-mode)
- [Part 8: OTP Fuse Programming (Production)](#part-8-otp-fuse-programming-production)
- [Part 9: Verified Kernel Boot (FIT Image)](#part-9-verified-kernel-boot-fit-image)
- [Part 10: Anti-Rollback Protection](#part-10-anti-rollback-protection)
- [Part 11: Recovery Strategy and A/B Boot](#part-11-recovery-strategy-and-ab-boot)
- [Part 12: Integration with Your Existing Project](#part-12-integration-with-your-existing-project)
- [Part 13: Testing and Validation Checklist](#part-13-testing-and-validation-checklist)
- [Appendix A: Troubleshooting](#appendix-a-troubleshooting)
- [Appendix B: Glossary](#appendix-b-glossary)
- [Appendix C: References](#appendix-c-references)

---

## Part 1: Understanding What You Are Building

### 1.1 What Is Secure Boot?

Secure boot is a chain of trust where each boot stage cryptographically verifies the next before executing it. If any stage has been tampered with, boot halts.

```
                          TRUST FLOWS DOWNWARD
                          ====================

    +------------------------------------------------------------------+
    |                                                                   |
    |  +------------------+                                            |
    |  |  STM32MP1 ROM    |  Immutable. Burned into silicon at ST     |
    |  |  (Boot ROM)      |  factory. You cannot change this.         |
    |  |                  |  Contains the first verification code.    |
    |  +--------+---------+                                            |
    |           |                                                      |
    |           | 1. Reads OTP fuses for public key hash              |
    |           | 2. Reads TF-A from eMMC boot partition              |
    |           | 3. Extracts public key from TF-A STM32 header       |
    |           | 4. Hashes that public key (SHA-256)                  |
    |           | 5. Compares hash against OTP fuse value              |
    |           | 6. If match: verifies TF-A ECDSA signature          |
    |           | 7. If signature valid: jumps to TF-A                |
    |           | 8. If anything fails: BOOT STOPS                    |
    |           |                                                      |
    |  +--------v---------+                                            |
    |  |  TF-A (BL2)      |  First software you control.             |
    |  |  "FSBL"           |  Initializes DDR, clocks, security.      |
    |  |                  |  Loads FIP package from eMMC.             |
    |  +--------+---------+                                            |
    |           |                                                      |
    |           | Loads FIP (Firmware Image Package)                   |
    |           | FIP contains: OP-TEE + U-Boot + certificates        |
    |           | Verifies each component using certificate chain     |
    |           | rooted in same key that signed TF-A                 |
    |           |                                                      |
    |  +--------v---------+    +------------------+                    |
    |  |  OP-TEE (BL32)   |    |  U-Boot (BL33)  |                    |
    |  |  Trusted OS       |    |  Bootloader      |                    |
    |  |  Secure world     |    |  Normal world    |                    |
    |  +------------------+    +--------+---------+                    |
    |                                   |                              |
    |                                   | Loads FIT image (kernel+DTB)|
    |                                   | Verifies RSA signature       |
    |                                   |                              |
    |                          +--------v---------+                    |
    |                          |  Linux Kernel     |                    |
    |                          |  + Device Tree    |                    |
    |                          +------------------+                    |
    +------------------------------------------------------------------+
```

### 1.2 STM32MP1 Secure Boot Specifics

The STM32MP1 ROM uses **ECDSA with prime256v1 (P-256)** for authentication. This is not configurable — the ROM is hardcoded for this algorithm.

Key facts:
- The ROM verifies TF-A (FSBL) using ECDSA-P256 signature
- The public key is embedded in the TF-A `.stm32` image header
- The SHA-256 hash of the public key is stored in OTP fuses (words 24-31 on some revisions, or 32-39 — check your silicon revision)
- TF-A verifies the FIP contents using a certificate chain (Chain of Trust / CoT)
- U-Boot verifies the kernel FIT image using RSA-2048

### 1.3 What Needs to Change

| Component | Current State | Target State |
|-----------|--------------|--------------|
| TF-A | Stock ST binary | Built from source, signed with your key |
| OP-TEE | Stock ST binary | Built from source, included in signed FIP |
| U-Boot | Stock ST binary | Built from source, FIT verification enabled |
| Linux Kernel | Stock ST kernel | Packaged as signed FIT image |
| Signing keys | None | ECDSA P-256 key pair + RSA-2048 for FIT |
| OTP fuses | Unprogrammed | Public key hash + secure boot bit |
| Your application | Docker cross-compiled | Same — deployed to rootfs separately |

---

## Part 2: STM32MP1 Packages — Which One and Why

ST provides three software packages. Here is how they compare:

```
    +------------------------------------------------------------------+
    |                                                                   |
    |  STARTER PACKAGE                                                 |
    |  ================                                                |
    |  Pre-built images. Flash and run.                                |
    |  No source code. No customization.                               |
    |  Good for: First-time evaluation.                                |
    |  Bad for: Anything production.                                   |
    |                                                                   |
    +------------------------------------------------------------------+
    |                                                                   |
    |  DEVELOPER PACKAGE  <-- THIS IS WHAT WE USE                      |
    |  =================                                               |
    |  Pre-built SDK (cross-compiler + sysroot).                       |
    |  Source code for TF-A, OP-TEE, U-Boot, Kernel (with ST patches).|
    |  You build individual components, not the whole OS.              |
    |  You start from a working Starter Package image on the board     |
    |  and replace individual binaries (TF-A, U-Boot, kernel, etc.).  |
    |                                                                   |
    |  Good for:                                                       |
    |   - Your setup (already cross-compiling the app separately)     |
    |   - Building and signing boot chain components                   |
    |   - Faster iteration than Yocto                                  |
    |   - Smaller disk and RAM footprint (~2GB vs ~200GB for Yocto)   |
    |                                                                   |
    |  Limits:                                                         |
    |   - Cannot customize rootfs packages (use Yocto for that)       |
    |   - No dm-verity integration (manual setup needed)              |
    |                                                                   |
    +------------------------------------------------------------------+
    |                                                                   |
    |  DISTRIBUTION PACKAGE                                            |
    |  ====================                                            |
    |  Full Yocto/OpenSTLinux build system.                            |
    |  Builds everything from source: bootloaders, kernel, rootfs.     |
    |  Complete control. Reproducible images.                          |
    |                                                                   |
    |  Good for: Final production images with dm-verity, read-only    |
    |  rootfs, custom rootfs content.                                  |
    |  Bad for: Getting started. Requires 200GB+ disk, hours of build.|
    |                                                                   |
    |  Use this AFTER you have secure boot working with Dev Package.  |
    |                                                                   |
    +------------------------------------------------------------------+
```

### 2.1 Why Developer Package First

The Developer Package approach matches your existing workflow:

```
    YOUR CURRENT WORKFLOW              WITH DEVELOPER PACKAGE
    ====================              ======================

    Docker                            Developer Package SDK
    └── arm-linux-gnueabihf-gcc       └── arm-ostl-linux-gnueabi-gcc
        └── Compile your app              ├── Compile your app
                                          ├── Compile TF-A (+ sign)
    SCP to board                          ├── Compile OP-TEE
    └── build/main                        ├── Compile U-Boot
                                          └── Compile kernel
    Board runs stock image
                                     Flash signed images to board
                                     └── STM32CubeProgrammer / dd
```

You keep your Docker setup for application development. You add the Developer Package SDK for building and signing boot chain components.

### 2.2 Roadmap

```
    PHASE 1: Developer Package (this guide)
    =========================================
    Get secure boot working.
    Sign TF-A, OP-TEE, U-Boot, kernel.
    Program OTP fuses.
    Verify the chain works.

              │
              ▼

    PHASE 2: Distribution Package (later, for production)
    =====================================================
    Migrate to Yocto for production images.
    Add dm-verity, read-only rootfs.
    Custom rootfs with your app baked in.
    Automated signing in CI/CD.
```

---

## Part 3: Setting Up the Developer Package

### 3.1 Host Machine Requirements

| Requirement | Minimum | Recommended |
|------------|---------|-------------|
| OS | Ubuntu 20.04 or 22.04 (native Linux) | Ubuntu 22.04 LTS |
| Disk | 10GB free | 20GB free |
| RAM | 4GB | 8GB+ |
| Internet | For initial download (~2GB) | - |

> **Windows users:** Use a Linux VM (VirtualBox, Hyper-V, or WSL2). WSL2 works for the Developer Package (unlike the Distribution Package which has issues with WSL). If using WSL2, store files on the Linux filesystem (`/home/...`), not the Windows mount (`/mnt/c/...`).

### 3.2 Install Host Dependencies

```bash
# On Ubuntu 22.04
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    git \
    wget \
    coreutils \
    python3 python3-pip \
    device-tree-compiler \
    libssl-dev \
    u-boot-tools \
    xxd \
    curl \
    unzip
```

### 3.3 Install STM32CubeProgrammer

This is required for flashing images and programming OTP fuses.

```bash
# Download from: https://www.st.com/en/development-tools/stm32cubeprog.html
# (Requires free ST account)

# Extract and install
unzip en.stm32cubeprog_*.zip -d /tmp/cubeprog
chmod +x /tmp/cubeprog/SetupSTM32CubeProgrammer-*.linux
/tmp/cubeprog/SetupSTM32CubeProgrammer-*.linux

# Default install path: ~/STMicroelectronics/STM32Cube/STM32CubeProgrammer/

# Add tools to PATH
cat >> ~/.bashrc << 'EOF'
export STM32CUBE_PATH="$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer"
export PATH="$STM32CUBE_PATH/bin:$PATH"
EOF
source ~/.bashrc

# Verify the tools you need:
STM32_Programmer_CLI --version
STM32MP_KeyGen_CLI --help        # Key generation tool
STM32MP_SigningTool_CLI --help   # Image signing tool

# Install udev rules (for USB DFU access without sudo)
sudo cp $STM32CUBE_PATH/Drivers/rules/*.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

> **Important:** STM32CubeProgrammer bundles both `STM32MP_KeyGen_CLI` and `STM32MP_SigningTool_CLI`. These are the official ST tools for STM32MP1 secure boot. You do NOT need separate downloads.

### 3.4 Download the Developer Package

Go to [st.com/stm32mp1sw](https://www.st.com/en/embedded-software/stm32mp1dev.html) (requires free ST account).

Download these components:

```
STM32MP1 Developer Package — latest version (e.g., v6.0 / Scarthgap)
    ├── SDK                 (mandatory — cross-compiler + sysroot)
    ├── TF-A sources        (mandatory — to build signed TF-A)
    ├── OP-TEE OS sources   (mandatory — to build OP-TEE for FIP)
    ├── U-Boot sources      (mandatory — to build U-Boot with FIT verify)
    └── Linux kernel sources (mandatory — to build signed FIT kernel)
```

### 3.5 Install the SDK

```bash
# Create workspace
mkdir -p ~/stm32mp1-secure
cd ~/stm32mp1-secure

# Extract SDK tarball
# (filename varies by version — example for v6.0 Scarthgap)
tar xf en.SDK-x86_64-stm32mp1-openstlinux-*.tar.gz

# Run the SDK installer
chmod +x stm32mp1-openstlinux-*/sdk/st-image-*-sdk-*.sh
./stm32mp1-openstlinux-*/sdk/st-image-*-sdk-*.sh -d ~/stm32mp1-secure/sdk

# The installer will ask for a target directory. Use: ~/stm32mp1-secure/sdk

# Source the SDK environment (do this in every new terminal)
source ~/stm32mp1-secure/sdk/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi

# Verify cross-compiler works
$CC --version
# Should show: arm-ostl-linux-gnueabi-gcc (GCC) ...
echo $ARCH
# Should show: arm
```

### 3.6 Extract Boot Chain Sources

```bash
cd ~/stm32mp1-secure

# Extract TF-A sources
tar xf en.SOURCES-tf-a-stm32mp1-*.tar.gz
cd stm32mp1-openstlinux-*/sources/arm-ostl-linux-gnueabi/tf-a-stm32mp-*

# Follow ST's README.HOW_TO.txt to apply patches
# This script sets up git and applies ST-specific patches
cat README.HOW_TO.txt
# Typically:
tar xf tf-a-stm32mp-*.tar.xz
cd tf-a-stm32mp-*
for p in ../*.patch; do git am "$p"; done

# Note the source directory path — you will need it later
TFA_SRC=$(pwd)
echo "TF-A source: $TFA_SRC"

cd ~/stm32mp1-secure

# Extract OP-TEE sources
tar xf en.SOURCES-optee-stm32mp1-*.tar.gz
cd stm32mp1-openstlinux-*/sources/arm-ostl-linux-gnueabi/optee-os-stm32mp-*
tar xf optee-os-stm32mp-*.tar.xz
cd optee-os-stm32mp-*
for p in ../*.patch; do git am "$p"; done
OPTEE_SRC=$(pwd)
echo "OP-TEE source: $OPTEE_SRC"

cd ~/stm32mp1-secure

# Extract U-Boot sources
tar xf en.SOURCES-u-boot-stm32mp1-*.tar.gz
cd stm32mp1-openstlinux-*/sources/arm-ostl-linux-gnueabi/u-boot-stm32mp-*
tar xf u-boot-stm32mp-*.tar.xz
cd u-boot-stm32mp-*
for p in ../*.patch; do git am "$p"; done
UBOOT_SRC=$(pwd)
echo "U-Boot source: $UBOOT_SRC"

cd ~/stm32mp1-secure

# Extract Linux kernel sources
tar xf en.SOURCES-linux-stm32mp1-*.tar.gz
cd stm32mp1-openstlinux-*/sources/arm-ostl-linux-gnueabi/linux-stm32mp-*
tar xf linux-stm32mp-*.tar.xz
cd linux-stm32mp-*
for p in ../*.patch; do git am "$p"; done
KERNEL_SRC=$(pwd)
echo "Kernel source: $KERNEL_SRC"
```

### 3.7 Directory Structure After Setup

```
~/stm32mp1-secure/
├── sdk/                               # Cross-compilation SDK
│   └── environment-setup-*            # Source this for cross-compiler
│
├── sources/                           # Boot chain sources (after extraction)
│   ├── tf-a-stm32mp-*/               # TF-A source with ST patches
│   ├── optee-os-stm32mp-*/           # OP-TEE source with ST patches
│   ├── u-boot-stm32mp-*/             # U-Boot source with ST patches
│   └── linux-stm32mp-*/              # Kernel source with ST patches
│
├── keys/                              # YOUR SIGNING KEYS (create in Part 5)
│   ├── privateKey.pem
│   ├── publicKey.pem
│   └── publicKeyhash.bin
│
├── build/                             # Build outputs (created during build)
│   ├── tf-a/
│   ├── optee/
│   ├── u-boot/
│   ├── kernel/
│   └── fip/
│
└── deploy/                            # Final signed images ready to flash
```

---

## Part 4: Building TF-A, OP-TEE, and U-Boot from Source

### 4.1 Build Order

Components must be built in this order because of dependencies:

```
    1. OP-TEE      (no dependencies)
    2. U-Boot      (no dependencies)
    3. TF-A + FIP  (needs OP-TEE and U-Boot outputs)
    4. Kernel      (needs U-Boot tools for FIT signing)
```

> **Before building:** Source the SDK environment in your terminal:
> ```bash
> source ~/stm32mp1-secure/sdk/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi
> ```

### 4.2 Build OP-TEE

```bash
cd $OPTEE_SRC

# Clean previous builds
make CROSS_COMPILE=arm-ostl-linux-gnueabi- clean

# Build OP-TEE for STM32MP157F-DK2
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLATFORM=stm32mp1 \
    CFG_EMBED_DTB_SOURCE_FILE=stm32mp157f-dk2.dts \
    CFG_TEE_CORE_LOG_LEVEL=3 \
    O=build

# Output files (needed for FIP creation):
ls build/core/
#   tee-header_v2.bin     <-- OP-TEE header
#   tee-pager_v2.bin      <-- OP-TEE pager
#   tee-pageable_v2.bin   <-- OP-TEE pageable part

echo "OP-TEE build complete."
echo "Header:    $OPTEE_SRC/build/core/tee-header_v2.bin"
echo "Pager:     $OPTEE_SRC/build/core/tee-pager_v2.bin"
echo "Pageable:  $OPTEE_SRC/build/core/tee-pageable_v2.bin"
```

**OP-TEE build options for later (production hardening):**

| Option | Dev Value | Prod Value | Purpose |
|--------|----------|------------|---------|
| `CFG_TEE_CORE_LOG_LEVEL` | 3 | 0 | Log verbosity |
| `CFG_RPMB_FS` | n | y | RPMB secure storage |
| `CFG_REE_FS` | y | n | REE filesystem (less secure) |
| `CFG_PKCS11_TA` | n | y | PKCS#11 trusted app |
| `CFG_WITH_HWRNG` | y | y | Hardware RNG |
| `CFG_STM32_CRYP` | y | y | Hardware crypto |

### 4.3 Build U-Boot

```bash
cd $UBOOT_SRC

# Clean
make CROSS_COMPILE=arm-ostl-linux-gnueabi- mrproper

# Configure for STM32MP157F-DK2 (trusted boot with OP-TEE)
make CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    DEVICE_TREE=stm32mp157f-dk2 \
    stm32mp15_trusted_defconfig

# Build
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    DEVICE_TREE=stm32mp157f-dk2 \
    all

# Output files (needed for FIP creation):
echo "U-Boot binary:  $UBOOT_SRC/u-boot-nodtb.bin"
echo "U-Boot DTB:     $UBOOT_SRC/u-boot.dtb"
```

**To enable FIT signature verification in U-Boot**, add these to the config before building:

```bash
# Create a config fragment for FIT verification
cat > fit_verify.cfg << 'EOF'
CONFIG_FIT=y
CONFIG_FIT_SIGNATURE=y
CONFIG_FIT_VERBOSE=y
CONFIG_RSA=y
CONFIG_RSA_VERIFY=y
EOF

# Merge into the defconfig
cd $UBOOT_SRC
scripts/kconfig/merge_config.sh \
    .config fit_verify.cfg

# Rebuild
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    DEVICE_TREE=stm32mp157f-dk2 \
    all
```

### 4.4 Build TF-A and Create the FIP

This is the central step. TF-A build also creates the FIP (Firmware Image Package) that bundles OP-TEE + U-Boot together.

**Step 1: Build TF-A without signing (verify it works first)**

```bash
cd $TFA_SRC

# Clean
make realclean

# Build TF-A (FSBL) + FIP (unsigned, for initial testing)
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 \
    ARCH=aarch32 \
    ARM_ARCH_MAJOR=7 \
    STM32MP15=1 \
    DTB_FILE_NAME=stm32mp157f-dk2.dtb \
    STM32MP_EMMC=1 \
    AARCH32_SP=optee \
    BL32=$OPTEE_SRC/build/core/tee-header_v2.bin \
    BL32_EXTRA1=$OPTEE_SRC/build/core/tee-pager_v2.bin \
    BL32_EXTRA2=$OPTEE_SRC/build/core/tee-pageable_v2.bin \
    BL33=$UBOOT_SRC/u-boot-nodtb.bin \
    BL33_CFG=$UBOOT_SRC/u-boot.dtb \
    all fip

# Output files:
echo "TF-A (FSBL): $TFA_SRC/build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32"
echo "FIP:         $TFA_SRC/build/stm32mp1/release/fip.bin"
```

**Step 2: Build TF-A with Trusted Board Boot (signed)**

```bash
cd $TFA_SRC

make realclean

# Build with TRUSTED_BOARD_BOOT enabled
# This requires mbedTLS for certificate handling
# and your ROT key (created in Part 5)
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 \
    ARCH=aarch32 \
    ARM_ARCH_MAJOR=7 \
    STM32MP15=1 \
    DTB_FILE_NAME=stm32mp157f-dk2.dtb \
    STM32MP_EMMC=1 \
    AARCH32_SP=optee \
    BL32=$OPTEE_SRC/build/core/tee-header_v2.bin \
    BL32_EXTRA1=$OPTEE_SRC/build/core/tee-pager_v2.bin \
    BL32_EXTRA2=$OPTEE_SRC/build/core/tee-pageable_v2.bin \
    BL33=$UBOOT_SRC/u-boot-nodtb.bin \
    BL33_CFG=$UBOOT_SRC/u-boot.dtb \
    TRUSTED_BOARD_BOOT=1 \
    MBEDTLS_DIR=/path/to/mbedtls \
    GENERATE_COT=1 \
    ROT_KEY=~/stm32mp1-secure/keys/privateKey.pem \
    all fip

echo "Signed TF-A and FIP built successfully."
```

> **Note:** If you don't have mbedTLS, see Part 6 for the alternative approach of signing images separately using `STM32MP_SigningTool_CLI`.

### 4.5 Build the Linux Kernel

```bash
cd $KERNEL_SRC

# Configure for STM32MP157
make ARCH=arm \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    multi_v7_defconfig fragment*.config

# Build
make -j$(nproc) ARCH=arm \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    zImage dtbs modules

# Output files:
echo "Kernel:  $KERNEL_SRC/arch/arm/boot/zImage"
echo "DTB:     $KERNEL_SRC/arch/arm/boot/dts/stm32mp157f-dk2.dtb"
```

---

## Part 5: Key Generation

### 5.1 Overview: Keys You Need

```
    KEY 1: ECDSA P-256 (for ROM → TF-A authentication)
    ====================================================
    Generated by: STM32MP_KeyGen_CLI (ST's official tool)
    Files produced:
        privateKey.pem     — Signs TF-A image
        publicKey.pem      — Embedded in TF-A header
        publicKeyhash.bin  — Burned into OTP fuses (SHA-256 of public key)

    Used by: STM32MP1 Boot ROM
    Algorithm: ECDSA with prime256v1 (P-256) — NOT configurable

    THIS IS YOUR ROOT OF TRUST. PROTECT THE PRIVATE KEY.


    KEY 2: RSA-2048 (for U-Boot → Kernel authentication)
    =====================================================
    Generated by: OpenSSL
    Files produced:
        fit_sign.key       — Signs FIT image (kernel + DTB)
        fit_sign.crt       — Embedded in U-Boot DTB

    Used by: U-Boot FIT image verification
    Algorithm: RSA-2048 with SHA-256
```

### 5.2 Generate ECDSA Key Pair (ROT Key) Using ST Tool

```bash
mkdir -p ~/stm32mp1-secure/keys
cd ~/stm32mp1-secure/keys

# Generate ECDSA P-256 key pair using ST's official tool
STM32MP_KeyGen_CLI \
    --absolute-path "$(pwd)" \
    --password "your-secure-password-here"

# The tool generates three files:
#   privateKey.pem      — ECDSA P-256 private key (encrypted with your password)
#   publicKey.pem       — Corresponding public key
#   publicKeyhash.bin   — SHA-256 hash of public key (32 bytes, for OTP)

# Verify the files were created
ls -la
# -rw-r--r-- 1 user user  311 ... privateKey.pem
# -rw-r--r-- 1 user user  178 ... publicKey.pem
# -rw-r--r-- 1 user user   32 ... publicKeyhash.bin

# Display the public key hash (these values go into OTP fuses)
echo "=== Public Key Hash for OTP Programming ==="
xxd -p publicKeyhash.bin | fold -w 8 | nl -v 0 | \
    awk '{printf "  OTP Word %d: 0x%s\n", $1, $2}'
```

**Example output (YOUR VALUES WILL BE DIFFERENT):**

```
=== Public Key Hash for OTP Programming ===
  OTP Word 0: 0x4e31bbcd
  OTP Word 1: 0x51e827dd
  OTP Word 2: 0x3511f521
  OTP Word 3: 0xfd9c11a2
  OTP Word 4: 0x5b997b82
  OTP Word 5: 0x8150adc5
  OTP Word 6: 0xa9c68fa9
  OTP Word 7: 0x72a3ba74
```

> **Record these values now.** You will need them for OTP programming. Write them down on paper and verify them twice.

> **CRITICAL — Backup the private key:**
> 1. Copy to an encrypted USB drive stored in a physical safe
> 2. Copy to a password manager's secure notes
> 3. Remember the password — without it, the key is useless
>
> If you lose `privateKey.pem` or forget the password, and you have already programmed OTP fuses, every fused device becomes a permanent brick.

### 5.3 Alternative: Generate ROT Key with OpenSSL

If you prefer unencrypted keys (simpler for development, NOT for production):

```bash
cd ~/stm32mp1-secure/keys

# Generate ECDSA P-256 key pair (unencrypted)
openssl ecparam -name prime256v1 -genkey -noout -out privateKey_noenc.pem
openssl ec -in privateKey_noenc.pem -pubout -out publicKey.pem

# Compute public key hash for OTP programming
openssl ec -in privateKey_noenc.pem -pubout -outform DER 2>/dev/null | \
    openssl dgst -sha256 -binary > publicKeyhash.bin

# Display as OTP words
echo "=== Public Key Hash for OTP Programming ==="
xxd -p publicKeyhash.bin | fold -w 8 | nl -v 0 | \
    awk '{printf "  OTP Word %d: 0x%s\n", $1, $2}'
```

### 5.4 Generate RSA Key Pair (FIT Signing Key)

```bash
cd ~/stm32mp1-secure/keys

# Generate RSA-2048 key pair for FIT image signing
openssl genrsa -out fit_sign.key 2048

# Generate self-signed certificate (U-Boot needs the certificate format)
openssl req -batch -new -x509 \
    -key fit_sign.key \
    -out fit_sign.crt \
    -days 7300 \
    -subj "/CN=SmartRTU FIT Signing Key/O=YourCompany"

# Verify
openssl x509 -in fit_sign.crt -text -noout | head -15
```

### 5.5 Key Summary

After this part, you should have:

```
~/stm32mp1-secure/keys/
├── privateKey.pem         # ECDSA P-256 private key (PROTECT — encrypted)
├── privateKey_noenc.pem   # ECDSA P-256 private key (PROTECT — unencrypted, dev only)
├── publicKey.pem          # ECDSA P-256 public key
├── publicKeyhash.bin      # SHA-256 hash (32 bytes) → goes into OTP fuses
├── fit_sign.key           # RSA-2048 private key (PROTECT)
└── fit_sign.crt           # RSA-2048 certificate → embedded in U-Boot DTB
```

### 5.6 Production: Use an HSM

For production, private keys must never exist as files on a general-purpose computer.

| HSM | Price | Interface | Recommendation |
|-----|-------|-----------|---------------|
| Nitrokey HSM 2 | ~$70 | USB PKCS#11 | Good for small teams |
| YubiHSM 2 | ~$650 | USB PKCS#11 | Better for larger teams |
| AWS CloudHSM | ~$1.50/hr | Network PKCS#11 | Good for CI/CD pipelines |

The signing tools support PKCS#11 URIs in place of file paths. This means the private key is generated inside the HSM and never leaves it — only the HSM can perform signing operations.

---

## Part 6: Signing the Boot Images

### 6.1 Two Signing Approaches

```
    APPROACH A: Sign during TF-A build (GENERATE_COT=1)
    ====================================================
    TF-A build process generates certificates and signs everything.
    Requires: mbedTLS source code, ROT key as file.
    Best for: CI/CD pipeline, automated builds.

    APPROACH B: Sign separately with STM32MP_SigningTool_CLI
    ========================================================
    Build TF-A first (unsigned), then sign the .stm32 binary.
    Requires: STM32CubeProgrammer tools installed.
    Best for: Getting started, manual workflow, HSM signing.

    THIS GUIDE COVERS BOTH. Start with Approach B (simpler).
```

### 6.2 Approach B: Sign with STM32MP_SigningTool_CLI (Recommended Start)

**Step 1: Build TF-A without signing (already done in Part 4.4 Step 1)**

```bash
# You should already have an unsigned TF-A binary at:
ls $TFA_SRC/build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32
```

**Step 2: Sign the TF-A binary**

```bash
cd ~/stm32mp1-secure

# Sign TF-A (FSBL) using ST's signing tool
STM32MP_SigningTool_CLI \
    -bin $TFA_SRC/build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32 \
    -pubk ~/stm32mp1-secure/keys/publicKey.pem \
    -prvk ~/stm32mp1-secure/keys/privateKey.pem \
    -pwd "your-secure-password-here" \
    -t fsbl \
    -o ~/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32

# Parameters explained:
#   -bin   : Input binary (.stm32 format)
#   -pubk  : Public key to embed in the image header
#   -prvk  : Private key for signing
#   -pwd   : Password for encrypted private key
#   -t     : Image type: "fsbl" for TF-A
#   -o     : Output signed binary
```

> If using an unencrypted private key (`privateKey_noenc.pem`), omit the `-pwd` parameter.

**Step 3: Verify the signed image**

```bash
# The signing tool modifies the STM32 header to include:
# - The ECDSA public key
# - The ECDSA signature over the image

# Compare file sizes (signed should be similar, header updated)
ls -la $TFA_SRC/build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32
ls -la ~/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32

# Inspect headers
hexdump -C ~/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32 | head -30
# Look for "STM32" magic and non-zero bytes in the signature area
```

### 6.3 Approach A: Sign During TF-A Build (GENERATE_COT)

This approach integrates signing into the build process. It requires mbedTLS source.

**Step 1: Get mbedTLS**

```bash
cd ~/stm32mp1-secure

# Clone mbedTLS (version compatible with your TF-A)
git clone https://github.com/Mbed-TLS/mbedtls.git -b v2.28.8
MBEDTLS_DIR=$(pwd)/mbedtls
```

**Step 2: Build TF-A with signing**

```bash
cd $TFA_SRC
make realclean

make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 \
    ARCH=aarch32 \
    ARM_ARCH_MAJOR=7 \
    STM32MP15=1 \
    DTB_FILE_NAME=stm32mp157f-dk2.dtb \
    STM32MP_EMMC=1 \
    AARCH32_SP=optee \
    BL32=$OPTEE_SRC/build/core/tee-header_v2.bin \
    BL32_EXTRA1=$OPTEE_SRC/build/core/tee-pager_v2.bin \
    BL32_EXTRA2=$OPTEE_SRC/build/core/tee-pageable_v2.bin \
    BL33=$UBOOT_SRC/u-boot-nodtb.bin \
    BL33_CFG=$UBOOT_SRC/u-boot.dtb \
    TRUSTED_BOARD_BOOT=1 \
    MBEDTLS_DIR=$MBEDTLS_DIR \
    GENERATE_COT=1 \
    ROT_KEY=~/stm32mp1-secure/keys/privateKey_noenc.pem \
    all fip

# This produces:
# - Signed TF-A (FSBL): build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32
# - Signed FIP:          build/stm32mp1/release/fip.bin
#   (FIP contains OP-TEE + U-Boot with certificate chain)
```

**What `GENERATE_COT=1` does internally:**

```
    TF-A Build Process with GENERATE_COT=1
    ========================================

    1. Generates certificate chain using ROT_KEY:
       ┌─────────────────────────────────────┐
       │  ROT Key (your privateKey.pem)       │
       │         │                            │
       │         ├── Trusted Key Certificate  │
       │         │         │                  │
       │         │         ├── OP-TEE Key Cert│
       │         │         │   └── OP-TEE     │
       │         │         │       Content Cert│
       │         │         │                  │
       │         │         └── U-Boot Key Cert│
       │         │             └── U-Boot     │
       │         │                 Content Cert│
       │         │                            │
       │         └── TF-A Content Certificate │
       └─────────────────────────────────────┘

    2. Packages certificates + binaries into FIP

    3. TF-A BL2 code verifies FIP certificates
       before loading OP-TEE and U-Boot
```

### 6.4 Signing the FIP Separately (If Using Approach B)

If you built TF-A without `GENERATE_COT=1`, the FIP is unsigned. The TF-A will load the FIP contents but won't verify their integrity. For full chain-of-trust, you need either:

1. Use Approach A (`GENERATE_COT=1`) — recommended
2. Sign the TF-A only, and rely on U-Boot FIT verification for the kernel

For initial development, signing just TF-A (Approach B) + FIT image verification is a good starting point. Add `GENERATE_COT` later when you're comfortable with the process.

### 6.5 What Gets Signed — Summary

| Image | Signed By | Verified By | Signing Method |
|-------|-----------|-------------|---------------|
| TF-A (FSBL) `.stm32` | Your ECDSA P-256 key | Boot ROM (using OTP hash) | `STM32MP_SigningTool_CLI` or build-time |
| FIP (OP-TEE + U-Boot) | Certificate chain from ROT key | TF-A BL2 | `GENERATE_COT=1` during TF-A build |
| FIT Image (Kernel + DTB) | Your RSA-2048 key | U-Boot | `mkimage` with `-k` flag |

---

## Part 7: Flashing and Testing (Development Mode)

### 7.1 Development Testing Strategy

In development mode, you test the signed boot chain **without programming OTP fuses**:

```
    DEVELOPMENT MODE — What gets verified:
    =======================================

    ROM ──(NO check)──> TF-A ──(verified if COT)──> OP-TEE + U-Boot
                                                              │
                                                    (verified)│
                                                              ▼
                                                        Kernel (FIT)

    The ROM→TF-A link is NOT verified (no key hash in OTP).
    TF-A→FIP is verified only if you used GENERATE_COT=1.
    U-Boot→Kernel is verified if FIT signing is configured.

    You can always recover by re-flashing any image.
```

### 7.2 Flash to eMMC via USB DFU

**Put the board in USB DFU mode:**
1. Set boot switches on the DK2: **BOOT0=0**, **BOOT2=1** (USB/UART boot)
2. Connect USB-C cable (the OTG port, not the ST-Link debug port)
3. Power on or press reset
4. The board should enumerate as a DFU device

```bash
# Verify USB DFU connection
STM32_Programmer_CLI -c port=USB1 -l

# Flash signed TF-A to eMMC boot partitions
# Boot partition 1:
STM32_Programmer_CLI -c port=USB1 \
    -w ~/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32 \
    0x0

# Flash FIP (OP-TEE + U-Boot) to eMMC user area
STM32_Programmer_CLI -c port=USB1 \
    -w $TFA_SRC/build/stm32mp1/release/fip.bin \
    0x00040000
```

**Alternative: Flash using ST's FlashLayout TSV file:**

ST provides `.tsv` layout files that describe where each partition goes on eMMC. This is more reliable for complete re-flashing.

```bash
# Use the TSV layout from the Starter Package
STM32_Programmer_CLI -c port=USB1 \
    -w FlashLayout_emmc_stm32mp157f-dk2-trusted.tsv
```

### 7.3 Set Boot Switches for Normal Boot

After flashing, change the boot switches for eMMC boot:
- **BOOT0=1**, **BOOT2=0** (eMMC boot)
- Press reset or power cycle

### 7.4 Monitor Boot via UART

Connect to the board's serial console (ST-Link USB virtual COM port):

```bash
# Linux:
minicom -D /dev/ttyACM0 -b 115200

# Windows: Use PuTTY / Tera Term on the ST-Link COM port (115200 baud)
```

**Expected output on successful signed boot:**

```
NOTICE:  CPU: STM32MP157FAC Rev.Z
NOTICE:  Model: STMicroelectronics STM32MP157F-DK2
NOTICE:  Board: MB1272 Var2.0 Rev.C-01
NOTICE:  BL2: v2.8(release):v2.8-stm32mp1-r1
NOTICE:  BL2: Built : 10:30:00, Sep 08 2026
NOTICE:  BL2: Booting BL32
NOTICE:  BL32: OP-TEE
I/TC: OP-TEE version: 3.19.0-stm32mp1-r1
NOTICE:  BL2: Booting BL33
...
U-Boot 2023.10 (Sep 08 2026)

Hit any key to stop autoboot: 0
...
Starting kernel ...
[    0.000000] Booting Linux on physical CPU 0x0
```

If you used `GENERATE_COT=1`, you should also see:

```
NOTICE:  BL2: Boot authentication Success
```

### 7.5 Negative Testing (CRITICAL)

You **must** verify that tampered and unsigned images are rejected before proceeding to OTP programming.

**Test 1: Tamper with TF-A (only meaningful after OTP fusing — skip for now)**

**Test 2: Tamper with FIT image (if FIT signing is enabled)**

```bash
# Create a tampered FIT image
cp fitImage fitImage.tampered
printf '\xFF' | dd of=fitImage.tampered bs=1 seek=1000 count=1 conv=notrunc

# Flash the tampered FIT and try to boot
# Expected: U-Boot prints "Bad hash" or "Signature check FAILED"
# and refuses to load the kernel

# RESTORE the correct image after testing!
```

---

## Part 8: OTP Fuse Programming (Production)

> **WARNING: This section contains IRREVERSIBLE operations.**
> Read the entire section before executing any command.
> Use a sacrificial test board for your first attempt.

### 8.1 What the OTP Fuses Do

```
    BEFORE OTP PROGRAMMING              AFTER OTP PROGRAMMING
    ======================              =====================

    ROM boots ANY TF-A                  ROM ONLY boots TF-A
    (signed or unsigned)                signed with YOUR key

    You can always recover              If key hash is wrong
    by re-flashing                      → DEVICE IS BRICKED

    Device is OPEN                      Device is CLOSED
    (development mode)                  (production mode)
```

### 8.2 Two Methods for OTP Programming

**Method 1: Via U-Boot console (`stm32key` commands) — RECOMMENDED**

Simpler, interactive, you can verify before committing.

**Method 2: Via STM32CubeProgrammer (external tool)**

Can be scripted for factory automation.

### 8.3 Method 1: OTP via U-Boot Console

**Step 1: Copy `publicKeyhash.bin` to the board**

```bash
# Copy the hash file to the board's rootfs or SD card
# Option A: via SCP (if board has network)
scp ~/stm32mp1-secure/keys/publicKeyhash.bin root@192.168.1.24:/home/root/

# Option B: put it on the SD card's ext4 partition
# Mount the SD card on your host and copy the file
```

**Step 2: Boot the board and stop at U-Boot prompt**

Press any key during the "Hit any key to stop autoboot" countdown.

**Step 3: Load and inspect the key hash**

```
STM32MP> # Load publicKeyhash.bin into RAM
STM32MP> load mmc 0:4 0xc0000000 publicKeyhash.bin
32 bytes read in 1 ms

STM32MP> # Read and display the hash values
STM32MP> stm32key read 0xc0000000

OTP value 24: 0x4e31bbcd
OTP value 25: 0x51e827dd
OTP value 26: 0x3511f521
OTP value 27: 0xfd9c11a2
OTP value 28: 0x5b997b82
OTP value 29: 0x8150adc5
OTP value 30: 0xa9c68fa9
OTP value 31: 0x72a3ba74
```

> **VERIFY:** Compare these values with what you recorded in Part 5.2. They must match EXACTLY. If they don't match, STOP — do not proceed.

> **Note:** The OTP word numbers may vary by silicon revision. Some revisions use words 24-31, others use 32-39. The `stm32key` command handles this automatically.

**Step 4: Fuse the key hash (IRREVERSIBLE)**

```
STM32MP> # THIS IS IRREVERSIBLE
STM32MP> # After this command, the key hash is permanently written to OTP
STM32MP> stm32key fuse 0xc0000000

Warning: OTP fuse programming is IRREVERSIBLE
Are you sure? [y/N] y

OTP value 24: 0x4e31bbcd
OTP value 25: 0x51e827dd
OTP value 26: 0x3511f521
OTP value 27: 0xfd9c11a2
OTP value 28: 0x5b997b82
OTP value 29: 0x8150adc5
OTP value 30: 0xa9c68fa9
OTP value 31: 0x72a3ba74
```

> The `-y` flag (e.g., `stm32key fuse -y 0xc0000000`) skips the confirmation prompt. Do NOT use `-y` on your first device.

**Step 5: Verify the fused values**

```
STM32MP> stm32key read 0xc0000000

# Should show the same values you just fused
# Compare each word carefully
```

**Step 6: Test before closing the device**

At this point, the key hash is in OTP but secure boot is NOT enforced. The ROM still boots unsigned images. This is intentional — it lets you verify everything works before making it permanent.

```
STM32MP> # Reboot and verify the signed TF-A boots correctly
STM32MP> reset
```

Verify normal boot through UART. If the signed TF-A boots correctly, proceed to Step 7.

**Step 7: Close the device (IRREVERSIBLE — enforces secure boot)**

```
STM32MP> # THIS IS IRREVERSIBLE
STM32MP> # After this command, the ROM will ONLY boot signed TF-A images
STM32MP> # whose public key hash matches the fused value.
STM32MP> #
STM32MP> # If your signed TF-A doesn't boot, the device is BRICKED.
STM32MP> #
STM32MP> # Make sure you have:
STM32MP> # 1. Verified the signed TF-A boots correctly (Step 6)
STM32MP> # 2. Backed up your private key
STM32MP> # 3. Tested on a sacrificial board first
STM32MP>
STM32MP> stm32key close

Warning: Secure boot closure is IRREVERSIBLE
The device will only boot images signed with the fused key
Are you sure? [y/N] y
```

> **What `stm32key close` does:** Sets bit 6 of OTP word 0 (`BOOT_SEC_EN`). This single bit tells the ROM to enforce authentication. Once set, it cannot be cleared.

**Step 8: Verify secure boot is enforced**

```bash
# Reboot the device
# It should boot normally with the signed TF-A

# Now test with an UNSIGNED TF-A:
# 1. Flash an unsigned TF-A binary via USB DFU
# 2. Try to boot
# 3. Expected: NO output on UART — ROM silently rejects the image
# 4. Re-flash the signed TF-A to restore boot
```

### 8.4 Method 2: OTP via STM32CubeProgrammer

For factory automation, use STM32CubeProgrammer from the host:

```bash
# Connect board via USB DFU (BOOT0=0, BOOT2=1)

# Read current OTP state
STM32_Programmer_CLI -c port=USB1 -otp displ

# Program key hash (word numbers depend on silicon revision)
# Example for words 24-31:
STM32_Programmer_CLI -c port=USB1 -otp write word=24 value=0x4e31bbcd
STM32_Programmer_CLI -c port=USB1 -otp write word=25 value=0x51e827dd
STM32_Programmer_CLI -c port=USB1 -otp write word=26 value=0x3511f521
STM32_Programmer_CLI -c port=USB1 -otp write word=27 value=0xfd9c11a2
STM32_Programmer_CLI -c port=USB1 -otp write word=28 value=0x5b997b82
STM32_Programmer_CLI -c port=USB1 -otp write word=29 value=0x8150adc5
STM32_Programmer_CLI -c port=USB1 -otp write word=30 value=0xa9c68fa9
STM32_Programmer_CLI -c port=USB1 -otp write word=31 value=0x72a3ba74

# Verify
STM32_Programmer_CLI -c port=USB1 -otp displ

# Enable secure boot (close device)
# This sets bit 6 of OTP word 0
STM32_Programmer_CLI -c port=USB1 -otp write word=0 value=0x00000040
```

### 8.5 Detailed Provisioning Script (For Factory)

```bash
#!/bin/bash
# ==============================================================
# provision_device.sh — Factory OTP provisioning
# ==============================================================
# WARNING: Programs IRREVERSIBLE OTP fuses.
# Test on a sacrificial board before batch production.
# ==============================================================

set -euo pipefail

# ---- Configuration ----
KEY_HASH_FILE="$HOME/stm32mp1-secure/keys/publicKeyhash.bin"
SIGNED_TFA="$HOME/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32"
SIGNED_FIP="$HOME/stm32mp1-secure/deploy/fip.bin"
PORT="USB1"

# Read the expected hash values from the binary file
EXPECTED_HASH=$(xxd -p "$KEY_HASH_FILE" | tr -d '\n')
echo "Expected key hash: $EXPECTED_HASH"

echo ""
echo "============================================================"
echo "  STM32MP1 SECURE BOOT PROVISIONING"
echo "  THIS PROGRAMS IRREVERSIBLE OTP FUSES"
echo "============================================================"
echo ""
read -p "Connect board in DFU mode and type 'READY': " confirm
[ "$confirm" != "READY" ] && echo "Aborted." && exit 1

# ---- Step 1: Flash signed images ----
echo ""
echo "[1/4] Flashing signed boot images..."
STM32_Programmer_CLI -c port=$PORT \
    -w "$SIGNED_TFA" 0x0

STM32_Programmer_CLI -c port=$PORT \
    -w "$SIGNED_FIP" 0x00040000

echo "  Signed images flashed."

# ---- Step 2: Verify boot (manual) ----
echo ""
echo "[2/4] Manual verification required:"
echo "  1. Set boot switches to eMMC (BOOT0=1, BOOT2=0)"
echo "  2. Power cycle the board"
echo "  3. Check UART output — board should boot normally"
echo "  4. Put board back in DFU mode (BOOT0=0, BOOT2=1)"
echo ""
read -p "Did the signed image boot successfully? (yes/no): " boot_ok
[ "$boot_ok" != "yes" ] && echo "Aborting. Do NOT proceed with OTP." && exit 1

# ---- Step 3: Program OTP key hash ----
echo ""
echo "[3/4] Programming OTP key hash..."

# Split hash into 32-bit words
for i in $(seq 0 7); do
    offset=$((i * 8))
    word_val="0x${EXPECTED_HASH:$offset:8}"
    otp_word=$((24 + i))   # Adjust base word for your silicon revision!
    echo "  OTP word $otp_word = $word_val"
    STM32_Programmer_CLI -c port=$PORT \
        -otp write word=$otp_word value=$word_val
done

echo "  Key hash programmed. Verifying..."

# Read back and verify
for i in $(seq 0 7); do
    offset=$((i * 8))
    expected="0x${EXPECTED_HASH:$offset:8}"
    otp_word=$((24 + i))
    actual=$(STM32_Programmer_CLI -c port=$PORT \
        -otp displ word=$otp_word 2>&1 | grep -oP 'Value\s*:\s*\K0x[0-9a-fA-F]+' || echo "ERROR")

    if [ "$actual" != "$expected" ]; then
        echo "  MISMATCH at word $otp_word: expected $expected, got $actual"
        echo "  DO NOT CLOSE THE DEVICE. Investigate."
        exit 1
    fi
done
echo "  Key hash verified."

# ---- Step 4: Close device ----
echo ""
echo "[4/4] Closing device (enabling secure boot)..."
echo "  WARNING: This is the final irreversible step."
read -p "  Type 'CLOSE DEVICE' to proceed: " close_confirm
[ "$close_confirm" != "CLOSE DEVICE" ] && echo "Aborted. Key hash is programmed but device is NOT closed." && exit 0

STM32_Programmer_CLI -c port=$PORT \
    -otp write word=0 value=0x00000040

echo ""
echo "============================================================"
echo "  PROVISIONING COMPLETE"
echo "============================================================"
echo "  Device is now locked to your signing key."
echo "  Verify: boot from eMMC, then test unsigned image rejection."
echo "============================================================"
```

---

## Part 9: Verified Kernel Boot (FIT Image)

### 9.1 What Is a FIT Image?

A FIT (Flattened Image Tree) bundles the kernel, device tree, and an RSA signature into a single file:

```
    fitImage (single binary)
    ========================
    ┌──────────────────────────────┐
    │  FIT Header                  │
    ├──────────────────────────────┤
    │  images/                     │
    │  ├── kernel-1                │
    │  │   └── zImage + sha256     │
    │  └── fdt-1                   │
    │      └── .dtb + sha256       │
    ├──────────────────────────────┤
    │  configurations/             │
    │  └── conf-1                  │
    │      ├── kernel = kernel-1   │
    │      ├── fdt = fdt-1         │
    │      └── RSA-2048 signature  │
    │          over sha256 hashes  │
    └──────────────────────────────┘

    U-Boot has the RSA public key (fit_sign.crt)
    embedded in its device tree blob.
    It verifies the signature before booting.
```

### 9.2 Create the FIT Image

**Step 1: Create the ITS (Image Tree Source) file**

```bash
cd ~/stm32mp1-secure/build

cat > kernel.its << 'EOF'
/dts-v1/;

/ {
    description = "Smart RTU Signed Kernel Image";
    #address-cells = <1>;

    images {
        kernel-1 {
            description = "Linux kernel";
            data = /incbin/("zImage");
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            load = <0xC2000040>;
            entry = <0xC2000040>;
            hash-1 {
                algo = "sha256";
            };
        };

        fdt-1 {
            description = "STM32MP157F-DK2 Device Tree";
            data = /incbin/("stm32mp157f-dk2.dtb");
            type = "flat_dt";
            arch = "arm";
            compression = "none";
            hash-1 {
                algo = "sha256";
            };
        };
    };

    configurations {
        default = "conf-1";
        conf-1 {
            description = "STM32MP157F-DK2 boot configuration";
            kernel = "kernel-1";
            fdt = "fdt-1";
            signature-1 {
                algo = "sha256,rsa2048";
                key-name-hint = "fit_sign";
                sign-images = "kernel", "fdt";
            };
        };
    };
};
EOF
```

**Step 2: Copy kernel and DTB**

```bash
cp $KERNEL_SRC/arch/arm/boot/zImage ~/stm32mp1-secure/build/
cp $KERNEL_SRC/arch/arm/boot/dts/stm32mp157f-dk2.dtb ~/stm32mp1-secure/build/
```

**Step 3: Create and sign the FIT image**

```bash
cd ~/stm32mp1-secure/build

# Create signed FIT image
# -k : directory containing fit_sign.key and fit_sign.crt
# -K : U-Boot DTB to embed the public key into
# -r : mark key as "required" (U-Boot enforces signature)
mkimage \
    -f kernel.its \
    -k ~/stm32mp1-secure/keys \
    -K $UBOOT_SRC/u-boot.dtb \
    -r \
    fitImage

# Verify the FIT image
mkimage -l fitImage
# Look for:
#   Sign algo:   sha256,rsa2048:fit_sign
#   Sign value:  <hex data>
#   Timestamp:   <date>
```

> **Important:** The `-K $UBOOT_SRC/u-boot.dtb` flag modifies U-Boot's DTB to embed the FIT signing public key. After this, you must **rebuild the FIP** with the updated U-Boot DTB so that the on-device U-Boot has the key.

**Step 4: Rebuild FIP with updated U-Boot DTB**

```bash
cd $TFA_SRC
make realclean

# Rebuild TF-A + FIP (same command as before, but U-Boot DTB now has FIT key)
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 \
    ARCH=aarch32 \
    ARM_ARCH_MAJOR=7 \
    STM32MP15=1 \
    DTB_FILE_NAME=stm32mp157f-dk2.dtb \
    STM32MP_EMMC=1 \
    AARCH32_SP=optee \
    BL32=$OPTEE_SRC/build/core/tee-header_v2.bin \
    BL32_EXTRA1=$OPTEE_SRC/build/core/tee-pager_v2.bin \
    BL32_EXTRA2=$OPTEE_SRC/build/core/tee-pageable_v2.bin \
    BL33=$UBOOT_SRC/u-boot-nodtb.bin \
    BL33_CFG=$UBOOT_SRC/u-boot.dtb \
    all fip

# Re-sign TF-A if using Approach B
STM32MP_SigningTool_CLI \
    -bin build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32 \
    -pubk ~/stm32mp1-secure/keys/publicKey.pem \
    -prvk ~/stm32mp1-secure/keys/privateKey.pem \
    -pwd "your-secure-password-here" \
    -t fsbl \
    -o ~/stm32mp1-secure/deploy/tf-a-stm32mp157f-dk2-signed.stm32
```

### 9.3 Flash and Test

```bash
# Flash everything: signed TF-A, updated FIP, signed FIT
# (via USB DFU or dd to eMMC)

# Boot and check UART for:
#   Verifying Hash Integrity ... sha256+ OK
#   ## Loading kernel from FIT Image
#   Verifying Hash Integrity ... sha256,rsa2048:fit_sign+ OK
```

---

## Part 10: Anti-Rollback Protection

### 10.1 How It Works

```
    Anti-rollback uses a monotonic counter in OTP fuses.
    The counter can only go UP, never down.

    Example:
    ┌──────────┬─────────┬─────────────┬─────────────────────┐
    │ Release  │ Version │ OTP Counter │ Boots Successfully? │
    ├──────────┼─────────┼─────────────┼─────────────────────┤
    │ v1.0     │   1     │      0      │ Yes                 │
    │ v1.1     │   2     │      0      │ Yes                 │
    │ v2.0     │   3     │      1      │ Yes (counter bumped)│
    │ v1.1     │   2     │      1      │ NO (2 < 1, rollback)│
    │ v2.1     │   4     │      1      │ Yes                 │
    │ v3.0     │   5     │      2      │ Yes (counter bumped)│
    │ v2.0     │   3     │      2      │ NO (3 < 2, rollback)│
    └──────────┴─────────┴─────────────┴─────────────────────┘
```

### 10.2 OTP Counter Limitations

- Each OTP word is 32 bits = **32 maximum increments** per counter
- STM32MP1 provides 2 counters:
  - `--tfw-nvctr` : TF-A version counter
  - `--ntfw-nvctr` : Non-trusted firmware version counter
- Increment sparingly — only for security-critical updates

### 10.3 Building with Anti-Rollback

```bash
# Set the version counter during TF-A build
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 \
    ... \
    TRUSTED_BOARD_BOOT=1 \
    GENERATE_COT=1 \
    ROT_KEY=~/stm32mp1-secure/keys/privateKey_noenc.pem \
    STM32_TF_VERSION=1 \
    all fip

# STM32_TF_VERSION=N sets the monotonic counter value in the image.
# TF-A compares this against the OTP counter at boot time.
```

### 10.4 Incrementing the OTP Counter

```bash
# Via U-Boot (after confirming new firmware works):
STM32MP> stm32key fuse_version 1

# Or via STM32CubeProgrammer:
# (Counter is typically in OTP word 4 for TF-A version)
STM32_Programmer_CLI -c port=USB1 -otp write word=4 value=0x00000001
```

> **WARNING:** Only increment the counter after you have verified the new firmware boots and runs correctly. If you increment the counter and then need to roll back, those devices can never boot the older firmware.

### 10.5 Recommended Increment Strategy

| When to Increment OTP | When NOT to Increment |
|------------------------|-----------------------|
| Critical security patches (CVE fixes) | Minor feature updates |
| Major version releases (v1→v2→v3) | Patch releases (v1.0→v1.1→v1.2) |
| After thorough field testing | During development |
| Maximum 2-3 times per year | On every build |

---

## Part 11: Recovery Strategy and A/B Boot

### 11.1 Boot Recovery Flow

```
    Power On
        │
        ▼
    ROM loads TF-A from eMMC boot partition 1
        │
        ├── Success → Normal boot continues
        │
        └── Failure → ROM tries eMMC boot partition 2 (backup)
                          │
                          ├── Success → Normal boot continues
                          │
                          └── Failure → ROM tries USB DFU
                                          (if not disabled in OTP)
                                          │
                                          ├── Available → Reflash via USB
                                          │
                                          └── Disabled → BRICKED
```

### 11.2 eMMC Boot Partition Redundancy

Always flash the signed TF-A to both eMMC boot partitions:

```bash
# eMMC has two hardware boot partitions (boot0 and boot1)
# Flash signed TF-A to both:

# From Linux on the board:
dd if=tf-a-signed.stm32 of=/dev/mmcblk1boot0 conv=fdatasync
dd if=tf-a-signed.stm32 of=/dev/mmcblk1boot1 conv=fdatasync

# Or via STM32CubeProgrammer (handles both partitions)
```

### 11.3 A/B Rootfs Scheme (U-Boot)

For OTA updates, use an A/B partition scheme with boot counting:

```
    U-Boot Environment Variables:
        boot_slot = "a" or "b"
        boot_attempts_a = 3 (max retries)
        boot_attempts_b = 3 (max retries)
        boot_ok = 0 (set to 1 by application after healthy start)

    Boot Logic:
    1. Try slot ${boot_slot}
    2. Decrement boot_attempts
    3. If application sets boot_ok=1: reset attempts to 3
    4. If attempts reach 0: switch to other slot
    5. If both slots exhausted: enter recovery
```

### 11.4 Application Boot Confirmation

Add this to your application (`main.c`) — call it after all subsystems initialize successfully:

```c
#include <stdlib.h>

/**
 * @brief Confirms boot success to U-Boot.
 *
 * Resets the boot attempt counter so the current slot
 * is not abandoned on next reboot. Only call this after
 * verifying that Modbus, MQTT, and other critical
 * subsystems are working.
 */
static void confirm_boot_success(void)
{
    int rc = system("fw_setenv boot_ok 1");
    if (rc != 0) {
        fprintf(stderr, "[BOOT] Failed to confirm boot success\n");
    } else {
        printf("[BOOT] Boot confirmed to U-Boot\n");
    }
}
```

> **Important:** Don't call this too early. Wait until MQTT connects, Modbus responds, etc. If you confirm prematurely, a partially broken firmware gets "locked in."

---

## Part 12: Integration with Your Existing Project

### 12.1 Your Workflow Doesn't Change

```
    DAILY DEVELOPMENT (unchanged)       RELEASE / SECURE BOOT
    ==============================      =====================

    Windows/WSL                         Ubuntu (native or VM)
    Docker cross-compile                Developer Package SDK
    └── build/main                      ├── Build TF-A + sign
                                        ├── Build OP-TEE
    SCP to running DK2                  ├── Build U-Boot
    Test and iterate                    ├── Build kernel + sign FIT
                                        └── Flash to DK2

    Fast, unsigned                      Slower, fully signed
    Development board (OTP open)        Production board (OTP closed)
```

### 12.2 Build Script for Signed Images

Create a script to automate the entire signed build:

```bash
#!/bin/bash
# build_signed.sh — Build all signed boot chain components
set -euo pipefail

# ---- Configuration ----
WORKSPACE="$HOME/stm32mp1-secure"
SDK_ENV="$WORKSPACE/sdk/environment-setup-cortexa7t2hf-neon-vfpv4-ostl-linux-gnueabi"
KEYS="$WORKSPACE/keys"
DEPLOY="$WORKSPACE/deploy"
KEY_PASSWORD="your-secure-password"

# Source paths (set these to your actual extracted paths)
TFA_SRC="$WORKSPACE/sources/tf-a-stm32mp-v2.8-r1"
OPTEE_SRC="$WORKSPACE/sources/optee-os-stm32mp-3.19-r1"
UBOOT_SRC="$WORKSPACE/sources/u-boot-stm32mp-v2023.10-r1"
KERNEL_SRC="$WORKSPACE/sources/linux-stm32mp-6.1-r1"

# ---- Setup ----
source "$SDK_ENV"
mkdir -p "$DEPLOY"

echo "=== Building Signed Boot Chain ==="

# ---- 1. Build OP-TEE ----
echo "[1/5] Building OP-TEE..."
cd "$OPTEE_SRC"
make -j$(nproc) CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLATFORM=stm32mp1 \
    CFG_EMBED_DTB_SOURCE_FILE=stm32mp157f-dk2.dts \
    CFG_TEE_CORE_LOG_LEVEL=3 \
    O=build

# ---- 2. Build U-Boot ----
echo "[2/5] Building U-Boot..."
cd "$UBOOT_SRC"
make CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    DEVICE_TREE=stm32mp157f-dk2 stm32mp15_trusted_defconfig
make -j$(nproc) CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    DEVICE_TREE=stm32mp157f-dk2 all

# ---- 3. Build Kernel ----
echo "[3/5] Building Kernel..."
cd "$KERNEL_SRC"
make -j$(nproc) ARCH=arm CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    zImage dtbs

# ---- 4. Create signed FIT image (embeds key in U-Boot DTB) ----
echo "[4/5] Creating signed FIT image..."
mkdir -p "$WORKSPACE/build"
cp "$KERNEL_SRC/arch/arm/boot/zImage" "$WORKSPACE/build/"
cp "$KERNEL_SRC/arch/arm/boot/dts/stm32mp157f-dk2.dtb" "$WORKSPACE/build/"

cd "$WORKSPACE/build"
# (kernel.its should already exist from Part 9)
mkimage -f kernel.its -k "$KEYS" -K "$UBOOT_SRC/u-boot.dtb" -r fitImage
cp fitImage "$DEPLOY/"

# ---- 5. Build TF-A + FIP + Sign ----
echo "[5/5] Building TF-A + FIP and signing..."
cd "$TFA_SRC"
make realclean
make -j$(nproc) \
    CROSS_COMPILE=arm-ostl-linux-gnueabi- \
    PLAT=stm32mp1 ARCH=aarch32 ARM_ARCH_MAJOR=7 STM32MP15=1 \
    DTB_FILE_NAME=stm32mp157f-dk2.dtb STM32MP_EMMC=1 \
    AARCH32_SP=optee \
    BL32="$OPTEE_SRC/build/core/tee-header_v2.bin" \
    BL32_EXTRA1="$OPTEE_SRC/build/core/tee-pager_v2.bin" \
    BL32_EXTRA2="$OPTEE_SRC/build/core/tee-pageable_v2.bin" \
    BL33="$UBOOT_SRC/u-boot-nodtb.bin" \
    BL33_CFG="$UBOOT_SRC/u-boot.dtb" \
    all fip

# Sign TF-A
STM32MP_SigningTool_CLI \
    -bin "build/stm32mp1/release/tf-a-stm32mp157f-dk2.stm32" \
    -pubk "$KEYS/publicKey.pem" \
    -prvk "$KEYS/privateKey.pem" \
    -pwd "$KEY_PASSWORD" \
    -t fsbl \
    -o "$DEPLOY/tf-a-stm32mp157f-dk2-signed.stm32"

cp "build/stm32mp1/release/fip.bin" "$DEPLOY/"

echo ""
echo "=== Build Complete ==="
echo "Signed images in: $DEPLOY/"
ls -la "$DEPLOY/"
```

### 12.3 When to Move to the Distribution Package (Yocto)

Move to Yocto when you need features the Developer Package cannot provide:

| Feature | Developer Package | Distribution Package (Yocto) |
|---------|------------------|------------------------------|
| Build TF-A/OP-TEE/U-Boot | Yes | Yes |
| Sign boot images | Yes | Yes |
| Custom rootfs packages | No (use stock rootfs) | Yes (full control) |
| dm-verity (verified rootfs) | Manual setup | Automated via `dm-verity-img` class |
| Read-only rootfs | Manual setup | `IMAGE_FEATURES += "read-only-rootfs"` |
| Your app baked into rootfs | No (deploy separately) | Yes (via recipe) |
| Reproducible builds | No | Yes |
| CI/CD integration | Possible but manual | Well-supported |

**Recommended transition point:** After secure boot is working and validated with the Developer Package, migrate to Yocto for production image generation. Your signing keys and OTP values carry over — only the build system changes.

---

## Part 13: Testing and Validation Checklist

### 13.1 Phase 1: Environment Setup

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 1 | SDK installed | `$CC --version` shows arm cross-compiler | [ ] |
| 2 | STM32CubeProgrammer installed | `STM32_Programmer_CLI --version` works | [ ] |
| 3 | `STM32MP_KeyGen_CLI` available | `STM32MP_KeyGen_CLI --help` works | [ ] |
| 4 | `STM32MP_SigningTool_CLI` available | `STM32MP_SigningTool_CLI --help` works | [ ] |
| 5 | Boot sources extracted and patched | TF-A, OP-TEE, U-Boot, kernel source dirs exist | [ ] |

### 13.2 Phase 2: Build Verification (Unsigned)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 6 | OP-TEE builds | `tee-header_v2.bin`, `tee-pager_v2.bin` exist | [ ] |
| 7 | U-Boot builds | `u-boot-nodtb.bin`, `u-boot.dtb` exist | [ ] |
| 8 | TF-A + FIP builds | `.stm32` and `fip.bin` exist | [ ] |
| 9 | Kernel builds | `zImage` and `.dtb` exist | [ ] |
| 10 | **Unsigned image boots on DK2** | Full boot to Linux shell via UART | [ ] |

### 13.3 Phase 3: Key Generation

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 11 | ECDSA key pair generated | `privateKey.pem`, `publicKey.pem`, `publicKeyhash.bin` | [ ] |
| 12 | Key hash displayed correctly | 8 x 32-bit words printed | [ ] |
| 13 | RSA FIT key generated | `fit_sign.key`, `fit_sign.crt` exist | [ ] |
| 14 | Keys backed up | Copies in 2+ secure locations | [ ] |
| 15 | Key hash recorded on paper | Written and verified by second person | [ ] |

### 13.4 Phase 4: Signed Boot (Dev Mode — OTP NOT Programmed)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 16 | TF-A signed successfully | `STM32MP_SigningTool_CLI` completes without error | [ ] |
| 17 | Signed TF-A boots on DK2 | Normal boot via UART | [ ] |
| 18 | FIT image signed | `mkimage -l fitImage` shows signature | [ ] |
| 19 | Signed FIT boots | `sha256,rsa2048:fit_sign+ OK` in UART | [ ] |
| 20 | **Tampered FIT rejected** | `FAILED` in UART, kernel doesn't boot | [ ] |
| 21 | Board recoverable via DFU | Can reflash via USB after any failure | [ ] |

### 13.5 Phase 5: OTP Programming (IRREVERSIBLE)

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 22 | Test board sacrificed | First fusing done on non-production board | [ ] |
| 23 | Key hash programmed | `stm32key read` matches expected values | [ ] |
| 24 | Device closed | `stm32key close` executed | [ ] |
| 25 | **Signed TF-A boots after closure** | Normal boot via UART | [ ] |
| 26 | **Unsigned TF-A REJECTED after closure** | No UART output, ROM refuses boot | [ ] |
| 27 | USB DFU recovery works after closure | Can reflash signed TF-A via USB | [ ] |

### 13.6 Phase 6: Full Chain Verification

| # | Test | Expected Result | Status |
|---|------|----------------|--------|
| 28 | ROM verifies TF-A | Boot only with signed FSBL | [ ] |
| 29 | TF-A verifies FIP (if COT) | `Boot authentication Success` in UART | [ ] |
| 30 | U-Boot verifies FIT | `sha256,rsa2048+ OK` in UART | [ ] |
| 31 | Application starts | Your `main` binary runs, Modbus/MQTT work | [ ] |

---

## Appendix A: Troubleshooting

### Build Errors

| Problem | Cause | Solution |
|---------|-------|----------|
| `arm-ostl-linux-gnueabi-gcc: not found` | SDK not sourced | Run `source ~/stm32mp1-secure/sdk/environment-setup-*` |
| `MBEDTLS_DIR not set` or mbedTLS errors | Missing mbedTLS for GENERATE_COT | Clone mbedTLS v2.28.x and set `MBEDTLS_DIR` |
| `ROT_KEY file not found` | Wrong path to key | Use absolute path to `privateKey.pem` |
| OP-TEE DTB not found | Wrong `CFG_EMBED_DTB_SOURCE_FILE` | Check available `.dts` files in OP-TEE's `core/arch/arm/plat-stm32mp1/` |
| FIT signing: `RSA key not found` | Key filename doesn't match `key-name-hint` | Ensure files are `fit_sign.key` and `fit_sign.crt` matching ITS `key-name-hint = "fit_sign"` |

### Boot Errors

| Problem | Cause | Solution |
|---------|-------|----------|
| No UART output at all after OTP closure | Unsigned TF-A or wrong key hash fused | Flash correct signed TF-A via USB DFU. If key hash is wrong: device is bricked |
| `Boot authentication Failed` | FIP signature mismatch | Rebuild FIP with `GENERATE_COT=1` using same ROT key |
| U-Boot `FIT signature FAILED` | FIT signed with different key than U-Boot DTB | Rebuild: sign FIT with `-K u-boot.dtb`, then rebuild FIP with updated DTB |
| Kernel boots but crashes | DTB mismatch or kernel config issue | Verify correct DTB in FIT, check kernel config |

### OTP Errors

| Problem | Cause | Solution |
|---------|-------|----------|
| `stm32key read` shows zeros after fusing | Fuse write failed or wrong word numbers | Retry with correct word numbers for your silicon revision |
| Board bricked after `stm32key close` | Key hash doesn't match signed image key | If USB DFU works: flash correctly signed TF-A. If not: board is unrecoverable |
| `stm32key close` didn't work | OTP write failed | Retry. Check power supply stability during fusing |

---

## Appendix B: Glossary

| Term | Definition |
|------|-----------|
| **BL2 / FSBL** | First Stage Boot Loader = TF-A. Loaded by ROM from eMMC. |
| **BL32** | Secure OS = OP-TEE. Runs in ARM TrustZone secure world. |
| **BL33 / SSBL** | Second Stage Boot Loader = U-Boot. Normal world bootloader. |
| **BSEC** | Boot and SECurity controller — manages OTP fuses on STM32MP1. |
| **COT** | Chain of Trust — certificate chain from ROM to application. |
| **DFU** | Device Firmware Upgrade — USB protocol for flashing. |
| **ECDSA P-256** | Elliptic Curve Digital Signature Algorithm — used by STM32MP1 ROM. |
| **FIP** | Firmware Image Package — bundles OP-TEE + U-Boot + certificates. |
| **FIT** | Flattened Image Tree — U-Boot format bundling kernel + DTB + signature. |
| **FSBL** | First Stage Boot Loader = TF-A (same as BL2). |
| **HSM** | Hardware Security Module — dedicated device for key protection. |
| **ITS** | Image Tree Source — text file describing FIT image layout. |
| **OTP** | One-Time Programmable fuses. Set once, never cleared. |
| **ROT** | Root of Trust — hardware anchor for the trust chain. |
| **SSBL** | Second Stage Boot Loader = U-Boot (same as BL33). |
| **TBB** | Trusted Board Boot — TF-A's authenticated boot feature. |

---

## Appendix C: References

- [ST Wiki: STM32MP1 Developer Package](https://wiki.st.com/stm32mpu/wiki/STM32MP1_Developer_Package)
- [ST Wiki: Signing Tool](https://wiki.st.com/stm32mpu/wiki/Signing_tool)
- [ST Wiki: TF-A Overview](https://wiki.st.com/stm32mpu/wiki/TF-A_overview)
- [ST Wiki: How to Configure TF-A BL2](https://wiki.st.com/stm32mpu/wiki/How_to_configure_TF-A_BL2)
- [ST Wiki: How to Build OP-TEE Components](https://wiki.st.com/stm32mpu/wiki/How_to_build_OP-TEE_components)
- [Trusted Firmware-A: STM32MP1 Platform](https://trustedfirmware-a.readthedocs.io/en/stable/plat/st/stm32mp1.html)
- [FoundriesFactory: Secure Boot on STM32MP1](https://docs.foundries.io/92/reference-manual/security/secure-boot-stm32mp1.html)
- [Zondax: Secure Boot STM32MP157C](https://docs.zondax.ch/tee-signer/Secure%20Boot/SecureBoot-STM32MP157C)
- [DHCOM STM32MP15 Secure Boot](https://wiki.dh-electronics.com/index.php/DHCOM_STM32MP15_Secure_Boot)
- [embetrix/stm32mp-sign-tool (open-source alternative)](https://github.com/embetrix/stm32mp-sign-tool)
- [mrnuke/stm32mp-keygen (open-source key generation)](https://github.com/mrnuke/stm32mp-keygen)

---

*Document version: 2.0 (Developer Package approach)*
*Last updated: 2026-09-08*
*For: STM32MP157F-DK2, OpenSTLinux, TF-A + OP-TEE + U-Boot*
