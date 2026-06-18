#!/bin/sh

# Shared helpers for Flatpak source staging.
#
# The checked-in manifest is a template. The concrete manifest pins Vivid's
# GitHub source to an exact commit so Flatpak source mirroring and module cache
# keys are stable and reproducible.

vivid_flatpak_absolute_path() {
    path="$1"
    realpath -m -- "${path}"
}

vivid_flatpak_require_file() {
    if [ ! -f "$1" ]; then
        echo "Missing required Flatpak source: $1" >&2
        exit 1
    fi
}

vivid_flatpak_sed_escape_replacement() {
    printf '%s' "$1" | sed 's/[&|\\]/\\&/g'
}

vivid_flatpak_git_branch_ref() {
    git_branch="$1"

    case "${git_branch}" in
        refs/*)
            printf '%s\n' "${git_branch}"
            ;;
        *)
            printf 'refs/heads/%s\n' "${git_branch}"
            ;;
    esac
}

vivid_flatpak_git_branch_commit() {
    git_url="$1"
    git_branch="$2"
    git_ref="$(vivid_flatpak_git_branch_ref "${git_branch}")"

    # The Flatpak manifest contains both branch and commit. flatpak-builder
    # validates that the named branch resolves to the exact commit, so resolve
    # the configured GitHub branch first instead of using the local checkout's
    # HEAD, which may contain commits that are not present on GitHub yet.
    output="$(git ls-remote --exit-code "${git_url}" "${git_ref}")" || {
        echo "Failed to resolve Flatpak git source ${git_url} ${git_ref}." >&2
        exit 1
    }

    set -- ${output}
    printf '%s\n' "$1"
}

vivid_flatpak_is_full_git_commit() {
    case "$1" in
        *[!0-9a-fA-F]*|'')
            return 1
            ;;
        *)
            [ "${#1}" -eq 40 ]
            ;;
    esac
}

vivid_flatpak_validate_app_release() {
    app_version="$1"
    release_date="$2"

    case "${app_version}" in
        ''|*[!0-9A-Za-z._+~-]*)
            echo "Invalid VIVID_FLATPAK_APP_VERSION=${app_version}; expected only 0-9, A-Z, a-z, '.', '_', '+', '~', or '-'." >&2
            exit 1
            ;;
    esac

    case "${release_date}" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9])
            ;;
        *)
            echo "Invalid VIVID_FLATPAK_RELEASE_DATE=${release_date}; expected YYYY-MM-DD." >&2
            exit 1
            ;;
    esac
}

vivid_flatpak_read_git_lock_commit() {
    lock_file="$1"
    expected_url="$2"
    expected_branch="$3"
    lock_url=""
    lock_branch=""
    lock_commit=""

    [ -f "${lock_file}" ] || return 1

    # This lock is a tiny key/value file written by the prefetch/build wrapper.
    # Parse only the keys we own instead of sourcing it; that keeps the offline
    # path data-only and prevents a corrupted .build file from executing shell.
    while IFS='=' read -r key value; do
        case "${key}" in
            VIVID_FLATPAK_GIT_URL)
                lock_url="${value}"
                ;;
            VIVID_FLATPAK_GIT_BRANCH)
                lock_branch="${value}"
                ;;
            VIVID_FLATPAK_GIT_COMMIT)
                lock_commit="${value}"
                ;;
        esac
    done < "${lock_file}"

    if [ "${lock_url}" != "${expected_url}" ] || [ "${lock_branch}" != "${expected_branch}" ]; then
        return 1
    fi
    vivid_flatpak_is_full_git_commit "${lock_commit}" || return 1

    printf '%s\n' "${lock_commit}"
}

vivid_flatpak_resolve_git_commit() {
    git_url="$1"
    git_branch="$2"
    requested_commit="$3"
    lock_file="$4"
    allow_network="$5"

    if [ -n "${requested_commit}" ] && ! vivid_flatpak_is_full_git_commit "${requested_commit}"; then
        echo "Invalid VIVID_FLATPAK_GIT_COMMIT=${requested_commit}; expected a full 40-character commit." >&2
        exit 1
    fi

    if [ "${allow_network}" = 0 ]; then
        if [ -n "${requested_commit}" ]; then
            printf '%s\n' "${requested_commit}"
            return
        fi
        if lock_commit="$(vivid_flatpak_read_git_lock_commit "${lock_file}" "${git_url}" "${git_branch}")"; then
            printf '%s\n' "${lock_commit}"
            return
        fi

        echo "Flatpak downloads are disabled, and no matching git source lock was found at ${lock_file}." >&2
        echo "Run tools/vivid.sh flatpak prefetch once, or set VIVID_FLATPAK_GIT_COMMIT to the cached commit." >&2
        exit 1
    fi

    branch_commit="$(vivid_flatpak_git_branch_commit "${git_url}" "${git_branch}")"

    if [ -n "${requested_commit}" ] && [ "${requested_commit}" != "${branch_commit}" ]; then
        echo "VIVID_FLATPAK_GIT_COMMIT=${requested_commit} does not match ${git_url} ${git_branch} (${branch_commit})." >&2
        echo "Flatpak validates branch+commit sources; push/update the branch or use a matching VIVID_FLATPAK_GIT_COMMIT." >&2
        exit 1
    fi

    if [ -n "${requested_commit}" ]; then
        printf '%s\n' "${requested_commit}"
    else
        printf '%s\n' "${branch_commit}"
    fi
}

vivid_flatpak_write_git_lock() {
    lock_file="$1"
    git_url="$2"
    git_branch="$3"
    git_commit="$4"
    tmp="${lock_file}.tmp.$$"

    mkdir -p "$(dirname "${lock_file}")"

    # The lock records the exact source that was rendered into the generated
    # manifest. Offline builds read this file so VIVID_FLATPAK_DISABLE_DOWNLOAD=1
    # can honor flatpak-builder's no-network contract without guessing which
    # remote branch commit was last prefetched.
    {
        printf 'VIVID_FLATPAK_GIT_URL=%s\n' "${git_url}"
        printf 'VIVID_FLATPAK_GIT_BRANCH=%s\n' "${git_branch}"
        printf 'VIVID_FLATPAK_GIT_COMMIT=%s\n' "${git_commit}"
    } > "${tmp}"
    mv "${tmp}" "${lock_file}"
}

vivid_flatpak_warn_git_source_not_local() {
    repo_root="$1"
    git_commit="$2"

    if [ -n "$(git -C "${repo_root}" status --porcelain)" ]; then
        echo "==> Flatpak git source uses the configured GitHub commit; uncommitted local changes are not included."
    fi

    local_commit="$(git -C "${repo_root}" rev-parse HEAD 2>/dev/null || true)"
    if [ -n "${local_commit}" ] && [ "${local_commit}" != "${git_commit}" ]; then
        echo "==> Flatpak git source commit differs from local HEAD (${local_commit}); local-only commits are not included."
    fi
}

vivid_flatpak_render_manifest() {
    template="$1"
    output="$2"
    git_url="$3"
    git_branch="$4"
    git_commit="$5"
    cef_archive="$6"
    native_build_root="$7"
    app_version="$8"
    release_date="$9"

    cef_sha256="$(sha256sum "${cef_archive}" | awk '{print $1}')"

    mkdir -p "$(dirname "${output}")"

    sed \
        -e '/@VIVID_TEMPLATE_COMMENT_BEGIN@/,/@VIVID_TEMPLATE_COMMENT_END@/d' \
        -e "s|@SOURCE_GIT_URL@|$(vivid_flatpak_sed_escape_replacement "${git_url}")|g" \
        -e "s|@SOURCE_GIT_BRANCH@|$(vivid_flatpak_sed_escape_replacement "${git_branch}")|g" \
        -e "s|@SOURCE_GIT_COMMIT@|$(vivid_flatpak_sed_escape_replacement "${git_commit}")|g" \
        -e "s|@CEF_ARCHIVE@|$(vivid_flatpak_sed_escape_replacement "${cef_archive}")|g" \
        -e "s|@CEF_ARCHIVE_SHA256@|$(vivid_flatpak_sed_escape_replacement "${cef_sha256}")|g" \
        -e "s|@NATIVE_BUILD_ROOT@|$(vivid_flatpak_sed_escape_replacement "${native_build_root}")|g" \
        -e "s|@APP_VERSION@|$(vivid_flatpak_sed_escape_replacement "${app_version}")|g" \
        -e "s|@RELEASE_DATE@|$(vivid_flatpak_sed_escape_replacement "${release_date}")|g" \
        "${template}" > "${output}"

    VIVID_FLATPAK_GIT_COMMIT="${git_commit}"
    VIVID_FLATPAK_CEF_ARCHIVE_SHA256="${cef_sha256}"
}
