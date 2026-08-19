#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
sudo mkdir -p /var/cloudstore/data /var/log/cloudstore
sudo chown -R "$USER":"$USER" /var/cloudstore /var/log/cloudstore 2>/dev/null || true
mysql -uroot -p < sql/init.sql || mysql -ucloud -pcloud123 < sql/init.sql
echo "starting server..."
exec ./build/cloud_server -c conf/server.conf
