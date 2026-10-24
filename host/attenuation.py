#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: 2026 Paul R. Decker

"""A PLAYLIST item's attenuation (att=) in exact integer arithmetic.

The player attenuates an item's output after the engine by 0 to 40.0 dB in
steps of 0.1 dB (docs/live-control-protocol.md, "PLAYLIST"). An attenuation
of n tenths of a dB is the gain 10^(-n/200), kept as a Q1.31 fraction: the
table holds round_half_even(2^31 * 10^(-n/200)) for n = 0 to 400, so 0 dB is
2^31 exactly. A PCM24 sample s becomes round_half_even(s * g / 2^31): an
exact integer product, so zero stays zero, 0 dB passes every sample through
unchanged, and the host predicts the board's samples bit for bit.

The closed form is evaluated in decimal arithmetic at 60 significant digits,
far past the 31 bits a gain keeps, so no entry depends on a binary floating
point rounding. The firmware's table, common/attenuation_gains.hpp, is
written from it by this tool once, and checked against it by the tests:

  python host/attenuation.py write common/attenuation_gains.hpp
  python host/attenuation.py check common/attenuation_gains.hpp

host/make_playlist.py takes the table and apply() from here. Standard
library only.
"""

from __future__ import annotations

import argparse
from decimal import ROUND_HALF_EVEN, Decimal, localcontext
from functools import lru_cache
from pathlib import Path
import re
import sys


MAX_TENTHS = 400
ENTRIES = MAX_TENTHS + 1
Q_BITS = 31
UNITY = 1 << Q_BITS
HALF = 1 << (Q_BITS - 1)
DIGITS = 60
_ATTENUATION = re.compile(r"(-?)(0|[1-9][0-9]?)(?:\.([0-9]))?")
_PER_LINE = 6
HEADER_PATH = "common/attenuation_gains.hpp"


class AttenuationError(ValueError):
    """An att= value the board refuses (PLAYLIST_ATT)."""


def gain_q31(tenths: int) -> int:
    """round_half_even(2^31 * 10^(-tenths/200)), the gain of `tenths` tenths of a dB."""
    if not 0 <= tenths <= MAX_TENTHS:
        raise AttenuationError(f"an attenuation is 0 to {MAX_TENTHS} tenths of a dB, not {tenths}")
    with localcontext() as context:
        context.prec = DIGITS
        exact = Decimal(UNITY) * (Decimal(10) ** (Decimal(-tenths) / Decimal(200)))
        return int(exact.to_integral_value(rounding=ROUND_HALF_EVEN))


@lru_cache(maxsize=1)
def table() -> tuple[int, ...]:
    """Every entry, from 0 dB to -40.0 dB."""
    return tuple(gain_q31(tenths) for tenths in range(ENTRIES))


def apply(sample: int, gain: int) -> int:
    """round_half_even(sample * gain / 2^31): a PCM24 sample attenuated by a Q1.31 gain."""
    quotient, remainder = divmod(sample * gain, UNITY)
    if remainder > HALF or (remainder == HALF and quotient % 2 == 1):
        quotient += 1
    return quotient


def parse(text: str) -> int:
    """att=<dB>: 0 (or 0.0, -0) or a negative level down to -40.0 with at most one decimal: its tenths of a dB."""
    match = _ATTENUATION.fullmatch(text) if isinstance(text, str) else None
    if match is None:
        raise AttenuationError(f"att= is 0 to -40.0 dB with at most one decimal, not {text!r}")
    tenths = int(match.group(2)) * 10 + int(match.group(3) or "0")
    if tenths > MAX_TENTHS:
        raise AttenuationError(f"att= goes down to -40.0 dB, not {text}")
    if tenths != 0 and not match.group(1):
        raise AttenuationError(f"att= never raises the level: {text} dB is positive")
    return tenths


def spelling(tenths: int) -> str:
    """The canonical att= value of `tenths` tenths of a dB: 0, -20, -0.5, -40."""
    if tenths == 0:
        return "0"
    whole, tenth = divmod(tenths, 10)
    return f"-{whole}" if tenth == 0 else f"-{whole}.{tenth}"


def header() -> bytes:
    """common/attenuation_gains.hpp as this tool writes it, LF line ends."""
    entries = table()
    rows = []
    for start in range(0, ENTRIES, _PER_LINE):
        rows.append("    " + ", ".join(f"{value}U" for value in entries[start:start + _PER_LINE]) + ",")
    lines = [
        "// SPDX-License-Identifier: GPL-3.0-only",
        "// SPDX-FileCopyrightText: 2026 Paul R. Decker",
        "",
        "// Written by host/attenuation.py from its closed form; do not edit (the",
        "// tests check this file against it). Entry n is the gain of an attenuation",
        "// of n tenths of a dB as a Q1.31 fraction: round_half_even(2^31 *",
        "// 10^(-n/200)), n = 0 (2^31, 0 dB) to 400 (-40.0 dB).",
        "",
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace waveform_generator::attenuation",
        "{",
        "",
        f"inline constexpr std::array<std::uint32_t, {ENTRIES}U> gains_q31{{",
        *rows,
        "};",
        "",
        "} // namespace waveform_generator::attenuation",
        "",
    ]
    return "\n".join(lines).encode("ascii")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("action", choices=("write", "check"), help="write the firmware's table, or check it against the closed form")
    parser.add_argument("path", type=Path, nargs="?", default=Path(HEADER_PATH))
    args = parser.parse_args(argv)
    expected = header()
    if args.action == "write":
        args.path.write_bytes(expected)
        return 0
    if args.path.read_bytes() != expected:
        print(f"attenuation: {args.path} is not the table the closed form gives", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
