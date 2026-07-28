#!/usr/bin/env bash
# Single entry point for vivid_display_v1.toml codegen and drift checks.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PROTO_DIR="${REPO_ROOT}/producer/src/protocol"
PROTO_GEN="${PROTO_DIR}/protocol_gen.py"
PROTO_SPEC="${PROTO_DIR}/vivid_display_v1.toml"

exec python3 "${PROTO_GEN}" \
  --spec "${PROTO_SPEC}" \
  --c-ids-out "${PROTO_DIR}/vivid_display_protocol_ids.h" \
  --gnome-ids-out "${REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol_ids.h" \
  --gnome-out "${REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-constants.js" \
  --kde-out "${REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_constants.hpp" \
  --kde-cpp-out "${REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_cpp.hpp" \
  --webui-out "${REPO_ROOT}/producer/src/webui/vivid_protocol_constants.py" \
  --gnome-codec-out "${REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-codec.js" \
  --gnome-meta-out "${REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-meta.js" \
  --kde-meta-out "${REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_meta.hpp" \
  --webui-codec-out "${REPO_ROOT}/producer/src/webui/vivid_protocol_codec.py" \
  --webui-meta-out "${REPO_ROOT}/producer/src/webui/vivid_protocol_meta.py" \
  --c-json-out "${PROTO_DIR}/vivid_display_protocol_json.h" \
  --gnome-json-h-out "${REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol_json.h" \
  --gnome-json-out "${REPO_ROOT}/consumer/gnome/extension/shell/helper/protocol-json-fields.js" \
  --kde-json-out "${REPO_ROOT}/consumer/kde/src/qml_module/vivid_protocol_json_fields.hpp" \
  --webui-json-out "${REPO_ROOT}/producer/src/webui/vivid_protocol_json_fields.py" \
  --config-schema-out "${REPO_ROOT}/producer/src/daemon/vivid_producer_config_schema.h" \
  --webui-config-keys-out "${REPO_ROOT}/producer/src/webui/vivid_protocol_config_keys.py" \
  --docs-reference-out "${REPO_ROOT}/docs/protocols-reference.md" \
  --umbrella-canonical "${PROTO_DIR}/vivid_display_protocol.h" \
  --umbrella-gnome "${REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_protocol.h" \
  --codec-canonical "${PROTO_DIR}/vivid_display_codec.c" \
  --codec-gnome "${REPO_ROOT}/consumer/gnome/src/protocol/vivid_display_codec.c" \
  "$@"
