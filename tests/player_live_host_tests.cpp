// Compile the actual firmware owner loop and CDC service against deterministic
// host I/O stubs. This tests lifecycle/protocol logic, not interrupt timing,
// DMA coherency, analog output, or hardware acceptance.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "rt1170/player.cpp"
#include "rt1170/usb_checkpoint.cpp"
#include "rt1170/tx_artifact.hpp"
#include "rt1170/tx_usb.hpp"

namespace
{
int failures{};
bool media_local = true;
bool mounted{};
std::map<std::string, std::vector<std::uint8_t>> files;
unsigned int open_files{};
std::uint32_t cycles{};
m110::imxrt1170::CodecBlockHook audio_hook{};
void* audio_context{};
bool stop_on_next_wait{};
unsigned int wait_calls{};
std::deque<char> rx;
std::string tx;
std::size_t write_budget = 100000U;
bool cdc_connected = true;
TestOcotp test_ocotp{{{0}, {0x12345678}, {0x9ABCDEF0}}};

void check(bool condition, const char* message)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

std::vector<std::uint8_t> wav(std::size_t frames, std::int16_t sample)
{
    std::vector<std::uint8_t> result(44U + frames * 2U);
    const auto put16 = [&](std::size_t at, std::uint16_t value) {
        result[at] = static_cast<std::uint8_t>(value);
        result[at + 1U] = static_cast<std::uint8_t>(value >> 8U);
    };
    const auto put32 = [&](std::size_t at, std::uint32_t value) {
        for (unsigned int index = 0U; index < 4U; ++index) result[at + index] = static_cast<std::uint8_t>(value >> (8U * index));
    };
    std::memcpy(result.data(), "RIFF", 4U);
    put32(4U, static_cast<std::uint32_t>(result.size() - 8U));
    std::memcpy(result.data() + 8U, "WAVEfmt ", 8U);
    put32(16U, 16U); put16(20U, 1U); put16(22U, 1U);
    put32(24U, 48000U); put32(28U, 96000U); put16(32U, 2U); put16(34U, 16U);
    std::memcpy(result.data() + 36U, "data", 4U);
    put32(40U, static_cast<std::uint32_t>(frames * 2U));
    for (std::size_t index = 0U; index < frames; ++index) put16(44U + 2U * index, static_cast<std::uint16_t>(sample));
    return result;
}

waveform_generator::ControlReply command(const char* text)
{
    waveform_generator::live_protocol::Request request;
    std::uint32_t previous{};
    check(waveform_generator::live_protocol::parse(std::string("1 ") + text, previous, request), "test command parses");
    check(waveform_generator::submit_player_command(request), "owner mailbox accepts test command");
    waveform_generator::service_player_command();
    waveform_generator::ControlReply reply;
    check(waveform_generator::take_player_reply(reply), "owner publishes acknowledgement");
    return reply;
}

void run_selected()
{
    check(command("PLAY").ok, "selected playback is accepted");
    check(!waveform_generator::player_allows_media_host(), "pending playback owns media before mount");
    waveform_generator::play_requested = false; // The player task normally consumes this flag.
    wait_calls = 0U;
    waveform_generator::run_player();
    check(!mounted && open_files == 0U && !m110::imxrt1170::wm8960_codec().is_running(), "run cleanup closes media and stops codec");
    check(waveform_generator::player_allows_media_host(), "finished owner permits host media access");
}

void feed(const std::string& bytes)
{
    rx.insert(rx.end(), bytes.begin(), bytes.end());
}

void pump_usb()
{
    for (unsigned int attempt = 0U; attempt < 20U; ++attempt)
    {
        waveform_generator::service_control();
        if (waveform_generator::command_pending) waveform_generator::service_player_command();
        if (rx.empty() && !waveform_generator::response_size && !waveform_generator::player_reply_waiting) return;
    }
}

void test_owner_lifecycle()
{
    check(waveform_generator::start_player_checkpoint().is_ok(), "player creates static owner task");
    waveform_generator::reset_selection("PLAY.WAV");
    waveform_generator::publish_live();
    files["2:/WG/PLAY.WAV"] = wav(50000U, 1000);
    files["2:/WG/SECOND.WAV"] = wav(10000U, -2000);
    files["2:/WG/SILENT.WAV"] = wav(10000U, 0);
    run_selected();
    auto status = waveform_generator::player_snapshot();
    auto live = waveform_generator::player_live_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::done) && status.file_frames_submitted == 50000U &&
              status.pcm_drained == 1U && status.underruns == 0U, "clean run drains every frame without underrun");
    check(live.live_frame == 50000U && live.reference_rms == 1000.0 / 32768.0, "automatic reference measures first second and rewinds source");
    std::vector<std::int16_t> expected(50000U, 1000);
    signal_lab::det::StreamDigest digest;
    digest.update(expected.data(), expected.size());
    check(live.live_digest == digest.value(), "firmware live digest covers all clean source samples");

    check(command("LOAD:SECOND.WAV").ok && command("REFERENCE:0.125").ok && command("CW ON").ok, "stopped source and preset controls configure");
    check(!command("SEED:42").ok, "acknowledged preset cannot be silently discarded by seed change");
    run_selected();
    live = waveform_generator::player_live_snapshot();
    check(live.live_frame == 10000U && live.live_events == 1U && live.reference_rms == 0.125, "second run reuses configured codec and applies preserved preset");

    check(command("LOAD:PLAY.WAV").ok, "source reload resets completed capture");
    stop_on_next_wait = true;
    run_selected();
    status = waveform_generator::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::aborted) && status.pcm_drained == 0U, "STOP aborts producer pacing and releases ownership");
    // STOP acknowledgement was delivered through the same owner mailbox during pacing.
    waveform_generator::ControlReply stop_reply;
    check(waveform_generator::take_player_reply(stop_reply) && stop_reply.ok, "STOP returns owner acknowledgement");
    check(command("LOAD:SECOND.WAV").ok, "LOAD is available after STOP");
    run_selected();
    check(waveform_generator::player_snapshot().file_frames_submitted == 10000U, "playback can restart after STOP without board reset");

    check(command("LOAD:SILENT.WAV").ok && command("CW ON").ok, "silent source preset is staged for validation");
    run_selected();
    status = waveform_generator::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::fault) &&
              status.error == static_cast<std::uint32_t>(waveform_generator::PlayerError::live_control) && status.file_frames_enqueued == 0U,
          "silent source cannot start a reference-relative preset without a reference override");

    const std::string invalid = "{bad scenario}";
    files["2:/WG/SECOND.SCN"] = {invalid.begin(), invalid.end()};
    check(command("LOAD:SECOND.WAV").ok, "LOAD recovers from prior run fault");
    run_selected();
    status = waveform_generator::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::done) && status.engine_state == 2U && status.underruns == 0U,
          "rejected base scenario is reported and leaves clean playback available");
}

