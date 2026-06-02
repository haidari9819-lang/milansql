#!/bin/bash
# ============================================================
# build-deb.sh — Build MilanSQL Debian/Ubuntu Package (.deb)
# Usage: ./packaging/build-deb.sh
# Requires: dpkg-deb, cmake, g++
# ============================================================
set -e

VERSION="5.7.0"
ARCH="amd64"
PKG="milansql_${VERSION}_${ARCH}"
DIST_DIR="dist/${PKG}"

echo "=== MilanSQL Debian Package Builder v${VERSION} ==="

# ── Step 1: Build binary ──────────────────────────────────────
echo "[1/4] Building milansql binary..."
cmake -B build -DCMAKE_BUILD_TYPE=Release -G Ninja
cmake --build build --parallel

# ── Step 2: Create package structure ─────────────────────────
echo "[2/4] Creating package structure..."
rm -rf "dist/${PKG}"
mkdir -p "${DIST_DIR}/DEBIAN"
mkdir -p "${DIST_DIR}/usr/bin"
mkdir -p "${DIST_DIR}/lib/systemd/system"
mkdir -p "${DIST_DIR}/usr/share/doc/milansql"
mkdir -p "${DIST_DIR}/usr/share/man/man1"

# ── Step 3: Copy files ────────────────────────────────────────
echo "[3/4] Copying files..."
cp build/milansql "${DIST_DIR}/usr/bin/"
strip "${DIST_DIR}/usr/bin/milansql"

cp packaging/debian/control    "${DIST_DIR}/DEBIAN/"
cp packaging/debian/postinst   "${DIST_DIR}/DEBIAN/"
cp packaging/debian/prerm      "${DIST_DIR}/DEBIAN/"
cp packaging/debian/milansql.service "${DIST_DIR}/lib/systemd/system/"

cp README.md  "${DIST_DIR}/usr/share/doc/milansql/"
cp LICENSE    "${DIST_DIR}/usr/share/doc/milansql/"
cp INSTALL.md "${DIST_DIR}/usr/share/doc/milansql/"

# Compress docs
gzip -9 "${DIST_DIR}/usr/share/doc/milansql/README.md" 2>/dev/null || true

# Set correct permissions
chmod 755 "${DIST_DIR}/DEBIAN/postinst"
chmod 755 "${DIST_DIR}/DEBIAN/prerm"
chmod 755 "${DIST_DIR}/usr/bin/milansql"

# ── Step 4: Build .deb ────────────────────────────────────────
echo "[4/4] Building .deb package..."
dpkg-deb --build "dist/${PKG}"

echo ""
echo "=== Package built: dist/${PKG}.deb ==="
echo "Install with: sudo dpkg -i dist/${PKG}.deb"
echo "Or:           sudo apt install ./dist/${PKG}.deb"
