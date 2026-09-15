// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Paul R. Decker

#include "os/task.hpp"
#include "platform/board.hpp"
#include "platform/runtime.hpp"

extern "C"
{
#include "fsl_common.h"
#include "FreeRTOS.h"
#include "task.h"
}

namespace
{

constexpr std::uint32_t application_stack_words = 1024U;
StaticTask_t application_control;
StackType_t application_stack[application_stack_words];
StaticTask_t idle_control;
StackType_t idle_stack[configMINIMAL_STACK_SIZE];
m110::ApplicationEntry application_entry;

class ApplicationTask final : public m110::RtosTask
{
private:
    void run() noexcept override
    {
        const auto application_status =
            application_entry() == 0 ? m110::PlatformStatus::success :
            m110::PlatformStatus::failure;

        for (;;)
        {
            m110::platform_set_status(application_status);
            vTaskDelay(pdMS_TO_TICKS(250U));
            m110::platform_set_status(m110::PlatformStatus::idle);
            vTaskDelay(pdMS_TO_TICKS(250U));
        }
    }
};

ApplicationTask application_task;

} // namespace

extern "C"
{

    void vApplicationGetIdleTaskMemory(StaticTask_t** control, StackType_t** stack,
                                       uint32_t* stack_size)
    {
        *control = &idle_control;
        *stack = idle_stack;
        *stack_size = configMINIMAL_STACK_SIZE;
    }

    void vApplicationStackOverflowHook(TaskHandle_t, char*)
    {
        __disable_irq();

        for (;;)
        {
            __NOP();
        }
    }

} // extern "C"

int m110::run_application(ApplicationEntry application) noexcept
{
    if (application == nullptr)
    {
        return 1;
    }

    platform_initialize();
    application_entry = application;
    const TaskOptions options
    {
        .stack_memory = application_stack,
        .stack_size_bytes = sizeof(application_stack),
        .control_block_memory = &application_control,
        .control_block_size_bytes = sizeof(application_control),
        .priority = 2,
    };
    const auto create_status = application_task.create("wfg", options);

    if (!create_status.is_ok())
    {
        return 2;
    }

    const auto start_status = application_task.start();

    if (!start_status.is_ok())
    {
        return 3;
    }

    vTaskStartScheduler();

    for (;;)
    {
        __NOP();
    }
}
