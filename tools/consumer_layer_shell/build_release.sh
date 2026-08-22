#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../consumer/layer-shell" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

BUILD_JOBS="${VIVID_BUILD_JOBS:-$(nproc)}"

WAYLAND_PROTOCOLS_VERSION=1.32
WAYLAND_PROTOCOLS_SHA256=7459799d340c8296b695ef857c07ddef24c5a09b09ab6a74f7b92640d2b1ba11
WAYLAND_PROTOCOLS_ARCHIVE="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}.tar.xz"
WAYLAND_PROTOCOLS_SOURCE="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}"
WAYLAND_PROTOCOLS_BUILD="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/wayland-protocols-build"

JSON_C_VERSION=0.19
JSON_C_SHA256=37ad0249902e301bd9052bf712e511fcc6acff4ecaad4b5900aad9ce564e26de
JSON_C_ARCHIVE="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/json-c-${JSON_C_VERSION}.tar.gz"
JSON_C_SOURCE="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/json-c-${JSON_C_VERSION}"
JSON_C_BUILD="${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}/json-c-build"

download_verified() {
    url="$1"
    expected_sha256="$2"
    archive="$3"
    download="${archive}.download"

    if [ -f "${archive}" ]; then
        actual_sha256="$(sha256sum "${archive}" | awk '{print $1}')"
        if [ "${actual_sha256}" != "${expected_sha256}" ]; then
            echo "Checksum mismatch for ${archive}." >&2
            exit 1
        fi
        return
    fi

    curl \
        --fail \
        --location \
        --retry 3 \
        --output "${download}" \
        "${url}"

    actual_sha256="$(sha256sum "${download}" | awk '{print $1}')"
    if [ "${actual_sha256}" != "${expected_sha256}" ]; then
        echo "Checksum mismatch for ${url}." >&2
        echo "Expected ${expected_sha256}, got ${actual_sha256}." >&2
        exit 1
    fi
    mv "${download}" "${archive}"
}

extract_once() {
    archive="$1"
    source_dir="$2"
    stamp="${source_dir}/.vivid-extracted"

    if [ -f "${stamp}" ]; then
        return
    fi

    mkdir -p "${source_dir}"
    tar -xf "${archive}" -C "${source_dir}" --strip-components=1
    touch "${stamp}"
}

case "${VIVID_BUILD_ARCH}" in
    x86_64)
        PORTABLE_C_FLAGS='-O2 -DNDEBUG -march=x86-64 -mtune=generic'
        ;;
    aarch64)
        PORTABLE_C_FLAGS='-O2 -DNDEBUG -march=armv8-a -mtune=generic'
        ;;
esac

mkdir -p "${VIVID_LAYER_SHELL_RELEASE_DEPS_DIR}" "${VIVID_LAYER_SHELL_DIST_DIR}"

echo "==> Preparing wayland-protocols ${WAYLAND_PROTOCOLS_VERSION}"
download_verified \
    "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/releases/${WAYLAND_PROTOCOLS_VERSION}/downloads/wayland-protocols-${WAYLAND_PROTOCOLS_VERSION}.tar.xz" \
    "${WAYLAND_PROTOCOLS_SHA256}" \
    "${WAYLAND_PROTOCOLS_ARCHIVE}"
extract_once "${WAYLAND_PROTOCOLS_ARCHIVE}" "${WAYLAND_PROTOCOLS_SOURCE}"

if [ -f "${WAYLAND_PROTOCOLS_BUILD}/build.ninja" ]; then
    meson setup --reconfigure \
        --prefix="${VIVID_LAYER_SHELL_RELEASE_PREFIX}" \
        --buildtype=release \
        -Dtests=false \
        "${WAYLAND_PROTOCOLS_BUILD}" \
        "${WAYLAND_PROTOCOLS_SOURCE}"
else
    meson setup \
        --prefix="${VIVID_LAYER_SHELL_RELEASE_PREFIX}" \
        --buildtype=release \
        -Dtests=false \
        "${WAYLAND_PROTOCOLS_BUILD}" \
        "${WAYLAND_PROTOCOLS_SOURCE}"
fi
meson compile -C "${WAYLAND_PROTOCOLS_BUILD}" -j "${BUILD_JOBS}"
meson install -C "${WAYLAND_PROTOCOLS_BUILD}"

echo "==> Preparing static json-c ${JSON_C_VERSION}"
download_verified \
    "https://s3.amazonaws.com/json-c_releases/releases/json-c-${JSON_C_VERSION}.tar.gz" \
    "${JSON_C_SHA256}" \
    "${JSON_C_ARCHIVE}"
extract_once "${JSON_C_ARCHIVE}" "${JSON_C_SOURCE}"

cmake \
    -S "${JSON_C_SOURCE}" \
    -B "${JSON_C_BUILD}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${VIVID_LAYER_SHELL_RELEASE_PREFIX}" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DDISABLE_EXTRA_LIBS=ON
cmake --build "${JSON_C_BUILD}" --parallel "${BUILD_JOBS}"
cmake --install "${JSON_C_BUILD}"

# json-c is linked statically because it is not guaranteed to be installed on
# every compositor host. Graphics, Wayland, and libc stay dynamically linked to
# their stable system ABIs so the executable uses the host's driver stack.
RELEASE_PKG_CONFIG_PATH="${VIVID_LAYER_SHELL_RELEASE_PREFIX}/lib64/pkgconfig:${VIVID_LAYER_SHELL_RELEASE_PREFIX}/lib/pkgconfig:${VIVID_LAYER_SHELL_RELEASE_PREFIX}/share/pkgconfig"
if [ -n "${PKG_CONFIG_PATH:-}" ]; then
    RELEASE_PKG_CONFIG_PATH="${RELEASE_PKG_CONFIG_PATH}:${PKG_CONFIG_PATH}"
fi

echo "==> Building layer-shell release ${VIVID_LAYER_SHELL_PACKAGE_VERSION} for ${VIVID_BUILD_ARCH}"
PKG_CONFIG_PATH="${RELEASE_PKG_CONFIG_PATH}" \
PYTHON="${PYTHON:-python3}" \
cmake \
    -S "${ROOT_DIR}" \
    -B "${VIVID_LAYER_SHELL_RELEASE_BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_FLAGS_RELEASE="${PORTABLE_C_FLAGS}" \
    -DVIVID_LAYER_SHELL_VERSION="${VIVID_LAYER_SHELL_PACKAGE_VERSION}"
PKG_CONFIG_PATH="${RELEASE_PKG_CONFIG_PATH}" \
PYTHON="${PYTHON:-python3}" \
cmake --build "${VIVID_LAYER_SHELL_RELEASE_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}"

install -Dm755 \
    "${VIVID_LAYER_SHELL_RELEASE_BUILD_DIR}/vivid-layer-shell-consumer" \
    "${VIVID_LAYER_SHELL_RELEASE_ARTIFACT}"
strip --strip-unneeded "${VIVID_LAYER_SHELL_RELEASE_ARTIFACT}"

echo "==> Layer-shell release written: ${VIVID_LAYER_SHELL_RELEASE_ARTIFACT}"
