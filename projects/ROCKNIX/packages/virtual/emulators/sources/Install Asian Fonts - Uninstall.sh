#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)

source /etc/profile

FONTS_DIR="/storage/.local/share/fonts"
REMOVED=0

for font in \
  "${FONTS_DIR}/NotoSansCJK-Regular.ttc" \
  "${FONTS_DIR}/NotoSerifCJK-VF.otf.ttc"; do
  if [ -f "${font}" ]; then
    rm -f "${font}"
    echo "Removed: ${font}"
    REMOVED=1
  fi
done

if [ $REMOVED -eq 0 ]; then
  echo "No CJK fonts found to remove."
  sleep 3
  exit 0
fi

fc-cache -f -v 2>/dev/null || true
echo ""
echo "CJK fonts uninstalled. System default fonts restored."
sleep 5
