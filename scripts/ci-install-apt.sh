#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo 'ci-install-apt: expected at least one package' >&2
  exit 2
fi

if [ "$(id -u)" -eq 0 ]; then
  apt-get update
  apt-get install -y "$@"
elif command -v sudo >/dev/null; then
  sudo apt-get update
  sudo apt-get install -y "$@"
else
  echo 'ci-install-apt: this runner needs root or sudo to install missing tools' >&2
  exit 1
fi
