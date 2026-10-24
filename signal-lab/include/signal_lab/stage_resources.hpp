// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

namespace signal_lab
{

class WattersonChannel;
class TapObserver;
class SecondarySource;

// Optional resources borrowed by typed stages. Each resource must outlive the
// engine's configured use. A stage that needs a missing resource fails configure.
// The board places the Watterson channel in OCRAM2; host tools own source I/O.
struct StageResources
{
    WattersonChannel* watterson{};
    TapObserver* tap_observer{}; // host only
    SecondarySource* secondary_source{};
};

} // namespace signal_lab
