#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "${SCRIPT_DIR}/../../.." && pwd)"
ROOT_DIR="${REPO_ROOT}/producer"
. "${SCRIPT_DIR}/../build_env.sh"
. "${SCRIPT_DIR}/manifest_source.sh"

MANIFEST_TEMPLATE="${VIVID_FLATPAK_MANIFEST}"
MANIFEST="${VIVID_FLATPAK_GENERATED_MANIFEST}"
SOURCE_LOCK="${VIVID_FLATPAK_SOURCE_LOCK}"
GIT_URL="${VIVID_FLATPAK_GIT_URL}"
GIT_BRANCH="${VIVID_FLATPAK_GIT_BRANCH}"
GIT_COMMIT="${VIVID_FLATPAK_GIT_COMMIT:-}"
BUILD_DIR="${VIVID_FLATPAK_BUILD_DIR}"
REPO_DIR="${VIVID_FLATPAK_REPO_DIR}"
BUNDLE="${VIVID_FLATPAK_BUNDLE}"
BUNDLE_BRANCH="${VIVID_FLATPAK_BUNDLE_BRANCH}"
STATE_DIR="${VIVID_FLATPAK_STATE_DIR}"
NATIVE_BUILD_ROOT="${VIVID_FLATPAK_NATIVE_BUILD_ROOT}"
DEFAULT_JOBS="${VIVID_FLATPAK_DEFAULT_JOBS:-12}"
BUILD_JOBS="${VIVID_FLATPAK_JOBS:-${DEFAULT_JOBS}}"
FORCE_CLEAN="${VIVID_FLATPAK_FORCE_CLEAN:-auto}"
DISABLE_DOWNLOAD="${VIVID_FLATPAK_DISABLE_DOWNLOAD:-0}"
DISABLE_ROFILES_FUSE="${VIVID_FLATPAK_DISABLE_ROFILES_FUSE:-0}"
# Do not keep flatpak-builder's numbered module work directories by default.
# With --keep-build-dirs, flatpak-builder creates vivid-producer-N directories
# and moves the stable build/vivid-producer symlink to the newest one on each
# cache miss. That is useful when debugging a failed module build, but it is not
# the stable CMake incremental build directory for this manifest. Iterative
# local rebuild speed comes from the mounted native CMake root, the Flatpak
# module cache, and --ccache instead.
KEEP_BUILD_DIRS="${VIVID_FLATPAK_KEEP_BUILD_DIRS:-0}"
CCACHE="${VIVID_FLATPAK_CCACHE:-1}"
INSTALL_DEPS_FROM="${VIVID_FLATPAK_INSTALL_DEPS_FROM:-}"
FLATPAK_CEF_ARCHIVE="${VIVID_CEF_ARCHIVE}"
APP_VERSION="${VIVID_FLATPAK_APP_VERSION}"
RELEASE_DATE="${VIVID_FLATPAK_RELEASE_DATE}"

export CCACHE_DIR="${VIVID_FLATPAK_CCACHE_DIR:-${STATE_DIR}/ccache}"
export CCACHE_MAXSIZE="${VIVID_FLATPAK_CCACHE_MAXSIZE:-20G}"

validate_bool() {
    name="$1"
    value="$2"
    case "${value}" in
        0|1)
            ;;
        *)
            echo "Invalid ${name}=${value}; expected 0 or 1." >&2
            exit 1
            ;;
    esac
}

case "${BUILD_JOBS}" in
    ''|*[!0-9]*|0)
        echo "Invalid VIVID_FLATPAK_JOBS=${BUILD_JOBS}; expected a positive integer." >&2
        exit 1
        ;;
esac

if [ -z "${BUNDLE_BRANCH}" ]; then
    echo "Invalid VIVID_FLATPAK_BUNDLE_BRANCH: expected a non-empty branch name." >&2
    exit 1
fi

case "${FORCE_CLEAN}" in
    auto|0|1)
        ;;
    *)
        echo "Invalid VIVID_FLATPAK_FORCE_CLEAN=${FORCE_CLEAN}; expected auto, 0, or 1." >&2
        exit 1
        ;;
esac

validate_bool VIVID_FLATPAK_DISABLE_DOWNLOAD "${DISABLE_DOWNLOAD}"
validate_bool VIVID_FLATPAK_DISABLE_ROFILES_FUSE "${DISABLE_ROFILES_FUSE}"
validate_bool VIVID_FLATPAK_KEEP_BUILD_DIRS "${KEEP_BUILD_DIRS}"
validate_bool VIVID_FLATPAK_CCACHE "${CCACHE}"
vivid_flatpak_validate_app_release "${APP_VERSION}" "${RELEASE_DATE}"

MANIFEST_TEMPLATE="$(vivid_flatpak_absolute_path "${MANIFEST_TEMPLATE}")"
MANIFEST="$(vivid_flatpak_absolute_path "${MANIFEST}")"
SOURCE_LOCK="$(vivid_flatpak_absolute_path "${SOURCE_LOCK}")"
BUILD_DIR="$(vivid_flatpak_absolute_path "${BUILD_DIR}")"
REPO_DIR="$(vivid_flatpak_absolute_path "${REPO_DIR}")"
BUNDLE="$(vivid_flatpak_absolute_path "${BUNDLE}")"
STATE_DIR="$(vivid_flatpak_absolute_path "${STATE_DIR}")"
NATIVE_BUILD_ROOT="$(vivid_flatpak_absolute_path "${NATIVE_BUILD_ROOT}")"
CCACHE_DIR="$(vivid_flatpak_absolute_path "${CCACHE_DIR}")"
FLATPAK_CEF_ARCHIVE="$(vivid_flatpak_absolute_path "${FLATPAK_CEF_ARCHIVE}")"

