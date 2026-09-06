#include "os/task.hpp"

extern "C"
{
#include "FreeRTOS.h"
#include "task.h"
}

#include <cstddef>
#include <cstdint>
#include <new>

namespace m110
{

#if defined(M110_FREERTOS_PERMANENT_TASKS)

extern "C" [[noreturn]] void m110_freertos_permanent_task_failed(void);

RtosTask::~RtosTask() noexcept = default;

Status RtosTask::create(const char* name, const TaskOptions& options) noexcept
{
    if (created_ || name == nullptr || options.stack_memory == nullptr ||
            options.control_block_memory == nullptr ||
            options.stack_size_bytes < sizeof(StackType_t) ||
            options.control_block_size_bytes < sizeof(StaticTask_t) ||
            options.priority < 0 || options.priority >= configMAX_PRIORITIES)
    {
        return {StatusCode::invalid_argument,
                "invalid or repeated permanent FreeRTOS task create"};
    }

    const auto id = xTaskCreateStatic(
        &RtosTask::native_entry, name,
        options.stack_size_bytes / sizeof(StackType_t), this,
        static_cast<UBaseType_t>(options.priority),
        static_cast<StackType_t*>(options.stack_memory),
        static_cast<StaticTask_t*>(options.control_block_memory));

    if (id == nullptr)
    {
        return {StatusCode::unavailable, "xTaskCreateStatic failed"};
    }

    created_ = true;
    return Status::success();
}

Status RtosTask::start() noexcept
{
    if (!created_ || started_)
    {
        return {StatusCode::invalid_argument, "permanent task is not startable"};
    }

    // U545 permanent tasks are all created before vTaskStartScheduler(). They
    // cannot execute until the complete static composition has been started.
    started_ = true;
    return Status::success();
}

Status RtosTask::join() noexcept
{
    return {StatusCode::unavailable, "permanent task cannot be joined"};
}

void RtosTask::native_entry(void* context) noexcept
{
    auto& task = *static_cast<RtosTask*>(context);
    task.run();

    // A permanent worker returning is a fatal system fault, not a shutdown
    // path. The platform hook publishes the failure and never returns.
    m110_freertos_permanent_task_failed();
}

#else

constexpr std::uint32_t start_flag = 1U;

struct FreeRtosTaskState
{
    TaskHandle_t id{};
};

static_assert(sizeof(FreeRtosTaskState) <= RtosTask::native_storage_size);
static_assert(alignof(FreeRtosTaskState) <= alignof(std::max_align_t));

RtosTask::~RtosTask() noexcept
{
    if (created_)
    {
        auto& state = *std::launder(reinterpret_cast<FreeRtosTaskState*>(native_storage_.data()));
        state.~FreeRtosTaskState();
    }
}

Status RtosTask::create(const char* name, const TaskOptions& options) noexcept
{
    if (created_ || name == nullptr || options.stack_memory == nullptr || options.control_block_memory == nullptr || options.stack_size_bytes < sizeof(StackType_t) ||
            options.control_block_size_bytes < sizeof(StaticTask_t) || options.priority < 0 || options.priority >= configMAX_PRIORITIES)
    {
        return {StatusCode::invalid_argument, "invalid or repeated FreeRTOS task create"};
    }

    auto* const state = ::new (native_storage_.data()) FreeRtosTaskState{};
    state->id = xTaskCreateStatic(&RtosTask::native_entry, name, options.stack_size_bytes / sizeof(StackType_t), this, static_cast<UBaseType_t>(options.priority),
                                  static_cast<StackType_t*>(options.stack_memory), static_cast<StaticTask_t*>(options.control_block_memory));

    if (state->id == nullptr)
    {
        state->~FreeRtosTaskState();
        return {StatusCode::unavailable, "xTaskCreateStatic failed"};
    }

    name_ = name;
    options_ = options;
    created_ = true;
    return Status::success();
}

Status RtosTask::start() noexcept
{
    if (!created_ || started_)
    {
        return {StatusCode::invalid_argument, "task is not startable"};
    }

    stop_requested_.store(false);
    auto& state = *std::launder(reinterpret_cast<FreeRtosTaskState*>(native_storage_.data()));

    if (xTaskNotify(state.id, start_flag, eSetBits) != pdPASS)
    {
        return {StatusCode::internal_error, "xTaskNotify failed"};
    }

    started_ = true;
    return Status::success();
}

Status RtosTask::join() noexcept
{
    if (!created_ || !started_ || xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return {StatusCode::invalid_argument, "task is not joinable"};
    }

    while (running_.load())
    {
        vTaskDelay(1U);
    }

    return Status::success();
}

void RtosTask::native_entry(void* context) noexcept
{
    auto& task = *static_cast<RtosTask*>(context);
    std::uint32_t flags{};

    if (xTaskNotifyWait(0U, UINT32_MAX, &flags, portMAX_DELAY) == pdPASS && (flags & start_flag) != 0U)
    {
        task.running_.store(true);
        task.run();
        task.running_.store(false);
    }

    vTaskSuspend(nullptr);
}

#endif

} // namespace m110
