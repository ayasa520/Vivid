#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../../producer" && pwd)"
. "${SCRIPT_DIR}/../build_env.sh"

APP_DIR="${VIVID_FLATPAK_BUILD_DIR}"
CONFIG_DIR="${VIVID_FLATPAK_RUN_CONFIG_DIR}"
CACHE_DIR="${VIVID_FLATPAK_RUN_CACHE_DIR}"
DATA_DIR="${VIVID_FLATPAK_RUN_DATA_DIR}"
STATE_DIR="${VIVID_FLATPAK_RUN_STATE_DIR}"

if [ ! -d "${APP_DIR}/files" ]; then
    echo "Missing Flatpak app dir: ${APP_DIR}" >&2
    echo "Run tools/vivid.sh flatpak build first." >&2
    exit 1
fi

mkdir -p "${CONFIG_DIR}" "${CACHE_DIR}" "${DATA_DIR}" "${STATE_DIR}"

if [ "$#" -eq 0 ]; then
    set -- "${VIVID_PRODUCER_LAUNCHER_NAME}"
fi

# flatpak build does not know the final application id, so it does not create
# the normal ~/.var/app/<appid>/config location that flatpak run would provide.
# Bind persistent test directories from .build and point the XDG locations at
# them; otherwise g_get_user_config_dir() falls back to ~/.config, which is
# read-only because the test sandbox exposes the host filesystem as host:ro.
exec flatpak build \
    --share=ipc \
    --share=network \
    --socket=wayland \
    --socket=pulseaudio \
    --device=dri \
    --filesystem=host:ro \
    --filesystem=xdg-run/vivid:create \
    --bind-mount="${VIVID_FLATPAK_CONFIG_MOUNT}=${CONFIG_DIR}" \
    --bind-mount="${VIVID_FLATPAK_CACHE_MOUNT}=${CACHE_DIR}" \
    --bind-mount="${VIVID_FLATPAK_DATA_MOUNT}=${DATA_DIR}" \
    --bind-mount="${VIVID_FLATPAK_STATE_MOUNT}=${STATE_DIR}" \
    --env=XDG_CONFIG_HOME="${VIVID_FLATPAK_CONFIG_MOUNT}" \
    --env=XDG_CACHE_HOME="${VIVID_FLATPAK_CACHE_MOUNT}" \
    --env=XDG_DATA_HOME="${VIVID_FLATPAK_DATA_MOUNT}" \
    --env=XDG_STATE_HOME="${VIVID_FLATPAK_STATE_MOUNT}" \
    --env=VIVID_PRODUCER_ONLY="${VIVID_PRODUCER_ONLY:-0}" \
    --env=VIVID_DISPLAY_SOCKET="${VIVID_DISPLAY_SOCKET:-}" \
    --env=GI_TYPELIB_PATH="${VIVID_FLATPAK_GI_TYPELIB_PATH}" \
    --env=LD_LIBRARY_PATH="${VIVID_FLATPAK_LD_LIBRARY_PATH}" \
    "${APP_DIR}" \
    "$@"
