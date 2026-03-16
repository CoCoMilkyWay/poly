#!/bin/bash
set -e

# Polygon Snap Node - Snapshot 下载脚本
# Bor 请自行安装: sudo dpkg -i bor-v2.7.0-beta2-amd64.deb bor-pbss-mainnet-sentry-config_v2.7.0-beta2-all.deb

DATA_DIR="$HOME/polygon-node/data"

echo "=========================================="
echo "Polygon Snapshot Download"
echo "=========================================="

# 检查磁盘空间
available=$(df -BG "$HOME" | awk 'NR==2 {print $4}' | tr -d 'G')
echo "Available disk: ${available}GB (need ~1.5TB)"

if [ "$available" -lt 1500 ]; then
    echo "WARNING: Low disk space!"
    read -p "Continue? (y/n) " -n 1 -r
    echo
    [[ ! $REPLY =~ ^[Yy]$ ]] && exit 1
fi

# 安装下载工具
sudo apt update && sudo apt install -y aria2 zstd

# 下载 snapshot
mkdir -p "$DATA_DIR"
cd "$(dirname "$DATA_DIR")"

if [ -d "$DATA_DIR/bor/chaindata" ] && [ "$(ls -A $DATA_DIR/bor/chaindata 2>/dev/null)" ]; then
    echo "Chain data exists. Skip download? (y/n)"
    read -n 1 -r
    echo
    [[ $REPLY =~ ^[Yy]$ ]] && exit 0
fi

echo "Downloading snapshot (~1TB, takes several hours)..."
wget -q "https://snapshot-download.polygon.technology/snapdown.sh" -O snapdown.sh
chmod +x snapdown.sh

# PBSS snapshot (更小更快)
./snapdown.sh --network mainnet --client bor --extract-dir "$DATA_DIR"

echo ""
echo "Done! Start bor with: sudo systemctl start bor"
echo "Data dir: $DATA_DIR"
