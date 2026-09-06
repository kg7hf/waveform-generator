#include "sdcard_probe.hpp"

#include "platform/board.hpp"
#include "platform/waveform_sd.h"

extern "C"
{
#include "FreeRTOS.h"
#include "fsl_common.h"
#include "fsl_sd.h"
#include "fsl_sdmmc_common.h"
#if defined(WFG_SD_READ_ONLY_IDENTIFY_TEST)
#include "ff.h"
#include "fsl_sd_disk.h"
#endif
#include "task.h"
}

#include <cstdint>

namespace
{

#if defined(WFG_SD_READ_ONLY_IDENTIFY_TEST)
sd_card_t& sd_probe_card = g_sd;
FATFS sd_probe_file_system{};
constexpr std::uint32_t probe_sector_size = 512U;
AT_NONCACHEABLE_SECTION_ALIGN(
    static std::uint8_t sd_probe_sector[probe_sector_size], 4U);
#else
sd_card_t sd_probe_card{};
#endif

void write_uint(std::uint32_t value) noexcept
{
    char digits[10]{};
    std::uint32_t digit_count = 0U;
    do
    {
        digits[digit_count] = static_cast<char>('0' + (value % 10U));
        digit_count++;
        value /= 10U;
    } while (value != 0U && digit_count < sizeof(digits));

    while (digit_count != 0U)
    {
        digit_count--;
        char text[2]{digits[digit_count], '\0'};
        m110::platform_write_diagnostic(text);
    }
}

void write_hex(std::uint32_t value) noexcept
{
    constexpr char digits[] = "0123456789abcdef";
    m110::platform_write_diagnostic("0x");
    for (std::uint32_t shift = 28U; shift <= 28U; shift -= 4U)
    {
        char text[2]{digits[(value >> shift) & 0x0fU], '\0'};
        m110::platform_write_diagnostic(text);
        if (shift == 0U)
        {
            break;
        }
    }
}

void marker(const char* text) noexcept
{
    m110::platform_write_diagnostic("SDPROBE ");
    m110::platform_write_diagnostic(text);
    m110::platform_write_diagnostic("\r\n");
}

void marker_status(const char* text, std::uint32_t status) noexcept
{
    m110::platform_write_diagnostic("SDPROBE ");
    m110::platform_write_diagnostic(text);
    m110::platform_write_diagnostic(" status=");
    write_uint(status);
    m110::platform_write_diagnostic("\r\n");
}

#if defined(WFG_SD_POWER_CONTROL_TEST)
void marker_status_response(const char* text, status_t status,
                            std::uint32_t response) noexcept
{
    m110::platform_write_diagnostic("SDPROBE ");
    m110::platform_write_diagnostic(text);
    m110::platform_write_diagnostic(" status=");
    write_uint(static_cast<std::uint32_t>(status));
    m110::platform_write_diagnostic(" response0=");
    write_hex(response);
    m110::platform_write_diagnostic("\r\n");
}

status_t send_cmd8_once(std::uint32_t& response) noexcept
{
    sdmmchost_transfer_t content{};
    sdmmchost_cmd_t command{};

    command.index = static_cast<std::uint32_t>(kSD_SendInterfaceCondition);
    command.argument = 0x1AAU;
    command.responseType = kCARD_ResponseTypeR7;

    content.command = &command;
    content.data = nullptr;
    const auto status = SDMMCHOST_TransferFunction(sd_probe_card.host, &content);
    response = command.response[0U];
    if (status != kStatus_Success)
    {
        return status;
    }
    if ((response & 0xffU) != 0xaaU)
    {
        return kStatus_SDMMC_CardNotSupport;
    }
    return kStatus_Success;
}
#endif

#if !defined(WFG_SD_POWER_CONTROL_TEST)
void marker_geometry() noexcept
{
    m110::platform_write_diagnostic("SDPROBE geometry block_count=");
    write_uint(sd_probe_card.blockCount);
    m110::platform_write_diagnostic(" block_size=");
    write_uint(sd_probe_card.blockSize);
    m110::platform_write_diagnostic("\r\n");
}

std::uint32_t read_le32(const std::uint8_t* bytes) noexcept
{
    return static_cast<std::uint32_t>(bytes[0U]) |
           (static_cast<std::uint32_t>(bytes[1U]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2U]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3U]) << 24U);
}

std::uint32_t fnv1a32(const std::uint8_t* bytes, std::uint32_t size) noexcept
{
    std::uint32_t hash = 2166136261U;
    for (std::uint32_t index = 0U; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 16777619U;
    }
    return hash;
}

void write_ascii(const std::uint8_t* bytes, std::uint32_t size) noexcept
{
    for (std::uint32_t index = 0U; index < size; ++index)
    {
        const std::uint8_t value = bytes[index];
        const char text[2]{
            static_cast<char>(value >= 0x20U && value <= 0x7eU ? value : '.'),
            '\0'};
        m110::platform_write_diagnostic(text);
    }
}

