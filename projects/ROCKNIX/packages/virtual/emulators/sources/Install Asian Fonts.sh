#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

source /etc/profile

NOTO_SOURCE_URL="https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/03_NotoSansCJK-OTC.zip"
GITHUB_PROXY=""
_best=999
for _p in https://ghfast.top/ https://gh.ddlc.top/ https://gh-proxy.com/; do
  _t=$(curl -s -o /dev/null -w "%{time_total}" --connect-timeout 3 --max-time 10 "${_p}https://github.com" 2>/dev/null)
  if [ -n "$_t" ] && [ "$_t" != "0.000" ] && [ "$_t" != "0" ]; then
    _ti=${_t%%.*}
    if [ "$_ti" -lt "$_best" ]; then
      _best=$_ti
      GITHUB_PROXY="$_p"
    fi
  fi
done
NOTO_WORK_DIR="/tmp/noto-cjk-fonts"
NOTO_DOWNLOAD_NAME="NotoSansCJK-OTC.zip"
NOTO_DOWNLOAD_DIR="NotoSansCJK-OTC"
NOTO_INSTALL_SOURCE="NotoSansCJK-Regular.ttc"
NOTO_INSTALL_DIR="/storage/.local/share/fonts"

mkdir -p "${NOTO_WORK_DIR}"
cd "${NOTO_WORK_DIR}"

wget -c -t 5 -O "${NOTO_DOWNLOAD_NAME}" "${GITHUB_PROXY}${NOTO_SOURCE_URL}" || \
  wget -c -t 5 -O "${NOTO_DOWNLOAD_NAME}" "${NOTO_SOURCE_URL}" || {
    echo "Failed to download Noto CJK fonts." >&2
    exit 1
  }

mkdir -p "${NOTO_DOWNLOAD_DIR}"
unzip -o "${NOTO_DOWNLOAD_NAME}" -d "${NOTO_DOWNLOAD_DIR}" || {
  echo "Failed to extract Noto CJK fonts." >&2
  exit 1
}

mkdir -p "${NOTO_INSTALL_DIR}"
cp "${NOTO_DOWNLOAD_DIR}/${NOTO_INSTALL_SOURCE}" "${NOTO_INSTALL_DIR}" || {
  echo "Failed to install Noto CJK fonts." >&2
  exit 1
}

fc-cache -f -v 2>/dev/null || true

rm -rf "${NOTO_WORK_DIR}"
echo ""
echo "Noto Sans CJK fonts installed successfully. Steam and Heroic will now display CJK text correctly."
sleep 5
