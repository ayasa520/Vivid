#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/index.html" "${VIVID_INSTALL_WEBUI_DIR}/index.html"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/styles.css" "${VIVID_INSTALL_WEBUI_DIR}/styles.css"
install -Dm644 "${VIVID_WEBUI_SOURCE_DIR}/main.js" "${VIVID_INSTALL_WEBUI_DIR}/main.js"
install -Dm755 "${VIVID_WEBUI_SOURCE_DIR}/vivid_webui_server.py" "${VIVID_INSTALL_WEBUI_DIR}/vivid_webui_server.py"
