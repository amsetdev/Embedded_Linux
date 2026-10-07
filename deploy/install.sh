#!/bin/sh
# install.sh — install, update or roll back the gateway on the board (DOCS/DEPLOYMENT.md).
#
# Run as root from the unpacked package (gateway-<version>.tar.gz):
#   sh install.sh                 install / update, migrate a legacy install, start
#   sh install.sh --no-migrate    don't take over config, certificates, buffered data
#                                 from the legacy directory (/home/root/edb_c/linking)
#   sh install.sh --no-start      install but leave the service stopped
#   sh install.sh --rollback      go back to the previously installed binary
#
# Layout:
#   /opt/gateway/bin/gateway      binary (gateway.prev = previous version, for --rollback)
#   /opt/gateway/lib/             bundled runtime libraries (found via the binary's RPATH)
#   /etc/gateway/smart_rtu_config.json, /etc/gateway/certs/   configuration, MQTT certificates
#   /var/lib/gateway/             storage/ (offline buffer), modbus_tcp.db, ota/
#   /etc/systemd/system/gateway.service
set -eu

PREFIX=/opt/gateway
ETC=/etc/gateway
DATA=/var/lib/gateway
UNIT=/etc/systemd/system/gateway.service
LEGACY=/home/root/edb_c/linking
PKG=$(cd "$(dirname "$0")" && pwd)

MIGRATE=1
START=1
ROLLBACK=0
for arg in "$@"; do
    case "$arg" in
        --no-migrate) MIGRATE=0 ;;
        --no-start)   START=0 ;;
        --rollback)   ROLLBACK=1 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $arg" >&2; exit 2 ;;
    esac
done

die() { echo "install.sh: $*" >&2; exit 1; }
log() { echo "[install] $*"; }

[ "$(id -u)" = 0 ] || die "run as root"

start_and_check() {
    want=$1
    [ "$START" = 1 ] || { log "not started (--no-start)"; return 0; }
    systemctl restart gateway
    sleep 3
    if ! systemctl is-active --quiet gateway; then
        journalctl -u gateway -n 40 --no-pager >&2 || true
        die "gateway.service is not running"
    fi
    got=$("$PREFIX/bin/gateway" --version)
    [ "$got" = "$want" ] || die "installed binary reports version '$got', expected '$want'"
    log "gateway $got running"
}

stop_all() {
    systemctl stop gateway 2>/dev/null || true
    # A legacy instance started by hand (nohup ./main) would poll the same RS485 bus.
    for pid in $(pidof main 2>/dev/null || true); do
        if [ "$(readlink "/proc/$pid/cwd" 2>/dev/null)" = "$LEGACY" ]; then
            log "stopping legacy instance (pid $pid) in $LEGACY"
            kill "$pid" 2>/dev/null || true
        fi
    done
    sleep 1
}

if [ "$ROLLBACK" = 1 ]; then
    [ -f "$PREFIX/bin/gateway.prev" ] || die "no previous binary ($PREFIX/bin/gateway.prev)"
    stop_all
    mv -f "$PREFIX/bin/gateway.prev" "$PREFIX/bin/gateway"
    v=$("$PREFIX/bin/gateway" --version)
    echo "$v" > "$PREFIX/VERSION"
    log "rolled back to $v"
    start_and_check "$v"
    exit 0
fi

[ -x "$PKG/bin/gateway" ] || die "no bin/gateway next to install.sh"
[ -f "$PKG/gateway.service" ] || die "no gateway.service next to install.sh"
VERSION=$(cat "$PKG/VERSION")
log "installing gateway $VERSION"

stop_all

install -d -m 0755 "$PREFIX/bin" "$DATA" "$DATA/storage" "$ETC"
install -d -m 0700 "$ETC/certs"

# Binary: keep the previous one for --rollback; replace atomically.
[ -f "$PREFIX/bin/gateway" ] && cp -p "$PREFIX/bin/gateway" "$PREFIX/bin/gateway.prev"
cp "$PKG/bin/gateway" "$PREFIX/bin/gateway.new"
chmod 0755 "$PREFIX/bin/gateway.new"
mv -f "$PREFIX/bin/gateway.new" "$PREFIX/bin/gateway"

# Bundled libraries: replace the whole directory.
if [ -d "$PKG/lib" ]; then
    rm -rf "$PREFIX/lib.new" "$PREFIX/lib.old"
    cp -a "$PKG/lib" "$PREFIX/lib.new"
    [ -d "$PREFIX/lib" ] && mv "$PREFIX/lib" "$PREFIX/lib.old"
    mv "$PREFIX/lib.new" "$PREFIX/lib"
    rm -rf "$PREFIX/lib.old"
fi
echo "$VERSION" > "$PREFIX/VERSION"

# First install on a board that ran the legacy layout: take over its config,
# certificates (paths rewritten to /etc/gateway/certs) and unsent telemetry.
if [ "$MIGRATE" = 1 ] && [ ! -f "$ETC/smart_rtu_config.json" ] && [ -f "$LEGACY/smart_rtu_config.json" ]; then
    log "migrating configuration from $LEGACY"
    for f in ca.crt client.crt private.key; do
        [ -f "$LEGACY/$f" ] && cp "$LEGACY/$f" "$ETC/certs/$f"
    done
    chmod 0600 "$ETC"/certs/* 2>/dev/null || true
    sed -e "s#$LEGACY/ca.crt#$ETC/certs/ca.crt#g" \
        -e "s#$LEGACY/client.crt#$ETC/certs/client.crt#g" \
        -e "s#$LEGACY/private.key#$ETC/certs/private.key#g" \
        "$LEGACY/smart_rtu_config.json" > "$ETC/smart_rtu_config.json"
    [ -f "$LEGACY/smart_rtu_config.csv" ] && cp "$LEGACY/smart_rtu_config.csv" "$ETC/"
    if [ -d "$LEGACY/storage" ]; then
        n=$(find "$LEGACY/storage" -maxdepth 1 -name '*.txt' | wc -l)
        find "$LEGACY/storage" -maxdepth 1 -name '*.txt' -exec mv {} "$DATA/storage/" \;
        log "moved $n buffered payload(s) to $DATA/storage"
    fi
fi
chmod 0600 "$ETC/smart_rtu_config.json" 2>/dev/null || true   # holds the Wi-Fi password

install -m 0644 "$PKG/gateway.service" "$UNIT"
systemctl daemon-reload
systemctl enable gateway >/dev/null 2>&1

start_and_check "$VERSION"