void test_usb_service()
{
    tx.clear(); rx.clear();
    waveform_generator::last_sequence = 0U;
    write_budget = 0U;
    feed("1 INFO?\n2 STATUS?\n");
    waveform_generator::service_control();
    check(tx.empty() && waveform_generator::response_size != 0U && !rx.empty(), "CDC backpressure keeps the next request unread");
    const auto remaining = rx.size();
    write_budget = 7U;
    waveform_generator::service_control();
    check(tx.size() == 7U && rx.size() == remaining, "partial response writes preserve request ordering");
    write_budget = 100000U;
    pump_usb();
    check(tx.find("\"seq\":1,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos &&
              tx.find("\"seq\":2,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos &&
              std::count(tx.begin(), tx.end(), '\n') == 2,
          "INFO and STATUS remain complete framed responses after partial writes");
    tx.clear();
    feed("2 PLAY\n3 UNKNOWN\n3 INFO?\n4 INFO?\n");
    pump_usb();
    check(std::count(tx.begin(), tx.end(), '\n') == 4 && tx.find("\"seq\":4,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos &&
              tx.find("BAD_REQUEST") != std::string::npos,
          "duplicate and malformed command sequences are rejected without preventing a later fresh sequence");
    tx.clear();
    feed(std::string("5 CW ") + std::string(210U, 'X') + "\n6 INFO?\n");
    pump_usb();
    check(tx.find("LINE_TOO_LONG") != std::string::npos && tx.find("\"seq\":6,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos,
          "oversized lines are drained and the next request remains intact");
    tx.clear();
    feed("7 CW FREQ ");
    pump_usb();
    cdc_connected = false;
    waveform_generator::service_control();
    cdc_connected = true;
    feed("1 INFO?\n");
    pump_usb();
    check(tx.find("\"seq\":1,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos && waveform_generator::control_line_size == 0U,
          "disconnect discards a partial line and resets connection sequence");
    tx.clear();
    feed("2 LOAD:SECOND.WAV\n");
    pump_usb();
    check(tx.find("\"accepted\":true") != std::string::npos, "CDC mutation waits for and returns producer acknowledgement");
}
} // namespace

TestOcotp* OCOTP = &test_ocotp;

extern "C"
{
TaskHandle_t xTaskCreateStatic(void (*)(void*), const char*, std::uint32_t, void*, UBaseType_t, StackType_t*, StaticTask_t*) { return reinterpret_cast<void*>(1); }
void xTaskNotifyGive(TaskHandle_t) {}
void vTaskSuspend(TaskHandle_t) { std::abort(); }
std::uint32_t ulTaskNotifyTake(BaseType_t, TickType_t ticks)
{
    if (++wait_calls > 10000U) { std::fputs("simulated player stalled\n", stderr); std::abort(); }
    if (stop_on_next_wait)
    {
        stop_on_next_wait = false;
        waveform_generator::live_protocol::Request request;
        request.kind = waveform_generator::live_protocol::Kind::stop;
        check(waveform_generator::submit_player_command(request), "STOP enters owner mailbox during pacing");
        return 1U;
    }
    if (m110::imxrt1170::wm8960_codec().is_running() && audio_hook != nullptr)
    {
        std::uint32_t playback[256]{};
        const auto frames = std::min<std::uint32_t>(ticks, 1000U) * 48U;
        for (std::uint32_t done = 0U; done < frames; done += 128U) audio_hook(audio_context, nullptr, playback, 128U);
    }
    return 0U;
}
void vTaskDelay(TickType_t ticks) { (void)ulTaskNotifyTake(pdTRUE, ticks); }
FRESULT f_mount(FATFS* filesystem, const char*, unsigned char) { mounted = filesystem != nullptr; return FR_OK; }
FRESULT f_open(FIL* file, const char* path, unsigned char)
{
    const auto found = files.find(path);
    if (!mounted || found == files.end()) return FR_NO_FILE;
    file->data = found->second.data(); file->size = static_cast<FSIZE_t>(found->second.size()); file->position = 0U; file->opened = 1;
    ++open_files;
    return FR_OK;
}
FRESULT f_read(FIL* file, void* destination, UINT requested, UINT* actual)
{
    if (!file->opened) return FR_DISK_ERR;
    *actual = std::min<UINT>(requested, file->size - file->position);
    std::memcpy(destination, file->data + file->position, *actual);
    file->position += *actual;
    return FR_OK;
}
FRESULT f_lseek(FIL* file, FSIZE_t offset) { if (offset > file->size) return FR_DISK_ERR; file->position = offset; return FR_OK; }
FRESULT f_close(FIL* file) { if (file->opened) --open_files; file->opened = 0; return FR_OK; }
bool WFG_MediaIsLocal() { return media_local; }
bool WFG_MediaSetLocal() { media_local = true; return true; }
bool WFG_MediaSetHost() { media_local = false; return true; }
wfg_media_host_request_result_t WFG_MediaRequestHost() { media_local = false; return kWFG_MediaHostRequestReady; }
wfg_media_snapshot_t WFG_MediaGetSnapshot() { wfg_media_snapshot_t snapshot{}; snapshot.state = media_local ? 3U : 2U; return snapshot; }
bool m110_usb_hardware_initialize() { return true; }
bool m110_tinyusb_stack_initialize() { return true; }
void m110_tinyusb_task(std::uint32_t) {}
void m110_tinyusb_cdc_flush() {}
bool m110_tinyusb_cdc_connected() { return cdc_connected; }
std::uint32_t m110_tinyusb_cdc_available() { return static_cast<std::uint32_t>(rx.size()); }
std::uint32_t m110_tinyusb_cdc_read(void* buffer, std::uint32_t size)
{
    const auto count = std::min<std::uint32_t>(size, static_cast<std::uint32_t>(rx.size()));
    auto* output = static_cast<char*>(buffer);
    for (std::uint32_t index = 0U; index < count; ++index) { output[index] = rx.front(); rx.pop_front(); }
    return count;
}
std::uint32_t m110_tinyusb_cdc_write(const void* buffer, std::uint32_t size)
{
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(size, write_budget));
    tx.append(static_cast<const char*>(buffer), count); write_budget -= count; return count;
}
}

namespace m110
{
std::uint32_t platform_cycle_counter() noexcept { cycles += 100U; return cycles; }
std::uint32_t platform_cycle_counter_frequency_hz() noexcept { return 996000000U; }
}
namespace m110::imxrt1170
{
Wm8960Codec& wm8960_codec() noexcept { static Wm8960Codec codec; return codec; }
Status Wm8960Codec::configure(const CodecConfig& config, CodecBlockHook hook, void* context) noexcept
{
    configured_ = true; frames_ = config.frames_per_block; audio_hook = hook; audio_context = context; return Status::success();
}
Status Wm8960Codec::start() noexcept { running_ = true; return Status::success(); }
Status Wm8960Codec::stop() noexcept { running_ = false; return Status::success(); }
Result<std::uint8_t> Wm8960Codec::set_transmit_level_percent(std::uint8_t value) noexcept { return value; }
}
namespace waveform_generator
{
bool tx_artifact_busy() noexcept { return false; }
bool submit_tx_request(const tx_protocol::Request&) noexcept { return false; }
bool take_tx_reply(tx_protocol::Reply&) noexcept { return false; }
TxArtifactSnapshot tx_artifact_snapshot() noexcept { return {}; }
void service_tx_artifact() noexcept {}
void abort_tx_artifact() noexcept {}
bool tx_usb_active() noexcept { return false; }
bool tx_usb_blocked() noexcept { return false; }
void tx_usb_feed(std::uint8_t) noexcept {}
void tx_usb_service() noexcept {}
void tx_usb_disconnect() noexcept {}
}

int main()
{
    test_owner_lifecycle();
    test_usb_service();
    if (failures == 0) { std::puts("player/USB host lifecycle tests: PASS"); return 0; }
    std::printf("player/USB host lifecycle tests: %d failures\n", failures);
    return 1;
}
