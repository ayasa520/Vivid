#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

usage() {
    cat <<'EOF'
Usage:
  tools/vivid.sh build {direct-run|gnome|kde|flatpak|all}
  tools/vivid.sh clean {direct-run|gnome|kde|flatpak|producer|consumer|all}
  tools/vivid.sh completion bash

  tools/vivid.sh direct-run {build|clean}
  tools/vivid.sh direct-run run
  tools/vivid.sh direct-run run-producer [producer-args...]
  tools/vivid.sh direct-run run-webui [webui-args...]

  tools/vivid.sh gnome {build|clean|install|zip|enable|disable|reset|uninstall|log}
  tools/vivid.sh kde {build|clean|install|zip|uninstall|log}

  tools/vivid.sh flatpak prefetch
  tools/vivid.sh flatpak {build|clean}
  tools/vivid.sh flatpak run-appdir

Aliases:
  tools/vivid.sh consumer gnome ...
  tools/vivid.sh consumer kde ...
  tools/vivid.sh producer build-direct-run
  tools/vivid.sh producer run-direct-run
  tools/vivid.sh producer prefetch
  tools/vivid.sh producer build-flatpak
EOF
}

die_usage() {
    echo "Error: $*" >&2
    echo >&2
    usage >&2
    exit 2
}

print_bash_completion() {
    cat <<'EOF'
_vivid_sh_completion() {
    local cur
    cur="${COMP_WORDS[COMP_CWORD]}"
    COMPREPLY=()

    local top_commands="build clean direct-run gnome consumer-gnome kde consumer-kde consumer flatpak producer completion help -h --help"
    local build_targets="direct-run producer gnome consumer-gnome kde consumer-kde flatpak all"
    local clean_targets="direct-run gnome consumer-gnome kde consumer-kde flatpak producer consumer all"
    local direct_run_actions="build clean run run-producer run-webui"
    local gnome_actions="build clean install zip enable disable reset uninstall log"
    local kde_actions="build clean install zip uninstall log"
    local flatpak_actions="prefetch build clean run-appdir"
    local producer_actions="build-direct-run run-direct-run run-direct-run-producer run-direct-run-webui prefetch build-flatpak run-flatpak-appdir clean-direct-run clean-flatpak clean"

    _vivid_complete_words() {
        COMPREPLY=( $(compgen -W "$1" -- "${cur}") )
    }

    case "${COMP_CWORD}" in
        1)
            _vivid_complete_words "${top_commands}"
            return 0
            ;;
    esac

    case "${COMP_WORDS[1]}" in
        build)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${build_targets}"
            fi
            ;;
        clean)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${clean_targets}"
            fi
            ;;
        direct-run)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${direct_run_actions}"
            elif [[ "${COMP_WORDS[2]}" == "run-producer" || "${COMP_WORDS[2]}" == "run-webui" ]]; then
                COMPREPLY=( $(compgen -f -- "${cur}") )
            fi
            ;;
        gnome|consumer-gnome)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${gnome_actions}"
            fi
            ;;
        kde|consumer-kde)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${kde_actions}"
            fi
            ;;
        consumer)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "gnome kde"
            elif [[ "${COMP_CWORD}" -eq 3 ]]; then
                case "${COMP_WORDS[2]}" in
                    gnome)
                        _vivid_complete_words "${gnome_actions}"
                        ;;
                    kde)
                        _vivid_complete_words "${kde_actions}"
                        ;;
                esac
            fi
            ;;
        flatpak)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${flatpak_actions}"
            fi
            ;;
        producer)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "${producer_actions}"
            elif [[ "${COMP_WORDS[2]}" == "run-direct-run-producer" || "${COMP_WORDS[2]}" == "run-direct-run-webui" ]]; then
                COMPREPLY=( $(compgen -f -- "${cur}") )
            fi
            ;;
        completion)
            if [[ "${COMP_CWORD}" -eq 2 ]]; then
                _vivid_complete_words "bash"
            fi
            ;;
    esac

    return 0
}

complete -F _vivid_sh_completion vivid vivid.sh tools/vivid.sh ./tools/vivid.sh
EOF
}

