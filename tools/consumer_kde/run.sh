#!/usr/bin/env bash

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Error: this script should not be run as root" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../consumer/kde" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

configure() {
    cmake -S "${ROOT_DIR}" -B "${VIVID_KDE_BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
        -DVIVID_KDE_PACKAGE_ID="${VIVID_KDE_PACKAGE_ID}" \
        -DVIVID_KDE_PACKAGE_ABI="${VIVID_KDE_PACKAGE_ABI}" \
        -DVIVID_KDE_QML_URI="${VIVID_KDE_QML_URI}" \
        -DVIVID_KDE_DIST_DIR="${VIVID_KDE_DIST_DIR}"
}

build() {
    configure
    cmake --build "${VIVID_KDE_BUILD_DIR}"
}

clean_dist_archives() {
    mkdir -p "${VIVID_KDE_DIST_DIR}"

    # Keep dist from advertising obsolete package identities after a rename.
    # CPack writes the new archive next to any previous ZIPs, so remove only
    # known KDE consumer archive names before producing the replacement.
    find "${VIVID_KDE_DIST_DIR}" -maxdepth 1 -type f \
        \( -name "vivid-kde-*.zip" -o -name "vivid-consumer-kde-*.zip" \) \
        -delete
}

stage_package() {
    rm -rf "${VIVID_KDE_STAGING_DIR}"
    cmake --install "${VIVID_KDE_BUILD_DIR}" \
        --component kde_consumer \
        --prefix "${VIVID_KDE_STAGING_DIR}"
}

case "${1:-help}" in
    build)
        build
        ;;
    install)
        build
        stage_package
        PACKAGE_DIR="${VIVID_KDE_STAGING_DIR}/${VIVID_KDE_PACKAGE_ID}"
        if kpackagetool6 --type Plasma/Wallpaper -u "${PACKAGE_DIR}"; then
            :
        else
            kpackagetool6 --type Plasma/Wallpaper -i "${PACKAGE_DIR}"
        fi
        ;;
    zip)
        configure
        clean_dist_archives
        cmake --build "${VIVID_KDE_BUILD_DIR}" --target package
        ;;
    uninstall)
        kpackagetool6 --type Plasma/Wallpaper -r "${VIVID_KDE_PACKAGE_ID}"
        ;;
    log)
        journalctl --user -f -o cat -u plasma-plasmashell.service
        ;;
    help|*)
        echo "Usage: $0 {build|install|zip|uninstall|log}"
        ;;
esac
