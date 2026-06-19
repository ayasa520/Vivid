#!/bin/sh

# Central path configuration for the KDE Plasma consumer. It mirrors the GNOME
# consumer layout by keeping generated files inside this consumer directory's
# own .build tree, while still separating intermediate files from dist output.

: "${ROOT_DIR:?ROOT_DIR must be set before sourcing tools/consumer_kde/build_env.sh}"

VIVID_KDE_ROOT_DIR="${VIVID_KDE_ROOT_DIR:-${ROOT_DIR}}"
VIVID_KDE_BUILD_ROOT="${VIVID_KDE_BUILD_ROOT:-${VIVID_KDE_ROOT_DIR}/.build}"
VIVID_KDE_BUILD_DIR="${VIVID_KDE_BUILD_DIR:-${VIVID_KDE_BUILD_ROOT}/build}"
VIVID_KDE_STAGING_DIR="${VIVID_KDE_STAGING_DIR:-${VIVID_KDE_BUILD_ROOT}/staging}"
VIVID_KDE_DIST_DIR="${VIVID_KDE_DIST_DIR:-${VIVID_KDE_BUILD_ROOT}/dist}"
VIVID_KDE_PACKAGE_ABI="${VIVID_KDE_PACKAGE_ABI:-$(uname -m)}"
VIVID_CONSUMER_PACKAGE_VERSION="${VIVID_CONSUMER_PACKAGE_VERSION:-1.0.0}"
VIVID_KDE_PACKAGE_VERSION="${VIVID_KDE_PACKAGE_VERSION:-${VIVID_CONSUMER_PACKAGE_VERSION}}"
VIVID_KDE_PACKAGE_ID="${VIVID_KDE_PACKAGE_ID:-dev.rikka.vivid.consumer.kde}"
VIVID_KDE_QML_URI="${VIVID_KDE_QML_URI:-Vivid.DisplayEmbed}"

case "${VIVID_KDE_PACKAGE_VERSION}" in
    ""|*[!0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._+~-]*)
        echo "Invalid VIVID_KDE_PACKAGE_VERSION=${VIVID_KDE_PACKAGE_VERSION}; expected only 0-9, A-Z, a-z, '.', '_', '+', '~', or '-'." >&2
        exit 1
        ;;
esac

VIVID_KDE_PACKAGE_VERSION_MAJOR="${VIVID_KDE_PACKAGE_VERSION%%.*}"
case "${VIVID_KDE_PACKAGE_VERSION_MAJOR}" in
    ""|*[!0123456789]*)
        echo "Invalid VIVID_KDE_PACKAGE_VERSION=${VIVID_KDE_PACKAGE_VERSION}; expected a numeric major version before the first '.'." >&2
        exit 1
        ;;
esac
