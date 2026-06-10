#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

BUILD_DIR="${VIVID_DIRECT_RUN_BUILD_DIR}"
DAEMON_BUILD_DIR="${VIVID_DIRECT_RUN_DAEMON_BUILD_DIR}"
SCENE_BUILD_DIR="${VIVID_DIRECT_RUN_SCENE_BUILD_DIR}"
VIDEO_BUILD_DIR="${VIVID_DIRECT_RUN_VIDEO_BUILD_DIR}"
WEB_BUILD_DIR="${VIVID_DIRECT_RUN_WEB_BUILD_DIR}"
CEF_DIR="${VIVID_DIRECT_RUN_CEF_DIR}"
CEF_ARCHIVE="${VIVID_CEF_ARCHIVE}"
PRODUCER_BIN="${VIVID_DIRECT_RUN_PRODUCER_BIN}"
GBM_SHIM_BUILD_DIR="${VIVID_DIRECT_RUN_GBM_SHIM_BUILD_DIR}"
GBM_SHIM_LIBRARY="${VIVID_DIRECT_RUN_GBM_USAGE_SHIM}"
JOBS="${VIVID_BUILD_JOBS:-${JOBS:-$(nproc)}}"
CMAKE_BIN="${CMAKE:-cmake}"
CC_BIN="${CC:-cc}"
PKG_CONFIG_BIN="${PKG_CONFIG:-pkg-config}"

mkdir -p "${BUILD_DIR}"

echo "==> Building scene renderer module for direct-run"
"${CMAKE_BIN}" -S "${VIVID_SCENE_SOURCE_DIR}" \
  -B "${SCENE_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}"
"${CMAKE_BIN}" --build "${SCENE_BUILD_DIR}" --target "${VIVID_SCENE_TARGET}" --parallel "${JOBS}" --verbose

echo "==> Building video renderer module for direct-run"
"${CMAKE_BIN}" -S "${VIVID_VIDEO_SOURCE_DIR}" \
  -B "${VIDEO_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}"
"${CMAKE_BIN}" --build "${VIDEO_BUILD_DIR}" --target "${VIVID_VIDEO_TARGET}" --parallel "${JOBS}" --verbose

echo "==> Extracting the CEF binary distribution for the web backend"
if [[ ! -f "${CEF_ARCHIVE}" ]]; then
  echo "CEF archive not found: ${CEF_ARCHIVE}" >&2
  echo "run tools/vivid.sh flatpak prefetch first" >&2
  exit 1
fi
CEF_STAMP="${CEF_DIR}/.extracted-stamp"
CEF_ARCHIVE_ID="$(stat -c '%s-%Y' "${CEF_ARCHIVE}")"
if [[ ! -f "${CEF_STAMP}" || "$(cat "${CEF_STAMP}" 2>/dev/null)" != "${CEF_ARCHIVE_ID}" ]]; then
  rm -rf "${CEF_DIR}"
  mkdir -p "${CEF_DIR}"
  tar xjf "${CEF_ARCHIVE}" -C "${CEF_DIR}" --strip-components=1
  printf '%s' "${CEF_ARCHIVE_ID}" > "${CEF_STAMP}"
else
  echo "    CEF already extracted at ${CEF_DIR}"
fi

echo "==> Building web renderer module for direct-run"
"${CMAKE_BIN}" -S "${VIVID_WEB_SOURCE_DIR}" \
  -B "${WEB_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}" \
  -DCEF_ROOT="${CEF_DIR}"
"${CMAKE_BIN}" --build "${WEB_BUILD_DIR}" --target "${VIVID_WEB_TARGET}" --target "${VIVID_WEB_HELPER_TARGET}" --parallel "${JOBS}" --verbose

echo "==> Building GBM usage shim for NVIDIA shared textures"
mkdir -p "${GBM_SHIM_BUILD_DIR}"
"${CC_BIN}" -O2 -g -fPIC -shared -o "${GBM_SHIM_LIBRARY}" \
  "${VIVID_GBM_USAGE_SHIM_SOURCE}" \
  $("${PKG_CONFIG_BIN}" --cflags --libs gbm) \
  -ldl

echo "==> Building ${PRODUCER_BIN}"
"${CMAKE_BIN}" -S "${VIVID_PRODUCER_DAEMON_DIR}" \
  -B "${DAEMON_BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}"
"${CMAKE_BIN}" --build "${DAEMON_BUILD_DIR}" --target vivid-producer --parallel "${JOBS}" --verbose
"${CMAKE_BIN}" -E copy_if_different "${DAEMON_BUILD_DIR}/vivid-producer" "${PRODUCER_BIN}"

echo "==> Direct-run build complete"
echo "    producer: ${PRODUCER_BIN}"
echo "    scene:    ${VIVID_DIRECT_RUN_SCENE_LIBRARY}"
echo "    video:    ${VIVID_DIRECT_RUN_VIDEO_LIBRARY}"
echo "    web:      ${VIVID_DIRECT_RUN_WEB_LIBRARY}"
echo "    cef:      ${VIVID_DIRECT_RUN_WEB_OUT_DIR}"
echo "    gbm-shim: ${GBM_SHIM_LIBRARY}"
