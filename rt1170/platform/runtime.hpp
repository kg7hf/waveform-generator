// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

namespace m110
{

using ApplicationEntry = int (*)() noexcept;

// Boot the selected target and invoke the common application entry. Embedded
// implementations may dispatch it through their scheduler; native builds call
// it directly.
int run_application(ApplicationEntry application) noexcept;

} // namespace m110
