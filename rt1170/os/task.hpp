// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#pragma once

#include "common/status.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace m110
{

struct TaskOptions
{
    void* stack_memory{};
    std::uint32_t stack_size_bytes{};
    void* control_block_memory{};
    std::uint32_t control_block_size_bytes{};
    std::int32_t priority{24};
};

class RtosTask
{
public:
    RtosTask() = default;
    #if defined(M110_FREERTOS_PERMANENT_TASKS)
    ~RtosTask() noexcept;
    #else
    virtual ~RtosTask() noexcept;
    #endif
    RtosTask(const RtosTask&) = delete;
    RtosTask& operator=(const RtosTask&) = delete;

    [[nodiscard]] Status create(const char* name, const TaskOptions& options = {}) noexcept;
    [[nodiscard]] Status start() noexcept;

    void request_stop() noexcept
    {
        #if !defined(M110_FREERTOS_PERMANENT_TASKS)

        if (!stop_requested_.exchange(true))
        {
            on_stop_requested();
        }

        #endif
    }

    [[nodiscard]] Status join() noexcept;

    [[nodiscard]] bool running() const noexcept
    {
        #if defined(M110_FREERTOS_PERMANENT_TASKS)
        return started_;
        #else
        return running_.load();
        #endif
    }

    [[nodiscard]] bool stop_requested() const noexcept
    {
        #if defined(M110_FREERTOS_PERMANENT_TASKS)
        return false;
        #else
        return stop_requested_.load();
        #endif
    }

    [[nodiscard]] const char* name() const noexcept
    {
        #if defined(M110_FREERTOS_PERMANENT_TASKS)
        return nullptr;
        #else
        return name_;
        #endif
    }

protected:
    virtual void run() noexcept = 0;

    #if !defined(M110_FREERTOS_PERMANENT_TASKS)
    // A task that can block indefinitely must override this hook and wake the
    // event/queue used by run(). request_stop() is otherwise only a pollable
    // cooperative cancellation request.
    virtual void on_stop_requested() noexcept
    {
    }
    #endif

public:
    #if !defined(M110_FREERTOS_PERMANENT_TASKS)
    static constexpr std::size_t native_storage_size = 64U;
    #endif

private:
    static void native_entry(void* context) noexcept;

    #if defined(M110_FREERTOS_PERMANENT_TASKS)
    bool created_ {};
    bool started_{};
    #else
    const char* name_ {};
    TaskOptions options_{};
    alignas(std::max_align_t) std::array<std::byte, native_storage_size> native_storage_{};
    std::atomic<bool> running_{};
    std::atomic<bool> stop_requested_{};
    bool created_{};
    bool started_{};
    #endif
};

} // namespace m110
