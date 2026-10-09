"""
core — Smart RTU Tool core logic.

Shared constants and re-exports used across transport modules and the UI.

Board layout (installed by deploy/install.sh, see DOCS/DEPLOYMENT.md):
    /etc/gateway/smart_rtu_config.json (+ .csv)   configuration
    /etc/gateway/certs/                            MQTT CA, device certificate, key
    gateway.service                                systemd service running the app
"""

REMOTE_CONFIG_DIR = "/etc/gateway"
REMOTE_CONFIG_CSV_PATH = "/etc/gateway/smart_rtu_config.csv"
REMOTE_CONFIG_JSON_PATH = "/etc/gateway/smart_rtu_config.json"
REMOTE_SERVICE = "gateway"

REMOTE_CERTS_DIR = "/etc/gateway/certs"
REMOTE_CA_CERT_PATH = "/etc/gateway/certs/ca.crt"
REMOTE_DEVICE_CERT_PATH = "/etc/gateway/certs/client.crt"
REMOTE_PRIVATE_KEY_PATH = "/etc/gateway/certs/private.key"
