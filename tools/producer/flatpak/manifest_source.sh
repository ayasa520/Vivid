#!/bin/sh

# Shared helpers for Flatpak source staging.
#
# The checked-in manifest is a template. Local development builds need to see
# uncommitted files, but flatpak-builder's `type: dir` source deliberately does
# not participate in module caching. To get both current checkout contents and
# cacheable Flatpak inputs, the build scripts create a deterministic archive
# under .build and render a concrete manifest that points at that archive.

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

vivid_flatpak_sha256() {
    sha256sum "$1" | awk '{print $1}'
}

vivid_flatpak_create_source_archive() {
    repo_root="$1"
    output="$2"
    tmp="${output}.tmp.$$"

    mkdir -p "$(dirname "${output}")"
    rm -f "${tmp}"

    # Keep this archive stable for identical source content. A deterministic
    # tarball gives flatpak-builder an ordinary archive source, so module cache
    # hits work even though the input originated from the local worktree.
    tar \
        --sort=name \
        --mtime='UTC 1970-01-01' \
        --owner=0 \
        --group=0 \
        --numeric-owner \
        --exclude-vcs \
        --exclude='producer/.build' \
        --exclude='producer/packaging/flatpak/sources' \
        -cf "${tmp}" \
        -C "${repo_root}" \
        producer/src \
        producer/resources \
        producer/third_party \
        producer/README.md \
        producer/LICENSE \
        producer/packaging/flatpak/vivid-launcher \
        tools/producer

    mv "${tmp}" "${output}"
}

vivid_flatpak_render_manifest() {
    template="$1"
    output="$2"
    source_archive="$3"
    cef_archive="$4"
    native_build_root="$5"

    source_sha256="$(vivid_flatpak_sha256 "${source_archive}")"
    cef_sha256="$(vivid_flatpak_sha256 "${cef_archive}")"

    mkdir -p "$(dirname "${output}")"

    sed \
        -e '/@VIVID_TEMPLATE_COMMENT_BEGIN@/,/@VIVID_TEMPLATE_COMMENT_END@/d' \
        -e "s|@SOURCE_ARCHIVE@|$(vivid_flatpak_sed_escape_replacement "${source_archive}")|g" \
        -e "s|@SOURCE_ARCHIVE_SHA256@|$(vivid_flatpak_sed_escape_replacement "${source_sha256}")|g" \
        -e "s|@CEF_ARCHIVE@|$(vivid_flatpak_sed_escape_replacement "${cef_archive}")|g" \
        -e "s|@CEF_ARCHIVE_SHA256@|$(vivid_flatpak_sed_escape_replacement "${cef_sha256}")|g" \
        -e "s|@NATIVE_BUILD_ROOT@|$(vivid_flatpak_sed_escape_replacement "${native_build_root}")|g" \
        "${template}" > "${output}"

    VIVID_FLATPAK_SOURCE_ARCHIVE_SHA256="${source_sha256}"
    VIVID_FLATPAK_CEF_ARCHIVE_SHA256="${cef_sha256}"
}
