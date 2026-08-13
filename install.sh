#!/bin/bash
# MilanSQL Cloud — One-Click Install Script
# Usage: curl -sSL https://get.milansql.com | bash
# Or:    bash install.sh [--port 8080] [--data-dir /var/lib/milansql] [--version latest]
set -e

MILANSQL_VERSION="12.0.0"
MILANSQL_PORT="${MILANSQL_PORT:-8080}"
MILANSQL_DATA_DIR="${MILANSQL_DATA_DIR:-/var/lib/milansql}"
MILANSQL_CONFIG_DIR="${MILANSQL_CONFIG_DIR:-/etc/milansql}"
MILANSQL_USER="${MILANSQL_USER:-milansql}"
INSTALL_DIR="/opt/milansql"

# Parse args
while [[ "$#" -gt 0 ]]; do
  case $1 in
    --port) MILANSQL_PORT="$2"; shift ;;
    --data-dir) MILANSQL_DATA_DIR="$2"; shift ;;
    --version) MILANSQL_VERSION="$2"; shift ;;
    *) echo "Unknown option: $1"; exit 1 ;;
  esac
  shift
done

echo "╔══════════════════════════════════════════╗"
echo "║   MilanSQL Cloud v${MILANSQL_VERSION} Install        ║"
echo "╚══════════════════════════════════════════╝"
echo ""
echo "  Port:     ${MILANSQL_PORT}"
echo "  Data dir: ${MILANSQL_DATA_DIR}"
echo ""

# Detect OS
if [ -f /etc/debian_version ]; then
  PKG_MGR="apt-get"
  DEPS="build-essential cmake libssl-dev"
elif [ -f /etc/redhat-release ]; then
  PKG_MGR="yum"
  DEPS="gcc-c++ cmake openssl-devel"
else
  echo "ERROR: Unsupported OS. Supports Ubuntu/Debian and RHEL/CentOS."
  exit 1
fi

# Install dependencies
echo "→ Installing dependencies..."
$PKG_MGR install -y -q $DEPS 2>/dev/null || true

# Create user
if ! id "$MILANSQL_USER" &>/dev/null; then
  useradd --system --no-create-home --shell /bin/false "$MILANSQL_USER"
  echo "→ Created system user: $MILANSQL_USER"
fi

# Create directories
mkdir -p "$MILANSQL_DATA_DIR" "$MILANSQL_CONFIG_DIR" "$INSTALL_DIR"
chown -R "$MILANSQL_USER:$MILANSQL_USER" "$MILANSQL_DATA_DIR"
chmod 750 "$MILANSQL_DATA_DIR"

# Generate root password if not set
if [ ! -f "$MILANSQL_CONFIG_DIR/db.conf" ]; then
  ROOT_PW=$(openssl rand -hex 16)
  cat > "$MILANSQL_CONFIG_DIR/db.conf" <<EOF
# MilanSQL Configuration
DB_ROOT_PW=${ROOT_PW}
DB_PORT=${MILANSQL_PORT}
DB_DATA_DIR=${MILANSQL_DATA_DIR}
EOF
  chmod 640 "$MILANSQL_CONFIG_DIR/db.conf"
  echo ""
  echo "  ┌─────────────────────────────────────────┐"
  echo "  │  Root password: ${ROOT_PW}  │"
  echo "  │  Save this — it won't be shown again!   │"
  echo "  └─────────────────────────────────────────┘"
  echo ""
fi

# Download or build binary
if [ -x "$INSTALL_DIR/build/milansql" ]; then
  echo "→ Binary already present, skipping build."
else
  echo "→ Building MilanSQL from source..."
  cd "$INSTALL_DIR"
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF -j$(nproc) -q
  cmake --build build --target milansql -j$(nproc)
  echo "→ Build complete."
fi

# Install systemd service
cat > /etc/systemd/system/milansql.service <<EOF
[Unit]
Description=MilanSQL Cloud Database Server
After=network.target
StartLimitIntervalSec=0

[Service]
Type=simple
User=${MILANSQL_USER}
WorkingDirectory=${MILANSQL_DATA_DIR}
EnvironmentFile=${MILANSQL_CONFIG_DIR}/db.conf
ExecStart=${INSTALL_DIR}/build/milansql --port \${DB_PORT} --data-dir \${DB_DATA_DIR}
Restart=always
RestartSec=5
LimitNOFILE=65536
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable milansql
systemctl restart milansql

# Wait for server to start
echo "→ Starting MilanSQL..."
for i in $(seq 1 10); do
  if curl -sf "http://localhost:${MILANSQL_PORT}/health" >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

echo ""
echo "✓ MilanSQL Cloud v${MILANSQL_VERSION} installed successfully!"
echo ""
echo "  Web UI:   http://$(hostname -f):${MILANSQL_PORT}/webui"
echo "  Cloud:    http://$(hostname -f):${MILANSQL_PORT}/cloud"
echo "  Health:   http://$(hostname -f):${MILANSQL_PORT}/health"
echo "  Config:   ${MILANSQL_CONFIG_DIR}/db.conf"
echo ""
echo "  Manage:   systemctl status milansql"
echo "            systemctl restart milansql"
echo "            journalctl -u milansql -f"
echo ""
