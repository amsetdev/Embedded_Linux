#!/usr/bin/env bash
# One-time HIL rig setup on this PC (DOCS/HIL_SETUP.md §2-3). Run as root:
#
#   sudo tests/hil/setup_runner.sh <glrt-runner-token> [gitlab-personal-access-token]
#
#   <glrt-runner-token>  GitLab → root/Embedded_Linux → Settings → CI/CD → Runners →
#                        New project runner (tags: stm32-hil, untagged: off) → copy token
#   [personal token]     optional, scope "api": also creates the CI variables
#                        HIL_BOARD_HOST and HIL_SSH_KEY (else add them in the web UI)
#
# What it does (each step only if not done yet):
#   1. backs up /etc/gitlab-runner/config.toml
#   2. registers runner "stm32-hil": Docker executor, network_mode host, the CH340
#      RS485 adapter as /dev/ttyUSB0, pull_policy if-not-present
#   3. removes the CH340 from the esp32-tester runner's devices (it is wired to the DK2)
#   4. opens TCP 5020 for the board in ufw (only if ufw is active)
#   5. restarts and verifies the runners
#   6. (with a personal token) sets the CI variables
set -euo pipefail

RUNNER_TOKEN=${1:-}
API_TOKEN=${2:-}
GITLAB=http://192.168.1.2
PROJECT_ID=15
BOARD=192.168.1.26
CH340=/dev/serial/by-id/usb-1a86_USB_Serial-if00-port0
CONFIG=/etc/gitlab-runner/config.toml
USER_HOME=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
SSH_KEY=$USER_HOME/.config/embedded_linux/hil/id_ed25519

[[ $(id -u) == 0 ]] || { echo "run with sudo" >&2; exit 1; }
[[ -n $RUNNER_TOKEN ]] || { sed -n '2,12p' "$0"; exit 2; }
[[ -e $CH340 ]] || echo "warning: $CH340 is not plugged in (the runner will fail to start jobs until it is)"

# 1. backup
cp -p "$CONFIG" "$CONFIG.bak-$(date +%Y%m%d-%H%M%S)"
echo "[1] backup: $CONFIG.bak-*"

# 2. register stm32-hil
if grep -q 'name = "stm32-hil"' "$CONFIG"; then
    echo "[2] runner stm32-hil already registered"
else
    gitlab-runner register --non-interactive \
        --url "$GITLAB" --token "$RUNNER_TOKEN" \
        --executor docker --docker-image python:3.11-slim \
        --description stm32-hil --name stm32-hil \
        --docker-network-mode host \
        --docker-devices "$CH340:/dev/ttyUSB0" \
        --docker-pull-policy if-not-present
    echo "[2] runner stm32-hil registered"
fi

# 3. CH340 out of esp32-tester
python3 - "$CONFIG" "$CH340" <<'EOF'
import re, sys
path, dev = sys.argv[1], sys.argv[2]
text = open(path).read()
blocks = re.split(r"(?m)^(?=\[\[runners\]\])", text)
changed = False
for i, b in enumerate(blocks):
    if 'name = "esp32-tester"' not in b:
        continue
    m = re.search(r"(?ms)^(\s*devices\s*=\s*\[)(.*?)(\])", b)
    if not m or dev not in m.group(2):
        continue
    entries = [e.strip() for e in re.findall(r'"[^"]*"', m.group(2)) if dev not in e]
    b = b[:m.start()] + m.group(1) + ", ".join(entries) + m.group(3) + b[m.end():]
    blocks[i] = b
    changed = True
if changed:
    open(path, "w").write("".join(blocks))
print("[3] CH340 removed from esp32-tester" if changed else "[3] esp32-tester does not map the CH340 (nothing to do)")
EOF

# 4. firewall
if command -v ufw >/dev/null && ufw status | grep -q "Status: active"; then
    ufw allow from "$BOARD" to any port 5020 proto tcp >/dev/null
    echo "[4] ufw: TCP 5020 allowed from $BOARD"
else
    echo "[4] ufw not active - if the board still can't reach port 5020, check iptables/nftables"
fi

# 5. restart + verify
gitlab-runner restart
sleep 3
gitlab-runner verify 2>&1 | tail -5
echo "[5] stm32-hil block:"
awk '/name = "stm32-hil"/{f=1} f&&/^\[\[runners\]\]/&&!/stm32-hil/{if(seen)exit} f{print; seen=1}' "$CONFIG" \
    | grep -E 'name|network_mode|devices|pull_policy|image' || true

# 6. CI variables
if [[ -n $API_TOKEN ]]; then
    [[ -f $SSH_KEY ]] || { echo "[6] $SSH_KEY missing (DOCS/HIL_SETUP.md §1)" >&2; exit 1; }
    setvar() {   # key type value
        local url="$GITLAB/api/v4/projects/$PROJECT_ID/variables"
        local code
        code=$(curl -s -o /dev/null -w '%{http_code}' -H "PRIVATE-TOKEN: $API_TOKEN" \
            "$url/$1")
        local method=POST target=$url
        [[ $code == 200 ]] && { method=PUT; target=$url/$1; }
        curl -sf -X "$method" -H "PRIVATE-TOKEN: $API_TOKEN" "$target" \
            --form "key=$1" --form "variable_type=$2" --form "value=$3" \
            --form "protected=false" --form "masked=false" >/dev/null
        echo "[6] CI variable $1 ($2) set"
    }
    setvar HIL_BOARD_HOST env_var "$BOARD"
    setvar HIL_SSH_KEY file "$(cat "$SSH_KEY")"
else
    echo "[6] no personal token: add HIL_BOARD_HOST and HIL_SSH_KEY in the web UI (DOCS/HIL_SETUP.md §2)"
fi
echo "done - run the manual hil-tests job of a ci/hil pipeline"
