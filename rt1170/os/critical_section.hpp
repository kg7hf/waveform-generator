// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// FreeRTOS critical sections as scoped guards
// Guide: docs/modules/rt1170-player-and-audio.md
// Ownership: a guard belongs to the stack frame that declares it and leaves its
// critical section on every exit path.
// Contract: TaskCriticalSection is for task context (taskENTER_CRITICAL);
// IsrCriticalSection is for an interrupt at or below
// configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, such as the USB ISR
// (taskENTER_CRITICAL_FROM_ISR); AnyContextCriticalSection is for code that
// may run in either. All mask every interrupt that may call FreeRTOS, the USB
// interrupt included. Keep them short: no hooks, logging, allocation or I/O
// inside (docs/coding-standard.md).

#pragma once

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

namespace waveform_generator
{

class TaskCriticalSection
{
public:
    TaskCriticalSection() noexcept { taskENTER_CRITICAL(); }

    ~TaskCriticalSection() { taskEXIT_CRITICAL(); }

    TaskCriticalSection(const TaskCriticalSection&) = delete;
    TaskCriticalSection& operator=(const TaskCriticalSection&) = delete;
    TaskCriticalSection(TaskCriticalSection&&) = delete;
    TaskCriticalSection& operator=(TaskCriticalSection&&) = delete;
};

class IsrCriticalSection
{
public:
    IsrCriticalSection() noexcept : saved_mask_{taskENTER_CRITICAL_FROM_ISR()} {}

    ~IsrCriticalSection() { taskEXIT_CRITICAL_FROM_ISR(saved_mask_); }

    IsrCriticalSection(const IsrCriticalSection&) = delete;
    IsrCriticalSection& operator=(const IsrCriticalSection&) = delete;
    IsrCriticalSection(IsrCriticalSection&&) = delete;
    IsrCriticalSection& operator=(IsrCriticalSection&&) = delete;

private:
    UBaseType_t saved_mask_; // the interrupt mask to restore on exit
};

// For code that runs in a task or in an interrupt at or below
// configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY and cannot tell which, such as
// TinyUSB's assert hook (usb_audio.cpp). On this port (ARM_CM7) the ISR form
// raises BASEPRI and restores the value it saved, which is as valid in a task:
// it masks the same interrupts, and inside a TaskCriticalSection it nests
// without touching that section's count.
using AnyContextCriticalSection = IsrCriticalSection;

} // namespace waveform_generator
