#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

ARCHIVE="${VIVID_CEF_ARCHIVE}"
DOWNLOAD="${ARCHIVE}.download"

verify_archive() {
    archive="$1"
    actual="$(sha1sum "${archive}" | awk '{print $1}')"
    if [ "${actual}" != "${VIVID_CEF_ARCHIVE_SHA1}" ]; then
        echo "CEF checksum mismatch for ${archive}." >&2
        echo "Expected ${VIVID_CEF_ARCHIVE_SHA1}, got ${actual}." >&2
        return 1
    fi
}

if [ -f "${ARCHIVE}" ]; then
    verify_archive "${ARCHIVE}"
    echo "==> CEF archive is ready: ${ARCHIVE}"
    exit 0
fi

mkdir -p "$(dirname "${ARCHIVE}")"

# Download to a separate path and verify the upstream checksum before the file
# becomes a build input. An interrupted or corrupt transfer therefore cannot be
# consumed by the direct-run or Flatpak build on a later invocation.
echo "==> Downloading CEF ${VIVID_CEF_VERSION} for ${VIVID_BUILD_ARCH}"
curl \
    --fail \
    --location \
    --retry 3 \
    --output "${DOWNLOAD}" \
    "${VIVID_CEF_ARCHIVE_URL}"
verify_archive "${DOWNLOAD}"
mv "${DOWNLOAD}" "${ARCHIVE}"

echo "==> CEF archive is ready: ${ARCHIVE}"
