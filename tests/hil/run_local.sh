#!/usr/bin/env bash
# Run the HIL suite on this PC exactly like the CI job (same image, same tests).
#
#   tests/hil/run_local.sh [build_dir]        default build_dir: build
#                                             (needs build_info.json + gateway-<ver>.tar.gz:
#                                              make package && python3 tools/ci/build_info.py build)
#
# Credentials, OUTSIDE the repo: $HIL_SECRETS_DIR (default ~/.config/embedded_linux/hil)
#   hil.env        HIL_BOARD_HOST=192.168.1.26        (KEY=value lines, no quotes)
#                  optional AWS: HIL_AWS_ENDPOINT=..., HIL_DEVICE_ID=...
#   id_ed25519     SSH key for root@board (or HIL_SSH_PASSWORD=... in hil.env)
#   optional AWS files: aws-ca.pem device.pem.crt device.pem.key observer.pem.crt observer.pem.key
# Extra pytest arguments: HIL_PYTEST_ARGS="-k modbus"
set -euo pipefail

REPO=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=$(cd "${1:-$REPO/build}" && pwd)
SECRETS=${HIL_SECRETS_DIR:-$HOME/.config/embedded_linux/hil}
RS485_BY_ID=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0   # CH340 wired to the DK2's RS485
IMAGE=python:3.11-slim

[[ -f "$BUILD/build_info.json" ]] || { echo "No build_info.json in $BUILD (see the header of this script)." >&2; exit 2; }
ls "$BUILD"/gateway-*.tar.gz >/dev/null 2>&1 || { echo "No gateway-<ver>.tar.gz in $BUILD (make package)." >&2; exit 2; }
[[ -f "$SECRETS/hil.env" ]] || { echo "No $SECRETS/hil.env (needs HIL_BOARD_HOST)." >&2; exit 2; }
if docker ps --format '{{.Names}}' | grep -q '^runner-'; then
    echo "A CI job is running on this PC (it may be using the board) - try again when it is done." >&2
    exit 2
fi

args=(--env-file "$SECRETS/hil.env" -v "$SECRETS:/hil-secrets:ro")
[[ -f "$SECRETS/id_ed25519" ]] && args+=(-e HIL_SSH_KEY=/hil-secrets/id_ed25519)
if [[ -e "$RS485_BY_ID" ]]; then
    args+=(--device="$RS485_BY_ID:/dev/ttyUSB0" -e HIL_RTU_PORT=/dev/ttyUSB0)
else
    echo "No RS485 adapter ($RS485_BY_ID) - Modbus RTU tests will be skipped."
fi
for pair in HIL_AWS_CA:aws-ca.pem HIL_DEVICE_CERT:device.pem.crt HIL_DEVICE_KEY:device.pem.key \
            HIL_OBSERVER_CERT:observer.pem.crt HIL_OBSERVER_KEY:observer.pem.key; do
    [[ -f "$SECRETS/${pair#*:}" ]] && args+=(-e "${pair%%:*}=/hil-secrets/${pair#*:}")
done

docker run --rm --network host -v "$REPO":/src -v "$BUILD":/fwbuild:ro -w /src \
    -e HIL_BUILD_DIR=/fwbuild -e PYTHONDONTWRITEBYTECODE=1 "${args[@]}" "$IMAGE" bash -c "
        pip install -q --root-user-action=ignore --disable-pip-version-check \
            -r tests/requirements.txt -r tests/hil/requirements.txt
        python -m pytest -c tests/pytest.ini tests/hil -m hardware -v -rs --tb=short -p no:cacheprovider \
            --junitxml=reports/hil.xml ${HIL_PYTEST_ARGS:-}
        status=\$?
        chown -R $(id -u):$(id -g) reports
        exit \$status"
