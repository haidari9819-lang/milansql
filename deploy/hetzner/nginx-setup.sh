#!/bin/bash
# Nginx + SSL Setup für milansql.de
# Auf Hetzner Server ausführen: bash nginx-setup.sh

set -e

echo "=== MilanSQL Nginx + SSL Setup ==="

# 1. Nginx Config für milansql.de
cat > /etc/nginx/sites-available/milansql << 'NGINX'
server {
    listen 80;
    server_name milansql.de www.milansql.de;
    charset utf-8;

    location / {
        proxy_pass http://localhost:8080;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection 'upgrade';
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_cache_bypass $http_upgrade;
        proxy_read_timeout 86400;
    }
}
NGINX

# 2. Aktivieren
ln -sf /etc/nginx/sites-available/milansql \
       /etc/nginx/sites-enabled/milansql
rm -f /etc/nginx/sites-enabled/default

# 3. Test + Restart
nginx -t && systemctl restart nginx
echo "✓ Nginx konfiguriert"

# 4. SSL mit Certbot
apt-get install -y certbot python3-certbot-nginx
certbot --nginx \
  -d milansql.de \
  -d www.milansql.de \
  --non-interactive \
  --agree-tos \
  --email haidari9819@gmail.com \
  --redirect
echo "✓ SSL Zertifikat installiert"

# 5. Auto-Renewal
systemctl enable certbot.timer
echo "✓ Auto-Renewal aktiviert"

# 6. Test
echo ""
echo "=== Setup Complete ==="
echo "✓ https://milansql.de/webui"
curl -s https://milansql.de/status | grep version
