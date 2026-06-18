#!/bin/sh

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
ROOT_DIR="$(CDPATH= cd -- "${SCRIPT_DIR}/../../producer" && pwd)"
. "${SCRIPT_DIR}/build_env.sh"

CMAKE_BIN="${CMAKE:-cmake}"
BUILD_JOBS="${VIVID_PRODUCER_BUILD_JOBS:-${FLATPAK_BUILDER_N_JOBS:-2}}"
DAEMON_BUILD_DIR="${VIVID_FLATPAK_DAEMON_BUILD_DIR}"
METAINFO_SOURCE="${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.metainfo.xml"
METAINFO_RENDERED="${DAEMON_BUILD_DIR}/${VIVID_PRODUCER_APP_ID}.metainfo.xml"

validate_flatpak_app_version() {
    case "${VIVID_FLATPAK_APP_VERSION}" in
        ''|*[!0-9A-Za-z._+~-]*)
            echo "Invalid VIVID_FLATPAK_APP_VERSION=${VIVID_FLATPAK_APP_VERSION}; expected only 0-9, A-Z, a-z, '.', '_', '+', '~', or '-'." >&2
            exit 1
            ;;
    esac

    case "${VIVID_FLATPAK_RELEASE_DATE}" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9])
            ;;
        *)
            echo "Invalid VIVID_FLATPAK_RELEASE_DATE=${VIVID_FLATPAK_RELEASE_DATE}; expected YYYY-MM-DD." >&2
            exit 1
            ;;
    esac
}

render_metainfo() {
    mkdir -p "$(dirname "${METAINFO_RENDERED}")"

    # Flatpak itself has no single manifest field for the user-visible
    # application version. Software centers read it from the AppStream release
    # entry, so render the checked-in metainfo into the build directory with the
    # requested release attributes instead of mutating the source XML.
    python3 - "${METAINFO_SOURCE}" "${METAINFO_RENDERED}" "${VIVID_FLATPAK_APP_VERSION}" "${VIVID_FLATPAK_RELEASE_DATE}" <<'PY'
import pathlib
import sys
import xml.etree.ElementTree as ET

source = pathlib.Path(sys.argv[1])
output = pathlib.Path(sys.argv[2])
version = sys.argv[3]
release_date = sys.argv[4]

tree = ET.parse(source)
root = tree.getroot()

releases = root.find("releases")
if releases is None:
    releases = ET.SubElement(root, "releases")

release = releases.find("release")
if release is None:
    release = ET.Element("release")
    releases.insert(0, release)

release.set("version", version)
release.set("date", release_date)

description = release.find("description")
if description is None:
    description = ET.SubElement(release, "description")
if description.find("p") is None:
    paragraph = ET.SubElement(description, "p")
    paragraph.text = "Flatpak package release."

ET.indent(tree, space="  ")
tree.write(output, encoding="UTF-8", xml_declaration=True)
PY
}

"${CMAKE_BIN}" -S "${VIVID_PRODUCER_DAEMON_DIR}" \
    -B "${DAEMON_BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${VIVID_CMAKE_BUILD_TYPE}" \
    -DCMAKE_INSTALL_PREFIX="${VIVID_INSTALL_PREFIX}"
"${CMAKE_BIN}" --build "${DAEMON_BUILD_DIR}" \
    --parallel "${BUILD_JOBS}" \
    --verbose \
    --target vivid-producer

"${CMAKE_BIN}" --install "${DAEMON_BUILD_DIR}" --component producer_daemon
install -Dm755 "${ROOT_DIR}/packaging/flatpak/vivid-launcher" "${VIVID_INSTALL_BIN_DIR}/${VIVID_PRODUCER_LAUNCHER_NAME}"
install -Dm644 "${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.desktop" "${VIVID_INSTALL_APPLICATIONS_DIR}/${VIVID_PRODUCER_APP_ID}.desktop"
validate_flatpak_app_version
render_metainfo
install -Dm644 "${METAINFO_RENDERED}" "${VIVID_INSTALL_METAINFO_DIR}/${VIVID_PRODUCER_APP_ID}.metainfo.xml"
install -Dm644 "${ROOT_DIR}/resources/${VIVID_PRODUCER_APP_ID}.svg" "${VIVID_INSTALL_ICON_DIR}/${VIVID_PRODUCER_APP_ID}.svg"
