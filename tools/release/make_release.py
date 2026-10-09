#!/usr/bin/env python3
"""Package a gateway release from the CI build artifacts (DOCS/CI_CD_GUIDE.md §5.8).

    python3 tools/release/make_release.py --check-only            # tag == APP_VERSION_DEF?
    python3 tools/release/make_release.py build release           # package (+ publish in CI)
    python3 tools/release/make_release.py build release --dry-run # package only

Produces in <out_dir>:
    stm32mp1-gateway_<ver>              the application binary (what app OTA downloads)
    gateway-<ver>.tar.gz                installable package: bin/gateway, lib/, gateway.service,
                                        install.sh, VERSION (DOCS/DEPLOYMENT.md)
    build_info.json, SHA256SUMS
    ota_command.json                    MQTT app-OTA payload {version, url, sha256}
    release_notes.md                    version, commit, checksums, how to deploy

In CI (CI_COMMIT_TAG set, not --dry-run) the files are uploaded to the
project's Generic Package Registry and a GitLab Release is created, both with
CI_JOB_TOKEN. Uses only the Python standard library.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SETTINGS_H = REPO / "inc" / "settings.h"
PACKAGE_NAME = "stm32mp1-gateway"

# No "https://" on purpose: if published unedited, the firmware's
# ota_parse_request() rejects it ("invalid_url") and never downloads anything.
OTA_URL_PLACEHOLDER = "REPLACE_WITH_HTTPS_URL_OF_{file}"

# What `file` must report for build/main (tools/ci/build_info.py records it).
REQUIRED_FILE_TYPE = ("ELF 32-bit LSB", "ARM, EABI5")


def app_version():
    """APP_VERSION_DEF from inc/settings.h."""
    m = re.search(r'#define\s+APP_VERSION_DEF\s+"([^"]+)"', SETTINGS_H.read_text())
    if not m:
        sys.exit("APP_VERSION_DEF not found in inc/settings.h")
    return m.group(1)


def check_tag(version):
    tag = os.environ.get("CI_COMMIT_TAG", "")
    if tag and tag != f"v{version}":
        sys.exit(
            f"Tag '{tag}' does not match APP_VERSION_DEF \"{version}\" in inc/settings.h.\n"
            f"The device reports this version in its OTA status messages, so they must agree:\n"
            f"bump APP_VERSION_DEF and tag v<version>, or delete this tag."
        )
    print(f"Version OK: APP_VERSION_DEF {version}" + (f", tag {tag}" if tag else " (no tag)"))


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def check_build(build, version):
    """The build artifacts are complete and belong to this version; returns build_info."""
    for rel in ("main", "build_info.json", f"gateway-{version}.tar.gz"):
        if not (build / rel).is_file():
            sys.exit(f"missing build artifact: {build / rel} (download the build-firmware artifacts)")
    with tarfile.open(build / f"gateway-{version}.tar.gz") as tar:
        names = set(n.lstrip("./") for n in tar.getnames())
    missing = {"bin/gateway", "install.sh", "gateway.service", "VERSION"} - names
    if missing or not any(n.startswith("lib/") and n != "lib/" for n in names):
        sys.exit(f"installable package gateway-{version}.tar.gz is incomplete (missing: "
                 f"{', '.join(sorted(missing)) or 'lib/*'})")

    info = json.loads((build / "build_info.json").read_text())
    if info.get("app_version") != version:
        sys.exit(f"Refusing to release: build_info.json says app_version {info.get('app_version')!r}, "
                 f"the source says {version!r} (artifacts of another build?)")
    ftype = info.get("binary_file_type", "")
    if not all(part in ftype for part in REQUIRED_FILE_TYPE):
        sys.exit(f"Refusing to release: build/main is not a 32-bit ARM EABI5 executable ({ftype!r})")
    if info.get("binary_sha256") != sha256(build / "main"):
        sys.exit("Refusing to release: build/main does not match binary_sha256 in build_info.json")
    return info


def package(build, out, version):
    build, out = Path(build), Path(out)
    info = check_build(build, version)
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    stem = f"{PACKAGE_NAME}_{version}"
    binary = out / stem
    shutil.copy2(build / "main", binary)
    binary.chmod(0o755)
    shutil.copy2(build / "build_info.json", out / "build_info.json")

    pkg = out / f"gateway-{version}.tar.gz"
    shutil.copy2(build / pkg.name, pkg)
    with tarfile.open(pkg) as tar:
        lib_files = sorted(Path(m.name).name for m in tar.getmembers()
                           if m.isfile() and m.name.lstrip("./").startswith("lib/"))

    names = sorted([binary.name, pkg.name, "build_info.json"])
    sums = "".join(f"{sha256(out / n)}  {n}\n" for n in names)
    (out / "SHA256SUMS").write_text(sums)

    bin_sha = sha256(binary)
    ota = {
        "url": os.environ.get("OTA_URL") or OTA_URL_PLACEHOLDER.format(file=binary.name),
        "version": version,
        "sha256": bin_sha,
    }
    assert re.fullmatch(r"[0-9a-f]{64}", ota["sha256"])   # what ota_parse_request() requires
    (out / "ota_command.json").write_text(json.dumps(ota, indent=2) + "\n")

    lib_list = "\n".join(f"- `{n}`" for n in lib_files)
    notes = f"""## STM32MP1 gateway {version}

