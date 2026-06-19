#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../consumer/gnome" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

BUILD_DIR="${VIVID_CONSUMER_PACKAGE_BUILD_DIR}"
DESTDIR_ROOT="${VIVID_CONSUMER_DESTDIR_ROOT}"
DIST_DIR="${VIVID_CONSUMER_DIST_DIR}"
PACKAGE_BASENAME="${VIVID_CONSUMER_PACKAGE_BASENAME}"
PACKAGE_ABI="${VIVID_CONSUMER_PACKAGE_ABI}"
PREFIX="${VIVID_CONSUMER_PACKAGE_PREFIX}"
DATADIR="${VIVID_CONSUMER_PACKAGE_DATADIR}"
UUID="${VIVID_CONSUMER_EXTENSION_UUID}"
SCHEMA_ID="${VIVID_CONSUMER_SCHEMA_ID}"
EXTENSION_NAME="${VIVID_CONSUMER_EXTENSION_NAME}"
EXTENSION_DESCRIPTION="${VIVID_CONSUMER_EXTENSION_DESCRIPTION}"
EXTENSION_AUTHOR="${VIVID_CONSUMER_EXTENSION_AUTHOR}"
EXTENSION_URL="${VIVID_CONSUMER_EXTENSION_URL}"
EXTENSION_LOG_PREFIX="${VIVID_CONSUMER_EXTENSION_LOG_PREFIX}"
PACKAGE_VERSION="${VIVID_CONSUMER_PACKAGE_VERSION}"

configure_consumer_identity() {
    meson configure "${BUILD_DIR}" \
        --prefix="${PREFIX}" \
        --datadir="${DATADIR}" \
        -Dpackage-version="${PACKAGE_VERSION}" \
        -Dextension-uuid="${UUID}" \
        -Dextension-schema="${SCHEMA_ID}" \
        -Dextension-name="${EXTENSION_NAME}" \
        -Dextension-description="${EXTENSION_DESCRIPTION}" \
        -Dextension-author="${EXTENSION_AUTHOR}" \
        -Dextension-url="${EXTENSION_URL}" \
        -Dextension-log-prefix="${EXTENSION_LOG_PREFIX}"
}

if [ -f "${BUILD_DIR}/build.ninja" ]; then
    # Keep packaged identity centralized so this display sink can be branded
    # independently from any legacy in-process wallpaper extension.
    configure_consumer_identity
    meson setup --reconfigure "${BUILD_DIR}" "${ROOT_DIR}"
else
    meson setup "${BUILD_DIR}" "${ROOT_DIR}" --prefix="${PREFIX}" --datadir="${DATADIR}"
    configure_consumer_identity
    meson setup --reconfigure "${BUILD_DIR}" "${ROOT_DIR}"
fi

meson compile -C "${BUILD_DIR}"

rm -rf "${DESTDIR_ROOT}" "${DIST_DIR}"
mkdir -p "${DESTDIR_ROOT}" "${DIST_DIR}"

DESTDIR="${DESTDIR_ROOT}" meson install -C "${BUILD_DIR}"

EXTENSION_DIR="${DESTDIR_ROOT}${PREFIX}/${DATADIR}/gnome-shell/extensions/${UUID}"
SCHEMA_FILE="${DESTDIR_ROOT}${PREFIX}/${DATADIR}/glib-2.0/schemas/${SCHEMA_ID}.gschema.xml"

if [ ! -f "${EXTENSION_DIR}/metadata.json" ]; then
    echo "Consumer extension metadata was not installed at ${EXTENSION_DIR}" >&2
    exit 1
fi

if [ ! -d "${EXTENSION_DIR}/display_consumer" ]; then
    echo "Display consumer module was not installed at ${EXTENSION_DIR}/display_consumer" >&2
    exit 1
fi

# GNOME extension ZIPs can record Unix symlinks, but user-facing extraction
# paths are not guaranteed to preserve them. The display receiver typelib names
# the library by SONAME, so ship real ELF files for lib*.so.0 instead of
# symlink entries that may become tiny text files after installation.
find "${EXTENSION_DIR}/display_consumer" -maxdepth 1 -type l | while IFS= read -r link; do
    target="$(readlink -f "${link}")"
    tmp="${link}.real"
    cp "${target}" "${tmp}"
    mv -f "${tmp}" "${link}"
done

gnome-extensions pack \
    --force \
    --out-dir="${DIST_DIR}" \
    --schema="${SCHEMA_FILE}" \
    --extra-source="${EXTENSION_DIR}/buildConfig.js" \
    --extra-source="${EXTENSION_DIR}/common" \
    --extra-source="${EXTENSION_DIR}/shell" \
    --extra-source="${EXTENSION_DIR}/display_consumer" \
    --extra-source="${EXTENSION_DIR}/LICENSE" \
    "${EXTENSION_DIR}"

PACKAGE_SOURCE="${DIST_DIR}/${UUID}.shell-extension.zip"
PACKAGE_DEST="${DIST_DIR}/${PACKAGE_BASENAME}-${PACKAGE_VERSION}-${PACKAGE_ABI}.zip"

# gnome-extensions pack always names the archive after the extension UUID.
# Keep the UUID as the GNOME installation identity, then normalize the public
# artifact to the shared consumer package contract:
# vivid-consumer-<desktop>-<project-version>-<abi>.zip.
if [ "${PACKAGE_SOURCE}" != "${PACKAGE_DEST}" ]; then
    mv -f "${PACKAGE_SOURCE}" "${PACKAGE_DEST}"
fi

echo "==> Consumer zip is in ${DIST_DIR}"
