#!/bin/bash
set -e

# DeepLux Linux Deployment Script
# Uses linuxdeployqt to bundle Qt dependencies into a portable package

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build"
DEPLOY_DIR="${PROJECT_DIR}/deploy"
APPDIR="${DEPLOY_DIR}/DeepLux"

echo "=== DeepLux Linux Deploy ==="
echo "Project: ${PROJECT_DIR}"
echo "Build:   ${BUILD_DIR}"

# Check build exists
if [ ! -f "${BUILD_DIR}/bin/DeepLux" ]; then
    echo "ERROR: DeepLux executable not found at ${BUILD_DIR}/bin/DeepLux"
    echo "Please build first: cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

# Check linuxdeployqt
LINUXDEPLOYQT="${DEPLOY_DIR}/linuxdeployqt"
if [ ! -x "${LINUXDEPLOYQT}" ]; then
    echo "Downloading linuxdeployqt..."
    mkdir -p "${DEPLOY_DIR}"
    wget -q -O "${LINUXDEPLOYQT}" \
        "https://github.com/probonopd/linuxdeployqt/releases/download/continuous/linuxdeployqt-continuous-x86_64.AppImage" \
        || { echo "ERROR: Failed to download linuxdeployqt"; exit 1; }
    chmod +x "${LINUXDEPLOYQT}"
    echo "linuxdeployqt downloaded."
fi

# Clean previous deploy
rm -rf "${APPDIR}"
mkdir -p "${APPDIR}/bin"
mkdir -p "${APPDIR}/lib"

# Copy main executable
cp "${BUILD_DIR}/bin/DeepLux" "${APPDIR}/bin/"

# Copy internal shared libraries
cp "${BUILD_DIR}/lib/libdeeplux_core.so"* "${APPDIR}/lib/" 2>/dev/null || true
cp "${BUILD_DIR}/lib/libdeeplux_ui.so"* "${APPDIR}/lib/" 2>/dev/null || true

# Copy plugins
echo "Copying plugins..."
mkdir -p "${APPDIR}/plugins"
for plugin in "${BUILD_DIR}/lib/"*Plugin.so; do
    [ -f "$plugin" ] || continue
    cp "$plugin" "${APPDIR}/plugins/"
done

# Run linuxdeployqt to bundle Qt dependencies
# Note: unset LD_LIBRARY_PATH to avoid picking up wrong Qt
echo "Running linuxdeployqt..."
unset LD_LIBRARY_PATH 2>/dev/null || true
"${LINUXDEPLOYQT}" "${APPDIR}/bin/DeepLux" \
    -appimage \
    -verbose=1 \
    -always-overwrite \
    -qmldir="${PROJECT_DIR}/src/ui" 2>/dev/null || {
    echo "WARNING: AppImage generation failed, falling back to tarball only"
    "${LINUXDEPLOYQT}" "${APPDIR}/bin/DeepLux" \
        -bundle-non-qt-libs \
        -verbose=1 \
        -always-overwrite 2>/dev/null || true
}

# If AppImage was created, move it
cd "${DEPLOY_DIR}"
APPIMAGE=$(ls DeepLux-x86_64.AppImage 2>/dev/null || true)
if [ -n "${APPIMAGE}" ]; then
    mv "${APPIMAGE}" "deeplux-linux-x86_64.AppImage"
    echo "AppImage created: ${DEPLOY_DIR}/deeplux-linux-x86_64.AppImage"
fi

# Always create tarball as fallback/primary
TARBALL="deeplux-linux-x86_64.tar.gz"
echo "Creating tarball: ${TARBALL}"
tar czf "${TARBALL}" -C "${DEPLOY_DIR}" DeepLux

echo ""
echo "=== Deploy Complete ==="
echo "Tarball: ${DEPLOY_DIR}/${TARBALL}"
[ -f "${DEPLOY_DIR}/deeplux-linux-x86_64.AppImage" ] && \
    echo "AppImage: ${DEPLOY_DIR}/deeplux-linux-x86_64.AppImage"
echo ""
echo "To run on target machine:"
echo "  tar xf ${TARBALL}"
echo "  ./DeepLux/bin/DeepLux"
