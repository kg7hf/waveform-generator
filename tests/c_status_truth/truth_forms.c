// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

/*
 * A C status read as a truth value, in each C form, and the reads that are
 * none: scripts/check-c-status-truth.py's fixture (#51).
 * Guide: docs/coding-standard.md (Failure gates)
 * Never built or linked: tests/test_check_c_status_truth.py parses it through
 * a compile database of its own, with -DNDEBUG as a release build has. A line
 * that reads a status as a truth value ends in a comment that names the
 * matchers that must find it (their bind names after "truth:", or "any" where
 * the C library's assert() decides which); no other line may give a finding.
 * The reads are wrong on purpose, so check-error-style.py's text rule for
 * them skips this directory.
 */

#include "wfg_status.h"

#include <assert.h>
#include <stdbool.h>

typedef enum wfg_status wfg_truth_fixture_status;

struct wfg_truth_fixture_result
{
    enum wfg_status status;
};

__attribute__((warn_unused_result)) enum wfg_status wfg_truth_fixture_probe(void);
__attribute__((warn_unused_result)) wfg_truth_fixture_status wfg_truth_fixture_alias(void);
void wfg_truth_fixture_take(bool ready);
void wfg_truth_fixture_pass(enum wfg_status status);

int wfg_truth_fixture_conditions(enum wfg_status status, const struct wfg_truth_fixture_result* result, bool ready)
{
    int count = 0;
    const enum wfg_status stored = wfg_truth_fixture_probe();

    if (wfg_truth_fixture_probe()) /* truth: if-condition */
    {
        ++count;
    }

    if ((wfg_truth_fixture_probe())) /* truth: if-condition */
    {
        ++count;
    }

    if (stored) /* truth: if-condition */
    {
        ++count;
    }

    if (result->status) /* truth: if-condition */
    {
        ++count;
    }

    if (wfg_truth_fixture_alias()) /* truth: if-condition */
    {
        ++count;
    }

    while (wfg_truth_fixture_probe()) /* truth: while-condition */
    {
        ++count;
    }

    do
    {
        ++count;
    } while (status); /* truth: do-condition */

    for (; wfg_truth_fixture_probe();) /* truth: for-condition */
    {
        ++count;
    }

    count += status ? 1 : 0; /* truth: conditional-condition */
    if (ready ? wfg_truth_fixture_probe() : status) /* truth: conditional-branch */
    {
        ++count;
    }
    return count;
}

int wfg_truth_fixture_operands(enum wfg_status status, bool ready)
{
    int count = 0;

    if (!wfg_truth_fixture_probe()) /* truth: not-operand */
    {
        ++count;
    }

    if (!status) /* truth: not-operand */
    {
        ++count;
    }

    if (ready && status) /* truth: logical-operand */
    {
        ++count;
    }

    if (wfg_truth_fixture_probe() || ready) /* truth: logical-operand */
    {
        ++count;
    }

    if (status || wfg_truth_fixture_probe()) /* truth: logical-operand */
    {
        ++count;
    }

    return count;
}

bool wfg_truth_fixture_conversions(enum wfg_status status)
{
    bool ok = wfg_truth_fixture_probe(); /* truth: bool-conversion */
    _Bool started = status; /* truth: bool-conversion */

    ok = status; /* truth: bool-conversion */
    wfg_truth_fixture_take(status); /* truth: bool-conversion */
    started = (bool)wfg_truth_fixture_probe(); /* truth: bool-cast */
    ok = (_Bool)status; /* truth: bool-cast */
    assert(wfg_truth_fixture_probe()); /* truth: any */
    wfg_truth_fixture_take(ok && started);
    return status; /* truth: bool-conversion */
}

