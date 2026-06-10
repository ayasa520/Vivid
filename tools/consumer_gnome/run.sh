#!/usr/bin/env bash

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
    echo "Error: this script should not be run as root" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../consumer/gnome" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

UUID="${VIVID_CONSUMER_EXTENSION_UUID}"

case "${1:-help}" in
    install)
        shift
        rm -rf "${VIVID_CONSUMER_BUILD_DIR}"
        meson setup "${VIVID_CONSUMER_BUILD_DIR}" "${ROOT_DIR}" --prefix="${VIVID_CONSUMER_INSTALL_PREFIX}" "$@"
        ninja -C "${VIVID_CONSUMER_BUILD_DIR}" install
        ;;
    zip)
        "${SCRIPT_DIR}/build_zip.sh"
        ;;
    enable)
        gnome-extensions enable "${UUID}"
        ;;
    disable)
        gnome-extensions disable "${UUID}"
        ;;
    reset)
        gnome-extensions reset "${UUID}"
        ;;
    uninstall)
        gnome-extensions uninstall "${UUID}"
        ;;
    log)
        journalctl -f -o cat "${VIVID_CONSUMER_GNOME_SHELL_JOURNAL}"
        ;;
    help|*)
        echo "Usage: $0 {install|zip|enable|disable|reset|uninstall|log}"
        ;;
esac
