#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/index.html" "${VIVID_INSTALL_WEBUI_DIR}/index.html"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/styles.css" "${VIVID_INSTALL_WEBUI_DIR}/styles.css"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/main.js" "${VIVID_INSTALL_WEBUI_DIR}/main.js"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/vivid_protocol_codec.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_protocol_codec.py"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/vivid_protocol_config_keys.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_protocol_config_keys.py"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/vivid_protocol_constants.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_protocol_constants.py"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/vivid_protocol_json_fields.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_protocol_json_fields.py"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/vivid_protocol_meta.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_protocol_meta.py"
install -Dm755 "${VIVID_WEBUI_SOURCE_DIR}/vivid_webui_server.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_webui_server.py"
