#!/bin/bash
# MilanSQL - Hetzner Cloud Setup Script
# Usage: curl -sSL https://raw.githubusercontent.com/haidari9819-lang/milansql/main/deploy/hetzner/setup.sh | bash
set -euo pipefail

###############################################################################
# Configuration
###############################################################################
MILANSQL_VERSION="${MILANSQL_VERSION:-latest}"
MILANSQL_PORT="${MILANSQL_PORT:-5433}"
MILANSQL_DATA_DIR="${MILANSQL_DATA_DIR:-/var/lib/milansql}"
MILANSQL_LOG_DIR="${MILANSQL_LOG_DIR:-/var/log/milansql}"
MILANSQL_USER="${MILANSQL_USER:-milansql}"
MILANSQL_GROUP="${MILANSQL_GROUP:-milansql}"
DOCKER_COMPOSE_VERSION="2.27.0"

###############################################################################
# Colors
###############################################################################
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log()     { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; exit 1; }
heading() { echo -e "\n${BLUE}====> $* ${NC}"; }

###############################################################################
# Preflight checks
###############################################################################
heading "Preflight checks"

[[ $EUID -eq 0 ]] || error "This script must be run as root. Use: sudo bash setup.sh"

. /etc/os-release
log "Detected OS: $PRETTY_NAME"

case "$ID" in
  ubuntu|debian) PKG_MGR="apt-get" ;;
  fedora|centos|rhel|rocky|almalinux) PKG_MGR="dnf" ;;
  *) error "Unsupported OS: $ID. Supported: Ubuntu, Debian, Fedora, CentOS, RHEL, Rocky, AlmaLinux" ;;
esac

###############################################################################
# System update & dependencies
###############################################################################
heading "Updating system packages"

if [[ "$PKG_MGR" == "apt-get" ]]; then
  apt-get update -qq
  apt-get install -y -qq \
    curl wget gnupg2 ca-certificates lsb-release \
    apt-transport-https software-properties-common \
    ufw fail2ban git unzip jq
else
  dnf update -y -q
  dnf install -y -q \
    curl wget gnupg2 ca-certificates \
    firewalld fail2ban git unzip jq
fi

log "Dependencies installed."

###############################################################################
# Docker
###############################################################################
heading "Installing Docker"

if command -v docker &>/dev/null; then
  log "Docker already installed: $(docker --version)"
else
  curl -fsSL https://get.docker.com | sh
  systemctl enable --now docker
  log "Docker installed: $(docker --version)"
fi

# Docker Compose plugin
if ! docker compose version &>/dev/null; then
  log "Installing Docker Compose v${DOCKER_COMPOSE_VERSION}..."
  mkdir -p /usr/local/lib/docker/cli-plugins
  curl -SL "https://github.com/docker/compose/releases/download/v${DOCKER_COMPOSE_VERSION}/docker-compose-linux-x86_64" \
    -o /usr/local/lib/docker/cli-plugins/docker-compose
  chmod +x /usr/local/lib/docker/cli-plugins/docker-compose
fi
log "Docker Compose: $(docker compose version)"

###############################################################################
# System user
###############################################################################
heading "Creating system user"

if ! id "$MILANSQL_USER" &>/dev/null; then
  useradd --system --no-create-home --shell /usr/sbin/nologin \
    --comment "MilanSQL service account" "$MILANSQL_USER"
  log "User '$MILANSQL_USER' created."
else
  log "User '$MILANSQL_USER' already exists."
fi

usermod -aG docker "$MILANSQL_USER"

###############################################################################
# Directories
###############################################################################
heading "Setting up directories"

for dir in "$MILANSQL_DATA_DIR" "$MILANSQL_LOG_DIR" /etc/milansql; do
  mkdir -p "$dir"
  chown "${MILANSQL_USER}:${MILANSQL_GROUP}" "$dir"
  chmod 750 "$dir"
  log "Directory ready: $dir"
done

###############################################################################
# Firewall
###############################################################################
heading "Configuring firewall"