clean_path() {
    local label="$1"
    local path="$2"
    local absolute

    absolute="$(realpath -m "${path}")"

    # This command intentionally performs rm -rf when the user asks for a clean
    # target, so keep the deletion boundary narrow and explicit. Build outputs
    # must live under this checkout and either be a .build directory or be inside
    # one. That protects source files, submodules, and downloaded offline source
    # archives from a bad environment override.
    if [[ "${absolute}" != "${REPO_ROOT}"/* ]]; then
        echo "Refusing to remove ${label}: outside repository: ${absolute}" >&2
        exit 1
    fi
    case "${absolute}" in
        */.build|*/.build/*)
            ;;
        *)
            echo "Refusing to remove ${label}: not a .build path: ${absolute}" >&2
            exit 1
            ;;
    esac

    if [[ ! -e "${absolute}" && ! -L "${absolute}" ]]; then
        echo "Skip missing ${label}: ${absolute}"
        return
    fi

    echo "Removing ${label}: ${absolute}"
    rm -rf -- "${absolute}"
}

clean_gnome_consumer() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../consumer/gnome" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/consumer_gnome/build_env.sh"

    clean_path "GNOME consumer build cache" "${VIVID_CONSUMER_BUILD_ROOT}"
}

clean_kde_consumer() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../consumer/kde" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/consumer_kde/build_env.sh"

    clean_path "KDE consumer build cache" "${VIVID_KDE_BUILD_ROOT}"
}

clean_direct_run() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../producer" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/producer/build_env.sh"

    clean_path "producer direct-run build cache" "${VIVID_DIRECT_RUN_BUILD_DIR}"
}

clean_flatpak() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../producer" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/producer/build_env.sh"

    clean_path "Flatpak download-only work dir" "${VIVID_FLATPAK_DOWNLOAD_DIR}"
    clean_path "Flatpak app build cache" "${VIVID_FLATPAK_BUILD_DIR}"
    clean_path "Flatpak repository cache" "${VIVID_FLATPAK_REPO_DIR}"
    clean_path "Flatpak builder state cache" "${VIVID_FLATPAK_STATE_DIR}"
    clean_path "Flatpak run appdir cache" "${VIVID_FLATPAK_RUN_DIR}"
}

clean_producer() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../producer" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/producer/build_env.sh"

    clean_path "producer build cache" "${VIVID_BUILD_ROOT}"
}

run_clean() {
    local target="${1:-}"
    if [[ -z "${target}" ]]; then
        die_usage "missing clean target"
    fi
    shift
    if [[ $# -ne 0 ]]; then
        die_usage "unexpected clean arguments: $*"
    fi

    case "${target}" in
        direct-run)
            clean_direct_run
            ;;
        gnome|consumer-gnome)
            clean_gnome_consumer
            ;;
        kde|consumer-kde)
            clean_kde_consumer
            ;;
        flatpak)
            clean_flatpak
            ;;
        producer)
            clean_producer
            ;;
        consumer)
            clean_gnome_consumer
            clean_kde_consumer
            ;;
        all)
            clean_producer
            clean_gnome_consumer
            clean_kde_consumer
            ;;
        *)
            die_usage "unknown clean target: ${target}"
            ;;
    esac
}

build_gnome_consumer() {
    local root_dir
    root_dir="$(cd "${SCRIPT_DIR}/../consumer/gnome" && pwd)"

    ROOT_DIR="${root_dir}"
    . "${SCRIPT_DIR}/consumer_gnome/build_env.sh"

    if [[ -f "${VIVID_CONSUMER_BUILD_DIR}/build.ninja" ]]; then
        meson setup --reconfigure "${VIVID_CONSUMER_BUILD_DIR}" "${root_dir}" \
            -Dpackage-version="${VIVID_CONSUMER_PACKAGE_VERSION}" \
            "$@"
    else
        meson setup "${VIVID_CONSUMER_BUILD_DIR}" "${root_dir}" \
            --prefix="${VIVID_CONSUMER_INSTALL_PREFIX}" \
            -Dpackage-version="${VIVID_CONSUMER_PACKAGE_VERSION}" \
            "$@"
    fi
    ninja -C "${VIVID_CONSUMER_BUILD_DIR}"
}

run_direct_run() {
    local action="${1:-}"
    if [[ -z "${action}" ]]; then
        die_usage "missing direct-run action"
    fi
    shift

    case "${action}" in
        build)
            "${SCRIPT_DIR}/producer/build_direct_run.sh" "$@"
            ;;
        clean)
            if [[ $# -ne 0 ]]; then
                die_usage "unexpected direct-run clean arguments: $*"
            fi
            clean_direct_run
            ;;
        run)
            "${SCRIPT_DIR}/producer/run_direct_run_all.sh" "$@"
            ;;
        run-producer)
            "${SCRIPT_DIR}/producer/run_direct_run_producer.sh" "$@"
            ;;
        run-webui)
            "${SCRIPT_DIR}/producer/run_direct_run_webui.sh" "$@"
            ;;
        *)
            die_usage "unknown direct-run action: ${action}"
            ;;
    esac
}

run_gnome() {
    local action="${1:-}"
    if [[ -z "${action}" ]]; then
        die_usage "missing GNOME consumer action"
    fi
    shift

    case "${action}" in
        build)
            build_gnome_consumer "$@"
            ;;
        clean)
            if [[ $# -ne 0 ]]; then
                die_usage "unexpected GNOME clean arguments: $*"
            fi
            clean_gnome_consumer
            ;;
        install|zip|enable|disable|reset|uninstall|log)
            "${SCRIPT_DIR}/consumer_gnome/run.sh" "${action}" "$@"
            ;;
        *)
            die_usage "unknown GNOME consumer action: ${action}"
            ;;
    esac
}

run_build_group() {
    local target="${1:-}"
    if [[ -z "${target}" ]]; then
        die_usage "missing build target"
    fi
    shift

    case "${target}" in
        direct-run|producer)
            run_direct_run build "$@"
            ;;
        gnome|consumer-gnome)
            build_gnome_consumer "$@"
            ;;
        kde|consumer-kde)
            run_kde build "$@"
            ;;
        flatpak)
            run_flatpak build "$@"
            ;;
        all)
            run_direct_run build "$@"
            build_gnome_consumer
            run_kde build
            ;;
        *)
            die_usage "unknown build target: ${target}"
            ;;
    esac
}

run_kde() {
    local action="${1:-}"
    if [[ -z "${action}" ]]; then
        die_usage "missing KDE consumer action"
    fi

    if [[ "${action}" = "clean" ]]; then
        shift
        if [[ $# -ne 0 ]]; then
            die_usage "unexpected KDE clean arguments: $*"
        fi
        clean_kde_consumer
        return
    fi

    "${SCRIPT_DIR}/consumer_kde/run.sh" "$@"
}

run_flatpak() {
    local action="${1:-}"
    if [[ -z "${action}" ]]; then
        die_usage "missing Flatpak action"
    fi
    shift

    case "${action}" in
        prefetch)
            "${SCRIPT_DIR}/producer/flatpak/prefetch_sources.sh" "$@"
            ;;
        build)
            "${SCRIPT_DIR}/producer/flatpak/build.sh" "$@"
            ;;
        clean)
            if [[ $# -ne 0 ]]; then
                die_usage "unexpected Flatpak clean arguments: $*"
            fi
            clean_flatpak
            ;;
        run-appdir)
            "${SCRIPT_DIR}/producer/flatpak/run_appdir.sh" "$@"
            ;;
        *)
            die_usage "unknown Flatpak action: ${action}"
            ;;
    esac
}

run_producer_alias() {
    local action="${1:-}"
    if [[ -z "${action}" ]]; then
        die_usage "missing producer action"
    fi
    shift

    case "${action}" in
        build-direct-run)
            run_direct_run build "$@"
            ;;
        run-direct-run)
            run_direct_run run "$@"
            ;;
        run-direct-run-producer)
            run_direct_run run-producer "$@"
            ;;
        run-direct-run-webui)
            run_direct_run run-webui "$@"
            ;;
        prefetch)
            run_flatpak prefetch "$@"
            ;;
        build-flatpak)
            run_flatpak build "$@"
            ;;
        clean-direct-run)
            run_direct_run clean "$@"
            ;;
        clean-flatpak)
            run_flatpak clean "$@"
            ;;
        clean)
            if [[ $# -ne 0 ]]; then
                die_usage "unexpected producer clean arguments: $*"
            fi
            clean_producer
            ;;
        run-flatpak-appdir)
            run_flatpak run-appdir "$@"
            ;;
        *)
            die_usage "unknown producer action: ${action}"
            ;;
    esac
}

case "${1:-help}" in
    build)
        shift
        run_build_group "$@"
        ;;
    clean)
        shift
        run_clean "$@"
        ;;
    direct-run)
        shift
        run_direct_run "$@"
        ;;
    gnome|consumer-gnome)
        shift
        run_gnome "$@"
        ;;
    kde|consumer-kde)
        shift
        run_kde "$@"
        ;;
    consumer)
        shift
        target="${1:-}"
        if [[ -z "${target}" ]]; then
            die_usage "missing consumer target"
        fi
        shift
        case "${target}" in
            gnome)
                run_gnome "$@"
                ;;
            kde)
                run_kde "$@"
                ;;
            *)
                die_usage "unknown consumer target: ${target}"
                ;;
        esac
        ;;
    flatpak)
        shift
        run_flatpak "$@"
        ;;
    producer)
        shift
        run_producer_alias "$@"
        ;;
    completion)
        shift
        case "${1:-}" in
            bash)
                print_bash_completion
                ;;
            *)
                die_usage "unknown completion shell: ${1:-}"
                ;;
        esac
        ;;
    help|-h|--help)
        usage
        ;;
    *)
        die_usage "unknown command: ${1}"
        ;;
esac
