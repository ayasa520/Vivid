#!/bin/sh

# CEF publishes architecture-specific binary distributions under the same
# Chromium revision. Keep the exact revision and upstream checksums together so
# native x86_64 and aarch64 release jobs cannot accidentally package different
# browser engines.
VIVID_CEF_VERSION='148.0.10+g7ee53f5+chromium-148.0.7778.218'
VIVID_CEF_DOWNLOAD_BASE_URL='https://cef-builds.spotifycdn.com'
VIVID_CEF_LINUX64_SHA1='8fe8e58d8dc5034050239500570fd4c50653ce34'
VIVID_CEF_LINUXARM64_SHA1='a00843f285d1a9c4a75f009f8289dc8a8cd1397f'