std::uint32_t marker_sector_zero() noexcept
{
    m110::platform_write_diagnostic("SDPROBE sector0 fnv1a32=");
    write_hex(fnv1a32(sd_probe_sector, probe_sector_size));
    m110::platform_write_diagnostic(" signature=");
    write_hex(static_cast<std::uint32_t>(sd_probe_sector[510U]) |
              (static_cast<std::uint32_t>(sd_probe_sector[511U]) << 8U));
    m110::platform_write_diagnostic(" prefix_le=");
    write_hex(read_le32(sd_probe_sector));
    m110::platform_write_diagnostic("\r\n");

    for (std::uint32_t partition = 0U; partition < 4U; ++partition)
    {
        const std::uint32_t offset = 446U + (partition * 16U);
        m110::platform_write_diagnostic("SDPROBE partition index=");
        write_uint(partition);
        m110::platform_write_diagnostic(" boot=");
        write_hex(sd_probe_sector[offset]);
        m110::platform_write_diagnostic(" type=");
        write_hex(sd_probe_sector[offset + 4U]);
        m110::platform_write_diagnostic(" start_lba=");
        write_uint(read_le32(&sd_probe_sector[offset + 8U]));
        m110::platform_write_diagnostic(" block_count=");
        write_uint(read_le32(&sd_probe_sector[offset + 12U]));
        m110::platform_write_diagnostic("\r\n");
    }

    return read_le32(&sd_probe_sector[454U]);
}

void marker_volume_boot(std::uint32_t lba) noexcept
{
    m110::platform_write_diagnostic("SDPROBE volume_boot lba=");
    write_uint(lba);
    m110::platform_write_diagnostic(" fnv1a32=");
    write_hex(fnv1a32(sd_probe_sector, probe_sector_size));
    m110::platform_write_diagnostic(" signature=");
    write_hex(static_cast<std::uint32_t>(sd_probe_sector[510U]) |
              (static_cast<std::uint32_t>(sd_probe_sector[511U]) << 8U));
    m110::platform_write_diagnostic(" oem_ascii=");
    write_ascii(&sd_probe_sector[3U], 8U);
    m110::platform_write_diagnostic("\r\n");
}

m110::Status inspect_file_system() noexcept
{
    constexpr char drive_path[] = "2:";
    marker("before read-only FatFs mount");
    const FRESULT mount_status = f_mount(&sd_probe_file_system, drive_path, 1U);
    marker_status("after read-only FatFs mount",
                  static_cast<std::uint32_t>(mount_status));
    if (mount_status != FR_OK)
    {
        return {m110::StatusCode::unavailable, "read-only FatFs mount failed"};
    }

    FILINFO information{};
    const FRESULT directory_status = f_stat("2:/WG", &information);
    marker_status("read-only FatFs stat WG",
                  static_cast<std::uint32_t>(directory_status));
    const FRESULT wav_status = f_stat("2:/WG/PLAY.WAV", &information);
    marker_status("read-only FatFs stat PLAY.WAV",
                  static_cast<std::uint32_t>(wav_status));
    const FRESULT manifest_status = f_stat("2:/WG/PLAY.WGM", &information);
    marker_status("read-only FatFs stat PLAY.WGM",
                  static_cast<std::uint32_t>(manifest_status));

    const FRESULT unmount_status = f_mount(nullptr, drive_path, 0U);
    marker_status("after read-only FatFs unmount",
                  static_cast<std::uint32_t>(unmount_status));
    if (unmount_status != FR_OK)
    {
        return {m110::StatusCode::unavailable, "read-only FatFs unmount failed"};
    }
    return m110::Status::success();
}
#endif

} // namespace