vivid_flatpak_require_file "${MANIFEST_TEMPLATE}"
vivid_flatpak_require_file "${FLATPAK_CEF_ARCHIVE}"

ALLOW_GIT_NETWORK=1
if [ "${DISABLE_DOWNLOAD}" = 1 ]; then
    ALLOW_GIT_NETWORK=0
fi
GIT_COMMIT="$(vivid_flatpak_resolve_git_commit "${GIT_URL}" "${GIT_BRANCH}" "${GIT_COMMIT}" "${SOURCE_LOCK}" "${ALLOW_GIT_NETWORK}")"
vivid_flatpak_warn_git_source_not_local "${REPO_ROOT}" "${GIT_COMMIT}"

ACTUAL_FORCE_CLEAN=0
if [ "${FORCE_CLEAN}" = 1 ]; then
    ACTUAL_FORCE_CLEAN=1
elif [ "${FORCE_CLEAN}" = auto ] && [ -d "${BUILD_DIR}" ] && [ -n "$(find "${BUILD_DIR}" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
    ACTUAL_FORCE_CLEAN=1
fi

mkdir -p \
    "$(dirname "${BUILD_DIR}")" \
    "$(dirname "${REPO_DIR}")" \
    "$(dirname "${BUNDLE}")" \
    "$(dirname "${MANIFEST}")" \
    "$(dirname "${SOURCE_LOCK}")" \
    "${STATE_DIR}" \
    "${NATIVE_BUILD_ROOT}" \
    "${CCACHE_DIR}"

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

echo "==> Flatpak producer manifest template: ${MANIFEST_TEMPLATE}"
echo "==> Flatpak producer generated manifest: ${MANIFEST}"
echo "==> Flatpak producer source lock: ${SOURCE_LOCK}"
echo "==> Flatpak producer git source: ${GIT_URL}"
echo "==> Flatpak producer git branch: ${GIT_BRANCH}"
echo "==> Flatpak producer git commit: ${VIVID_FLATPAK_GIT_COMMIT}"
echo "==> Flatpak app version: ${APP_VERSION} (${RELEASE_DATE})"
echo "==> Flatpak producer app dir: ${BUILD_DIR}"
echo "==> Flatpak producer repository: ${REPO_DIR}"
echo "==> Flatpak producer bundle: ${BUNDLE}"
echo "==> Flatpak producer bundle branch: ${BUNDLE_BRANCH}"
echo "==> Flatpak builder state: ${STATE_DIR}"
echo "==> Flatpak native CMake root: ${NATIVE_BUILD_ROOT}"
echo "==> Flatpak producer build jobs: ${BUILD_JOBS}"
if [ "${CCACHE}" = 1 ]; then
    echo "==> Flatpak ccache: ${CCACHE_DIR} (${CCACHE_MAXSIZE})"
else
    echo "==> Flatpak ccache is disabled by VIVID_FLATPAK_CCACHE=0."
fi
echo "==> Flatpak CEF archive source: ${FLATPAK_CEF_ARCHIVE}"
if [ "${DISABLE_DOWNLOAD}" = 1 ]; then
    echo "==> Flatpak downloads are disabled; run tools/vivid.sh flatpak prefetch first."
else
    echo "==> Flatpak downloads are enabled for manifest URL sources."
fi
if [ "${DISABLE_ROFILES_FUSE}" = 1 ]; then
    echo "==> Flatpak rofiles-fuse is disabled by VIVID_FLATPAK_DISABLE_ROFILES_FUSE=1."
fi
if [ "${KEEP_BUILD_DIRS}" = 1 ]; then
    echo "==> Flatpak module build directories will be kept for debugging."
fi
if [ "${ACTUAL_FORCE_CLEAN}" = 1 ]; then
    echo "==> Flatpak app dir will be refreshed with --force-clean."
fi

set -- \
    --jobs="${BUILD_JOBS}" \
    --state-dir="${STATE_DIR}" \
    --repo="${REPO_DIR}" \
    --default-branch="${BUNDLE_BRANCH}" \
    "${BUILD_DIR}" \
    "${MANIFEST}"

if [ "${ACTUAL_FORCE_CLEAN}" = 1 ]; then
    set -- --force-clean "$@"
fi
# CCACHE_DIR only chooses the cache location; flatpak-builder wires ccache into
# the SDK compiler path only when --ccache is passed.
if [ "${CCACHE}" = 1 ]; then
    set -- --ccache "$@"
fi
if [ "${DISABLE_DOWNLOAD}" = 1 ]; then
    set -- --disable-download "$@"
fi
if [ "${DISABLE_ROFILES_FUSE}" = 1 ]; then
    set -- --disable-rofiles-fuse "$@"
fi
if [ "${KEEP_BUILD_DIRS}" = 1 ]; then
    set -- --keep-build-dirs "$@"
fi
if [ -n "${INSTALL_DEPS_FROM}" ]; then
    set -- --install-deps-from="${INSTALL_DEPS_FROM}" "$@"
fi

flatpak-builder "$@"

flatpak build-bundle \
    "${REPO_DIR}" \
    "${BUNDLE}" \
    "${VIVID_PRODUCER_APP_ID}" \
    "${BUNDLE_BRANCH}"