int wfg_truth_fixture_promoted_conditions(enum wfg_status status, bool ready)
{
    int count = 0;

    while (ready ? status : wfg_truth_fixture_probe()) /* truth: conditional-branch */
    {
        ++count;
    }

    do
    {
        ++count;
    } while (ready ? wfg_truth_fixture_probe() : status); /* truth: conditional-branch */

    for (; ready ? status : wfg_truth_fixture_probe();) /* truth: conditional-branch */
    {
        ++count;
    }

    if (!(ready ? status : wfg_truth_fixture_probe())) /* truth: conditional-not-operand */
    {
        ++count;
    }

    if (ready && (ready ? status : wfg_truth_fixture_probe())) /* truth: conditional-logical-operand */
    {
        ++count;
    }

    if ((ready ? wfg_truth_fixture_probe() : status) || ready) /* truth: conditional-logical-operand */
    {
        ++count;
    }

    if ((ready ? status : wfg_truth_fixture_probe()) == kWFG_StatusOk) /* valid conditional status comparison */
    {
        ++count;
    }

    return count;
}

bool wfg_truth_fixture_promoted_conversion(enum wfg_status status, bool ready)
{
    _Bool accepted = ready ? status : wfg_truth_fixture_probe(); /* truth: conditional-bool-conversion */
    return ready ? status : wfg_truth_fixture_probe(); /* truth: conditional-bool-conversion */
}

int wfg_truth_fixture_integer_cast_conditions(enum wfg_status status,
        const struct wfg_truth_fixture_result* result, bool ready)
{
    int count = 0;
    _Bool converted = (int)status; /* truth: integer-cast-truth */

    if ((int)status) /* truth: integer-cast-truth */
    {
        ++count;
    }
    while ((int)wfg_truth_fixture_probe()) /* truth: integer-cast-truth */
    {
        break;
    }
    do
    {
        ++count;
    } while ((int)result->status); /* truth: integer-cast-truth */
    for (; (int)status;) /* truth: integer-cast-truth */
    {
        break;
    }
    if (((int)status) ? ready : false) /* truth: integer-cast-truth */
    {
        ++count;
    }
    if (!(int)status) /* truth: integer-cast-truth */
    {
        ++count;
    }
    if (ready && (int)status) /* truth: integer-cast-truth */
    {
        ++count;
    }
    if ((int)status || ready) /* truth: integer-cast-truth */
    {
        ++count;
    }
    if ((int)status || (int)wfg_truth_fixture_probe()) /* truth: integer-cast-truth */
    {
        ++count;
    }
    if (ready ? (int)status : 0) /* truth: integer-cast-conditional-branch */
    {
        ++count;
    }
    if (!(ready ? (int)wfg_truth_fixture_probe() : 0)) /* truth: integer-cast-conditional-branch */
    {
        ++count;
    }
    if (ready && (ready ? (int)result->status : 0)) /* truth: integer-cast-conditional-branch */
    {
        ++count;
    }
    converted = ready ? (int)status : 0; /* truth: integer-cast-conditional-branch */
    return count + converted;
}

/* The old bool returns' accumulation, now a bit operation on a status: with
   kWFG_StatusOk 0, ok &= f() is false on success (and on every even failure). */
bool wfg_truth_fixture_bits(enum wfg_status status)
{
    bool ok = true;

    ok &= wfg_truth_fixture_probe(); /* truth: bitwise-operand */
    ok |= status; /* truth: bitwise-operand */
    ok ^= status; /* truth: bitwise-operand */
    ok = ok & wfg_truth_fixture_probe(); /* truth: bitwise-operand */
    ok = status | ok; /* truth: bitwise-operand */
    ok = status ^ wfg_truth_fixture_probe(); /* truth: bitwise-operand */
    return ok;
}

/* None of these reads a status as a truth value. */
__attribute__((warn_unused_result)) enum wfg_status wfg_truth_fixture_reads(enum wfg_status status, bool ready)
{
    int count = 0;
    const enum wfg_status copy = wfg_truth_fixture_probe();
    const bool ok = status == kWFG_StatusOk;
    const int numeric = (int)status;

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

    if (wfg_truth_fixture_probe() == kWFG_StatusOk && ready)
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
    wfg_truth_fixture_take(ok && count > 0);
    return copy;
}
