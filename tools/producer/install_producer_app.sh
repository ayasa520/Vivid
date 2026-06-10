#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

CMAKE_BIN="${CMAKE:-cmake}"
BUILD_JOBS="${VIVID_PRODUCER_BUILD_JOBS:-${FLATPAK_BUILDER_N_JOBS:-2}}"
DAEMON_BUILD_DIR="${VIVID_FLATPAK_DAEMON_BUILD_DIR}"

"${CMAKE_BIN}" -S "${VIVID_PRODUCER_DAEMON_DIR}" \
    -B "${DAEMON_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${VIVID_INSTALL_PREFIX}"
"${CMAKE_BIN}" --build "${DAEMON_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}" \
    --verbose \
    --target vivid-producer

"${CMAKE_BIN}" --install "${DAEMON_BUILD_DIR}" --component producer_daemon
install -Dm755 "${ROOT_DIR}/packaging/flatpak/vivid-launcher" "${VIVID_INSTALL_BIN_DIR}/${VIVID_PRODUCER_LAUNCHER_NAME}"
install -Dm644 "${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.desktop" "${VIVID_INSTALL_APPLICATIONS_DIR}/${VIVID_PRODUCER_APP_ID}.desktop"
install -Dm644 "${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.metainfo.xml" "${VIVID_INSTALL_METAINFO_DIR}/${VIVID_PRODUCER_APP_ID}.metainfo.xml"
install -Dm644 "${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.svg" "${VIVID_INSTALL_ICON_DIR}/${VIVID_PRODUCER_APP_ID}.svg"
