// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// A C status read as a truth value, in each C++ form, and the reads that are
// none: scripts/check-c-status-truth.py's fixture (#51).
// Guide: docs/coding-standard.md (Failure gates)
// Never built or linked: tests/test_check_c_status_truth.py parses it through
// a compile database of its own, with -DNDEBUG as a release build has. A line
// that reads a status as a truth value ends in a comment that names the
// matchers that must find it (their bind names after "truth:", or "any" where
// the C library's assert() decides which); no other line may give a finding.
// In C++ every truth read converts the status to bool, so bool-conversion
// finds each one beside the matcher of its form. The reads are wrong on
// purpose, so check-error-style.py's text rule for them skips this directory.

#include "c_status_map.hpp"

#include <cassert>

extern "C"
{
    __attribute__((warn_unused_result)) wfg_status wfg_truth_fixture_probe(void);
    void wfg_truth_fixture_take(bool ready);
    void wfg_truth_fixture_pass(wfg_status status);
}

namespace wfg_truth_fixture
{

using status_alias = wfg_status;

struct Result
{
    wfg_status status;
};

[[nodiscard]] status_alias alias();

template <typename T> [[nodiscard]] bool truthy(T value)
{
    return value; // truth: bool-conversion
}

int conditions(wfg_status status, const Result& result, bool ready)
{
    int count = 0;
    const wfg_status stored = wfg_truth_fixture_probe();

    if (wfg_truth_fixture_probe()) // truth: if-condition bool-conversion
    {
        ++count;
    }

    if ((wfg_truth_fixture_probe())) // truth: if-condition bool-conversion
    {
        ++count;
    }

    if (stored) // truth: if-condition bool-conversion
    {
        ++count;
    }

    if (result.status) // truth: if-condition bool-conversion
    {
        ++count;
    }

    if (alias()) // truth: if-condition bool-conversion
    {
        ++count;
    }

    if (const wfg_status held = wfg_truth_fixture_probe(); held) // truth: if-condition bool-conversion
    {
        ++count;
    }

    while (wfg_truth_fixture_probe()) // truth: while-condition bool-conversion
    {
        ++count;
    }

    do
    {
        ++count;
    } while (status); // truth: do-condition bool-conversion

    for (; wfg_truth_fixture_probe();) // truth: for-condition bool-conversion
    {
        ++count;
    }

    count += status ? 1 : 0; // truth: conditional-condition bool-conversion

    if (ready ? wfg_truth_fixture_probe() : status) // truth: conditional-branch conditional-bool-conversion bool-conversion if-condition
    {
        ++count;
    }

    return count;
}

int operands(wfg_status status, bool ready)
{
    int count = 0;

    if (!wfg_truth_fixture_probe()) // truth: not-operand bool-conversion
    {
        ++count;
    }

    if (!status) // truth: not-operand bool-conversion
    {
        ++count;
    }

    if (ready && status) // truth: logical-operand bool-conversion
    {
        ++count;
    }

    if (wfg_truth_fixture_probe() || ready) // truth: logical-operand bool-conversion
    {
        ++count;
    }

    if (status || wfg_truth_fixture_probe()) // truth: logical-operand bool-conversion
    {
        ++count;
    }

    return count;
}

bool conversions(wfg_status status)
{
    bool ok = wfg_truth_fixture_probe(); // truth: bool-conversion
    bool started = truthy(status);
    ok = status;                               // truth: bool-conversion
    wfg_truth_fixture_take(status);            // truth: bool-conversion
    started = (bool)wfg_truth_fixture_probe(); // truth: bool-cast bool-conversion
    ok = static_cast<bool>(status);            // truth: bool-cast bool-conversion
    started = bool(status);                    // truth: bool-cast bool-conversion
    assert(wfg_truth_fixture_probe());         // truth: any
    wfg_truth_fixture_take(ok && started);
    return status; // truth: bool-conversion
}

int integer_cast_conditions(wfg_status status, const Result& result, bool ready)
{
    int count = 0;
    bool converted = static_cast<int>(status); // truth: integer-cast-truth

    if ((int)status) // truth: integer-cast-truth
    {
        ++count;
    }

    while (static_cast<int>(alias())) // truth: integer-cast-truth
    {
        break;
    }

    do
    {
        ++count;
    } while (static_cast<int>(result.status)); // truth: integer-cast-truth

    for (; static_cast<int>(status);) // truth: integer-cast-truth
    {
        break;
    }

    if (static_cast<int>(status) ? ready : false) // truth: integer-cast-truth
    {
        ++count;
    }

    if (!static_cast<int>(status)) // truth: integer-cast-truth
    {
        ++count;
    }

    if (ready && static_cast<int>(status)) // truth: integer-cast-truth
    {
        ++count;
    }

    if ((int)status || ready) // truth: integer-cast-truth
    {
        ++count;
    }

    if ((int)status || (int)wfg_truth_fixture_probe()) // truth: integer-cast-truth
    {
        ++count;
    }

    if (ready ? static_cast<int>(status) : 0) // truth: integer-cast-conditional-branch
    {
        ++count;
    }

    if (!(ready ? (int)alias() : 0)) // truth: integer-cast-conditional-branch
    {
        ++count;
    }

    if (ready && (ready ? static_cast<int>(result.status) : 0)) // truth: integer-cast-conditional-branch
    {
        ++count;
    }

    converted = ready ? static_cast<int>(status) : 0; // truth: integer-cast-conditional-branch
    return count + static_cast<int>(converted);
}

bool boolean_comparisons(wfg_status status)
{
    const bool equal_false = wfg_truth_fixture_probe() == false; // truth: boolean-comparison
    const bool unequal_true = status != true;                    // truth: boolean-comparison
    return equal_false || unequal_true;
}

// The old bool returns' accumulation, now a bit operation on a status: with
// kWFG_StatusOk 0, ok &= f() is false on success (and on every even failure).
bool bits(wfg_status status)
{
    bool ok = true;
    ok &= wfg_truth_fixture_probe();         // truth: bitwise-operand
    ok |= status;                            // truth: bitwise-operand
    ok ^= status;                            // truth: bitwise-operand
    ok = ok & wfg_truth_fixture_probe();     // truth: bitwise-operand
    ok = status | ok;                        // truth: bitwise-operand
    ok = status ^ wfg_truth_fixture_probe(); // truth: bitwise-operand
    return ok;
}

// None of these reads a status as a truth value.
[[nodiscard]] wfg_status reads(wfg_status status, bool ready)
{
    int count = 0;
    const wfg_status copy = wfg_truth_fixture_probe();
    const bool ok = status == kWFG_StatusOk;
    const int numeric = static_cast<int>(status);
    const m110::Status outcome = waveform_generator::status_of(wfg_truth_fixture_probe());

    if (status != kWFG_StatusOk)
    {
        ++count;
    }

    if (numeric == 0 || (int)status == 0)
    {
        ++count;
    }

    if (numeric)
    {
        ++count;
    }

    if (static_cast<int>(status) != kWFG_StatusOk)
    {
        ++count;
    }

    if (wfg_truth_fixture_probe() == kWFG_StatusOk && ready)
    {
        ++count;
    }

    if (!waveform_generator::status_of(status).is_ok())
    {
        ++count;
    }

    if (const wfg_status held = wfg_truth_fixture_probe(); held != kWFG_StatusOk)
    {
        ++count;
    }

    if ((ready ? status : wfg_truth_fixture_probe()) == kWFG_StatusOk)
    {
        ++count;
    }

    switch (status)
    {
    case kWFG_StatusOk:
        ++count;
        break;

    default:
        break;
    }

    wfg_truth_fixture_pass(status);
    wfg_truth_fixture_take(ok && outcome.is_ok() && count > 0);
    return copy;
}

} // namespace wfg_truth_fixture
