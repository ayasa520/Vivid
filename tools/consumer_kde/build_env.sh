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
VIVID_KDE_PACKAGE_ID="${VIVID_KDE_PACKAGE_ID:-dev.rikka.vivid.consumer.kde}"
VIVID_KDE_QML_URI="${VIVID_KDE_QML_URI:-Vivid.DisplayEmbed}"