| | |
|---|---|
| Version | `{version}` (`APP_VERSION_DEF`; reported as `previous_version` in OTA status messages) |
| Commit | `{info.get("git_sha", "")}` |
| Pipeline | {info.get("pipeline_id", "")} |
| Compiler | {info.get("compiler", "")} |
| Binary | {binary.stat().st_size:,} bytes, {info.get("binary_file_type", "").split(",")[0]} |
| Binary SHA256 | `{bin_sha}` |

### Update over the air (app OTA)
1. Upload `{binary.name}` to HTTPS storage (e.g. S3) and create a (pre-signed) **https** URL.
2. Put the URL into `ota_command.json` and publish it to `devices/<device_id>/ota/app`
   (the device must have `ota.enable = 1`). `version` and `sha256` are already correct:
   the device refuses a command without a valid sha256 or a non-https URL.
3. The device reports progress on `devices/<device_id>/ota/status` and restarts with the new binary.

### Install or update over SSH (new board, or a board on the legacy layout)
```
scp {pkg.name} root@<board>:/tmp/
ssh root@<board> 'mkdir -p /tmp/gw && tar -C /tmp/gw -xzf /tmp/{pkg.name} && sh /tmp/gw/install.sh'
```
Installs `/opt/gateway`, `/etc/gateway`, `/var/lib/gateway` and `gateway.service`; a legacy
install in `/home/root/edb_c/linking` is migrated (config, certificates, buffered data).
`sh install.sh --rollback` returns to the previous binary. See `DOCS/DEPLOYMENT.md`.

### Bundled runtime libraries (`/opt/gateway/lib`)
{lib_list}

### Checksums
```
{sums}```
"""
    (out / "release_notes.md").write_text(notes)
    print(f"Packaged {len(list(out.iterdir()))} files in {out}")
    return sorted(p for p in out.iterdir() if p.is_file())


def api(method, url, data=None, content_type=None):
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("JOB-TOKEN", os.environ["CI_JOB_TOKEN"])
    if content_type:
        req.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        sys.exit(f"{method} {url} -> HTTP {e.code}: {e.read().decode(errors='replace')[:500]}")


def publish(files, version):
    base = f"{os.environ['CI_API_V4_URL']}/projects/{os.environ['CI_PROJECT_ID']}"
    links = []
    for f in files:
        url = f"{base}/packages/generic/{PACKAGE_NAME}/{version}/{urllib.parse.quote(f.name)}"
        api("PUT", url, f.read_bytes(), "application/octet-stream")
        print(f"uploaded {f.name}")
        is_package = f.name.startswith(PACKAGE_NAME)
        links.append({"name": f.name, "url": url, "link_type": "package" if is_package else "other"})

    tag = os.environ["CI_COMMIT_TAG"]
    body = {
        "name": f"STM32MP1 gateway {version}",
        "tag_name": tag,
        "description": (files[0].parent / "release_notes.md").read_text(),
        "assets": {"links": links},
    }
    status, _ = api("POST", f"{base}/releases", json.dumps(body).encode(), "application/json")
    print(f"Release {tag} created (HTTP {status}): {os.environ.get('CI_PROJECT_URL', '')}/-/releases/{tag}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build_dir", nargs="?", default="build")
    ap.add_argument("out_dir", nargs="?", default="release")
    ap.add_argument("--check-only", action="store_true", help="only check tag vs APP_VERSION_DEF")
    ap.add_argument("--dry-run", action="store_true", help="package without uploading")
    args = ap.parse_args(argv)

    version = app_version()
    check_tag(version)
    if args.check_only:
        return
    files = package(args.build_dir, args.out_dir, version)
    if args.dry_run or not os.environ.get("CI_COMMIT_TAG"):
        print("Not publishing (dry run or no tag).")
        return
    publish(files, version)


if __name__ == "__main__":
    main()
