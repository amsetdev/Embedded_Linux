# STM32MP1 Industrial IoT Gateway — Production Security Hardening Guide

**Target:** STM32MP157F-DK2 (STM32MP1 family, Cortex-A7 + Cortex-M4)
**Boot media:** eMMC
**OS:** OpenSTLinux (Yocto, kernel 6.x)
**Boot chain:** ROM -> TF-A -> OP-TEE -> U-Boot -> Linux
**Connectivity:** Ethernet, Wi-Fi, MQTT/TLS to AWS IoT Core
**Standard:** IEC 62443 / NIST SP 800-193

> **Audience:** Embedded engineers taking this product from development to production deployment. Every section distinguishes *development* vs *production* workflows and marks irreversible steps with warnings.

---

## Table of Contents

1. [Threat Model](#1-threat-model)
2. [Secure Boot Chain](#2-secure-boot-chain)
3. [Cryptographic Architecture](#3-cryptographic-architecture)
4. [Secure Manufacturing](#4-secure-manufacturing)
5. [Debug Protection](#5-debug-protection)
6. [Storage Security](#6-storage-security)
7. [OTA Security](#7-ota-security)
8. [Network Hardening](#8-network-hardening)
9. [Linux Hardening](#9-linux-hardening)
10. [STM32MP1-Specific Implementation](#10-stm32mp1-specific-implementation)
11. [Compliance Mapping](#11-compliance-mapping)
12. [Final Checklist](#12-final-checklist)

---

## 1. Threat Model

### 1.1 Attack Surface Diagram

```
                    +-----------------------+
                    |   Cloud (AWS IoT)     |
                    +----------+------------+
                               | TLS 1.2+ (port 8883)
                               |
                    +----------+------------+
     RS485 Bus     |   STM32MP157 Gateway   |     Ethernet/Wi-Fi
  <--------------->|                        |<----> LAN / Internet
   Modbus RTU      |  eMMC  |  JTAG/SWD    |
                    +---+----+------+-------+
                        |           |
                    Physical    Debug Port
                    Access
```

### 1.2 Threat Categories

#### 1.2.1 Physical Attacks

| Threat | Description | Likelihood | Impact | Mitigation |
|--------|-------------|-----------|--------|------------|
| eMMC chip-off | Attacker desolders eMMC, reads firmware offline | Medium | High — IP extraction, credential theft | Secure boot + encrypted storage, dm-verity |
| Bus probing | Logic analyzer on I2C/SPI/UART during boot | Medium | High — key extraction during boot | Disable boot console in production, minimize key exposure window |
| Glitching | Voltage/clock glitch to bypass secure boot checks | Low | Critical — full compromise | STM32MP1 ROM has limited glitch resistance; use tamper detection, epoxy potting |
| Cold boot | RAM freezing to extract keys | Low | High — DRAM key extraction | OP-TEE secure memory isolation, short key lifetimes |

#### 1.2.2 Firmware Tampering

| Threat | Description | Likelihood | Impact | Mitigation |
|--------|-------------|-----------|--------|------------|
| Unsigned firmware install | Attacker writes malicious firmware via eMMC or OTA | High | Critical — full device control | Secure boot chain, signed OTA |
| Rootfs modification | Attacker mounts and modifies rootfs | Medium | Critical — persistent backdoor | dm-verity, read-only rootfs |
| Bootloader modification | Replace U-Boot with trojanized version | Medium | Critical — bypasses all OS security | TF-A signature verification of U-Boot |
| Rollback attack | Install older firmware with known vulnerabilities | Medium | High — exploit known CVEs | Anti-rollback counters in OTP |

#### 1.2.3 Supply-Chain Attacks

| Threat | Description | Likelihood | Impact | Mitigation |
|--------|-------------|-----------|--------|------------|
| Counterfeit SoC | Non-genuine STM32MP1 without security features | Low | Critical — no hardware RoT | Verify STM32MP1 UID during provisioning |
| Tampered firmware image | Modified image injected at CM/factory | Medium | Critical — pre-compromised device | Sign images before shipping to factory, verify at first boot |
| Key leakage at factory | OEM private keys exposed at contract manufacturer | Medium | Critical — all devices compromised | HSM-based signing, keys never leave HSM, factory only receives public key hash |

#### 1.2.4 Network Attacks

| Threat | Description | Likelihood | Impact | Mitigation |
|--------|-------------|-----------|--------|------------|
| MQTT hijacking | Attacker intercepts or injects MQTT messages | High | High — false data, unauthorized commands | Mutual TLS (X.509), certificate pinning |
| Man-in-the-middle | Intercept OTA updates or cloud traffic | High | Critical — malicious firmware delivery | TLS 1.2+ with certificate validation, signed OTA payloads |
| Lateral movement | Compromised gateway used to attack OT network | Medium | Critical — ICS/SCADA compromise | Network segmentation, firewall, least-privilege |
| SSH brute force | Automated credential attacks on SSH | High | High — remote shell access | Key-only SSH auth, fail2ban, disable root login |
| DNS spoofing | Redirect cloud endpoint to attacker server | Medium | High — credential theft | Certificate pinning, DNSSEC where possible |

#### 1.2.5 Debug Interface Abuse

| Threat | Description | Likelihood | Impact | Mitigation |
|--------|-------------|-----------|--------|------------|
| Open JTAG/SWD | Attacker connects debugger, reads memory, modifies code | High (if left open) | Critical — full device compromise | Close JTAG in production via OTP fuses |
| UART console | Attacker gains root shell via serial console | High (if left open) | Critical — full device compromise | Disable console in production kernel cmdline |
| Engineering sample leak | Development units with open debug reach customers | Medium | Critical — no security enforced | Separate dev/prod build pipelines, factory verification |

---

## 2. Secure Boot Chain

### 2.1 Boot Flow Overview

```
+------------------+     +------------------+     +------------------+
|   STM32MP1 ROM   |---->|   TF-A (BL2)     |---->|   OP-TEE (BL32)  |
| (Hardware RoT)   |     | Signed by OEM    |     | Signed by OEM    |
| Verifies TF-A    |     | Verifies OP-TEE  |     | Provides TEE     |
| using OTP hash   |     | + U-Boot         |     | services          |
+------------------+     +------------------+     +------------------+
                                                          |
                          +------------------+            |
                          |   U-Boot (BL33)  |<-----------+
                          | Signed by OEM    |
                          | Verifies Linux   |
                          | kernel + DTB     |
                          +------------------+
                                  |
                          +------------------+
                          |   Linux Kernel   |
                          | Verified boot    |
                          | dm-verity rootfs |
                          +------------------+
```

### 2.2 Key Hierarchy

```
                    +-----------------------+
                    |   OEM Root Key Pair   |  <-- RSA-2048 or ECDSA-256
                    |   (stored in HSM)     |  <-- NEVER leaves HSM
                    +-----------+-----------+
                                |
                    +-----------+-----------+
                    | Public Key Hash (SHA-256)|
                    | Burned into OTP fuses   |
                    +-------------------------+
                                |
              +-----------------+-----------------+
              |                 |                 |
      +-------+-------+ +------+------+ +--------+--------+
      | TF-A Signing  | | OP-TEE     | | U-Boot Signing  |
      | Key (derived) | | Signing Key| | Key (derived)   |
      +---------------+ +------------+ +-----------------+
                                                |
                                        +-------+-------+
                                        | FIT Image     |
                                        | Signing Key   |
                                        | (kernel+DTB)  |
                                        +---------------+
```

**Key management rules:**
- The OEM root private key MUST be stored in an HSM (e.g., Nitrokey HSM 2, YubiHSM 2, or cloud HSM).
- Signing is performed on a hardened, air-gapped build server or via HSM API.
- The public key hash is the only cryptographic material stored on-device (in OTP).
- Intermediate signing keys may be derived but should also be HSM-protected.

### 2.3 Image Signing Flow

#### 2.3.1 Generate OEM Root Key Pair (one-time)

```bash
# Using OpenSSL (for development only — production MUST use HSM)
openssl ecparam -name prime256v1 -genkey -noout -out oem_root_key.pem
openssl ec -in oem_root_key.pem -pubout -out oem_root_key_pub.pem

# Compute public key hash for OTP programming
openssl ec -in oem_root_key.pem -pubout -outform DER | \
  openssl dgst -sha256 -binary | xxd -p
```

> **Production:** Use `pkcs11-tool` or your HSM's CLI to generate the key pair inside the HSM. The private key never exists outside the HSM.

#### 2.3.2 Sign TF-A Image

STM32MP1 uses the STM32 signing tool (`STM32_SigningTool_CLI`):

```bash
# Sign TF-A BL2 binary
STM32_SigningTool_CLI -bin tf-a-stm32mp157f-dk2.stm32 \
  -nk -of 0 \
  -t fsbl \
  -d \
  -s oem_root_key.pem \
  -hv 2.1 \
  --public-key oem_root_key_pub.pem

# For production with HSM (PKCS#11):
STM32_SigningTool_CLI -bin tf-a-stm32mp157f-dk2.stm32 \
  -nk -of 0 \
  -t fsbl \
  -d \
  --signing-key "pkcs11:token=OEM_HSM;object=root_key" \
  -hv 2.1
```

#### 2.3.3 Sign U-Boot FIT Image

```bash
# Create RSA key pair for FIT signing (or use HSM)
openssl genrsa -out uboot_fit_sign.key 2048
openssl req -batch -new -x509 -key uboot_fit_sign.key -out uboot_fit_sign.crt

# Add public key to U-Boot DTB
mkimage -f fit_image.its -k keys/ -K u-boot.dtb -r fit_image.itb
```

#### 2.3.4 Kernel + DTB Signing via FIT

```bash
# ITS (Image Tree Source) with signature node
cat > kernel.its << 'EOFITS'
/dts-v1/;
/ {
    description = "Kernel + DTB signed image";
    images {
        kernel {
            description = "Linux kernel";
            data = /incbin/("zImage");
            type = "kernel";
            arch = "arm";
            os = "linux";
            compression = "none";
            load = <0xC2000040>;
            entry = <0xC2000040>;
            hash-1 { algo = "sha256"; };
        };
        fdt-1 {
            description = "Device Tree";
            data = /incbin/("stm32mp157f-dk2.dtb");
            type = "flat_dt";
            arch = "arm";
            compression = "none";
            hash-1 { algo = "sha256"; };
        };
    };
    configurations {
        default = "conf-1";
        conf-1 {
            kernel = "kernel";
            fdt = "fdt-1";
            signature-1 {
                algo = "sha256,rsa2048";
                key-name-hint = "uboot_fit_sign";
                sign-images = "kernel", "fdt";
            };
        };
    };
};
EOFITS

mkimage -f kernel.its -k keys/ -K u-boot.dtb -r kernel.itb
```

### 2.4 Anti-Rollback Protection

The STM32MP1 provides monotonic counters in OTP for anti-rollback:

```
OTP Word 24 (BSEC): Anti-rollback counter for TF-A
OTP Word 25 (BSEC): Anti-rollback counter for application firmware
```

**Implementation:**

1. Each signed image includes a version number in its header.
2. TF-A compares the image version against the OTP counter value.
3. Boot proceeds only if `image_version >= otp_counter`.
4. After successful boot and validation, the OTP counter is incremented to match the new version.

```c
/* In TF-A: plat/st/stm32mp1/stm32mp1_security.c */
/* Pseudo-code for anti-rollback check */
uint32_t otp_counter = bsec_read_otp(OTP_ROLLBACK_WORD);
uint32_t image_version = get_image_version(image_header);
if (image_version < otp_counter) {
    ERROR("Anti-rollback: image version %u < OTP counter %u\n",
          image_version, otp_counter);
    panic();
}
```

> **WARNING:** OTP counter increments are irreversible. A bug in the increment logic can brick devices. Always validate the new firmware completely before incrementing the counter.

**Development workflow:** Do NOT enable anti-rollback during development. Use a build flag:
```makefile
# TF-A build
make STM32MP_ANTI_ROLLBACK=0   # Development
make STM32MP_ANTI_ROLLBACK=1   # Production
```

### 2.5 OTP Programming Sequence

See [Section 4: Secure Manufacturing](#4-secure-manufacturing) for the full sequence.

### 2.6 Recovery Strategy

If the primary boot image fails, the STM32MP1 ROM boot sequence supports fallback:

1. **eMMC boot partition 1** (primary) -> **eMMC boot partition 2** (backup)
2. If both fail, ROM falls back to **USB DFU** or **UART** boot (if not disabled in OTP).

**Production recovery plan:**
- Maintain two copies of TF-A in eMMC boot partitions 1 and 2.
- U-Boot implements A/B boot logic for kernel/rootfs (see [Section 7: OTA](#7-ota-security)).
- If A/B both fail after N retries, U-Boot enters a minimal recovery mode that only accepts signed recovery images via USB.
- USB/UART boot from ROM can be permanently disabled via OTP for highest security, but this removes the last recovery path.

> **WARNING:** Disabling USB/UART boot in OTP is irreversible. If all eMMC images are corrupted and ROM fallback is disabled, the device is permanently bricked. Only disable ROM fallback after thorough testing of the A/B recovery mechanism.

---

## 3. Cryptographic Architecture

### 3.1 Root of Trust

The STM32MP1 Root of Trust (RoT) is hardware-anchored:

```
+---------------------------------------------------+
|                  STM32MP1 SoC                      |
|                                                    |
|  +-------------+    +---------------------------+  |
|  |  Boot ROM   |    |  BSEC (OTP Fuses)         |  |
|  | (immutable) |    |  - OEM root key hash      |  |
|  | Verifies    |    |  - Security config         |  |
|  | TF-A sig    |    |  - Anti-rollback counters  |  |
|  +------+------+    |  - JTAG config             |  |
|         |           |  - Device UID              |  |
|         v           +---------------------------+  |
|  +-------------+                                   |
|  |  CRYP/HASH  |    +---------------------------+  |
|  | HW accel    |    |  RNG (TRNG)               |  |
|  | AES/DES/SHA |    |  True Random Number Gen   |  |
|  +-------------+    +---------------------------+  |
+---------------------------------------------------+
```

**Trust chain:** ROM (immutable) -> OTP public key hash (immutable) -> signed TF-A -> signed OP-TEE -> signed U-Boot -> signed kernel -> dm-verity rootfs.

### 3.2 Key Types and Storage

| Key | Purpose | Storage | Lifetime |
|-----|---------|---------|----------|
| OEM Root Key | Signs all boot images | HSM (off-device) | Product lifetime |
| OEM Root Key Hash | Verifies TF-A signature | OTP fuses (on-device) | Permanent (fused) |
| TF-A Signing Key | Signs TF-A binary | HSM (off-device) | Product lifetime |
| U-Boot FIT Key | Signs kernel + DTB | HSM (off-device) | Product lifetime |
| OTA Signing Key | Signs OTA update packages | HSM (off-device) | Product lifetime |
| Device Identity Key | TLS client cert for AWS IoT | OP-TEE secure storage | Device lifetime |
| HUK (Hardware Unique Key) | Derives device-unique keys | BSEC OTP (SoC internal) | Permanent |
| OP-TEE TA Encryption Key | Encrypts Trusted App data | Derived from HUK | Per-boot |
| MQTT TLS Key | AWS IoT mTLS authentication | OP-TEE secure storage | Rotatable |

### 3.3 Device-Unique Key Derivation

The STM32MP1 has a Hardware Unique Key (HUK) in OTP that OP-TEE uses for key derivation:

```
                    +------------------+
                    |   HUK (OTP)      |  <-- Unique per SoC, set during manufacturing
                    +--------+---------+
                             |
                    HKDF-SHA256 derivation
                             |
              +--------------+--------------+
              |              |              |
      +-------+------+ +----+-----+ +------+-------+
      | Secure       | | File     | | RPMB         |
      | Storage Key  | | Enc Key  | | Auth Key     |
      | (OP-TEE TA   | | (fscrypt)| | (eMMC RPMB   |
      |  data at     | |          | |  partition)  |
      |  rest)       | |          | |              |
      +--------------+ +----------+ +--------------+
```

OP-TEE's key derivation uses the HUK combined with a static string and TA UUID to produce per-TA unique keys:

```c
/* OP-TEE core/tee/tee_svc_cryp.c — simplified */
TEE_Result tee_derive_ta_key(const TEE_UUID *uuid, uint8_t *key, size_t key_sz)
{
    /* HUK + "TA_DERIVED_KEY" + TA UUID -> HKDF -> per-TA key */
    return huk_subkey_derive(HUK_SUBKEY_TA, uuid, sizeof(*uuid),
                             key, key_sz);
}
```

### 3.4 Secure Storage in OP-TEE

OP-TEE provides two secure storage backends on STM32MP1:

1. **REE FS (default):** Encrypted files stored on the Linux filesystem, encrypted with HUK-derived keys. Integrity-protected with HMAC.
2. **RPMB (recommended for production):** Uses the eMMC Replay Protected Memory Block. Provides hardware-enforced replay protection.

**Configuring RPMB storage in OP-TEE:**

```makefile
# In OP-TEE OS configuration (core/arch/arm/plat-stm32mp1/conf.mk)
CFG_RPMB_FS=y
CFG_RPMB_FS_DEV_ID=0           # eMMC device index
CFG_REE_FS=n                    # Disable REE FS fallback
CFG_RPMB_WRITE_KEY=y            # Program RPMB auth key on first boot
```

**Storing device keys (example — MQTT private key):**

```c
/* Trusted Application storing a key in OP-TEE secure storage */
#include <tee_internal_api.h>

TEE_Result store_mqtt_key(uint32_t param_types, TEE_Param params[4])
{
    TEE_ObjectHandle object;
    TEE_Result res;
    const char *obj_id = "mqtt_private_key";

    res = TEE_CreatePersistentObject(TEE_STORAGE_PRIVATE,
                                     obj_id, strlen(obj_id),
                                     TEE_DATA_FLAG_ACCESS_WRITE,
                                     TEE_HANDLE_NULL,
                                     params[0].memref.buffer,
                                     params[0].memref.size,
                                     &object);
    if (res == TEE_SUCCESS)
        TEE_CloseObject(object);

    return res;
}
```

### 3.5 PKCS#11 Integration

OP-TEE includes a PKCS#11 Trusted Application that exposes cryptographic operations to Linux userspace:

```
+-------------------+     +-------------------+     +-------------------+
|  Application      |     |  OP-TEE PKCS#11   |     |  OP-TEE Secure    |
|  (mqtt.c)         |---->|  TA               |---->|  Storage (RPMB)   |
|  Uses             |     |  Crypto ops       |     |  Keys never       |
|  libp11/pkcs11    |     |  inside TEE       |     |  leave TEE        |
+-------------------+     +-------------------+     +-------------------+
```

**Setup:**

```bash
# Install OP-TEE PKCS#11 TA on target
cp <optee-build>/out/ta/fd02c9da-306c-48c7-a49c-bbd827ae86ee.ta /lib/optee_armtz/

# Install userspace client library
# (built as part of optee_client)
cp libckteec.so /usr/lib/

# Configure OpenSSL to use PKCS#11 engine
cat > /etc/ssl/openssl_pkcs11.cnf << 'EOF'
openssl_conf = openssl_init

[openssl_init]
engines = engine_section

[engine_section]
pkcs11 = pkcs11_section

[pkcs11_section]
engine_id = pkcs11
MODULE_PATH = /usr/lib/libckteec.so
init = 0
EOF
```

**Using PKCS#11 for MQTT TLS:**

```c
/* In mqtt.c — configure mosquitto to use PKCS#11 for client key */
mosquitto_tls_set(mosq,
    settings->mqtt.ca_cert_path,     /* CA certificate (file) */
    NULL,
    settings->mqtt.client_cert_path, /* Client certificate (file) */
    NULL,                            /* Private key via PKCS#11, not file */
    NULL);

/* Set PKCS#11 engine for private key operations */
mosquitto_tls_engine_set(mosq, "pkcs11");
mosquitto_tls_keyform_set(mosq, "engine");
mosquitto_tls_engine_kpass_sha1_set(mosq, NULL);
```

---

## 4. Secure Manufacturing

### 4.1 Manufacturing Flow Overview

```
+------------------+     +-------------------+     +-------------------+
|  OEM Build       |     |  Factory Station  |     |  Device Under     |
|  Server (HSM)    |---->|  (Provisioning    |---->|  Test (DUT)       |
|                  |     |   Host)           |     |  STM32MP157       |
|  - Signs images  |     |  - Programs OTP   |     |  - Boots signed   |
|  - Generates     |     |  - Writes eMMC    |     |    firmware       |
|    device certs  |     |  - Runs tests     |     |  - Validates      |
+------------------+     +-------------------+     +-------------------+
```

### 4.2 Key Provisioning

**Step 1: Prepare signing infrastructure (one-time, OEM site)**

```bash
# Generate OEM root key pair in HSM
pkcs11-tool --module /usr/lib/libsofthsm2.so \
  --login --pin $HSM_PIN \
  --keypairgen --key-type EC:prime256v1 \
  --id 01 --label "OEM_ROOT_KEY"

# Export public key for hash computation
pkcs11-tool --module /usr/lib/libsofthsm2.so \
  --login --pin $HSM_PIN \
  --read-object --type pubkey --id 01 | \
  openssl ec -pubin -inform DER -outform DER | \
  openssl dgst -sha256 -binary > oem_root_key_hash.bin

# Display hash for OTP programming
xxd -p oem_root_key_hash.bin
# Output: e.g., a1b2c3d4e5f6...  (32 bytes / 8 OTP words)
```

**Step 2: Pre-sign all boot images on build server**

All images are signed before being shipped to the factory. The factory never has access to signing keys.

### 4.3 OTP Fuse Programming

> **WARNING: OTP fuse programming is IRREVERSIBLE. Once a fuse is blown, it cannot be unblown. Incorrect values will permanently brick the device or permanently compromise security. Always verify values before programming.**

**OTP Memory Map (relevant words):**

| OTP Word | Content | Notes |
|----------|---------|-------|
| 0-7 | Reserved / ST | Do not modify |
| 8 | OTP_CFG0 | Boot config, security mode |
| 9-12 | MAC addresses | Network configuration |
| 24 | Anti-rollback TF-A | Monotonic counter |
| 25 | Anti-rollback FW | Monotonic counter |
| 32-39 | OEM Root Key Hash | SHA-256 of public key (8 x 32-bit words) |
| 57 | OTP_CFG57 | JTAG/debug configuration |
| 60-63 | OEM data | Available for device-specific data |

**Programming sequence (using STM32CubeProgrammer):**

```bash
# Step 1: Connect to device via USB DFU or ST-Link
STM32_Programmer_CLI -c port=USB1

# Step 2: Read current OTP state (VERIFY BEFORE WRITING)
STM32_Programmer_CLI -c port=USB1 -otp displ

# Step 3: Program OEM root key hash (words 32-39)
# VERIFY the hash matches your HSM key!
STM32_Programmer_CLI -c port=USB1 \
  -otp write word=32 value=0xa1b2c3d4
STM32_Programmer_CLI -c port=USB1 \
  -otp write word=33 value=0xe5f67890
# ... repeat for words 34-39

# Step 4: Enable secure boot (OTP_CFG0)
# Bit 0: BOOT_SEC_EN — enables authentication
STM32_Programmer_CLI -c port=USB1 \
  -otp write word=8 value=0x00000001

# Step 5: Lock the key hash (make it read-only, prevent modification)
# Write lock bit for OTP words 32-39
STM32_Programmer_CLI -c port=USB1 \
  -otp lock word=32
# ... repeat for words 33-39
```

> **CRITICAL WARNING:** Step 4 (enabling BOOT_SEC_EN) is the point of no return. After this:
> - The ROM will ONLY boot TF-A images signed with the key whose hash matches OTP words 32-39.
> - If the key hash is wrong, the device is BRICKED.
> - If you lose the private key, you can NEVER update the device's boot firmware.
>
> **ALWAYS** test with an unlocked development board first. Program a test device and verify it boots the signed image before programming production devices.

### 4.4 Certificate Generation (Per-Device)

Each device needs a unique X.509 certificate for AWS IoT Core:

```bash
#!/bin/bash
# generate_device_cert.sh — run on provisioning host

DEVICE_UID=$1  # Read from STM32MP1 UID OTP
CA_KEY="path/to/device_ca.key"
CA_CERT="path/to/device_ca.crt"

# Generate device key pair
openssl ecparam -name prime256v1 -genkey -noout \
  -out "device_${DEVICE_UID}.key"

# Generate CSR
openssl req -new -key "device_${DEVICE_UID}.key" \
  -out "device_${DEVICE_UID}.csr" \
  -subj "/CN=${DEVICE_UID}/O=YourCompany/OU=SmartRTU"

# Sign with device CA
openssl x509 -req -in "device_${DEVICE_UID}.csr" \
  -CA "$CA_CERT" -CAkey "$CA_KEY" -CAcreateserial \
  -out "device_${DEVICE_UID}.crt" \
  -days 3650 -sha256

# Register certificate with AWS IoT Core
aws iot register-certificate \
  --certificate-pem "file://device_${DEVICE_UID}.crt" \
  --ca-certificate-pem "file://$CA_CERT" \
  --set-as-active

# Write to device (via provisioning host USB/serial)
# The private key should be stored in OP-TEE secure storage
# This is a simplified example; production should use a secure channel
scp "device_${DEVICE_UID}.key" root@${DEVICE_IP}:/tmp/
scp "device_${DEVICE_UID}.crt" root@${DEVICE_IP}:/etc/ssl/device/

# On device: store key in OP-TEE (via custom TA or PKCS#11)
ssh root@${DEVICE_IP} "optee_pkcs11_import /tmp/device_${DEVICE_UID}.key"
ssh root@${DEVICE_IP} "rm /tmp/device_${DEVICE_UID}.key"
```

### 4.5 Factory Verification Checklist

| Step | Verification | Pass Criteria | Fail Action |
|------|-------------|---------------|-------------|
| 1 | Read SoC UID | UID matches expected batch | Reject unit |
| 2 | Verify OTP blank | Key hash words 32-39 are 0x00000000 | Unit was previously programmed; investigate |
| 3 | Flash eMMC | All partitions written successfully | Re-flash or reject |
| 4 | Program OTP key hash | Read-back matches expected hash | **STOP** — do not continue |
| 5 | Enable secure boot | OTP_CFG0 bit 0 set | Verify and retry (if not yet locked) |
| 6 | Lock OTP key words | Lock bits set for words 32-39 | Verify and retry |
| 7 | Reboot test | Device boots signed TF-A -> OP-TEE -> U-Boot -> Linux | **CRITICAL FAIL** — device may be bricked |
| 8 | Certificate provisioning | Device cert installed, AWS IoT connection succeeds | Re-provision |
| 9 | Functional test | Modbus RTU/TCP read, MQTT publish, display active | Debug and re-test |
| 10 | Debug port verification | JTAG returns "access denied" or no response | Investigate OTP debug config |
| 11 | Serial console check | No login prompt on UART | Check kernel cmdline |
| 12 | Final seal | Enclosure closed, tamper sticker applied | Physical inspection |

### 4.6 Avoiding Bricked Devices

1. **Golden sample:** Program ONE device first. Verify complete boot chain before batch programming.
2. **OTP programming order:** Always program key hash BEFORE enabling secure boot. If secure boot is enabled without a valid key hash, the device is bricked.
3. **Key verification:** After programming OTP words 32-39, read them back and compare against the expected hash byte-by-byte.
4. **Backup ROM boot:** Do NOT disable USB/UART ROM boot until you have verified A/B recovery works.
5. **Test unsigned rejection:** After enabling secure boot, attempt to boot an unsigned image — it MUST fail. This confirms secure boot is working correctly.
6. **Batch size:** Program in small batches (10-20 units), verify each batch, before scaling up.

---

## 5. Debug Protection

### 5.1 STM32MP1 Debug Lifecycle

The STM32MP1 supports multiple debug security states controlled by OTP fuses:

```
+-------------------+     +-------------------+     +-------------------+
|   OPEN            |---->|  SECURED (OEM)    |---->|  CLOSED           |
|                   |     |                   |     |                   |
| - Full JTAG       |     | - Authenticated   |     | - JTAG disabled   |
| - Full SWD        |     |   debug only      |     | - No debug access |
| - Boot console    |     | - Password/cert   |     | - No boot console |
| - Development     |     |   required        |     | - Production      |
+-------------------+     +-------------------+     +-------------------+
```

### 5.2 OTP Debug Configuration (OTP Word 57)

| Bit | Name | Value | Effect |
|-----|------|-------|--------|
| 0 | DBG_AUTH | 1 | Enable authenticated debug |
| 1 | JTAG_DIS | 1 | Permanently disable JTAG |
| 2 | SWD_DIS | 1 | Permanently disable SWD |
| 3 | INVASIVE_DIS | 1 | Disable invasive debug |
| 4 | NON_INVASIVE_DIS | 1 | Disable non-invasive debug |
| 5-7 | Reserved | - | Do not set |

### 5.3 Development Configuration

During development, keep debug fully open:

```bash
# OTP word 57 = 0x00000000 (default — all debug enabled)
# Do NOT program any debug-related OTP fuses during development
```

**Protect development boards from accidental production fusing:**
- Label all development boards clearly.
- Use a separate build configuration that skips OTP programming.
- Never run the production provisioning script on a development board.

### 5.4 Production Configuration

**Option A: Authenticated Debug (recommended for field-serviceable products)**

```bash
# Program OTP word 57 to enable authenticated debug
# Bit 0 (DBG_AUTH) = 1: require authentication
STM32_Programmer_CLI -c port=USB1 \
  -otp write word=57 value=0x00000001

# Store debug authentication certificate in OTP or TF-A
# Only holders of the debug key can attach a debugger
```

This allows authorized service personnel to debug field-returned units without opening them up.

**Option B: Debug Permanently Disabled (highest security)**

```bash
# Program OTP word 57 to disable all debug
# Bits 1-4 = 1: disable JTAG, SWD, invasive, non-invasive
STM32_Programmer_CLI -c port=USB1 \
  -otp write word=57 value=0x0000001E
```

> **WARNING:** This is irreversible. You will NEVER be able to attach a debugger to this device again. This includes ST-Link, JTAG probes, and any SWD connection. Field debugging becomes impossible.

### 5.5 UART Console Protection

```bash
# In U-Boot environment or kernel cmdline:
# Development:
console=ttySTM0,115200

# Production:
console=           # Empty — disables kernel console output
loglevel=0         # Suppress all kernel messages
```

In U-Boot configuration:
```
# configs/stm32mp15_defconfig additions for production
CONFIG_SILENT_CONSOLE=y
CONFIG_DISABLE_CONSOLE=y
CONFIG_SILENT_U_BOOT_ENV="silent"
```

---

## 6. Storage Security

### 6.1 Partition Layout

```
eMMC Device (/dev/mmcblk1)
+--------------------------------------------------+
| Boot Partition 1 (/dev/mmcblk1boot0)             |
| TF-A (signed) + FIP (OP-TEE + U-Boot, signed)   |
+--------------------------------------------------+
| Boot Partition 2 (/dev/mmcblk1boot1)             |
| TF-A backup (signed) + FIP backup (signed)       |
+--------------------------------------------------+
| RPMB Partition                                    |
| OP-TEE secure storage (keys, sensitive data)     |
+--------------------------------------------------+
| User Data Area:                                   |
| +----------------------------------------------+ |
| | Partition 1: /boot (ext4, 64MB)              | |
| | FIT image (kernel + DTB, signed)             | |
| +----------------------------------------------+ |
| | Partition 2: rootfs_a (squashfs, 256MB)      | |
| | Verified with dm-verity, read-only           | |
| +----------------------------------------------+ |
| | Partition 3: rootfs_b (squashfs, 256MB)      | |
| | A/B slot for OTA, verified with dm-verity    | |
| +----------------------------------------------+ |
| | Partition 4: verity_a (16MB)                 | |
| | dm-verity hash tree for rootfs_a             | |
| +----------------------------------------------+ |
| | Partition 5: verity_b (16MB)                 | |
| | dm-verity hash tree for rootfs_b             | |
| +----------------------------------------------+ |
| | Partition 6: data (ext4, remaining space)    | |
| | Persistent data, encrypted with fscrypt      | |
| | SQLite DB, config, logs                      | |
| +----------------------------------------------+ |
+--------------------------------------------------+
```

### 6.2 dm-verity (Verified Root Filesystem)

dm-verity provides transparent integrity checking of the root filesystem using a Merkle tree:

```bash
# Build the verity hash tree (during image build)
veritysetup format /path/to/rootfs.squashfs /path/to/verity_hash.img \
  --hash=sha256 \
  --data-block-size=4096 \
  --hash-block-size=4096 \
  > verity_params.txt

# Extract the root hash (embed in signed kernel cmdline or FIT)
ROOT_HASH=$(grep "Root hash:" verity_params.txt | awk '{print $3}')

# Kernel command line (set in FIT image or U-Boot env):
# The root hash is part of the signed FIT image, so it cannot be tampered
root=/dev/dm-0 dm-mod.create="verity,,,ro,0 524288 verity 1 /dev/mmcblk1p2 /dev/mmcblk1p4 4096 4096 65536 1 sha256 ${ROOT_HASH} ${SALT}"
```

**Yocto integration:**

```bash
# In local.conf or image recipe:
IMAGE_FSTYPES += "squashfs"
INHERIT += "dm-verity-img"
DM_VERITY_IMAGE = "core-image-minimal"
DM_VERITY_IMAGE_TYPE = "squashfs"

# The root hash will be embedded in the FIT image at build time
```

### 6.3 fscrypt (Encrypted User Data)

The data partition stores mutable data (SQLite DB, configuration, logs) and should be encrypted:

```bash
# On first boot (provisioning script):
# 1. Format data partition with encryption support
mkfs.ext4 -O encrypt /dev/mmcblk1p6

# 2. Set encryption policy
# Use OP-TEE-derived key (via PKCS#11 or custom TA)
fscryptctl set_policy --key-descriptor=0123456789abcdef /mnt/data

# Alternative: use fscrypt tool
fscrypt setup /mnt/data
fscrypt encrypt /mnt/data/sensitive --source=custom_passphrase
```

**Integration with OP-TEE for key management:**

```
Boot sequence:
1. OP-TEE starts, derives data encryption key from HUK
2. Linux boots, mounts data partition
3. systemd service calls OP-TEE TA to retrieve encryption key
4. Key is passed to kernel fscrypt subsystem via keyring
5. Data partition becomes accessible
```

```bash
# systemd service to unlock encrypted data at boot
# /etc/systemd/system/unlock-data.service
[Unit]
Description=Unlock encrypted data partition
After=local-fs-pre.target
Before=local-fs.target

[Service]
Type=oneshot
ExecStart=/usr/sbin/unlock-data.sh
RemainAfterExit=yes

[Install]
WantedBy=local-fs.target
```

### 6.4 Secure Element / TPM Comparison

| Feature | OP-TEE + RPMB (current) | External TPM 2.0 | External Secure Element (e.g., STSAFE) |
|---------|------------------------|-------------------|---------------------------------------|
| Key storage | RPMB partition | TPM NV storage | SE internal flash |
| Tamper resistance | Software TEE + eMMC replay protection | Hardware tamper detection | Certified tamper resistance (CC EAL5+) |
| Crypto acceleration | STM32MP1 CRYP peripheral | TPM crypto engine | SE crypto engine |
| Cost | No additional BOM | $2-5 per unit | $1-3 per unit |
| Certification | Platform-dependent | FIPS 140-2/3 available | CC EAL5+ available |
| Recommendation | Sufficient for IEC 62443 SL-2 | Recommended for SL-3+ / FIPS | Recommended for SL-3+ / FIPS |

**Recommendation:** For IEC 62443 Security Level 2 (SL-2), the built-in OP-TEE + RPMB is sufficient. For SL-3 or SL-4, add an external secure element (STSAFE-A110 is pin-compatible with ST's ecosystem) or a TPM 2.0 module.

---

## 7. OTA Security

### 7.1 OTA Architecture

```
+-----------------+     +-----------------+     +-------------------+
|  Build Server   |     |  AWS IoT /      |     |  STM32MP1 Device  |
|  (HSM signing)  |---->|  S3 Bucket      |---->|                   |
|                 |     |  (Signed image  |     | 1. Verify sig     |
| 1. Build image  |     |   distribution) |     | 2. Write to       |
| 2. Sign image   |     |                 |     |    inactive slot  |
| 3. Upload       |     |                 |     | 3. Verify written |
+-----------------+     +-----------------+     | 4. Switch slot    |
                                                | 5. Reboot         |
                                                | 6. Validate boot  |
                                                | 7. Commit or      |
                                                |    rollback        |
                                                +-------------------+
```

### 7.2 A/B Partition Scheme

```
+-------------------------------------------+
|  U-Boot Environment                       |
|  boot_slot = "a" | "b"                    |
|  boot_attempts_a = 3                      |
|  boot_attempts_b = 3                      |
|  boot_success = 0 | 1                     |
+-------------------------------------------+

Boot Logic (in U-Boot script):
1. Read boot_slot (e.g., "a")
2. If boot_attempts_${boot_slot} > 0:
   a. Decrement boot_attempts_${boot_slot}
   b. Boot from rootfs_${boot_slot}
3. If boot_attempts_${boot_slot} == 0:
   a. Switch to other slot
   b. If other slot also exhausted: enter recovery mode
4. After successful Linux boot:
   a. Application sets boot_success = 1
   b. U-Boot on next reboot resets boot_attempts to 3
```

**U-Boot boot script:**

```bash
# boot.scr (simplified)
if test "${boot_slot}" = "a"; then
    setenv rootpart 2
    setenv veritypart 4
else
    setenv rootpart 3
    setenv veritypart 5
fi

# Verify and boot FIT image
if load mmc 1:1 ${loadaddr} fitImage; then
    if iminfo ${loadaddr}; then
        # Signature verified by U-Boot FIT verification
        bootm ${loadaddr}
    else
        echo "FIT signature verification FAILED"
        # Switch to other slot
        if test "${boot_slot}" = "a"; then
            setenv boot_slot b
        else
            setenv boot_slot a
        fi
        saveenv
        reset
    fi
fi
```

### 7.3 Signed OTA Updates

**Image signing (build server):**

```bash
# Sign the OTA update package
# Option 1: Using SWUpdate with signed images
openssl dgst -sha256 -sign ota_signing_key.pem \
  -out sw-description.sig sw-description

# Create SWU package (SWUpdate format)
echo "sw-description sw-description.sig rootfs.squashfs verity_hash.img" | \
  cpio -ov -H crc > update.swu

# Option 2: Simple signed image
# Create manifest
cat > manifest.json << EOF
{
    "version": "2.1.0",
    "anti_rollback_version": 5,
    "rootfs_hash": "$(sha256sum rootfs.squashfs | cut -d' ' -f1)",
    "verity_root_hash": "${ROOT_HASH}",
    "timestamp": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

# Sign manifest
openssl dgst -sha256 -sign ota_signing_key.pem \
  -out manifest.sig manifest.json
```

**On-device verification (ota.c integration):**

```c
/* Verify OTA signature before applying */
int verify_ota_signature(const char *manifest_path, const char *sig_path,
                         const char *pubkey_path)
{
    /* 1. Load OTA signing public key (from read-only rootfs or OP-TEE) */
    /* 2. Verify manifest signature */
    /* 3. Parse manifest, check anti_rollback_version >= current */
    /* 4. Verify rootfs hash matches manifest after download */
    /* 5. Return 0 on success, non-zero on any failure */
}
```

### 7.4 Rollback Protection for OTA

```
Anti-rollback counter (stored in U-Boot env + OTP):

U-Boot env: fw_rollback_counter = 5  (mutable, for current tracking)
OTP word 25: minimum_fw_version = 3   (immutable floor)

Rules:
1. New firmware version must be >= fw_rollback_counter
2. fw_rollback_counter is updated after successful boot validation
3. Periodically (e.g., every major release), increment OTP word 25
   to create an immutable minimum version floor
4. OTP increment is a manual, deliberate operation (not automated)
```

### 7.5 Failure Recovery

| Failure Scenario | Detection | Recovery |
|-----------------|-----------|----------|
| Download interrupted | Incomplete file / hash mismatch | Retry download, inactive slot unchanged |
| Signature invalid | Signature verification fails | Reject update, stay on current slot |
| Write failure | Post-write hash verification fails | Mark slot as bad, stay on current slot |
| Boot failure (kernel panic) | U-Boot boot_attempts reaches 0 | Auto-switch to previous slot |
| Application failure | Watchdog timeout / health check fail | Watchdog reboot, U-Boot switches slot after max attempts |
| Both slots corrupted | Both slots fail boot | Enter minimal recovery mode (USB DFU if enabled) |

---

## 8. Network Hardening

### 8.1 TLS Configuration

**MQTT TLS (mqtt.c):**

```c
/* Production TLS settings */
mosquitto_tls_set(mosq,
    ca_cert_path,       /* AWS IoT Root CA (AmazonRootCA1.pem) */
    NULL,               /* No CA directory */
    client_cert_path,   /* Device-unique X.509 certificate */
    client_key_path,    /* Private key (or PKCS#11 URI) */
    NULL                /* No password callback */
);

/* Enforce TLS 1.2 minimum */
mosquitto_tls_opts_set(mosq, SSL_VERIFY_PEER,
    "tlsv1.2",          /* Minimum TLS version */
    NULL                /* Default cipher list */
);

/* Restrict cipher suites to strong algorithms */
mosquitto_tls_opts_set(mosq, SSL_VERIFY_PEER,
    NULL,
    "ECDHE-ECDSA-AES256-GCM-SHA384:"
    "ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-AES128-GCM-SHA256:"
    "ECDHE-RSA-AES128-GCM-SHA256"
);
```

**HTTPS for OTA (curl in ota.c):**

```c
/* Production curl TLS settings */
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert_path);
curl_easy_setopt(curl, CURLOPT_PINNEDPUBLICKEY, "sha256//YhKJG3...=");
```

### 8.2 SSH Hardening

```bash
# /etc/ssh/sshd_config (production)

# Disable root login
PermitRootLogin no

# Key-based authentication only
PasswordAuthentication no
ChallengeResponseAuthentication no
PubkeyAuthentication yes

# Restrict to authorized keys only
AuthorizedKeysFile .ssh/authorized_keys

# Strong algorithms only
KexAlgorithms curve25519-sha256,diffie-hellman-group16-sha512
Ciphers chacha20-poly1305@openssh.com,aes256-gcm@openssh.com
MACs hmac-sha2-512-etm@openssh.com,hmac-sha2-256-etm@openssh.com
HostKeyAlgorithms ssh-ed25519,rsa-sha2-512

# Disable unnecessary features
X11Forwarding no
AllowTcpForwarding no
AllowAgentForwarding no
PermitTunnel no

# Session limits
MaxAuthTries 3
MaxSessions 2
LoginGraceTime 30
ClientAliveInterval 300
ClientAliveCountMax 2

# Restrict to specific user
AllowUsers service-user

# Logging
LogLevel VERBOSE
```

**Consider disabling SSH entirely in production** if remote access is not required. All management can be done via MQTT commands or OTA updates.

### 8.3 Firewall (iptables/nftables)

```bash
#!/bin/bash
# /etc/nftables.conf — Production firewall rules

nft flush ruleset

nft add table inet filter

# Input chain — default drop
nft add chain inet filter input '{ type filter hook input priority 0; policy drop; }'

# Allow loopback
nft add rule inet filter input iif lo accept

# Allow established/related connections
nft add rule inet filter input ct state established,related accept

# Allow ICMP (ping) — rate limited
nft add rule inet filter input ip protocol icmp icmp type echo-request \
  limit rate 5/second accept

# Allow SSH from management VLAN only (if SSH is enabled)
nft add rule inet filter input tcp dport 22 ip saddr 192.168.10.0/24 accept

# Allow Modbus TCP from local OT network only (if acting as TCP slave)
nft add rule inet filter input tcp dport 502 ip saddr 10.0.0.0/8 accept

# Drop everything else (implicit via policy drop)

# Output chain — allow specific destinations only
nft add chain inet filter output '{ type filter hook output priority 0; policy drop; }'

nft add rule inet filter output oif lo accept
nft add rule inet filter output ct state established,related accept

# Allow DNS
nft add rule inet filter output udp dport 53 accept
nft add rule inet filter output tcp dport 53 accept

# Allow NTP
nft add rule inet filter output udp dport 123 accept

# Allow MQTT to AWS IoT Core
nft add rule inet filter output tcp dport 8883 accept

# Allow HTTPS (OTA downloads)
nft add rule inet filter output tcp dport 443 accept

# Forward chain — drop all (gateway should not route)
nft add chain inet filter forward '{ type filter hook forward priority 0; policy drop; }'
```

### 8.4 Device Identity and Secure Provisioning

```
Device Identity Flow:

1. Manufacturing:
   - SoC UID read from OTP
   - Device key pair generated (ECDSA P-256)
   - CSR created with CN = SoC UID
   - Certificate signed by Device CA
   - Certificate registered with AWS IoT Core
   - Private key stored in OP-TEE secure storage

2. First Boot:
   - Device reads its certificate from filesystem
   - Device retrieves private key from OP-TEE (via PKCS#11)
   - Connects to AWS IoT Core with mutual TLS
   - AWS IoT verifies certificate chain
   - Device is authenticated and authorized

3. Key Rotation:
   - Device generates new key pair (in OP-TEE)
   - Creates CSR, sends to provisioning service via existing TLS channel
   - Receives new certificate
   - Activates new certificate, deactivates old
   - Old key material is securely deleted
```

### 8.5 Network Segmentation Recommendations

```
+------------------+     +-------------------+     +------------------+
|  OT Network      |     |  STM32MP1 Gateway |     |  IT Network /    |
|  (Modbus RTU/TCP)|<--->|                   |<--->|  Cloud           |
|  10.0.0.0/24     |     |  eth0: 10.0.0.1   |     |  via NAT/FW     |
|                  |     |  (no IP routing)  |     |                  |
+------------------+     +-------------------+     +------------------+

Rules:
- Gateway does NOT route between OT and IT networks
- Gateway has separate network interfaces (or VLANs) for OT and IT
- Modbus traffic never reaches the internet
- Cloud traffic (MQTT) goes through IT interface only
- IP forwarding is DISABLED: echo 0 > /proc/sys/net/ipv4/ip_forward
```

---

## 9. Linux Hardening

### 9.1 Kernel Configuration

```
# Security-relevant Kconfig options for production

# Enable security modules
CONFIG_SECURITY=y
CONFIG_SECURITYFS=y
CONFIG_SECURITY_NETWORK=y

# SELinux or AppArmor (choose one — see 9.2)
CONFIG_SECURITY_APPARMOR=y
CONFIG_DEFAULT_SECURITY_APPARMOR=y

# Disable unnecessary features
CONFIG_DEVKMEM=n                    # No /dev/kmem
CONFIG_DEVMEM=n                     # No /dev/mem (or use CONFIG_STRICT_DEVMEM=y)
CONFIG_STRICT_DEVMEM=y              # Restrict /dev/mem if enabled
CONFIG_IO_STRICT_DEVMEM=y           # Restrict I/O access
CONFIG_KEXEC=n                      # No kexec (prevent loading unsigned kernels)
CONFIG_HIBERNATION=n                # No hibernation (prevents memory dump attacks)
CONFIG_BINFMT_MISC=n                # No misc binary formats

# Stack protector
CONFIG_STACKPROTECTOR=y
CONFIG_STACKPROTECTOR_STRONG=y

# Hardened usercopy
CONFIG_HARDENED_USERCOPY=y

# Address space layout randomization
CONFIG_RANDOMIZE_BASE=y             # KASLR (if supported on ARM)

# Module signing
CONFIG_MODULE_SIG=y
CONFIG_MODULE_SIG_FORCE=y           # Reject unsigned modules
CONFIG_MODULE_SIG_SHA256=y
CONFIG_MODULE_SIG_ALL=y

# Restrict kernel logs
CONFIG_SECURITY_DMESG_RESTRICT=y

# Disable magic sysrq in production
CONFIG_MAGIC_SYSRQ=n

# Enable audit
CONFIG_AUDIT=y
CONFIG_AUDITSYSCALL=y

# Network security
CONFIG_SYN_COOKIES=y                # SYN flood protection

# Disable debugging features
CONFIG_DEBUG_KERNEL=n
CONFIG_KGDB=n
CONFIG_KALLSYMS=n                   # No symbol table
CONFIG_DEBUG_FS=n                   # No debugfs
CONFIG_FTRACE=n                     # No function tracing (prod)
CONFIG_PROFILING=n
```

**Kernel command line (production):**

```
root=/dev/dm-0 ro quiet loglevel=0 panic=10
dm-mod.create="verity,,,ro,0 524288 verity 1 /dev/mmcblk1p2 /dev/mmcblk1p4 4096 4096 65536 1 sha256 ROOT_HASH SALT"
apparmor=1 security=apparmor
slub_debug=FZP
init_on_alloc=1
init_on_free=1
page_alloc.shuffle=1
```

### 9.2 SELinux vs AppArmor

| Criterion | SELinux | AppArmor |
|-----------|---------|----------|
| Policy model | Label-based (every object has a security context) | Path-based (rules based on file paths) |
| Complexity | High — requires complete policy for all processes | Medium — only confine specific processes |
| Yocto support | `meta-selinux` layer | Built-in `meta-security` |
| STM32MP1 suitability | Overkill for single-application gateway | Good fit — confine main application + services |
| Resource overhead | Higher (label processing) | Lower |
| IEC 62443 compliance | Meets MAC requirements | Meets MAC requirements |

**Recommendation:** Use **AppArmor** for this project. It provides sufficient mandatory access control (MAC) with lower complexity, which is appropriate for a dedicated single-application device.

**AppArmor profile for the gateway application:**

```
# /etc/apparmor.d/usr.bin.smart_rtu
#include <tunables/global>

/usr/bin/smart_rtu {
    #include <abstractions/base>

    # Network access
    network inet stream,          # TCP (MQTT, HTTP)
    network inet dgram,           # UDP (NTP, DNS)

    # Serial port access (Modbus RTU)
    /dev/ttySTM[0-9]* rw,

    # GPIO access (RS485 DE pin)
    /dev/gpiochip* rw,
    /sys/class/gpio/** rw,

    # DRM display
    /dev/dri/** rw,

    # Touch input
    /dev/input/event* r,

    # Configuration (read-only)
    /etc/smart_rtu/ r,
    /etc/smart_rtu/smart_rtu_config.json r,

    # Certificates (read-only)
    /etc/ssl/device/ r,
    /etc/ssl/device/** r,

    # Data partition (read-write)
    /var/lib/smart_rtu/ rw,
    /var/lib/smart_rtu/** rwk,

    # SQLite
    /var/lib/smart_rtu/*.db rwk,
    /var/lib/smart_rtu/*.db-journal rwk,
    /var/lib/smart_rtu/*.db-wal rwk,

    # Temporary files
    /tmp/smart_rtu_* rw,

    # Libraries
    /usr/lib/*.so* mr,
    /lib/*.so* mr,

    # OP-TEE client
    /dev/tee* rw,
    /dev/teepriv* rw,

    # Deny everything else (implicit)
}
```

### 9.3 Read-Only Root Filesystem

```bash
# In Yocto local.conf:
IMAGE_FEATURES += "read-only-rootfs"

# Mount options in fstab:
/dev/dm-0  /       squashfs  ro,noatime            0  0
tmpfs      /tmp    tmpfs     rw,nosuid,nodev,size=32M  0  0
tmpfs      /var/run tmpfs    rw,nosuid,nodev,size=8M   0  0
tmpfs      /var/lock tmpfs   rw,nosuid,nodev,size=4M   0  0
/dev/mmcblk1p6  /var/lib  ext4  rw,nosuid,nodev,noexec  0  2
```

**Writable overlay for `/etc` (if needed):**

```bash
# Use overlayfs for minimal writable config
mount -t overlay overlay \
  -o lowerdir=/etc,upperdir=/var/lib/etc-overlay,workdir=/var/lib/etc-work \
  /etc
```

### 9.4 Secure Logging

```bash
# Use journald with persistent storage to encrypted data partition
# /etc/systemd/journald.conf
[Journal]
Storage=persistent
SystemMaxUse=50M
SystemMaxFileSize=10M
MaxRetentionSec=30d
Compress=yes
Seal=yes                    # Forward-secure sealing (prevents log tampering)
SplitMode=uid

# Forward logs to remote syslog (if available)
ForwardToSyslog=yes
```

**Log integrity with systemd journal sealing:**

```bash
# Generate sealing key pair
journalctl --setup-keys
# Store the verification key off-device (on management server)
# The sealing key on-device is rotated automatically

# Verify log integrity
journalctl --verify
```

### 9.5 Least-Privilege Services

```bash
# systemd service file for the gateway application
# /etc/systemd/system/smart-rtu.service
[Unit]
Description=Smart RTU Industrial Gateway
After=network-online.target unlock-data.service
Wants=network-online.target
Requires=unlock-data.service

[Service]
Type=simple
ExecStart=/usr/bin/smart_rtu
Restart=on-failure
RestartSec=5
WatchdogSec=30

# Security hardening directives
User=smartrtu
Group=smartrtu
SupplementaryGroups=dialout gpio i2c video input

# Capabilities — only what's needed
CapabilityBoundingSet=CAP_NET_BIND_SERVICE CAP_NET_RAW
AmbientCapabilities=CAP_NET_BIND_SERVICE CAP_NET_RAW
NoNewPrivileges=yes

# Filesystem restrictions
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/smart_rtu /tmp
ReadOnlyPaths=/etc/smart_rtu /etc/ssl/device
PrivateTmp=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes

# System call filtering
SystemCallFilter=@system-service @io-event @network-io
SystemCallFilter=~@debug @mount @reboot @swap @raw-io
SystemCallArchitectures=native

# Device access control
DevicePolicy=closed
DeviceAllow=/dev/ttySTM0 rw
DeviceAllow=/dev/ttySTM1 rw
DeviceAllow=/dev/gpiochip4 rw
DeviceAllow=/dev/dri/card0 rw
DeviceAllow=/dev/input/event0 r
DeviceAllow=/dev/tee0 rw
DeviceAllow=/dev/teepriv0 rw

# Memory protection
MemoryDenyWriteExecute=yes

# Network restrictions
RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX
IPAddressDeny=any
IPAddressAllow=10.0.0.0/8 172.16.0.0/12 192.168.0.0/16
# Add specific cloud endpoints as needed

[Install]
WantedBy=multi-user.target
```

### 9.6 Additional Hardening

```bash
# Disable unnecessary kernel modules
cat > /etc/modprobe.d/hardening.conf << 'EOF'
install usb-storage /bin/true
install firewire-core /bin/true
install cramfs /bin/true
install freevxfs /bin/true
install jffs2 /bin/true
install hfs /bin/true
install hfsplus /bin/true
install udf /bin/true
EOF

# Sysctl hardening
cat > /etc/sysctl.d/90-hardening.conf << 'EOF'
# Disable IP forwarding
net.ipv4.ip_forward = 0
net.ipv6.conf.all.forwarding = 0

# Ignore ICMP redirects
net.ipv4.conf.all.accept_redirects = 0
net.ipv6.conf.all.accept_redirects = 0
net.ipv4.conf.all.send_redirects = 0

# Ignore source-routed packets
net.ipv4.conf.all.accept_source_route = 0
net.ipv6.conf.all.accept_source_route = 0

# Enable SYN cookies
net.ipv4.tcp_syncookies = 1

# Log martian packets
net.ipv4.conf.all.log_martians = 1

# Disable IPv6 if not used
net.ipv6.conf.all.disable_ipv6 = 1

# Restrict kernel pointer exposure
kernel.kptr_restrict = 2

# Restrict dmesg access
kernel.dmesg_restrict = 1

# Restrict perf events
kernel.perf_event_paranoid = 3

# Restrict unprivileged user namespaces
kernel.unprivileged_userns_clone = 0

# ASLR — full randomization
kernel.randomize_va_space = 2

# Restrict ptrace
kernel.yama.ptrace_scope = 3

# Restrict core dumps
fs.suid_dumpable = 0
EOF
```

---

## 10. STM32MP1-Specific Implementation

### 10.1 TF-A Build Configuration

```makefile
# TF-A build for STM32MP157F-DK2 with security features
make CROSS_COMPILE=arm-linux-gnueabihf- \
     PLAT=stm32mp1 \
     ARCH=aarch32 \
     ARM_ARCH_MAJOR=7 \
     AARCH32_SP=optee \
     DTB_FILE_NAME=stm32mp157f-dk2.dtb \
     BL33_CFG=$(UBOOT_DIR)/u-boot.dtb \
     STM32MP_SDMMC=0 \
     STM32MP_EMMC=1 \
     TRUSTED_BOARD_BOOT=1 \
     MBEDTLS_DIR=$(MBEDTLS_DIR) \
     GENERATE_COT=1 \
     ROT_KEY=keys/oem_root_key.pem \
     STM32MP_ANTI_ROLLBACK=1 \
     LOG_LEVEL=0 \
     all fip
```

**Key TF-A options:**

| Option | Development | Production |
|--------|------------|------------|
| `TRUSTED_BOARD_BOOT` | 0 | 1 |
| `STM32MP_ANTI_ROLLBACK` | 0 | 1 |
| `LOG_LEVEL` | 40 (verbose) | 0 (none) |
| `DEBUG` | 1 | 0 |
| `GENERATE_COT` | 1 | 1 |
| `MBEDTLS_DIR` | path | path |

### 10.2 OP-TEE Build Configuration

```makefile
# OP-TEE OS build for STM32MP157F
make CROSS_COMPILE=arm-linux-gnueabihf- \
     PLATFORM=stm32mp1 \
     PLATFORM_FLAVOR=157F_DK2 \
     CFG_EMBED_DTB_SOURCE_FILE=stm32mp157f-dk2.dts \
     CFG_RPMB_FS=y \
     CFG_RPMB_FS_DEV_ID=0 \
     CFG_REE_FS=n \
     CFG_RPMB_WRITE_KEY=y \
     CFG_PKCS11_TA=y \
     CFG_SYSTEM_PTA=y \
     CFG_TA_STRICT_ANNOTATION_CHECKS=y \
     CFG_CORE_HEAP_SIZE=131072 \
     CFG_TEE_CORE_LOG_LEVEL=0 \
     CFG_WITH_SOFTWARE_PRNG=n \
     CFG_WITH_HWRNG=y \
     CFG_STM32_CRYP=y \
     CFG_STM32_HASH=y \
     CFG_STM32_RNG=y \
     all
```

**Key OP-TEE options:**

| Option | Purpose |
|--------|---------|
| `CFG_RPMB_FS=y` | Use eMMC RPMB for secure storage |
| `CFG_REE_FS=n` | Disable less-secure REE filesystem storage |
| `CFG_PKCS11_TA=y` | Build PKCS#11 Trusted Application |
| `CFG_WITH_HWRNG=y` | Use hardware TRNG |
| `CFG_STM32_CRYP=y` | Use hardware crypto accelerator |
| `CFG_TEE_CORE_LOG_LEVEL=0` | No OP-TEE log output in production |

### 10.3 U-Boot Configuration

```
# Production additions to stm32mp15_defconfig

# Verified boot (FIT signature)
CONFIG_FIT=y
CONFIG_FIT_SIGNATURE=y
CONFIG_FIT_SIGNATURE_ENFORCE=y
CONFIG_RSA=y
CONFIG_RSA_VERIFY=y
CONFIG_IMAGE_FORMAT_LEGACY=n

# Disable interactive shell in production
CONFIG_AUTOBOOT=y
CONFIG_BOOTDELAY=0
CONFIG_CMDLINE_EDITING=n
CONFIG_HUSH_PARSER=n
CONFIG_SYS_GENERIC_BOARD=y

# Disable unnecessary commands
CONFIG_CMD_BOOTEFI=n
CONFIG_CMD_ELF=n
CONFIG_CMD_IMLS=n
CONFIG_CMD_XIMG=n
CONFIG_CMD_EDITENV=n
CONFIG_CMD_SAVEENV=n          # Prevent env modification (if env is compiled-in)

# Disable network boot (no TFTP, NFS)
CONFIG_CMD_NET=n
CONFIG_CMD_NFS=n
CONFIG_CMD_DHCP=n

# Disable USB boot
CONFIG_CMD_USB=n

# Silent boot
CONFIG_SILENT_CONSOLE=y
CONFIG_DISABLE_CONSOLE=y

# Watchdog
CONFIG_WDT=y
CONFIG_WDT_STM32MP=y
```

### 10.4 Device Tree Security Settings

```dts
/* stm32mp157f-dk2-production.dts — security-relevant nodes */

/* Enable hardware crypto */
&cryp1 {
    status = "okay";
};

&hash1 {
    status = "okay";
};

&rng1 {
    status = "okay";
};

/* Enable hardware watchdog */
&iwdg2 {
    status = "okay";
    timeout-sec = <32>;
};

/* UART console — disabled in production */
&usart3 {
    status = "disabled";  /* Was "okay" in development */
};

/* OP-TEE shared memory */
&optee {
    status = "okay";
};

/* eMMC — enable RPMB access */
&sdmmc2 {
    status = "okay";
    non-removable;
    no-sd;
    no-sdio;
    st,neg-edge;
    bus-width = <8>;
    vmmc-supply = <&v3v3>;
    mmc-ddr-3_3v;
};
```

### 10.5 Yocto Layer Configuration

**Layer structure:**

```
meta-smart-rtu/
├── conf/
│   ├── layer.conf
│   └── machine/
│       └── stm32mp157f-dk2-production.conf
├── recipes-bsp/
│   ├── tf-a/
│   │   └── tf-a-stm32mp_%.bbappend
│   ├── optee/
│   │   └── optee-os-stm32mp_%.bbappend
│   └── u-boot/
│       └── u-boot-stm32mp_%.bbappend
├── recipes-core/
│   ├── images/
│   │   └── smart-rtu-image.bb
│   └── systemd/
│       └── systemd_%.bbappend
├── recipes-security/
│   ├── apparmor/
│   │   └── apparmor_%.bbappend
│   └── dm-verity/
│       └── dm-verity-image.bbclass
└── recipes-app/
    └── smart-rtu/
        └── smart-rtu_1.0.bb
```

**local.conf (production build):**

```bash
# Machine
MACHINE = "stm32mp157f-dk2"

# Distribution
DISTRO = "openstlinux-weston"

# Security features
IMAGE_FEATURES += "read-only-rootfs"
IMAGE_FEATURES_remove = "debug-tweaks"
IMAGE_FEATURES_remove = "allow-empty-password"
IMAGE_FEATURES_remove = "allow-root-login"

# dm-verity
INHERIT += "dm-verity-img"
DM_VERITY_IMAGE = "smart-rtu-image"
DM_VERITY_IMAGE_TYPE = "squashfs"

# Image type
IMAGE_FSTYPES = "squashfs"
IMAGE_FSTYPES_remove = "ext4 tar.gz"

# Kernel module signing
KERNEL_MODULE_SIG_KEY = "${TOPDIR}/keys/kernel_module_sign.pem"

# Remove development tools
IMAGE_INSTALL_remove = "gdb strace ltrace"
PACKAGE_EXCLUDE = "openssh-sftp-server packagegroup-core-tools-debug"

# Hardened compiler flags
SECURITY_CFLAGS = "-fstack-protector-strong -D_FORTIFY_SOURCE=2"
SECURITY_LDFLAGS = "-Wl,-z,relro,-z,now"
TARGET_CFLAGS_append = " ${SECURITY_CFLAGS}"
TARGET_LDFLAGS_append = " ${SECURITY_LDFLAGS}"
```

**TF-A bbappend (production):**

```bash
# recipes-bsp/tf-a/tf-a-stm32mp_%.bbappend

EXTRA_OEMAKE += " \
    TRUSTED_BOARD_BOOT=1 \
    MBEDTLS_DIR=${STAGING_DIR_HOST}/usr/share/mbedtls \
    GENERATE_COT=1 \
    ROT_KEY=${TOPDIR}/keys/oem_root_key.pem \
    STM32MP_ANTI_ROLLBACK=1 \
    LOG_LEVEL=0 \
"
```

### 10.6 OTP Programming Commands (STM32CubeProgrammer)

```bash
#!/bin/bash
# provision_device.sh — Factory provisioning script
# WARNING: This script programs IRREVERSIBLE OTP fuses.
# Only run on production devices after thorough verification.

set -euo pipefail

EXPECTED_KEY_HASH="a1b2c3d4e5f67890..."  # Your actual hash
DEVICE_PORT="USB1"

echo "=== STM32MP1 Production Provisioning ==="
echo "WARNING: This will program IRREVERSIBLE OTP fuses!"
read -p "Type 'PRODUCTION' to continue: " confirm
if [ "$confirm" != "PRODUCTION" ]; then
    echo "Aborted."
    exit 1
fi

# Step 1: Read device UID
echo "[1/7] Reading device UID..."
STM32_Programmer_CLI -c port=$DEVICE_PORT -otp displ word=0

# Step 2: Verify OTP is blank
echo "[2/7] Verifying OTP key words are blank..."
for word in $(seq 32 39); do
    val=$(STM32_Programmer_CLI -c port=$DEVICE_PORT -otp displ word=$word | grep "Value" | awk '{print $3}')
    if [ "$val" != "0x00000000" ]; then
        echo "ERROR: OTP word $word is not blank (value: $val). Aborting."
        exit 1
    fi
done
echo "OTP key words are blank. Proceeding."

# Step 3: Program key hash
echo "[3/7] Programming OEM root key hash..."
# Split EXPECTED_KEY_HASH into 8 x 32-bit words
for i in $(seq 0 7); do
    offset=$((i * 8))
    word_val="0x${EXPECTED_KEY_HASH:$offset:8}"
    otp_word=$((32 + i))
    echo "  Writing OTP word $otp_word = $word_val"
    STM32_Programmer_CLI -c port=$DEVICE_PORT \
        -otp write word=$otp_word value=$word_val
done

# Step 4: Verify key hash
echo "[4/7] Verifying programmed key hash..."
for i in $(seq 0 7); do
    offset=$((i * 8))
    expected="0x${EXPECTED_KEY_HASH:$offset:8}"
    otp_word=$((32 + i))
    actual=$(STM32_Programmer_CLI -c port=$DEVICE_PORT \
        -otp displ word=$otp_word | grep "Value" | awk '{print $3}')
    if [ "$actual" != "$expected" ]; then
        echo "ERROR: OTP word $otp_word mismatch! Expected $expected, got $actual"
        echo "DO NOT enable secure boot. Investigate immediately."
        exit 1
    fi
done
echo "Key hash verified successfully."

# Step 5: Enable secure boot
echo "[5/7] Enabling secure boot (IRREVERSIBLE)..."
read -p "Final confirmation — type 'FUSE' to enable secure boot: " confirm2
if [ "$confirm2" != "FUSE" ]; then
    echo "Aborted. Key hash is programmed but secure boot is NOT enabled."
    exit 1
fi
STM32_Programmer_CLI -c port=$DEVICE_PORT \
    -otp write word=8 value=0x00000001

# Step 6: Configure debug access
echo "[6/7] Configuring debug access..."
# Enable authenticated debug (not fully disabled)
STM32_Programmer_CLI -c port=$DEVICE_PORT \
    -otp write word=57 value=0x00000001

# Step 7: Lock key hash OTP words
echo "[7/7] Locking OTP key hash words..."
for word in $(seq 32 39); do
    STM32_Programmer_CLI -c port=$DEVICE_PORT \
        -otp lock word=$word
done

echo "=== Provisioning complete ==="
echo "Reboot the device and verify it boots the signed firmware."
```

### 10.7 Boot Verification Testing Procedure

```bash
#!/bin/bash
# verify_secure_boot.sh — Run on provisioned device

echo "=== Secure Boot Verification ==="

# Test 1: Verify boot chain
echo "[Test 1] Checking boot chain..."
if dmesg | grep -q "optee: probing"; then
    echo "  PASS: OP-TEE is loaded"
else
    echo "  FAIL: OP-TEE not detected"
fi

# Test 2: Verify dm-verity
echo "[Test 2] Checking dm-verity..."
if dmsetup status | grep -q "verity"; then
    echo "  PASS: dm-verity is active"
else
    echo "  FAIL: dm-verity not active"
fi

# Test 3: Verify read-only rootfs
echo "[Test 3] Checking rootfs is read-only..."
if mount | grep "on / " | grep -q "ro,"; then
    echo "  PASS: rootfs is read-only"
else
    echo "  FAIL: rootfs is read-write"
fi

# Test 4: Verify AppArmor
echo "[Test 4] Checking AppArmor..."
if aa-status 2>/dev/null | grep -q "enforce"; then
    echo "  PASS: AppArmor profiles in enforce mode"
else
    echo "  FAIL: AppArmor not active or not enforcing"
fi

# Test 5: Verify JTAG is restricted
echo "[Test 5] Checking debug port status..."
# This must be tested externally with a JTAG probe
echo "  MANUAL: Connect JTAG probe and verify access is denied"

# Test 6: Verify serial console is disabled
echo "[Test 6] Checking serial console..."
if ! grep -q "console=ttySTM" /proc/cmdline; then
    echo "  PASS: Serial console not in kernel cmdline"
else
    echo "  WARN: Serial console is enabled — check if intentional"
fi

# Test 7: Verify firewall
echo "[Test 7] Checking firewall..."
if nft list ruleset | grep -q "policy drop"; then
    echo "  PASS: Default drop policy active"
else
    echo "  FAIL: Firewall not configured or permissive"
fi

# Test 8: Verify OP-TEE secure storage
echo "[Test 8] Checking OP-TEE secure storage..."
if ls /dev/tee* > /dev/null 2>&1; then
    echo "  PASS: TEE device nodes present"
else
    echo "  FAIL: TEE device nodes missing"
fi

# Test 9: Verify SSH hardening
echo "[Test 9] Checking SSH configuration..."
if sshd -T 2>/dev/null | grep -q "passwordauthentication no"; then
    echo "  PASS: Password authentication disabled"
else
    echo "  FAIL: Password authentication may be enabled"
fi

# Test 10: Verify kernel module signing
echo "[Test 10] Checking kernel module signing enforcement..."
if grep -q "module.sig_enforce" /proc/cmdline || \
   cat /proc/config.gz 2>/dev/null | zcat | grep -q "CONFIG_MODULE_SIG_FORCE=y"; then
    echo "  PASS: Module signature enforcement active"
else
    echo "  WARN: Module signature enforcement status unknown"
fi

# Test 11: Attempt to boot unsigned image (negative test)
echo "[Test 11] Negative test — must be run manually:"
echo "  1. Create an unsigned kernel image"
echo "  2. Place it in the boot partition"
echo "  3. Reboot — the device MUST refuse to boot it"
echo "  4. Restore the signed image"

echo ""
echo "=== Verification Complete ==="
echo "Review MANUAL and FAIL items above."
```

---

## 11. Compliance Mapping

### 11.1 IEC 62443 Mapping

| IEC 62443 Requirement | Section | Security Feature | Implementation |
|----------------------|---------|-----------------|----------------|
| **FR 1: Identification & Authentication** | | | |
| SR 1.1 — Human user identification | 8.2 | SSH key-only auth | `sshd_config: PubkeyAuthentication yes` |
| SR 1.2 — Software process identification | 8.4 | X.509 device identity | Per-device certificate in OP-TEE |
| SR 1.5 — Authenticator management | 3.5 | PKCS#11 key management | OP-TEE PKCS#11 TA |
| SR 1.7 — Strength of password-based auth | 8.2 | No passwords used | Key-only SSH, certificate-based MQTT |
| **FR 2: Use Control** | | | |
| SR 2.1 — Authorization enforcement | 9.2 | AppArmor MAC | Per-application AppArmor profiles |
| SR 2.4 — Mobile code | 9.1 | No code execution | `CONFIG_BINFMT_MISC=n`, signed modules only |
| SR 2.5 — Session lock | 8.2 | SSH session timeout | `ClientAliveInterval 300` |
| **FR 3: System Integrity** | | | |
| SR 3.1 — Communication integrity | 8.1 | TLS 1.2+ everywhere | MQTT mTLS, HTTPS for OTA |
| SR 3.2 — Malicious code protection | 2.0, 6.2 | Secure boot + dm-verity | Signed boot chain, verified rootfs |
| SR 3.4 — Software/info integrity | 7.3 | Signed OTA updates | RSA/ECDSA signature verification |
| SR 3.7 — Error handling | 9.5 | Least-privilege services | systemd hardening directives |
| **FR 4: Data Confidentiality** | | | |
| SR 4.1 — Information confidentiality | 6.3 | fscrypt data encryption | Encrypted data partition |
| SR 4.3 — Use of cryptography | 3.0 | Hardware crypto, HSM signing | STM32MP1 CRYP, external HSM |
| **FR 5: Restricted Data Flow** | | | |
| SR 5.1 — Network segmentation | 8.5 | Firewall, no IP forwarding | nftables rules, sysctl |
| SR 5.2 — Zone boundary protection | 8.3 | Firewall ingress/egress rules | Default-deny policy |
| **FR 6: Timely Response to Events** | | | |
| SR 6.1 — Audit log accessibility | 9.4 | journald with integrity | Forward-secure sealing |
| SR 6.2 — Continuous monitoring | 5.0, 9.5 | Watchdog thread | Hardware watchdog + thread monitoring |
| **FR 7: Resource Availability** | | | |
| SR 7.1 — DoS protection | 8.3 | Rate limiting, SYN cookies | nftables rate limit, sysctl |
| SR 7.2 — Resource management | 9.5 | systemd resource limits | MemoryMax, CPUQuota |
| SR 7.4 — Control system recovery | 7.5 | A/B partition + rollback | U-Boot A/B boot logic |
| SR 7.6 — Network/security config settings | 4.0 | Secure manufacturing | OTP provisioning, factory checklist |

### 11.2 NIST SP 800-193 (Platform Firmware Resiliency) Mapping

| NIST 800-193 Guideline | Implementation |
|------------------------|----------------|
| Protection of firmware code and data | Secure boot chain (Section 2), dm-verity (Section 6.2) |
| Detection of firmware corruption | dm-verity hash verification, boot signature checks |
| Recovery from firmware corruption | A/B partitions (Section 7.2), U-Boot fallback |
| Protection of update mechanisms | Signed OTA (Section 7.3), TLS transport |
| Detection of update integrity | SHA-256 hash + RSA/ECDSA signature on OTA packages |
| Recovery of update failures | A/B rollback, boot attempt counter (Section 7.4) |
| Protection of the Root of Trust | OTP-stored key hash, HSM for private keys (Section 3.1) |

### 11.3 OWASP IoT Top 10 Mapping

| OWASP IoT Risk | Mitigation | Section |
|---------------|------------|---------|
| 1. Weak/default passwords | No passwords — key/cert-based auth only | 8.2, 8.4 |
| 2. Insecure network services | Firewall default-deny, minimal open ports | 8.3 |
| 3. Insecure ecosystem interfaces | TLS 1.2+ for all external comms | 8.1 |
| 4. Lack of secure update | Signed A/B OTA with rollback protection | 7.0 |
| 5. Insecure/outdated components | Yocto CVE monitoring, regular updates | 7.0, 9.0 |
| 6. Insufficient privacy | fscrypt for data at rest, TLS in transit | 6.3, 8.1 |
| 7. Insecure data transfer | Mutual TLS, certificate pinning | 8.1 |
| 8. Lack of device management | MQTT-based management, OTA, watchdog | 7.0, 9.5 |
| 9. Insecure default settings | `IMAGE_FEATURES_remove = "debug-tweaks"` | 10.5 |
| 10. Lack of physical hardening | OTP fuses, JTAG disable, console disable | 5.0 |

---

## 12. Final Checklist

### 12.1 Security Feature Status Table

| # | Feature | Why It Matters | Mandatory / Optional | Status |
|---|---------|---------------|---------------------|--------|
| **Boot Security** | | | | |
| 1 | OEM root key in HSM | Protects signing key from theft | Mandatory | [ ] Not started |
| 2 | TF-A signature verification | Prevents boot-level malware | Mandatory | [ ] Not started |
| 3 | OP-TEE signed | Protects trusted execution environment | Mandatory | [ ] Not started |
| 4 | U-Boot FIT signature | Prevents kernel/DTB tampering | Mandatory | [ ] Not started |
| 5 | Anti-rollback counters | Prevents downgrade attacks | Mandatory | [ ] Not started |
| 6 | OTP key hash programmed | Hardware root of trust | Mandatory | [ ] Not started |
| 7 | Secure boot enabled (OTP) | Enforces signed-only boot | Mandatory | [ ] Not started |
| **Cryptography** | | | | |
| 8 | HUK provisioned | Device-unique key derivation | Mandatory (ST default) | [ ] Verify |
| 9 | OP-TEE RPMB storage | Replay-protected key storage | Mandatory | [ ] Not started |
| 10 | PKCS#11 for MQTT keys | Keys never leave TEE | Recommended | [ ] Not started |
| 11 | Per-device X.509 certificate | Unique device identity | Mandatory | [ ] Not started |
| **Debug Protection** | | | | |
| 12 | JTAG restricted/disabled | Prevents physical debug attacks | Mandatory | [ ] Not started |
| 13 | UART console disabled | Prevents serial shell access | Mandatory | [ ] Not started |
| 14 | U-Boot shell disabled | Prevents boot-time manipulation | Mandatory | [ ] Not started |
| **Storage** | | | | |
| 15 | dm-verity on rootfs | Prevents rootfs tampering | Mandatory | [ ] Not started |
| 16 | Read-only root filesystem | Prevents persistent modification | Mandatory | [ ] Not started |
| 17 | fscrypt on data partition | Protects data at rest | Recommended | [ ] Not started |
| 18 | A/B partition layout | Enables safe OTA and rollback | Mandatory | [ ] Not started |
| **OTA** | | | | |
| 19 | Signed OTA packages | Prevents malicious updates | Mandatory | [ ] Not started |
| 20 | OTA signature verification | Validates update authenticity | Mandatory | [ ] Not started |
| 21 | A/B rollback on boot failure | Automatic recovery | Mandatory | [ ] Not started |
| 22 | Anti-rollback for OTA | Prevents version downgrade | Recommended | [ ] Not started |
| **Network** | | | | |
| 23 | TLS 1.2+ for MQTT | Encrypted cloud communication | Mandatory | [ ] In progress |
| 24 | Mutual TLS (X.509) | Authenticated cloud connection | Mandatory | [ ] In progress |
| 25 | SSH key-only auth | Prevents brute-force attacks | Mandatory (if SSH used) | [ ] Not started |
| 26 | Firewall (default deny) | Minimizes attack surface | Mandatory | [ ] Not started |
| 27 | IP forwarding disabled | Prevents gateway abuse | Mandatory | [ ] Not started |
| 28 | Certificate pinning | Prevents MitM attacks | Recommended | [ ] Not started |
| **Linux** | | | | |
| 29 | AppArmor enforcing | Mandatory access control | Mandatory | [ ] Not started |
| 30 | Kernel module signing | Prevents rogue modules | Mandatory | [ ] Not started |
| 31 | Hardened sysctl | Kernel-level attack surface reduction | Mandatory | [ ] Not started |
| 32 | systemd service hardening | Least-privilege execution | Mandatory | [ ] Not started |
| 33 | No development tools on image | Reduces attack surface | Mandatory | [ ] Not started |
| 34 | Hardened compiler flags | Exploit mitigation (ASLR, stack canary, etc.) | Mandatory | [ ] Not started |
| **Manufacturing** | | | | |
| 35 | Factory provisioning script | Consistent, repeatable provisioning | Mandatory | [ ] Not started |
| 36 | Golden sample verification | Prevents batch bricking | Mandatory | [ ] Not started |
| 37 | Post-provisioning boot test | Validates device functionality | Mandatory | [ ] Not started |
| 38 | AWS IoT certificate registration | Cloud connectivity | Mandatory | [ ] Not started |
| **Monitoring** | | | | |
| 39 | Hardware watchdog | Recovers from hangs | Mandatory | [ ] In progress |
| 40 | Secure logging (journal seal) | Tamper-evident logs | Recommended | [ ] Not started |
| 41 | Thread heartbeat monitoring | Detects stuck threads | Mandatory | [ ] In progress |

### 12.2 Pre-Production Gate Criteria

Before moving from development to production:

- [ ] All "Mandatory" items above are implemented and tested.
- [ ] Golden sample has been provisioned and fully verified.
- [ ] Negative tests passed (unsigned boot rejected, invalid OTA rejected, JTAG denied).
- [ ] Penetration test conducted (or scheduled).
- [ ] Key management procedures documented and HSM operational.
- [ ] Factory provisioning script tested on 10+ units without failure.
- [ ] A/B rollback tested: intentionally corrupt slot A, verify automatic recovery to slot B.
- [ ] OTA end-to-end tested: build -> sign -> upload -> device downloads -> verifies -> applies -> boots.
- [ ] IEC 62443 self-assessment completed for target Security Level.
- [ ] Incident response plan documented (what happens if a signing key is compromised).

### 12.3 Irreversible Operations Summary

> These operations cannot be undone. Triple-check before executing.

| Operation | OTP Word | Effect | Recovery if Wrong |
|-----------|----------|--------|-------------------|
| Program OEM key hash | 32-39 | Only this key can sign boot images | **None** — device locked to this key forever |
| Enable secure boot | 8, bit 0 | ROM rejects unsigned TF-A | **None** — must have correct key hash first |
| Lock key hash words | 32-39 lock bits | Key hash cannot be modified | **None** — hash is permanent |
| Disable JTAG | 57, bits 1-4 | No debug access ever | **None** — hardware debug permanently disabled |
| Disable USB/UART boot | 8, specific bits | No ROM-level recovery boot | **None** — if eMMC is corrupted, device is bricked |
| Increment anti-rollback | 24, 25 | Older firmware versions rejected | **None** — counter only goes up |

---

## Appendix A: Reference Architecture Diagram

```
+===========================================================================+
|                        STM32MP1 SECURITY ARCHITECTURE                      |
+===========================================================================+
|                                                                            |
|  HARDWARE ROOT OF TRUST                                                    |
|  +------------------+  +------------------+  +------------------+          |
|  |  Boot ROM        |  |  BSEC (OTP)      |  |  CRYP/HASH/RNG  |          |
|  |  (Immutable)     |  |  Key Hash        |  |  HW Crypto       |          |
|  +--------+---------+  |  Security Config  |  |  Accelerators    |          |
|           |             |  Anti-rollback   |  +------------------+          |
|           v             |  Debug Config    |                               |
|  +--------+---------+  +------------------+                               |
|  |  TF-A (Signed)   |                                                     |
|  |  TRUSTED_BOARD_BOOT=1                                                  |
|  +--------+---------+                                                     |
|           |                                                                |
|  +--------+---------+  +------------------+                               |
|  |  OP-TEE (Signed) |  |  RPMB Secure     |                               |
|  |  PKCS#11 TA      |<>|  Storage          |                               |
|  |  Key Derivation   |  |  Device Keys     |                               |
|  +--------+---------+  +------------------+                               |
|           |                                                                |
|  +--------+---------+                                                     |
|  |  U-Boot (Signed) |                                                     |
|  |  FIT Verification |                                                     |
|  |  A/B Boot Logic   |                                                     |
|  +--------+---------+                                                     |
|           |                                                                |
|  +--------+----------------------------------------------------------+    |
|  |  LINUX KERNEL (Verified FIT)                                       |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  |  |  dm-verity       |  |  AppArmor        |  |  fscrypt         |  |    |
|  |  |  rootfs integrity|  |  MAC enforcement  |  |  data encryption |  |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  |  |  nftables        |  |  systemd         |  |  journald        |  |    |
|  |  |  firewall        |  |  least-privilege  |  |  sealed logs     |  |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  +--------------------------------------------------------------------+    |
|                                                                            |
|  APPLICATION LAYER                                                         |
|  +--------------------------------------------------------------------+    |
|  |  Smart RTU Application                                              |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  |  |  Modbus RTU/TCP  |  |  MQTT (mTLS)     |  |  OTA (Signed)    |  |    |
|  |  |  via fieldbus    |  |  via PKCS#11     |  |  A/B verified    |  |    |
|  |  +------------------+  +------------------+  +------------------+  |    |
|  +--------------------------------------------------------------------+    |
|                                                                            |
+============================================================================+
```

## Appendix B: Development vs Production Build Matrix

| Component | Development | Production |
|-----------|------------|------------|
| TF-A `TRUSTED_BOARD_BOOT` | 0 | 1 |
| TF-A `LOG_LEVEL` | 40 | 0 |
| TF-A `DEBUG` | 1 | 0 |
| TF-A `STM32MP_ANTI_ROLLBACK` | 0 | 1 |
| OP-TEE `CFG_TEE_CORE_LOG_LEVEL` | 4 | 0 |
| OP-TEE `CFG_RPMB_FS` | n (use REE_FS) | y |
| U-Boot `CONFIG_BOOTDELAY` | 3 | 0 |
| U-Boot `CONFIG_HUSH_PARSER` | y | n |
| U-Boot `CONFIG_SILENT_CONSOLE` | n | y |
| U-Boot `CONFIG_FIT_SIGNATURE_ENFORCE` | n | y |
| Kernel `console=` | `ttySTM0,115200` | (empty) |
| Kernel `CONFIG_DEBUG_KERNEL` | y | n |
| Kernel `CONFIG_MODULE_SIG_FORCE` | n | y |
| Yocto `debug-tweaks` | enabled | removed |
| Yocto rootfs | ext4 (rw) | squashfs + dm-verity (ro) |
| OTP secure boot | not programmed | programmed |
| OTP JTAG | open | restricted/disabled |
| SSH | enabled, password OK | key-only or disabled |
| Firewall | permissive | default-deny |
| AppArmor | complain mode | enforce mode |

---

*Document version: 1.0*
*Last updated: 2026-09-08*
*Classification: CONFIDENTIAL — OEM Internal Use Only*