namespace waveform_generator
{

m110::Status run_sdcard_probe() noexcept
{
    marker("start");
    marker("before WFG_SD_Config");
    WFG_SD_Config(&sd_probe_card);
    marker("after WFG_SD_Config");
    marker_status("sourceClock_Hz", WFG_SD_SourceClockHz(&sd_probe_card));

    marker("before SD_HostInit");
    const status_t host_status = SD_HostInit(&sd_probe_card);
    marker_status("after SD_HostInit", static_cast<std::uint32_t>(host_status));
    if (host_status != kStatus_Success)
    {
        return {m110::StatusCode::unavailable, "SD_HostInit failed"};
    }

#if defined(WFG_SD_POWER_CONTROL_TEST)
    marker("before SD_SetCardPower false");
    SD_SetCardPower(&sd_probe_card, false);
    marker("after SD_SetCardPower false");
    vTaskDelay(pdMS_TO_TICKS(1000U));

    marker("before SD_SetCardPower true");
    SD_SetCardPower(&sd_probe_card, true);
    marker("after SD_SetCardPower true");
    vTaskDelay(pdMS_TO_TICKS(1000U));

    marker("before WFG_SD_PrepareDataTransfer");
    WFG_SD_PrepareDataTransfer(&sd_probe_card);
    marker("after WFG_SD_PrepareDataTransfer");

    marker("before CMD0 while power on");
    const auto idle_on_status = SDMMC_GoIdle(sd_probe_card.host);
    marker_status("after CMD0 while power on", static_cast<std::uint32_t>(idle_on_status));

    std::uint32_t cmd8_on_response = 0U;
    marker("before CMD8 while power on");
    const auto cmd8_on_status = send_cmd8_once(cmd8_on_response);
    marker_status_response("after CMD8 while power on", cmd8_on_status,
                           cmd8_on_response);
    if (idle_on_status != kStatus_Success || cmd8_on_status != kStatus_Success ||
        (cmd8_on_response & 0xfffU) != 0x1aaU)
    {
        return {m110::StatusCode::unavailable, "power-on CMD8 failed"};
    }

    marker("before SD_SetCardPower false for off-state CMD8");
    SD_SetCardPower(&sd_probe_card, false);
    marker("after SD_SetCardPower false for off-state CMD8");
    vTaskDelay(pdMS_TO_TICKS(1000U));

    std::uint32_t cmd8_off_response = 0U;
    marker("before CMD8 while power off");
    const auto cmd8_off_status = send_cmd8_once(cmd8_off_response);
    marker_status_response("after CMD8 while power off", cmd8_off_status,
                           cmd8_off_response);
    if (cmd8_off_status == kStatus_Success &&
        (cmd8_off_response & 0xfffU) == 0x1aaU)
    {
        marker("power_control_result card_responded_while_off");
    }
    else
    {
        marker("power_control_result card_unresponsive_while_off");
    }

    marker("before SD_SetCardPower true after off-state CMD8");
    SD_SetCardPower(&sd_probe_card, true);
    marker("after SD_SetCardPower true after off-state CMD8");
    marker("pass");
    return m110::Status::success();
#else
    marker("before SD_SetCardPower false");
    SD_SetCardPower(&sd_probe_card, false);
    marker("after SD_SetCardPower false");
    marker("before SD_SetCardPower true");
    SD_SetCardPower(&sd_probe_card, true);
    marker("after SD_SetCardPower true");

    std::uint32_t host_detect_status = UINT32_MAX;
    marker("before DAT3 host detect");
    const bool host_detect_read =
        WFG_SD_ReadHostDetectStatus(&sd_probe_card, &host_detect_status);
    marker_status("after DAT3 host detect read", host_detect_read ? 1U : 0U);
    marker_status("after DAT3 host detect value", host_detect_status);

    marker("before WFG_SD_PrepareDataTransfer");
    WFG_SD_PrepareDataTransfer(&sd_probe_card);
    marker("after WFG_SD_PrepareDataTransfer");

    marker("before direct SD_CardInit");
    const status_t card_status = SD_CardInit(&sd_probe_card);
    marker_status("after direct SD_CardInit", static_cast<std::uint32_t>(card_status));
    if (card_status != kStatus_Success)
    {
        return {m110::StatusCode::unavailable, "SD_CardInit failed"};
    }

    marker_geometry();
    if (sd_probe_card.blockCount == 0U || sd_probe_card.blockSize == 0U)
    {
        return {m110::StatusCode::unavailable, "invalid SD geometry"};
    }

#if defined(WFG_SD_READ_ONLY_IDENTIFY_TEST)
    if (sd_probe_card.blockSize != probe_sector_size)
    {
        return {m110::StatusCode::unavailable, "unexpected SD block size"};
    }
    marker("before read-only sector 0");
    const status_t read_status =
        SD_ReadBlocks(&sd_probe_card, sd_probe_sector, 0U, 1U);
    marker_status("after read-only sector 0", static_cast<std::uint32_t>(read_status));
    if (read_status != kStatus_Success)
    {
        return {m110::StatusCode::unavailable, "sector 0 read failed"};
    }
    const std::uint32_t first_partition_lba = marker_sector_zero();
    if (first_partition_lba != 0U && first_partition_lba < sd_probe_card.blockCount)
    {
        marker("before read-only first partition boot sector");
        const status_t volume_read_status =
            SD_ReadBlocks(&sd_probe_card, sd_probe_sector, first_partition_lba, 1U);
        marker_status("after read-only first partition boot sector",
                      static_cast<std::uint32_t>(volume_read_status));
        if (volume_read_status != kStatus_Success)
        {
            return {m110::StatusCode::unavailable,
                    "partition boot sector read failed"};
        }
        marker_volume_boot(first_partition_lba);
    }
    const auto file_system_status = inspect_file_system();
    if (!file_system_status.is_ok())
    {
        return file_system_status;
    }
#endif

    marker("pass");
    return m110::Status::success();
#endif
}

} // namespace waveform_generator
