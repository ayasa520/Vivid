#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

CMAKE_BIN="${CMAKE:-cmake}"
CC_BIN="${CC:-cc}"
PKG_CONFIG_BIN="${PKG_CONFIG:-pkg-config}"
BUILD_JOBS="${VIVID_RENDERER_BUILD_JOBS:-${FLATPAK_BUILDER_N_JOBS:-2}}"

absolute_path() {
    path="$1"
    case "${path}" in
        /*)
            printf '%s\n' "${path}"
            ;;
        *)
            printf '%s/%s\n' "$(CDPATH= cd -- "$(dirname -- "${path}")" && pwd)" "$(basename -- "${path}")"
            ;;
    esac
}

report_cache_state() {
    label="$1"
    path="$2"

    if [ -f "${path}/CMakeCache.txt" ]; then
        echo "    ${label}: reusing CMake cache at ${path}/CMakeCache.txt"
    else
        echo "    ${label}: no CMake cache at ${path}/CMakeCache.txt"
    fi
}

mkdir -p \
    "${VIVID_INSTALL_LIB_DIR}" \
    "${VIVID_INSTALL_WEB_CEF_DIR}" \
    "${VIVID_FLATPAK_RENDERER_BUILD_ROOT}"

# Flatpak extracts the CEF archive through the manifest before this command
# runs. The renderer CMake trees may be mounted from a stable host-side .build
# cache by the Flatpak manifest; keeping those paths stable lets an interrupted
# or failed module rebuild reuse CMake object files instead of relying on
# flatpak-builder's numbered vivid-producer-N work directories.
echo "==> Renderer build root: ${VIVID_FLATPAK_RENDERER_BUILD_ROOT}"
echo "==> Renderer CEF root: ${VIVID_FLATPAK_CEF_ROOT}"
report_cache_state "scene" "${VIVID_FLATPAK_SCENE_BUILD_DIR}"
report_cache_state "video" "${VIVID_FLATPAK_VIDEO_BUILD_DIR}"
report_cache_state "web" "${VIVID_FLATPAK_WEB_BUILD_DIR}"

echo "==> Building ${VIVID_SCENE_TARGET} in ${VIVID_FLATPAK_SCENE_BUILD_DIR}"
"${CMAKE_BIN}" -S "${VIVID_SCENE_SOURCE_DIR}" \
    -B "${VIVID_FLATPAK_SCENE_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}"
"${CMAKE_BIN}" --build "${VIVID_FLATPAK_SCENE_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}" \
    --verbose \
    --target "${VIVID_SCENE_TARGET}"
install -Dm755 "${VIVID_FLATPAK_SCENE_LIBRARY}" \
    "${VIVID_INSTALL_LIB_DIR}/${VIVID_SCENE_LIBRARY_NAME}"
install -Dm755 "${VIVID_FLATPAK_DXCOMPILER_LIBRARY}" \
    "${VIVID_INSTALL_LIB_DIR}/${VIVID_DXCOMPILER_LIBRARY_NAME}"

echo "==> Building ${VIVID_VIDEO_TARGET} in ${VIVID_FLATPAK_VIDEO_BUILD_DIR}"
"${CMAKE_BIN}" -S "${VIVID_VIDEO_SOURCE_DIR}" \
    -B "${VIVID_FLATPAK_VIDEO_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}"
"${CMAKE_BIN}" --build "${VIVID_FLATPAK_VIDEO_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}" \
    --verbose \
    --target "${VIVID_VIDEO_TARGET}"
install -Dm755 "${VIVID_FLATPAK_VIDEO_LIBRARY}" \
    "${VIVID_INSTALL_LIB_DIR}/${VIVID_VIDEO_LIBRARY_NAME}"

if [ ! -d "${VIVID_FLATPAK_CEF_ROOT}/cmake" ]; then
    echo "CEF root is not an extracted CEF binary distribution: ${VIVID_FLATPAK_CEF_ROOT}" >&2
    echo "Check the CEF archive source in producer/packaging/flatpak/io.github.ayasa520.Vivid.yml." >&2
    exit 1
fi
cef_root="$(absolute_path "${VIVID_FLATPAK_CEF_ROOT}")"

echo "==> Building ${VIVID_WEB_TARGET} and ${VIVID_WEB_HELPER_TARGET} in ${VIVID_FLATPAK_WEB_BUILD_DIR}"
"${CMAKE_BIN}" -S "${VIVID_WEB_SOURCE_DIR}" \
    -B "${VIVID_FLATPAK_WEB_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}" \
    -DCEF_ROOT="${cef_root}"
"${CMAKE_BIN}" --build "${VIVID_FLATPAK_WEB_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}" \
    --verbose \
    --target "${VIVID_WEB_TARGET}" \
    --target "${VIVID_WEB_HELPER_TARGET}"

# libVividWeb.so uses $ORIGIN to find libcef.so, and CEF itself needs its
# resource files plus locales beside the helper executable. Copying the CMake
# out/ directory as one runtime bundle keeps that contract intact in /app.
cp -a "${VIVID_FLATPAK_WEB_OUT_DIR}/." "${VIVID_INSTALL_WEB_CEF_DIR}/"

echo "==> Building Vivid GBM usage shim for NVIDIA shared textures"
mkdir -p "${VIVID_FLATPAK_GBM_SHIM_BUILD_DIR}"
"${CC_BIN}" -O2 -g -fPIC -shared -o "${VIVID_FLATPAK_GBM_USAGE_SHIM}" \
    "${VIVID_GBM_USAGE_SHIM_SOURCE}" \
    $("${PKG_CONFIG_BIN}" --cflags --libs gbm) \
    -ldl
install -Dm755 "${VIVID_FLATPAK_GBM_USAGE_SHIM}" \
    "${VIVID_INSTALL_GBM_USAGE_SHIM}"
