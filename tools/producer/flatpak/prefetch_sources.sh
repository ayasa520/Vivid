#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../../.." && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../../producer" && pwd)"
. "${SCRIPT_DIR}/../build_env.sh"
. "${SCRIPT_DIR}/manifest_source.sh"

MANIFEST_TEMPLATE="${VIVID_FLATPAK_MANIFEST}"
MANIFEST="${VIVID_FLATPAK_GENERATED_MANIFEST}"
SOURCE_LOCK="${VIVID_FLATPAK_SOURCE_LOCK}"
GIT_URL="${VIVID_FLATPAK_GIT_URL}"
GIT_BRANCH="${VIVID_FLATPAK_GIT_BRANCH}"
GIT_COMMIT="${VIVID_FLATPAK_GIT_COMMIT:-}"
DOWNLOAD_DIR="${VIVID_FLATPAK_DOWNLOAD_DIR}"
STATE_DIR="${VIVID_FLATPAK_STATE_DIR}"
NATIVE_BUILD_ROOT="${VIVID_FLATPAK_NATIVE_BUILD_ROOT}"
FLATPAK_CEF_ARCHIVE="${VIVID_CEF_ARCHIVE}"
APP_VERSION="${VIVID_FLATPAK_APP_VERSION}"
RELEASE_DATE="${VIVID_FLATPAK_RELEASE_DATE}"
DISABLE_ROFILES_FUSE="${VIVID_FLATPAK_DISABLE_ROFILES_FUSE:-0}"

case "${DISABLE_ROFILES_FUSE}" in
    0|1)
        ;;
    *)
        echo "Invalid VIVID_FLATPAK_DISABLE_ROFILES_FUSE=${DISABLE_ROFILES_FUSE}; expected 0 or 1." >&2
        exit 1
        ;;
esac

MANIFEST_TEMPLATE="$(vivid_flatpak_absolute_path "${MANIFEST_TEMPLATE}")"
MANIFEST="$(vivid_flatpak_absolute_path "${MANIFEST}")"
SOURCE_LOCK="$(vivid_flatpak_absolute_path "${SOURCE_LOCK}")"
DOWNLOAD_DIR="$(vivid_flatpak_absolute_path "${DOWNLOAD_DIR}")"
STATE_DIR="$(vivid_flatpak_absolute_path "${STATE_DIR}")"
NATIVE_BUILD_ROOT="$(vivid_flatpak_absolute_path "${NATIVE_BUILD_ROOT}")"
FLATPAK_CEF_ARCHIVE="$(vivid_flatpak_absolute_path "${FLATPAK_CEF_ARCHIVE}")"

vivid_flatpak_require_file "${MANIFEST_TEMPLATE}"
vivid_flatpak_require_file "${FLATPAK_CEF_ARCHIVE}"
vivid_flatpak_validate_app_release "${APP_VERSION}" "${RELEASE_DATE}"

GIT_COMMIT="$(vivid_flatpak_resolve_git_commit "${GIT_URL}" "${GIT_BRANCH}" "${GIT_COMMIT}" "${SOURCE_LOCK}" 1)"
vivid_flatpak_warn_git_source_not_local "${REPO_ROOT}" "${GIT_COMMIT}"

mkdir -p \
    "${DOWNLOAD_DIR}" \
    "${STATE_DIR}" \
    "${NATIVE_BUILD_ROOT}" \
    "$(dirname "${SOURCE_LOCK}")"

vivid_flatpak_render_manifest \
    "${MANIFEST_TEMPLATE}" \
    "${MANIFEST}" \
    "${GIT_URL}" \
    "${GIT_BRANCH}" \
    "${GIT_COMMIT}" \
    "${FLATPAK_CEF_ARCHIVE}" \
    "${NATIVE_BUILD_ROOT}" \
    "${APP_VERSION}" \
    "${RELEASE_DATE}"
vivid_flatpak_write_git_lock "${SOURCE_LOCK}" "${GIT_URL}" "${GIT_BRANCH}" "${GIT_COMMIT}"

echo "==> Prefetching Flatpak URL sources from generated manifest: ${MANIFEST}"
echo "==> Flatpak manifest template: ${MANIFEST_TEMPLATE}"
echo "==> Flatpak source lock: ${SOURCE_LOCK}"
echo "==> Flatpak git source: ${GIT_URL}"
echo "==> Flatpak git branch: ${GIT_BRANCH}"
echo "==> Flatpak git commit: ${VIVID_FLATPAK_GIT_COMMIT}"
echo "==> Flatpak app version: ${APP_VERSION} (${RELEASE_DATE})"
echo "==> Flatpak download-only dir: ${DOWNLOAD_DIR}"
echo "==> Flatpak builder state: ${STATE_DIR}"
echo "==> Local CEF archive source: ${FLATPAK_CEF_ARCHIVE}"

set -- \
    --download-only \
    --force-clean \
    --state-dir="${STATE_DIR}" \
    "${DOWNLOAD_DIR}" \
    "${MANIFEST}"

if [ "${DISABLE_ROFILES_FUSE}" = 1 ]; then
    set -- --disable-rofiles-fuse "$@"
fi

exec flatpak-builder "$@"
