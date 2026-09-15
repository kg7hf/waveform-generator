#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Paul R. Decker

"""Command-line entry for signal_lab.render (python host/render_scenario.py --scenario X --output Y)."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from signal_lab.render import main  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