if [[ "$PKG_MGR" == "apt-get" ]]; then
  ufw --force reset
  ufw default deny incoming
  ufw default allow outgoing
  ufw allow 22/tcp   comment "SSH"
  ufw allow 80/tcp   comment "HTTP"
  ufw allow 443/tcp  comment "HTTPS"
  ufw allow "${MILANSQL_PORT}/tcp" comment "MilanSQL"
  ufw --force enable
  log "UFW configured."
else
  systemctl enable --now firewalld
  firewall-cmd --permanent --add-service=ssh
  firewall-cmd --permanent --add-service=http
  firewall-cmd --permanent --add-service=https
  firewall-cmd --permanent --add-port="${MILANSQL_PORT}/tcp"
  firewall-cmd --reload
  log "firewalld configured."
fi

###############################################################################
# Fail2ban
###############################################################################
heading "Configuring fail2ban"

cat > /etc/fail2ban/jail.local <<'EOF'
[DEFAULT]
bantime  = 1h
findtime = 10m
maxretry = 5

[sshd]
enabled = true
port    = ssh
logpath = %(sshd_log)s
backend = %(syslog_backend)s
EOF

systemctl enable --now fail2ban
log "fail2ban configured."

###############################################################################
# MilanSQL docker-compose
###############################################################################
heading "Writing docker-compose for MilanSQL"

cat > /etc/milansql/docker-compose.yml <<EOF
version: "3.9"

services:
  milansql:
    image: ghcr.io/haidari9819-lang/milansql:${MILANSQL_VERSION}
    container_name: milansql
    restart: unless-stopped
    user: "${MILANSQL_USER}"
    ports:
      - "${MILANSQL_PORT}:5433"
    volumes:
      - ${MILANSQL_DATA_DIR}:/var/lib/milansql
      - ${MILANSQL_LOG_DIR}:/var/log/milansql
    environment:
      MILANSQL_PORT: "5433"
      MILANSQL_LOG_LEVEL: "info"
    healthcheck:
      test: ["CMD", "milansql", "--ping"]
      interval: 30s
      timeout: 10s
      retries: 3
      start_period: 15s

volumes:
  milansql_data:
    driver: local
EOF

log "docker-compose.yml written to /etc/milansql/"

###############################################################################
# Systemd service
###############################################################################
heading "Creating systemd service"

cat > /etc/systemd/system/milansql.service <<EOF
[Unit]
Description=MilanSQL Database Server
Documentation=https://github.com/haidari9819-lang/milansql
After=docker.service network-online.target
Requires=docker.service

[Service]
Type=simple
User=${MILANSQL_USER}
Group=${MILANSQL_GROUP}
WorkingDirectory=/etc/milansql
ExecStartPre=/usr/bin/docker compose -f /etc/milansql/docker-compose.yml pull
ExecStart=/usr/bin/docker compose -f /etc/milansql/docker-compose.yml up
ExecStop=/usr/bin/docker compose -f /etc/milansql/docker-compose.yml down
Restart=on-failure
RestartSec=10s
StandardOutput=journal
StandardError=journal
SyslogIdentifier=milansql

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable milansql
log "Systemd service 'milansql' enabled."

###############################################################################
# Done
###############################################################################
heading "Setup complete"

cat <<EOF
${GREEN}MilanSQL has been set up successfully on this Hetzner server.${NC}

  Data directory : ${MILANSQL_DATA_DIR}
  Log directory  : ${MILANSQL_LOG_DIR}
  Config         : /etc/milansql/docker-compose.yml
  Service        : systemctl start milansql
  Port           : ${MILANSQL_PORT}

Next steps:
  1. Start the service : systemctl start milansql
  2. Check status      : systemctl status milansql
  3. Follow logs       : journalctl -u milansql -f
  4. Connect           : milansql-cli --host 127.0.0.1 --port ${MILANSQL_PORT}

GitHub : https://github.com/haidari9819-lang/milansql
EOF
