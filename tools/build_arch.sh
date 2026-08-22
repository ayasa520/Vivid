#!/bin/sh

# Normalize architecture names once so package filenames, Flatpak architecture
# selection, and architecture-specific third-party downloads all use the same
# public ABI label.
vivid_normalize_build_arch() {
    case "$1" in
        x86_64|amd64)
            printf '%s\n' x86_64
            ;;
        aarch64|arm64)
            printf '%s\n' aarch64
            ;;
        *)
            echo "Unsupported build architecture: $1" >&2
            return 1
            ;;
    esac
}
