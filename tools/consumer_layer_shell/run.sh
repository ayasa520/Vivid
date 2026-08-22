#!/usr/bin/env bash

set -euo pipefail

ACTION="${1:-help}"

if [ "$(id -u)" -eq 0 ] && [ "${ACTION}" = run ]; then
    echo "Error: this script should not be run as root" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/../../consumer/layer-shell" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

configure() {
    cmake -S "${ROOT_DIR}" -B "${VIVID_LAYER_SHELL_BUILD_DIR}" -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
        -DVIVID_LAYER_SHELL_VERSION="${VIVID_LAYER_SHELL_PACKAGE_VERSION}"
}

build() {
    configure
    cmake --build "${VIVID_LAYER_SHELL_BUILD_DIR}"
}

run_consumer() {
    build
    exec "${VIVID_LAYER_SHELL_BUILD_DIR}/vivid-layer-shell-consumer" "$@"
}

case "${ACTION}" in
    build)
        shift
        build "$@"
        ;;
    run)
        shift
        run_consumer "$@"
        ;;
    release)
        shift
        if [ "$#" -ne 0 ]; then
            echo "Usage: $0 release" >&2
            exit 2
        fi
        sh "${SCRIPT_DIR}/build_release.sh"
        ;;
    help|*)
        echo "Usage: $0 {build|release|run} [--layer background|bottom|top|overlay]"
        echo "  build  configure and compile the layer-shell consumer"
        echo "  release  build a versioned portable release executable"
        echo "  run    build, then exec vivid-layer-shell-consumer"
        echo
        echo "Compositor autostart snippets: consumer/layer-shell/examples/"
        echo "  Hyprland  Lua autostart + hl.layer_rule namespace vivid"
        echo "  Sway      exec vivid-layer-shell-consumer"
        echo "  niri      spawn-at-startup"
        echo "Default layer is bottom;"
        echo "niri auto-selects background. Override with --layer."
        ;;
esac
