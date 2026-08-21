#!/bin/sh

# Path configuration for the catch-all layer-shell consumer.
# Mirrors gnome/kde: generated files stay under this consumer's .build tree.

: "${ROOT_DIR:?ROOT_DIR must be set before sourcing tools/consumer_layer_shell/build_env.sh}"

VIVID_LAYER_SHELL_ROOT_DIR="${VIVID_LAYER_SHELL_ROOT_DIR:-${ROOT_DIR}}"
VIVID_LAYER_SHELL_BUILD_ROOT="${VIVID_LAYER_SHELL_BUILD_ROOT:-${VIVID_LAYER_SHELL_ROOT_DIR}/.build}"
VIVID_LAYER_SHELL_BUILD_DIR="${VIVID_LAYER_SHELL_BUILD_DIR:-${VIVID_LAYER_SHELL_BUILD_ROOT}/build}"
VIVID_LAYER_SHELL_PACKAGE_VERSION="${VIVID_LAYER_SHELL_PACKAGE_VERSION:-${VIVID_CONSUMER_PACKAGE_VERSION:-1.0.0}}"
VIVID_LAYER_SHELL_PACKAGE_ABI="${VIVID_LAYER_SHELL_PACKAGE_ABI:-$(uname -m)}"
