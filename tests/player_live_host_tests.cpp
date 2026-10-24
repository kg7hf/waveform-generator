// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

// Compile the actual firmware owner loop and CDC service against deterministic
// host I/O stubs. This tests lifecycle/protocol logic, not interrupt timing,
// DMA coherency, analog output, or hardware acceptance. The USB route runs the
// real sound-card stream (usb_audio.cpp) under a simulated host and USB ISR.
#include <algorithm>
#include <array>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "common/crc32.hpp"
#include "live_protocol_corpus.hpp"
#include "rt1170/card_listing.hpp"
#include "rt1170/player.cpp"
#include "rt1170/tx_artifact.hpp"
#include "rt1170/tx_usb.hpp"
#include "rt1170/usb_service.cpp"
#include "usb/usb_audio_callbacks.h"

namespace
{
bool media_local = true;
bool mounted{};
std::map<std::string, std::vector<std::uint8_t>> files;
std::map<const FIL*, std::string> opened_file_paths;
unsigned int open_files{};
struct MockDirectoryEntry
{
    std::string name;
    std::string alias;
    FSIZE_t size{};
    std::uint8_t attributes{};
};

struct DirectoryFaults
{
    bool fail_mount{};
    bool fail_unmount{};
    bool fail_open{};
    bool fail_close{};
    std::optional<unsigned int> reads_before_failure{};
    unsigned int mount_calls{};
    unsigned int unmount_calls{};
    unsigned int open_attempts{};
    unsigned int successful_opens{};
    unsigned int read_calls{};
    unsigned int close_calls{};
    unsigned int open_directories{};
    std::string last_path{};
};

std::vector<MockDirectoryEntry> directory_entries;

[[nodiscard]] DirectoryFaults& directory_faults()
{
    static DirectoryFaults faults{};
    return faults;
}

void reset_directory_fixture()
{
    directory_entries.clear();
    directory_faults() = {};
}

// The card's faults: a seek to this offset fails, and from this read on
// (0: the next) every read fails.
struct CardFaults
{
    std::optional<FSIZE_t> seek;
    std::optional<unsigned int> reads_before_failure;
    std::string close_path{};
    bool fail_close{};
    unsigned int close_calls{};
};

struct SecondaryCardFaults
{
    bool fail_open{};
    bool fail_close{};
    std::optional<FSIZE_t> seek;
    std::optional<unsigned int> reads_before_failure;
    std::optional<unsigned int> reads_before_short;
    unsigned int open_attempts{};
    unsigned int successful_opens{};
    unsigned int read_calls{};
    unsigned int close_calls{};
    unsigned int seek_calls{};
    FSIZE_t last_seek{};
    std::function<void()> after_read{};
};

[[nodiscard]] SecondaryCardFaults& secondary_card_faults()
{
    static SecondaryCardFaults faults{};
    return faults;
}

[[nodiscard]] int& secondary_file_tag()
{
    static int tag{};
    return tag;
}

[[nodiscard]] bool is_secondary_file(const FIL* file) noexcept
{
    return file != nullptr && file->context == &secondary_file_tag();
}

[[nodiscard]] bool is_secondary_path(std::string_view path) noexcept
{
    return path == "2:/WG/SECOND.WAV" || path == "2:/WG/SILENT.WAV";
}

[[nodiscard]] CardFaults& card_faults()
{
    static CardFaults faults{};
    return faults;
}

// f_read() calls long enough to hold a whole 512-byte sector whose destination
// is not at the file position's place within a 32-bit word: on the board
// FatFs then hands the SD driver a misaligned buffer for the read's whole
// sectors, which it reads one block per command. (Shorter reads, such as the
// WAV header's, never transfer a sector straight into their destination.)
constexpr UINT sector_bytes = 512U;

[[nodiscard]] unsigned int& unaligned_sector_reads()
{
    static unsigned int count{};
    return count;
}

std::uint32_t cycles{};
m110::imxrt1170::CodecBlockHook audio_hook{};
void* audio_context{};
bool stop_on_next_wait{};

// While set, the player's waits run no codec blocks: the codec's output
// takes no frames, as a stand-in for a drain that lasts longer.
[[nodiscard]] bool& codec_blocks_held()
{
    static bool held{};
    return held;
}

unsigned int wait_calls{};
// The simulated player's waits before it is taken for stalled: a run's are
// far fewer; a playlist's gaps take more (playlist_wait_call_limit).
constexpr unsigned int default_wait_call_limit = 10000U;

[[nodiscard]] unsigned int& wait_call_limit()
{
    static unsigned int limit = default_wait_call_limit;
    return limit;
}

std::deque<char> rx;
std::string tx;
std::size_t write_budget = 100000U;
bool cdc_connected = true;
TestOcotp test_ocotp{{{0}, {0x12345678}, {0x9ABCDEF0}}};
bool player_task_created = true;
unsigned int player_task_creations{};

// Inputs of the WFG_MediaGetSnapshot() and tx_artifact_snapshot() link-seam
// stubs below. By default they report an idle board; the status-contract
// fixture drives them to their maxima for one render.
struct StatusSeam
{
    std::optional<wfg_media_snapshot_t> media{};
    waveform_generator::TxArtifactSnapshot tx_artifact{};
    bool tx_artifact_busy{}; // tx_artifact_busy(): a file generation runs
};

// The one instance the stubs read; ServiceStateGuard restores it.
[[nodiscard]] StatusSeam& status_seam()
{
    static StatusSeam seam{};
    return seam;
}

// What the USB bring-up stand-ins below return (m110_usb_hardware_initialize,
// m110_tinyusb_stack_initialize, and xTaskCreateStatic for the "wfg_usb"
// task), and the calls the USB service made of them.
struct UsbBringUp
{
    wfg_status hardware{kWFG_StatusOk};
    wfg_status stack{kWFG_StatusOk};
    bool task_created{true};
    unsigned int hardware_starts{};
    unsigned int task_creations{};
};

[[nodiscard]] UsbBringUp& usb_bring_up()
{
    static UsbBringUp bring_up{};
    return bring_up;
}

// The --usb-task runs: usb_task_entry() after a TinyUSB start that fails or
// succeeds. The task never returns (it suspends itself after a failed start,
// and otherwise runs TinyUSB's task for ever), so the stand-ins of
// vTaskSuspend and m110_tinyusb_task end the process, exit 0 when the run
// expected that call.
enum class UsbTaskRun : std::uint8_t
{
    none,
    stack_fails,
    stack_starts,
};

[[nodiscard]] UsbTaskRun& usb_task_run()
{
    static UsbTaskRun run{UsbTaskRun::none};
    return run;
}

// Ends a --usb-task run at a call it made: `expected` is the run's, and
// `what` says what the task did.
[[noreturn]] void end_usb_task_run(bool expected, const char* what)
{
    std::fputs(expected ? "as expected: " : "FAIL: ", stdout);
    std::fputs(what, stdout);
    std::fputs("\n", stdout);
    static_cast<void>(std::fflush(stdout));
    std::_Exit(expected ? EXIT_SUCCESS : EXIT_FAILURE);
}

void check(bool condition, const char* message)
{
    INFO(message);
    CHECK(condition);
}

namespace ua = waveform_generator::usb_audio;

// Whether `destination` sits at file byte `position`'s place within a 32-bit
// word, so that the file's sector boundaries fall on word boundaries of the
// destination. std::align gives the padding to the next word boundary.
[[nodiscard]] bool word_aligned_with(void* destination, std::size_t position)
{
    constexpr std::size_t word = sizeof(std::uint32_t);
    std::size_t space = 2U * word;
    void* aligned = destination;

    if (std::align(word, 1U, aligned, space) == nullptr)
    {
        return false;
    }

    const std::size_t padding = 2U * word - space;
    return (padding + position) % word == 0U;
}

// TinyUSB's 16-bit FIFO counts and 11-bit frame number; both values fit.
[[nodiscard]] std::uint16_t to_u16(std::size_t value) noexcept
{
    return static_cast<std::uint16_t>(std::min<std::size_t>(value, std::numeric_limits<std::uint16_t>::max()));
}

// What a play on the USB route does to the host part way, counted in
// milliseconds of the play being armed.
struct ArmedPlayEvents
{
    std::optional<std::uint32_t> close_stream_at{};       // alternate 0
    std::optional<std::uint32_t> stop_polling_at{};       // the host stops polling
    std::optional<std::uint32_t> resume_polling_at{};     // and polls again
    std::optional<std::uint32_t> send_stop_at{};          // the player receives STOP
    std::optional<std::size_t> close_after_file_frames{}; // alternate 0 right after the completion that confirms this many sent
    std::optional<std::uint32_t> skip_after_disarm{};     // once the play disarms with file frames queued, the host polls nothing for this many milliseconds
    std::optional<std::uint32_t> late_isr_at{};           // the USB ISR for this millisecond's completion is held off late_isr_by milliseconds
    std::uint32_t late_isr_by{};
    std::optional<std::uint32_t> late_read_at{};     // this millisecond's completion is a late read (UsbHost::run_isr), and the player task runs straight after it
    bool late_read_last{};                           // the same for the completion that confirms the play's last file frames sent
    std::optional<std::uint32_t> switch_format_at{}; // the host selects switch_to's alternate directly, never alternate 0
    ua::StreamFormat switch_to{};
    std::optional<std::uint32_t> short_reports_at{}; // from this millisecond on, short_reports completions report 0 bytes for packets the host received whole (#45)
    std::uint32_t short_reports{};
    std::optional<std::uint32_t> lost_reports_at{}; // from this millisecond on, lost_reports completions report 0 bytes for packets the host never received
    std::uint32_t lost_reports{};
    std::optional<std::uint32_t> starve_loads_at{}; // from this millisecond on, the controller loads nothing from the FIFO (UsbHost::starve_loads)
};

// The PC side of the sound card's capture stream and TinyUSB's IN FIFO behind
// the m110_tinyusb_audio_in_* shim. Every millisecond is one USB frame and one
// RTOS tick: a polling host completes the packet in flight, the controller
// loads the next one from the FIFO, and the USB ISR reports the completion
// (wfg_usb_audio_tx_done). In each millisecond the host leaves out, the
// service watchdog reports the packet still primed (wfg_usb_audio_watchdog_expired).
// The player's waits (ulTaskNotifyTake) let time pass.
class UsbHost
{
public:
    // A new session in `format` (alternate 1's unless given): the frame
    // numbers restart just below the 11-bit wrap, so a play's first frames
    // cross it and the extended number is the frame count.
    void open_stream(const ua::StreamFormat& format = ua::pcm24_stereo)
    {
        fifo_.clear();
        in_flight_.clear();
        sent_.clear();
        before_switch_.clear();
        completions_.clear();
        frame_ = first_frame;
        watchdog_running_ = false;
        isr_held_off_ = 0U;
        late_reads_ = 0U;
        short_reports_ = 0U;
        lost_reports_ = 0U;
        starved_ = false;
        tick_skew_ms_ = 0U;
        select(format);
        wfg_usb_audio_configure_stream();
        check(wfg_usb_audio_open_stream(format.alternate) == kWFG_UacRequestAccepted, "the host opens the capture stream");
        stream_open_ = true;
        polling_ = true;
    }

    // The host selects another format straight away: SET_INTERFACE from one
    // format's alternate to another's, never alternate 0. TinyUSB leaves the
    // open alternate (the close callback), drops its FIFO and the packet in
    // flight, opens the endpoint with the new wMaxPacketSize behind a
    // zero-length primer, then calls the open callback. What the host had
    // received is kept apart (received_before_switch()); the stream's frame
    // indices restart.
    void switch_format(const ua::StreamFormat& format)
    {
        wfg_usb_audio_close_stream();
        fifo_.clear();
        in_flight_.clear();
        before_switch_ = std::exchange(sent_, {});
        completions_.clear();
        watchdog_running_ = false;
        select(format);
        check(wfg_usb_audio_open_stream(format.alternate) == kWFG_UacRequestAccepted, "the host selects another format directly");
    }

    // Alternate 0: TinyUSB drops its FIFO and the packet in flight.
    void close_stream()
    {
        wfg_usb_audio_close_stream();
        fifo_.clear();
        in_flight_.clear();
        stream_open_ = false;
        watchdog_running_ = false;
    }

    // The host takes the packet in flight now, but the USB ISR that handles
    // the completion is held off `late_by` milliseconds, past the host's poll
    // in each of them and in the one it runs in: those polls find nothing
    // primed. The service watchdog expired meanwhile, so the ISR reports that
    // first. Then the host polls again every millisecond.
    void complete_with_late_isr(std::uint32_t late_by)
    {
        isr_held_off_ = late_by;
        millisecond();
    }

    // The host polls nothing until resume_polling() or a scheduled resume.
    void stop_polling() noexcept { polling_ = false; }

    void resume_polling() noexcept { polling_ = true; }

    // The host polls nothing for `milliseconds`, then polls again.
    void skip_polling(std::uint32_t milliseconds)
    {
        polling_ = false;
        run(milliseconds);
        polling_ = true;
    }

    // The controller loads nothing after the next completion, so the one after
    // it is a zero-length packet (a short packet while streaming). The FIFO
    // keeps its bytes for the interval after that.
    void hold_next_load() noexcept { hold_next_load_ = true; }

    // The next `completions` completions report 0 bytes, though the host
    // receives each packet whole: as the ChipIdea driver reports a transfer
    // that ended in error, expected minus remaining (#45).
    void report_short(std::uint32_t completions) noexcept { short_reports_ = completions; }

    // The next `completions` completions report 0 bytes, and the host receives
    // nothing in them: an ISO IN transfer that ended in error before its data
    // went out. The board cannot tell these from report_short()'s.
    void lose_packets(std::uint32_t completions) noexcept { lost_reports_ = completions; }

    // From now on, until feed_loads(), the controller loads nothing after a
    // completion: the host keeps polling and completes zero-length packets,
    // and the FIFO keeps its bytes. A stand-in for a stream whose
    // completions keep coming while none of the play's queued chunks goes
    // out.
    void starve_loads() noexcept { starved_ = true; }

    void feed_loads() noexcept { starved_ = false; }

    // Called at the start of every millisecond until replaced; an empty
    // function calls nothing. The PLAYLIST tests act there on what the player
    // task has done so far.
    void on_tick(std::function<void()> hook) { tick_hook_ = std::move(hook); }

    // Called right after every completion the USB ISR reported, until
    // replaced; an empty function calls nothing.
    void on_completion(std::function<void()> hook) { completion_hook_ = std::move(hook); }

    void schedule(const ArmedPlayEvents& events) noexcept
    {
        events_ = events;
        armed_milliseconds_ = 0U;
        media_host_allowed_while_armed_ = false;
        fault_tick_.reset();
        resume_tick_.reset();
        resume_frame_index_.reset();
        skip_ends_at_.reset();
        was_armed_ = false;
    }

    // `milliseconds` of USB time, cut short by a late read: the player task
    // runs straight after one, before the next completion can take it back.
    void run(std::uint32_t milliseconds)
    {
        wake_player_ = false;

        for (std::uint32_t elapsed = 0U; elapsed < milliseconds && !wake_player_; ++elapsed)
        {
            millisecond();
        }
    }

    // The late reads since the stream opened.
    [[nodiscard]] std::uint32_t late_reads() const noexcept { return late_reads_; }

    [[nodiscard]] std::uint16_t queued() const noexcept { return to_u16(fifo_.size()); }

    [[nodiscard]] std::uint16_t free_space() const noexcept { return to_u16(ua::fifo_capacity_bytes.value - fifo_.size()); }

    // The next write is refused, whatever room the FIFO has.
    void refuse_next_write() noexcept { refuse_next_write_ = true; }

    // From now on the RTOS tick runs faster than the stream: every
    // `milliseconds` USB frames it gains one tick, so with 10 it runs 10 %
    // fast. 0 (as at every open_stream()) keeps the two in step.
    void skew_ticks(std::uint32_t milliseconds) noexcept { tick_skew_ms_ = milliseconds; }

    // Unlike TinyUSB's FIFO, a write that does not fit is refused.
    [[nodiscard]] std::uint16_t write(std::span<const std::uint8_t> bytes)
    {
        if (std::exchange(refuse_next_write_, false) || fifo_.size() + bytes.size() > ua::fifo_capacity_bytes.value)
        {
            return 0U;
        }

        fifo_.insert(fifo_.end(), bytes.begin(), bytes.end());
        return to_u16(bytes.size());
    }

    // Every frame the host received since the stream opened, in order, read
    // as the 24-bit stereo format.
    [[nodiscard]] std::vector<ua::StereoFrame> received() const { return stereo_frames(sent_); }

    // As received(), of the stream before the last switch_format().
    [[nodiscard]] std::vector<ua::StereoFrame> received_before_switch() const { return stereo_frames(before_switch_); }

    // Every byte the host received since the stream opened, in order.
    [[nodiscard]] const std::vector<std::uint8_t>& received_bytes() const noexcept { return sent_; }

    // How many frames of the stream's format the host received since it opened.
    [[nodiscard]] std::size_t received_frames() const noexcept { return sent_.size() / frame_bytes_; }

    [[nodiscard]] std::uint32_t tick() const noexcept { return tick_; }

    // The tick of the latest completion the USB ISR reported.
    [[nodiscard]] std::uint32_t last_completion_tick() const noexcept { return last_completion_tick_; }

    // The tick at which the player task latched a fault since schedule(): the
    // task runs between milliseconds, so the next one sees it at that tick.
    [[nodiscard]] std::optional<std::uint32_t> fault_tick() const noexcept { return fault_tick_; }

    // The tick of the first completion after a scheduled resume_polling_at or
    // skip_after_disarm.
    [[nodiscard]] std::optional<std::uint32_t> resume_tick() const noexcept { return resume_tick_; }

    // The stream index of the first frame that completion delivered: the
    // frames received before it.
    [[nodiscard]] std::optional<std::size_t> resume_frame_index() const noexcept { return resume_frame_index_; }

    // The frame number the controller reported for the completion that
    // delivered stream frame `index`, extended across wraps.
    [[nodiscard]] std::uint32_t frame_that_sent(std::size_t index) const noexcept
    {
        const auto found = std::ranges::find_if(completions_, [index](const auto& completion) { return completion.second > index; });

        return found == completions_.end() ? 0U : found->first;
    }

    [[nodiscard]] std::uint32_t armed_milliseconds() const noexcept { return armed_milliseconds_; }

    [[nodiscard]] bool media_host_allowed_while_armed() const noexcept { return media_host_allowed_while_armed_; }

private:
    [[nodiscard]] static std::vector<ua::StereoFrame> stereo_frames(const std::vector<std::uint8_t>& bytes)
    {
        std::vector<ua::StereoFrame> frames(bytes.size() / sizeof(ua::StereoFrame));
        std::memcpy(frames.data(), bytes.data(), frames.size() * sizeof(ua::StereoFrame));
        return frames;
    }

    // The endpoint's packet and frame sizes in `format`.
    void select(const ua::StreamFormat& format) noexcept
    {
        packet_bytes_ = ua::packet_bytes(format).value;
        frame_bytes_ = ua::frame_bytes(format).value;
    }

    void millisecond()
    {
        if (tick_hook_)
        {
            tick_hook_();
        }

        if (!fault_tick_.has_value() && waveform_generator::faulted)
        {
            fault_tick_ = tick_;
        }

        observe_armed_play();
        observe_disarm();

        if (stream_open_ && polling_)
        {
            const bool late = late_read_due();
            const auto completed = take_in_flight();
            hold_isr_off();
            run_isr(completed, late);
            observe_completion();
            wake_player_ = late;

            if (completion_hook_)
            {
                completion_hook_();
            }
        }
        else if (stream_open_ && watchdog_running_)
        {
            // The service watchdog samples half a millisecond after each poll
            // the host leaves out, with the packet the last completion primed
            // still waiting.
            wfg_usb_audio_watchdog_expired(true);
        }

        ++frame_;
        ++tick_;

        if (tick_skew_ms_ != 0U && frame_ % tick_skew_ms_ == 0U)
        {
            ++tick_;
        }
    }

    // A held-off USB ISR (complete_with_late_isr, late_isr_at) runs that many
    // milliseconds after the completion, and reports the watchdog's expiry
    // first, with nothing primed.
    void hold_isr_off()
    {
        const std::uint32_t held_off = std::exchange(isr_held_off_, 0U);

        if (held_off == 0U)
        {
            return;
        }

        frame_ += held_off;
        tick_ += held_off;
        wfg_usb_audio_watchdog_expired(false);
    }

    // The host completes the packet in flight; returns the size the
    // controller reports for it (report_short(), lose_packets()).
    [[nodiscard]] std::uint16_t take_in_flight()
    {
        const bool lost = lost_reports_ != 0U;
        const auto completed = short_reports_ == 0U && !lost ? to_u16(in_flight_.size()) : std::uint16_t{};
        short_reports_ -= std::min(short_reports_, 1U);
        lost_reports_ -= std::min(lost_reports_, 1U);

        if (!lost)
        {
            sent_.insert(sent_.end(), in_flight_.begin(), in_flight_.end());
        }

        completions_.emplace_back(frame_, sent_.size() / frame_bytes_);
        in_flight_.clear();
        return completed;
    }

    // Whether this millisecond's completion is a scheduled late read: at
    // late_read_at, or with late_read_last the one that retires the play's
    // last file frames (one chunk's worth or fewer queued, after its first
    // were confirmed).
    [[nodiscard]] bool late_read_due() const
    {
        const auto play = ua::play_statistics();
        const bool scheduled = play.armed && events_.late_read_at == armed_milliseconds_;
        const bool last = events_.late_read_last && play.file_frames != ua::FrameCount{} && play.pending_frames != ua::FrameCount{} && play.pending_frames <= ua::chunk_frames;
        return scheduled || last;
    }

    // The USB ISR for a completion of `completed` bytes: the controller loads
    // the next packet from the FIFO, the watchdog restarts, and the ISR
    // reports the completion. A late read is handled just past the end of its
    // frame, so it reads the next frame's number: a gap of 2 frames, then a
    // gap of 0 to the next completion, which comes in that frame.
    void run_isr(std::uint16_t completed, bool late = false)
    {
        last_completion_tick_ = tick_;
        const bool load = !std::exchange(hold_next_load_, false) && !starved_;

        while (load && !fifo_.empty() && in_flight_.size() < packet_bytes_)
        {
            in_flight_.push_back(fifo_.front());
            fifo_.pop_front();
        }

        watchdog_running_ = true;
        const std::uint32_t read_frame = late ? frame_ + 1U : frame_;
        wfg_usb_audio_tx_done(completed, to_u16(read_frame & ua::usb_frame_number_mask), tick_);

        if (late)
        {
            ++late_reads_;
        }
    }

    // Closes the stream right after the completion that confirms the scheduled
    // number of file frames sent, while the play is still armed.
    void observe_completion()
    {
        const auto play = ua::play_statistics();

        if (play.armed && events_.close_after_file_frames == std::size_t{play.file_frames.value})
        {
            close_stream();
        }
    }

    // Probes MEDIA HOST while the play is armed and fires the scheduled events.
    void observe_armed_play()
    {
        if (!ua::play_statistics().armed)
        {
            return;
        }

        ++armed_milliseconds_;
        media_host_allowed_while_armed_ = media_host_allowed_while_armed_ || waveform_generator::player_allows_media_host();

        if (events_.close_stream_at == armed_milliseconds_)
        {
            close_stream();
        }

        if (events_.stop_polling_at == armed_milliseconds_)
        {
            polling_ = false;
        }

        if (events_.resume_polling_at == armed_milliseconds_)
        {
            polling_ = true;
            resume_tick_ = tick_;
            resume_frame_index_ = sent_.size() / frame_bytes_;
        }

        if (events_.late_isr_at == armed_milliseconds_)
        {
            isr_held_off_ = events_.late_isr_by;
            resume_frame_index_ = sent_.size() / frame_bytes_;
        }

        if (events_.switch_format_at == armed_milliseconds_)
        {
            switch_format(events_.switch_to);
        }

        if (events_.short_reports_at == armed_milliseconds_)
        {
            report_short(events_.short_reports);
        }

        if (events_.lost_reports_at == armed_milliseconds_)
        {
            lose_packets(events_.lost_reports);
        }

        if (events_.starve_loads_at == armed_milliseconds_)
        {
            starve_loads();
        }

        if (events_.send_stop_at == armed_milliseconds_)
        {
            waveform_generator::live_protocol::Request request;
            request.kind = waveform_generator::live_protocol::Kind::stop;
            check(waveform_generator::submit_player_command(request).is_ok(), "STOP enters the owner mailbox during a USB play");
        }
    }

    // Starts the scheduled skip_after_disarm in the first millisecond after the
    // play disarms with file frames still queued, and ends it: the host then
    // polls again and the queued chunks go out.
    void observe_disarm()
    {
        const auto play = ua::play_statistics();

        if (was_armed_ && !play.armed && play.pending_frames != ua::FrameCount{} && events_.skip_after_disarm.has_value())
        {
            polling_ = false;
            skip_ends_at_ = tick_ + events_.skip_after_disarm.value_or(0U);
        }

        was_armed_ = play.armed;

        if (skip_ends_at_ == tick_)
        {
            polling_ = true;
            skip_ends_at_.reset();
            resume_tick_ = tick_;
            resume_frame_index_ = sent_.size() / frame_bytes_;
        }
    }

    std::deque<std::uint8_t> fifo_;
    std::vector<std::uint8_t> in_flight_;
    std::vector<std::uint8_t> sent_;
    std::vector<std::uint8_t> before_switch_;
    std::size_t packet_bytes_{ua::packet_bytes(ua::pcm24_stereo).value};
    std::size_t frame_bytes_{ua::frame_bytes(ua::pcm24_stereo).value};
    std::vector<std::pair<std::uint32_t, std::size_t>> completions_;
    ArmedPlayEvents events_{};
    std::optional<std::uint32_t> fault_tick_{};
    std::optional<std::uint32_t> resume_tick_{};
    std::optional<std::size_t> resume_frame_index_{};
    std::optional<std::uint32_t> skip_ends_at_{};
    static constexpr std::uint32_t first_frame = 2040U;
    std::uint32_t frame_{first_frame};
    std::uint32_t tick_{};
    std::uint32_t last_completion_tick_{};
    std::uint32_t armed_milliseconds_{};
    std::uint32_t isr_held_off_{};
    std::uint32_t late_reads_{};
    std::uint32_t short_reports_{};
    std::uint32_t lost_reports_{};
    std::uint32_t tick_skew_ms_{};
    bool stream_open_{};
    bool polling_{true};
    bool wake_player_{};
    bool watchdog_running_{};
    bool hold_next_load_{};
    bool refuse_next_write_{};
    bool starved_{};
    bool media_host_allowed_while_armed_{};
    bool was_armed_{};
    std::function<void()> tick_hook_{};
    std::function<void()> completion_hook_{};
};

[[nodiscard]] UsbHost& usb_host()
{
    static UsbHost host;
    return host;
}

// The simulated producer's speed: each read of a whole card block while the
// output plays lets this many milliseconds of USB time pass, as a slower SD
// card or engine would; 0 (the default) costs none.
[[nodiscard]] std::uint32_t& producer_block_ms()
{
    static std::uint32_t milliseconds{};
    return milliseconds;
}

// The canonical 44-byte header: RIFF, a 16-byte fmt chunk, the data chunk header.
constexpr std::size_t wav_header_bytes = 44U;

// A mono 48 kHz PCM24 WAV of the given samples, its data chunk at byte 44.
std::vector<std::uint8_t> pcm24_wav(std::span<const std::int32_t> samples)
{
    const std::size_t frames = samples.size();
    std::vector<std::uint8_t> result(44U + frames * 3U);
    const auto put16 = [&](std::size_t at, std::uint16_t value)
    {
        result[at] = static_cast<std::uint8_t>(value);
        result[at + 1U] = static_cast<std::uint8_t>(value >> 8U);
    };

    const auto put32 = [&](std::size_t at, std::uint32_t value)
    {
        for (unsigned int index = 0U; index < 4U; ++index)
        {
            result[at + index] = static_cast<std::uint8_t>(value >> (8U * index));
        }
    };

    std::memcpy(result.data(), "RIFF", 4U);
    put32(4U, static_cast<std::uint32_t>(result.size() - 8U));
    std::memcpy(result.data() + 8U, "WAVEfmt ", 8U);
    put32(16U, 16U);
    put16(20U, 1U);
    put16(22U, 1U);
    put32(24U, 48000U);
    put32(28U, 144000U);
    put16(32U, 3U);
    put16(34U, 24U);
    std::memcpy(result.data() + 36U, "data", 4U);
    put32(40U, static_cast<std::uint32_t>(frames * 3U));
    const auto data_chunk = std::span{result}.subspan(wav_header_bytes);

    for (std::size_t index = 0U; index < frames; ++index)
    {
        waveform_generator::audio::write_pcm24_le(data_chunk.subspan(waveform_generator::audio::pcm24_bytes_per_sample * index).data(), samples[index]);
    }

    return result;
}

std::vector<std::uint8_t> wav(std::size_t frames, std::int16_t sample)
{
    constexpr std::int32_t pcm16_to_pcm24 = 256;
    return pcm24_wav(std::vector<std::int32_t>(frames, std::int32_t{sample} * pcm16_to_pcm24));
}

waveform_generator::ControlReply command(const char* text)
{
    std::uint32_t previous{};
    const auto parsed = waveform_generator::live_protocol::parse(std::string("1 ") + text, previous);

    if (!parsed.has_value())
    {
        const auto length = static_cast<int>(std::min<std::size_t>(std::strlen(text), 128U));
        std::fprintf(stderr, "command parse diagnostic: `%.*s`\n", length, text);
    }

    check(parsed.has_value(), "test command parses");
    const auto request = parsed.has_value() ? parsed.value() : waveform_generator::live_protocol::Request{};
    check(waveform_generator::submit_player_command(request).is_ok(), "owner mailbox accepts test command");
    waveform_generator::service_player_command();
    waveform_generator::ControlReply reply;
    check(waveform_generator::take_player_reply(reply), "owner publishes acknowledgement");
    return reply;
}

// After an accepted PLAY: `usb_milliseconds` of USB time, then the player task
// runs it.
void run_accepted(std::uint32_t usb_milliseconds = 0U, bool expect_held_cleanup = false)
{
    usb_host().run(usb_milliseconds);
    check(!waveform_generator::player_allows_media_host(), "pending playback owns media before mount");
    waveform_generator::play_requested = false; // The player task normally consumes this flag.
    wait_calls = 0U;
    waveform_generator::run_player();

    if (expect_held_cleanup)
    {
        check(mounted && open_files != 0U && !m110::imxrt1170::wm8960_codec().is_running(), "failed close keeps media mounted and owned after stopping the codec");
        check(!waveform_generator::player_allows_media_host(), "failed cleanup retains exclusive media ownership");
    }
    else
    {
        check(!mounted && open_files == 0U && !m110::imxrt1170::wm8960_codec().is_running(), "run cleanup closes media and stops codec");
        check(waveform_generator::player_allows_media_host(), "finished owner permits host media access");
    }
}

// PLAY, then `usb_milliseconds` of USB time before the player task runs it.
void run_selected(std::uint32_t usb_milliseconds = 0U, bool expect_held_cleanup = false)
{
    check(command("PLAY").ok, "selected playback is accepted");
    run_accepted(usb_milliseconds, expect_held_cleanup);
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

        if (waveform_generator::command_pending)
        {
            waveform_generator::service_player_command();
        }

        if (rx.empty() && !waveform_generator::response_size && !waveform_generator::player_reply_waiting)
        {
            return;
        }
    }
}

void test_owner_lifecycle()
{
    check(waveform_generator::player_engine == nullptr, "player engine starts unconstructed");
    std::ranges::fill(waveform_generator::player_engine_storage, std::byte{0xA5});
    player_task_created = false;
    check(!waveform_generator::start_player().is_ok(), "failed owner task creation is reported");
    auto* const constructed_engine = waveform_generator::player_engine;
    check(constructed_engine != nullptr && constructed_engine->stats().stage_count == 0U, "poisoned OCRAM engine storage is initialized before use");
    constructed_engine->configure_passthrough();
    check(player_task_creations == 1U, "first player start attempts owner-task creation once");
    player_task_created = true;
    check(waveform_generator::start_player().is_ok(), "player retries static owner task creation");
    check(waveform_generator::player_engine == constructed_engine && constructed_engine->configured() && player_task_creations == 2U, "retry reuses the live OCRAM engine without reconstruction");
    check(!waveform_generator::start_player().is_ok() && player_task_creations == 2U && constructed_engine->configured(),
          "already-started refusal retains engine state and creates no additional task");
    waveform_generator::reset_selection("PLAY.WAV");
    waveform_generator::publish_live();
    files["2:/WG/PLAY.WAV"] = wav(50000U, 1000);
    files["2:/WG/SECOND.WAV"] = wav(10000U, -2000);
    files["2:/WG/SILENT.WAV"] = wav(10000U, 0);
    run_selected();
    auto status = waveform_generator::player_snapshot();
    auto live = waveform_generator::player_live_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::done) && status.file_frames_submitted == 50000U && status.pcm_drained == 1U && status.underruns == 0U,
          "clean run drains every frame without underrun");
    check(live.live_frame == 50000U && live.reference_rms == 1000.0 / 32768.0, "automatic reference measures first second and rewinds source");
    std::vector<float> expected(50000U, 1000.0F / 32768.0F);
    signal_lab::det::StreamDigest digest;
    digest.update(expected.data(), expected.size());
    check(live.live_digest == digest.value(), "firmware live digest covers all clean source samples");
    check(command("LOAD:SECOND.WAV").ok && command("REFERENCE:0.125").ok && command("CW ON").ok, "stopped source and preset controls configure");
    check(!command("SEED:42").ok, "acknowledged preset cannot be silently discarded by seed change");
    run_selected();
    live = waveform_generator::player_live_snapshot();
    check(live.live_frame == 10000U && live.live_events == 1U && live.reference_rms == 0.125, "second run reuses configured codec and applies preserved preset");
    waveform_generator::response_sequence = 99U;
    waveform_generator::queue_status("status");
    const std::string live_status(waveform_generator::response_buffer, waveform_generator::response_size);
    check(live_status.find("\"live_reference_rms\":0.125") != std::string::npos, "STATUS publishes a valid round-trippable live reference number");
    waveform_generator::response_size = waveform_generator::response_written = 0U;
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
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::fault) && status.error == static_cast<std::uint32_t>(waveform_generator::PlayerError::live_control) &&
              status.file_frames_enqueued == 0U,
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
    tx.clear();
    rx.clear();
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
    check(tx.find("\"seq\":1,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos && tx.find("\"seq\":2,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos &&
              tx.find("\"multi-cw\"") != std::string::npos && std::count(tx.begin(), tx.end(), '\n') == 2,
          "INFO advertises multi-CW and STATUS remains framed after partial writes");
    check(tx.find("\"encoders\":[]") != std::string::npos && tx.find("sd-wav-generation") == std::string::npos && tx.find("dd008-payload-upload") == std::string::npos,
          "encoder-free INFO preserves effects without advertising generation");
    tx.clear();
    feed("2 PLAY\n3 UNKNOWN\n3 INFO?\n4 INFO?\n");
    pump_usb();
    check(std::count(tx.begin(), tx.end(), '\n') == 4 && tx.find("\"seq\":4,\"protocol\":\"WFG-LIVE/1\",\"ok\":true") != std::string::npos && tx.find("BAD_REQUEST") != std::string::npos,
          "duplicate and malformed command sequences are rejected without preventing a later fresh sequence");
    tx.clear();
    feed(std::string("5 CW ") + std::string(waveform_generator::live_protocol::wire_capacity, 'X') + "\n6 INFO?\n");
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

// Whether the USB service hands `text` to the parser: it fits the control
// line, and without a CR at its end it is no unsequenced line (STATUS, MEDIA
// HOST, MEDIA LOCAL or a legacy player line).
[[nodiscard]] bool reaches_parser(std::string_view text)
{
    if (text.size() >= waveform_generator::control_line_capacity)
    {
        return false;
    }

    std::string_view line = text;

    if (line.ends_with('\r'))
    {
        line.remove_suffix(1U);
    }

    return line != "STATUS" && line != "MEDIA HOST" && line != "MEDIA LOCAL" && !waveform_generator::find_legacy_player_line(line).has_value();
}

// start_usb_service() over the C statuses of the USB bring-up
// (usb/usb_tinyusb_port.h): a failed hardware start is the hardware's own
// Status and starts no task; a task that cannot be created is unavailable;
// then the task starts once, and a second start is refused and starts
// nothing, the hardware included.
void test_start_usb_service()
{
    UsbBringUp& bring_up = usb_bring_up();
    bring_up = UsbBringUp{.hardware = kWFG_StatusUsbClockFailed};
    const m110::Status clock_failed = waveform_generator::start_usb_service();
    check(clock_failed.code == m110::StatusCode::io_error && std::string_view{clock_failed.message} == "USB_CLOCK_FAILED" && bring_up.hardware_starts == 1U && bring_up.task_creations == 0U,
          "a failed USB hardware start is the hardware's Status (io_error, USB_CLOCK_FAILED), and no USB task starts");
    bring_up.hardware = kWFG_StatusOk;
    bring_up.task_created = false;
    const m110::Status not_created = waveform_generator::start_usb_service();
    check(not_created.code == m110::StatusCode::unavailable && std::string_view{not_created.message} == "USB diagnostic task creation failed" && bring_up.hardware_starts == 2U &&
              bring_up.task_creations == 1U,
          "a USB task that cannot be created is unavailable");
    bring_up.task_created = true;
    check(waveform_generator::start_usb_service().is_ok() && bring_up.hardware_starts == 3U && bring_up.task_creations == 2U, "then the USB hardware starts and the USB task is created");
    const m110::Status again = waveform_generator::start_usb_service();
    check(again.code == m110::StatusCode::invalid_argument && std::string_view{again.message} == "USB diagnostic already started" && bring_up.hardware_starts == 3U && bring_up.task_creations == 2U,
          "a second start is refused and starts nothing, the hardware included");
}

// A --usb-task run: usb_task_entry() after TinyUSB's start fails
// ("stack-fails": the task suspends itself, and TinyUSB's task never runs) or
// succeeds ("stack-starts": TinyUSB's task runs, and the task never
// suspends). The stand-ins end the process at the first of those calls.
[[nodiscard]] int run_usb_task(std::string_view form)
{
    if (form == "stack-fails")
    {
        usb_task_run() = UsbTaskRun::stack_fails;
        usb_bring_up().stack = kWFG_StatusUsbStackFailed;
    }
    else if (form == "stack-starts")
    {
        usb_task_run() = UsbTaskRun::stack_starts;
    }
    else
    {
        std::fputs("usage: --usb-task stack-fails|stack-starts\n", stderr);
        return EXIT_FAILURE;
    }

    waveform_generator::usb_task_entry(nullptr);
    std::puts("FAIL: usb_task_entry returned");
    return EXIT_FAILURE;
}

// Every line of the request corpus (live_protocol_corpus.hpp) that the parser
// refuses, and that reaches it: the USB service's reply is the error the
// corpus gives, with the seq it gives, and the connection's last sequence is
// the corpus's after it.
void test_request_corpus()
{
    namespace wg = waveform_generator;
    namespace corpus = live_protocol_corpus;
    constexpr std::size_t least_refusals = 100000U;
    const auto live = wg::player_live_snapshot();
    const std::string run_id = live.run_id[0] == '\0' ? std::string{"null"} : '"' + std::string{std::data(live.run_id)} + '"';
    const std::string after_seq = R"(,"protocol":"WFG-LIVE/1","ok":false,"state":)" + corpus::number(wg::player_snapshot().state) + R"(,"run_id":)" + run_id + R"(,"error":{"code":")";
    std::size_t sent = 0U;
    std::size_t wrong = 0U;
    std::string first_wrong;

    for (const corpus::Line& seed : corpus::seeds())
    {
        std::vector<std::string> texts = corpus::edits_of(seed.text);
        texts.insert(texts.begin(), seed.text);

        for (const std::string& text : texts)
        {
            const corpus::Outcome outcome = corpus::outcome_of({.last_before = seed.last_before, .text = text});

            if (outcome.accepted || !reaches_parser(text))
            {
                continue;
            }

            std::ranges::copy(text, wg::control_line().begin());
            wg::control_line_size = text.size();
            wg::last_sequence = seed.last_before;
            wg::handle_control_line();
            std::string expected = R"({"seq":)" + corpus::number(outcome.reply_seq);
            expected += after_seq;
            expected += outcome.error;
            expected += R"(","message":")";
            expected += outcome.error;
            expected += "\"}}\n";
            ++sent;

            if (std::string_view{std::data(wg::response_buffer), wg::response_size} != expected || wg::last_sequence != outcome.last_after)
            {
                first_wrong = wrong == 0U ? corpus::escaped(text) : first_wrong;
                ++wrong;
            }
        }
    }

    wg::control_line_size = 0U;
    wg::last_sequence = 0U;
    wg::response_size = 0U;
    wg::response_written = 0U;
    const std::string message =
        "every refused corpus line gets the corpus's error and seq from the USB service" + (wrong == 0U ? std::string{} : " (" + std::to_string(wrong) + " wrong, the first \"" + first_wrong + "\")");
    check(sent >= least_refusals && wrong == 0U, message.c_str());
}

// The complete lines the device has sent since tx was last cleared.
[[nodiscard]] std::vector<std::string> sent_lines()
{
    std::vector<std::string> lines;
    std::string_view rest{tx};

    for (auto end = rest.find('\n'); end != std::string_view::npos; end = rest.find('\n'))
    {
        lines.emplace_back(rest.substr(0U, end));
        rest.remove_prefix(end + 1U);
    }

    return lines;
}

// Sends requests on a fresh connection, whose host reads every reply, and
// returns the replies.
[[nodiscard]] std::vector<std::string> exchange(const std::string& requests)
{
    tx.clear();
    rx.clear();
    waveform_generator::last_sequence = 0U;
    write_budget = std::numeric_limits<std::size_t>::max();
    feed(requests);
    pump_usb();
    return sent_lines();
}

// The read-only card query keeps FatFs order, paginates regular entries, and
// closes the directory on both complete scans and every failure path.
void test_card_listing_reader()
{
    namespace listing = waveform_generator::card_listing;
    mounted = true;
    reset_directory_fixture();
    directory_entries.push_back({.name = "Long file name.wav", .alias = "LONGFI~1.WAV", .size = 1234U});
    directory_entries.push_back({.name = "ignored folder", .alias = "", .size = 0U, .attributes = AM_DIR});
    directory_entries.push_back({.name = "ignored volume", .alias = "", .size = 0U, .attributes = AM_VOL});
    std::string cp437_name;
    cp437_name.push_back(static_cast<char>(0x82));
    cp437_name += ".WAV";
    directory_entries.push_back({.name = "ignored unicode source.wav", .alias = cp437_name, .size = 987654U, .attributes = static_cast<std::uint8_t>(0x02U | 0x04U | 0x01U)});
    directory_entries.push_back({.name = "FALLBACK.SCN", .alias = "", .size = 87U});

    for (unsigned int index = 0U; index < 30U; ++index)
    {
        directory_entries.push_back({.name = std::string{"F"} + (index < 10U ? "0" : "") + std::to_string(index) + ".WAV", .alias = "", .size = static_cast<FSIZE_t>(index * 17U)});
    }

    listing::Page first{};
    const auto first_status = listing::read_page(0U, first);
    check(first_status.is_ok() && first.offset == 0U && first.total == 33U && first.count == listing::page_capacity,
          "FILES page zero returns the first 24 regular files and total after filtering directories and volumes");
    check(std::string_view{first.entries[0].name} == "LONGFI~1.WAV" && first.entries[0].size_bytes == 1234U && std::string_view{first.entries[1].name} == "\xC3\xA9.WAV" &&
              first.entries[1].size_bytes == 987654U && std::string_view{first.entries[2].name} == "FALLBACK.SCN" && first.entries[2].size_bytes == 87U,
          "FILES prefers the short alias, converts CP437 aliases to UTF-8, and falls back to fname");
    check((directory_entries[3].attributes & 0x07U) == 0x07U && first.count == listing::page_capacity, "hidden, system and read-only regular files remain listed");
    check(directory_faults().last_path == "2:/WG" && directory_faults().open_directories == 0U && directory_faults().successful_opens == 1U && directory_faults().close_calls == 1U,
          "FILES opens exactly the WG directory and closes it after a successful scan");
    listing::Page middle{};
    const auto middle_status = listing::read_page(24U, middle);
    check(middle_status.is_ok() && middle.offset == 24U && middle.total == 33U && middle.count == 9U && std::string_view{middle.entries[0].name} == "F21.WAV" &&
              std::string_view{middle.entries[8].name} == "F29.WAV",
          "FILES offset pages preserve FatFs enumeration order without gaps or duplicates");
    listing::Page end{};
    const auto end_status = listing::read_page(33U, end);
    check(end_status.is_ok() && end.count == 0U && end.offset == 33U && end.total == 33U, "FILES accepts the exact terminal offset as an empty page");
    // exFAT has no SFN aliases: FatFs returns an empty altname and the actual
    // (possibly longer than 8.3) name in fname. This matches names already on
    // the target card such as CANARY2400L.WAV and M110B24S.JSON.
    reset_directory_fixture();
    directory_entries.push_back({.name = "CANARY2400L.WAV", .alias = "", .size = 1526444U});
    directory_entries.push_back({.name = "M110B24S.JSON", .alias = "", .size = 1645U});
    std::string long_cp437_name(13U, static_cast<char>(0x82));
    long_cp437_name += ".WAV";
    directory_entries.push_back({.name = long_cp437_name, .alias = "", .size = 123U});
    listing::Page long_names{};
    const auto long_names_status = listing::read_page(0U, long_names);
    check(long_names_status.is_ok() && long_names.total == 3U && long_names.count == 3U && std::string_view{long_names.entries[0].name} == "CANARY2400L.WAV" &&
              std::string_view{long_names.entries[1].name} == "M110B24S.JSON" &&
              std::string_view{long_names.entries[2].name} == "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9.WAV",
          "FILES accepts bounded FatFs LFN fallback names and converts their OEM CP437 bytes to UTF-8");
    reset_directory_fixture();
    const std::string exact_capacity_name = std::string(32U, 'X') + ".WAV";
    directory_entries.push_back({.name = exact_capacity_name, .alias = "", .size = 456U});
    listing::Page exact_capacity{};
    const auto exact_capacity_status = listing::read_page(0U, exact_capacity);
    check(exact_capacity_status.is_ok() && exact_capacity.count == 1U && std::string_view{exact_capacity.entries[0].name} == exact_capacity_name,
          "FILES accepts an LFN that exactly fills the 36-byte UTF-8 field");
    reset_directory_fixture();
    std::string expanded_beyond_capacity(19U, static_cast<char>(0x82));
    expanded_beyond_capacity += ".WAV";
    directory_entries.push_back({.name = expanded_beyond_capacity, .alias = "", .size = 789U});
    listing::Page expanded_failure{};
    const auto expanded_status = listing::read_page(0U, expanded_failure);
    check(expanded_beyond_capacity.size() <= listing::name_capacity - 1U && std::string_view{expanded_status.message} == "FILES_NAME" && expanded_failure.count == 0U && expanded_failure.total == 0U,
          "FILES rejects an OEM LFN whose CP437-to-UTF-8 expansion exceeds 36 bytes without publishing a partial page");
    reset_directory_fixture();
    directory_entries.push_back({.name = "?", .alias = "", .size = 1U});
    listing::Page unrepresentable{};
    const auto unrepresentable_status = listing::read_page(0U, unrepresentable);
    check(std::string_view{unrepresentable_status.message} == "FILES_NAME" && unrepresentable.count == 0U, "FILES refuses FatFs's '?' sentinel for an exFAT name outside the configured OEM code page");
    reset_directory_fixture();
    directory_faults().fail_open = true;
    listing::Page failed{};
    const auto open_status = listing::read_page(0U, failed);
    check(std::string_view{open_status.message} == "FILES_OPEN" && directory_faults().close_calls == 0U && directory_faults().open_directories == 0U,
          "FILES reports open failure without attempting to close an unopened directory");
    reset_directory_fixture();
    directory_entries.push_back({.name = "OK.WAV", .alias = "", .size = 1U});
    directory_entries.push_back({.name = "SECOND.WAV", .alias = "", .size = 2U});
    directory_faults().reads_before_failure = 1U;
    failed = {};
    const auto read_status = listing::read_page(0U, failed);
    check(std::string_view{read_status.message} == "FILES_READ" && failed.count == 0U && failed.total == 0U && directory_faults().close_calls == 1U && directory_faults().open_directories == 0U,
          "FILES clears a partial page and closes after a directory read failure");
    reset_directory_fixture();
    directory_entries.push_back({.name = std::string(37U, 'A') + ".WAV", .alias = "", .size = 9U});
    failed = {};
    const auto name_status = listing::read_page(0U, failed);
    check(std::string_view{name_status.message} == "FILES_NAME" && failed.count == 0U && directory_faults().close_calls == 1U,
          "FILES refuses a no-alias FatFs name that exceeds the fixed UTF-8 bound and closes the directory");
    reset_directory_fixture();
    directory_entries.push_back({.name = std::string{"BAD"} + static_cast<char>(0x01) + ".WAV", .alias = "", .size = 1U});
    failed = {};
    const auto control_status = listing::read_page(0U, failed);
    check(std::string_view{control_status.message} == "FILES_NAME" && directory_faults().close_calls == 1U, "FILES rejects control bytes in FatFs names");
    reset_directory_fixture();
    directory_entries.push_back({.name = "ONE.WAV", .alias = "", .size = 1U});
    directory_faults().fail_close = true;
    failed = {};
    const auto close_status = listing::read_page(0U, failed);
    check(std::string_view{close_status.message} == "FILES_CLOSE" && failed.count == 0U && directory_faults().close_calls == 1U && directory_faults().open_directories == 0U,
          "FILES treats a directory close error as failure and does not publish the page");
    reset_directory_fixture();
    failed = {};
    const auto range_status = listing::read_page(1U, failed);
    check(std::string_view{range_status.message} == "FILES_RANGE" && directory_faults().close_calls == 1U && directory_faults().open_directories == 0U,
          "FILES rejects an offset beyond the current total after closing the directory");
    mounted = false;
}

// The output route: CODEC at boot, set by OUTPUT CODEC / OUTPUT USB in both
// grammars, idempotent, BUSY for a change while a run is pending, and reported
// by STATUS (output_route) and INFO ("output").
void test_output_route()
{
    namespace wg = waveform_generator;
    using wg::OutputRoute;
    using wg::live_protocol::Kind;
    check(wg::player_output_route() == OutputRoute::codec, "the output route is CODEC at boot");
    check(!wg::live_protocol::is_control(Kind::output_codec) && !wg::live_protocol::is_control(Kind::output_usb), "OUTPUT is configuration, not a live control");
    auto lines = exchange("1 OUTPUT USB\n2 OUTPUT USB\nSTATUS\n3 INFO?\n");
    check(lines.size() == 4U && lines[0].starts_with(R"({"seq":1,)") && lines[0].contains(R"("ok":true)") && lines[1].starts_with(R"({"seq":2,)") && lines[1].contains(R"("ok":true)") &&
              wg::player_output_route() == OutputRoute::usb,
          "WFG-LIVE/1 OUTPUT USB selects the USB route, and naming it again is OK");
    check(lines.size() == 4U && lines[2].contains(" output_route=1 ") && lines[3].contains(R"("output":"usb")"), "STATUS and INFO report the USB route");
    lines = exchange("OUTPUT CODEC\nOUTPUT CODEC\r\n");
    check(lines == std::vector<std::string>{"OK OUTPUT CODEC", "OK OUTPUT CODEC"} && wg::player_output_route() == OutputRoute::codec,
          "the legacy OUTPUT lines answer OK with the line, also for the current route");
    check(command("LOAD:SECOND.WAV").ok && command("PLAY").ok, "a pending PLAY makes the player busy");
    const auto busy = command("OUTPUT USB");
    check(!busy.ok && std::string_view{busy.error} == "BUSY" && wg::player_output_route() == OutputRoute::codec, "a route change while a run is pending is BUSY and changes nothing");
    check(command("OUTPUT CODEC").ok, "naming the current route stays OK while a run is pending");
    lines = exchange("OUTPUT USB\n");
    check(lines == std::vector<std::string>{"ERR BUSY"}, "the legacy line reports BUSY too");
    wg::play_requested = false; // The player task normally consumes this flag.
    wait_calls = 0U;
    wg::run_player();
    check(command("OUTPUT USB").ok && wg::player_output_route() == OutputRoute::usb, "the route changes once the player has stopped");
    check(command("OUTPUT CODEC").ok && wg::player_output_route() == OutputRoute::codec, "and changes back");
}

// STATUS carries the media layer's host-eject latch.
void test_status_reports_eject()
{
    wfg_media_snapshot_t media{};
    media.state = kWFG_MediaHost;
    media.msc_ready = 1U;
    media.eject_requests = 2U;
    media.msc_ejected = 1U;
    status_seam().media = media;
    const auto lines = exchange("STATUS\n1 STATUS?\n");
    status_seam().media.reset();
    check(lines.size() == 2U && lines[0].contains(" eject_requests=2 ") && lines[0].contains(" msc_ejected=1 "), "text STATUS reports msc_ejected after the eject count");
    check(lines.size() == 2U && lines[1].contains(R"("msc_ejected":1)"), "JSON STATUS reports msc_ejected");
}

// A media state and host hold, and the MEDIA LOCAL replies of both grammars and
// the text STATUS fields they give.
struct MediaLocalCase
{
    std::uint32_t state;
    std::uint32_t host_hold;
    std::string_view legacy;
    std::string_view json;
    std::string_view status_fields;
    const char* what;
    std::string_view media_state; // the published number STATUS renders for `state`
};

// MEDIA LOCAL names the host's hold on its volume in both grammars and leaves
// the card where it was; STATUS reports the hold as msc_host_hold.
void test_media_local_waits_for_host()
{
    const bool was_local = media_local;
    constexpr std::uint32_t both = kWFG_MediaHostHoldRemovalPrevented | kWFG_MediaHostHoldWriting;
    constexpr auto cases = std::to_array<MediaLocalCase>({
        {.state = kWFG_MediaHost,
         .host_hold = kWFG_MediaHostHoldRemovalPrevented,
         .legacy = "ERR HOST_LOCKED",
         .json = R"("error":{"code":"HOST_LOCKED",)",
         .status_fields = " msc_ejected=0 msc_host_hold=1 ",
         .what = "a host removal lock is HOST_LOCKED, reported as hold 1",
         .media_state = "2"},
        {.state = kWFG_MediaHost,
         .host_hold = both,
         .legacy = "ERR HOST_WRITING",
         .json = R"("error":{"code":"HOST_WRITING",)",
         .status_fields = " msc_ejected=0 msc_host_hold=3 ",
         .what = "a host write in progress is HOST_WRITING, also under a lock",
         .media_state = "2"},
        {.state = kWFG_MediaFault,
         .host_hold = kWFG_MediaHostHoldNone,
         .legacy = "ERR MEDIA LOCAL FAILED",
         .json = R"("error":{"code":"BUSY",)",
         .status_fields = " msc_ejected=0 msc_host_hold=0 ",
         .what = "a card in neither ownership keeps the old failure replies",
         .media_state = "255"},
    });

    for (const auto& refusal : cases)
    {
        media_local = false;
        wfg_media_snapshot_t media{};
        media.state = refusal.state;
        media.msc_host_hold = refusal.host_hold;
        status_seam().media = media;
        const auto lines = exchange("MEDIA LOCAL\n1 MEDIA LOCAL\nSTATUS\n2 STATUS?\n");
        status_seam().media.reset();
        check(lines.size() == 4U && lines[0] == refusal.legacy && lines[1].contains(refusal.json) && !media_local, refusal.what);
        check(lines.size() == 4U && lines[2].contains(refusal.status_fields) && lines[3].contains(R"(,"msc_host_hold":)" + std::to_string(refusal.host_hold) + ","),
              "both STATUS forms report the host's hold, after msc_ejected");
        check(lines.size() == 4U && lines[2].contains(" media_state=" + std::string{refusal.media_state} + " ") && lines[3].contains(R"(,"media_state":)" + std::string{refusal.media_state} + ","),
              "both STATUS forms report media_state by its published number: 2 the host's, 255 a fault");
    }

    const auto lines = exchange("MEDIA LOCAL\n1 MEDIA LOCAL\n");
    check(lines.size() == 2U && lines[0] == "OK MEDIA LOCAL" && lines[1].contains(R"("result":{"accepted":true})") && media_local,
          "MEDIA LOCAL once the host holds nothing answers OK in both grammars");
    media_local = was_local;
}

// One rendered STATUS member. String values keep their quotes; a nested object
// is a member whose value is "{", followed by its own members.
struct StatusMember
{
    std::string name;
    std::string value;
};

// The five replies the status-contract fixture renders.
struct RenderedReplies
{
    std::string text_status;
    std::string json_counters;
    std::string json_status;
    std::string info;
    std::string usb;
};

constexpr std::string_view text_status_prefix{"OK STATUS"};
constexpr int hex_base = 16;
// How far the fixture's artifact error overruns its budgeted width.
constexpr std::size_t tx_error_overrun_chars = 16U;

template <class T>
concept Arithmetic = std::integral<T> || std::floating_point<T>;

// Enumerations are excluded: numeric_limits is not specialised for them and
// would silently yield zero instead of the widest value.
template <Arithmetic T> void set_max(T& value)
{
    value = std::numeric_limits<T>::max();
}

// Sets each listed member of record to its type maximum. The members must cover
// every byte of the record, so a member added later breaks the build instead of
// escaping the maxima.
template <class Record, Arithmetic... Types> void set_all_max(Record& record, Types Record::*... members)
{
    static_assert(sizeof(Record) == (sizeof(Types) + ...), "list every member of the record");
    (set_max(record.*members), ...);
}

// Fills text with value and terminates it in its last element.
constexpr void fill_text(std::span<char> text, char value) noexcept
{
    if (text.empty())
    {
        return;
    }

    std::ranges::fill(text.first(text.size() - 1U), value);
    text.back() = '\0';
}

// Longer than its budgeted width, so STATUS? must truncate it. Static storage
// outlives every reader of the TxArtifactSnapshot::error pointer it lends.
constexpr auto long_tx_error = []
{
    std::array<char, waveform_generator::tx_error_max_chars + tx_error_overrun_chars + 1U> text{};
    fill_text(text, 'E');
    return text;
}();

[[nodiscard]] waveform_generator::EngineCounters max_engine_counters()
{
    using Counters = waveform_generator::EngineCounters;
    Counters counters{};
    set_all_max(counters, &Counters::state, &Counters::error, &Counters::stages, &Counters::frames_in, &Counters::frames_out, &Counters::clipped, &Counters::digest_hi, &Counters::digest_lo,
                &Counters::source_digest_hi, &Counters::source_digest_lo, &Counters::max_block_cycles, &Counters::events_applied, &Counters::events_dropped, &Counters::arena_bytes);
    return counters;
}

[[nodiscard]] wfg_media_snapshot_t max_media_snapshot()
{
    using Media = wfg_media_snapshot_t;
    Media media{};
    set_all_max(media, &Media::state, &Media::error, &Media::card_ready, &Media::msc_ready, &Media::msc_read_only, &Media::block_count, &Media::block_size, &Media::init_attempts,
                &Media::host_init_status, &Media::host_detect_status, &Media::card_init_status, &Media::disk_init_status, &Media::test_ready_calls, &Media::not_ready_responses, &Media::read_calls,
                &Media::read_blocks, &Media::write_calls, &Media::write_blocks, &Media::read_errors, &Media::write_errors, &Media::invalid_requests, &Media::eject_requests, &Media::sync_cache_calls,
                &Media::ownership_handoffs, &Media::cd_gpio3_level, &Media::cd_cm7_gpio3_level, &Media::cd_inserted, &Media::msc_ejected, &Media::msc_host_hold, &Media::msc_max_read_us,
                &Media::msc_max_write_us, &Media::msc_max_blocks_per_call, &Media::sd_bus_clock_hz);
    return media;
}

// Saves the service, player and link-seam state the status-contract fixture
// drives and restores it when the fixture leaves scope by any path, so later
// tests start from the state they would have seen without it. The response
// buffer is not saved: the fixture runs only with no reply queued.
class ServiceStateGuard
{
public:
    ServiceStateGuard() = default;
    ServiceStateGuard(const ServiceStateGuard&) = delete;
    ServiceStateGuard& operator=(const ServiceStateGuard&) = delete;
    ServiceStateGuard(ServiceStateGuard&&) = delete;
    ServiceStateGuard& operator=(ServiceStateGuard&&) = delete;

    ~ServiceStateGuard()
    {
        waveform_generator::live_snapshot = live_;
        waveform_generator::engine_counters = engine_;
        waveform_generator::response_sequence = response_sequence_;
        waveform_generator::response_size = response_size_;
        waveform_generator::response_written = response_written_;
        status_seam() = seam_;
    }

private:
    waveform_generator::LiveSnapshot live_{waveform_generator::live_snapshot};
    waveform_generator::EngineCounters engine_{waveform_generator::engine_counters};
    std::uint32_t response_sequence_{waveform_generator::response_sequence};
    std::size_t response_size_{waveform_generator::response_size};
    std::size_t response_written_{waveform_generator::response_written};
    StatusSeam seam_{status_seam()};
};

// Every STATUS input the host can reach, at its type maximum: the media,
// artifact and live snapshots, the engine counters and the reply sequence.
void drive_inputs_to_maxima()
{
    namespace wg = waveform_generator;
    auto& seam = status_seam();
    seam.media = max_media_snapshot();
    set_max(seam.tx_artifact.state);
    set_max(seam.tx_artifact.payload_bytes);
    set_max(seam.tx_artifact.frames_written);
    set_max(seam.tx_artifact.total_frames);
    fill_text(seam.tx_artifact.filename, 'F');
    fill_text(seam.tx_artifact.wav_sha256, 'a');
    seam.tx_artifact.error = long_tx_error.data();
    set_max(wg::live_snapshot.seed);
    set_max(wg::live_snapshot.live_frame);
    set_max(wg::live_snapshot.live_digest);
    set_max(wg::live_snapshot.live_clipped);
    set_max(wg::live_snapshot.live_events);
    set_max(wg::live_snapshot.queue_free);
    set_max(wg::live_snapshot.capture_free);
    set_max(wg::live_snapshot.reference_rms);
    fill_text(wg::live_snapshot.run_id, 'R');
    fill_text(wg::live_snapshot.selected_file, 'S');
    wg::engine_counters = max_engine_counters();
    set_max(wg::response_sequence);
}

// The reply usb_service.cpp queued, read only within its response buffer.
[[nodiscard]] std::string rendered_reply()
{
    const std::span<const char> buffer{waveform_generator::response_buffer};
    check(waveform_generator::response_size <= buffer.size(), "the queued reply fits its response buffer");
    const auto reply = buffer.first(std::min(waveform_generator::response_size, buffer.size()));
    return {reply.begin(), reply.end()};
}

void test_card_listing_reply_budget()
{
    namespace wg = waveform_generator;
    namespace listing = waveform_generator::card_listing;
    const ServiceStateGuard saved;
    check(wg::response_size == 0U, "the FILES? budget fixture starts without a queued reply");
    listing::Page widest{};
    widest.offset = std::numeric_limits<std::uint64_t>::max() - listing::page_capacity;
    widest.total = std::numeric_limits<std::uint64_t>::max();
    widest.count = listing::page_capacity;

    for (auto& entry : widest.entries)
    {
        std::fill(std::begin(entry.name), std::end(entry.name) - 1, '"');
        entry.name[listing::name_capacity - 1U] = '\0';
        entry.size_bytes = std::numeric_limits<std::uint64_t>::max();
    }

    wg::response_sequence = std::numeric_limits<std::uint32_t>::max();
    wg::queue_files(widest);
    const auto reply = rendered_reply();
    const auto files_at = reply.find(R"("files":{)");
    const auto offset_at = reply.find(R"("offset":)");
    const auto total_at = reply.find(R"("total":)");
    const auto next_at = reply.find(R"("next_offset":null)");
    const auto entries_at = reply.find(R"("entries":[)");
    const auto name_at = reply.find(R"("name":)");
    const auto size_at = reply.find(R"("size_bytes":)");
    check(reply.starts_with(R"({"seq":4294967295,)") && reply.size() < wg::cdc_response_capacity && reply.ends_with("]}}\n"),
          "a worst-case 24-entry FILES? reply with escaped names and uint64 maxima fits the CDC response buffer");
    check(files_at < offset_at && offset_at < total_at && total_at < next_at && next_at < entries_at && entries_at < name_at && name_at < size_at,
          "FILES? serializes offset, total, next offset and entries in the documented member order");
    check(reply.find(R"("offset":18446744073709551591,"total":18446744073709551615,"next_offset":null)") != std::string::npos &&
              reply.find(R"("size_bytes":18446744073709551615)") != std::string::npos && reply.find("\\\"\\\"\\\"") != std::string::npos,
          "FILES? handles uint64 terminal offsets and JSON-escapes arbitrary alias characters");
}

// Renders text STATUS, COUNTERS?, STATUS?, INFO? and USB? through
// usb_service.cpp with every reachable input at its maximum; the guard
// restores the inputs.
[[nodiscard]] RenderedReplies render_at_maxima()
{
    namespace wg = waveform_generator;
    const ServiceStateGuard saved;
    check(wg::response_size == 0U, "the status fixture starts with no reply queued");
    drive_inputs_to_maxima();
    wg::queue_status();
    auto text_status = rendered_reply();
    wg::queue_status("counters");
    auto json_counters = rendered_reply();
    wg::queue_status("status");
    auto json_status = rendered_reply();
    wg::queue_info();
    auto info = rendered_reply();
    wg::queue_usb_diagnostics();
    auto usb = rendered_reply();
    return {.text_status = std::move(text_status), .json_counters = std::move(json_counters), .json_status = std::move(json_status), .info = std::move(info), .usb = std::move(usb)};
}

[[nodiscard]] std::vector<StatusMember> text_status_members(std::string_view response)
{
    std::vector<StatusMember> members;
    auto at = text_status_prefix.size();

    while (at < response.size() && response[at] == ' ')
    {
        const auto equals = response.find('=', at);
        const auto end = response.find_first_of(" \n", equals);

        if (equals == std::string_view::npos || end == std::string_view::npos)
        {
            break;
        }

        members.push_back({.name = std::string{response.substr(at + 1U, equals - at - 1U)}, .value = std::string{response.substr(equals + 1U, end - equals - 1U)}});
        at = end;
    }

    return members;
}

// End of the JSON value that starts at value: past a string's closing quote,
// just past an object's '{', or at the ',' or '}' after a number or literal.
[[nodiscard]] std::size_t json_value_end(std::string_view response, std::size_t value)
{
    if (response[value] == '"')
    {
        return std::min(response.find('"', value + 1U), response.size() - 1U) + 1U;
    }

    if (response[value] == '{')
    {
        return value + 1U;
    }

    return std::min(response.find_first_of(",}", value), response.size());
}

// STATUS JSON values never contain ',', '}' or an escaped quote.
[[nodiscard]] std::vector<StatusMember> json_status_members(std::string_view response)
{
    std::vector<StatusMember> members;
    std::size_t at = 0U;

    while (response.substr(at).starts_with("{\"") || response.substr(at).starts_with(",\""))
    {
        const auto name = at + 2U;
        const auto name_end = response.find("\":", name);

        if (name_end == std::string_view::npos || name_end + 2U >= response.size())
        {
            break;
        }

        const auto value = name_end + 2U;
        const auto end = json_value_end(response, value);
        members.push_back({.name = std::string{response.substr(name, name_end - name)}, .value = std::string{response.substr(value, end - value)}});
        // A nested object's own members follow its '{'.
        at = response[value] == '{' ? value : end;
    }

    return members;
}

// Parse only immediate members so nested status objects remain a single value.
[[nodiscard]] std::size_t direct_json_value_end(std::string_view response, std::size_t value)
{
    if (value >= response.size())
    {
        return response.size();
    }

    if (response[value] == '"')
    {
        for (std::size_t at = value + 1U; at < response.size(); ++at)
        {
            if (response[at] == '\\')
            {
                ++at;
                continue;
            }

            if (response[at] == '"')
            {
                return at + 1U;
            }
        }

        return response.size();
    }

    if (response[value] == '{' || response[value] == '[')
    {
        std::size_t depth{};
        bool in_string = false;

        for (std::size_t at = value; at < response.size(); ++at)
        {
            const char ch = response[at];

            if (in_string)
            {
                if (ch == '\\')
                {
                    ++at;
                }
                else if (ch == '"')
                {
                    in_string = false;
                }

                continue;
            }

            if (ch == '"')
            {
                in_string = true;
            }
            else if (ch == '{' || ch == '[')
            {
                ++depth;
            }
            else if (ch == '}' || ch == ']')
            {
                if (--depth == 0U)
                {
                    return at + 1U;
                }
            }
        }

        return response.size();
    }

    return std::min(response.find_first_of(",}", value), response.size());
}

[[nodiscard]] std::vector<StatusMember> direct_json_object_members(std::string_view response)
{
    std::vector<StatusMember> members;
    std::size_t at = response.find('{');

    if (at == std::string_view::npos)
    {
        return members;
    }

    ++at;

    while (at < response.size())
    {
        while (at < response.size() && (response[at] == ' ' || response[at] == '\n' || response[at] == '\r' || response[at] == '\t' || response[at] == ','))
        {
            ++at;
        }

        if (at >= response.size() || response[at] == '}')
        {
            break;
        }

        const auto key_start = at;
        const auto key_end = direct_json_value_end(response, key_start);

        if (key_end <= key_start || key_end > response.size() || response[key_start] != '"')
        {
            break;
        }

        const auto colon = response.find(':', key_end);

        if (colon == std::string_view::npos)
        {
            break;
        }

        const auto value_start = response.find_first_not_of(" \t\r\n", colon + 1U);

        if (value_start == std::string_view::npos)
        {
            break;
        }

        const auto value_end = direct_json_value_end(response, value_start);

        if (value_end <= value_start || value_end > response.size())
        {
            break;
        }

        members.push_back({.name = std::string{response.substr(key_start + 1U, key_end - key_start - 2U)}, .value = std::string{response.substr(value_start, value_end - value_start)}});
        at = value_end;
    }

    return members;
}

// The value of the member called name, or an empty string when there is none.
[[nodiscard]] std::string member_value(std::span<const StatusMember> members, std::string_view name)
{
    const auto found = std::ranges::find(members, name, &StatusMember::name);
    return found == members.end() ? std::string{} : found->value;
}

[[nodiscard]] bool is_decimal(std::string_view value, std::size_t max_digits)
{
    return !value.empty() && value.size() <= max_digits && value.find_first_not_of("0123456789") == std::string_view::npos;
}

[[nodiscard]] std::string quoted(std::string_view value)
{
    std::string text{'"'};
    text += value;
    text += '"';
    return text;
}

// The numeric fields every STATUS form emits, each a uint32.
[[nodiscard]] std::vector<waveform_generator::StatusJsonField> numeric_status_layout()
{
    std::vector<waveform_generator::StatusJsonField> fields;

    for (const char* name : waveform_generator::status_field_names)
    {
        fields.push_back({.name = name, .max_value_chars = waveform_generator::max_uint32_decimal_chars});
    }

    return fields;
}

// Every JSON COUNTERS? member in emission order: begin_json(), the counters
// object header, the numeric fields, then the JSON-only members.
[[nodiscard]] std::vector<waveform_generator::StatusJsonField> json_status_layout()
{
    namespace wg = waveform_generator;
    std::vector<wg::StatusJsonField> fields{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "counters", .max_value_chars = wg::literal_length("{")},
        {.name = "format", .max_value_chars = wg::json_string_chars(wg::literal_length("engineering-counters"))},
    };
    const auto numeric = numeric_status_layout();
    fields.insert(fields.end(), numeric.begin(), numeric.end());
    fields.insert(fields.end(), wg::status_json_fields.begin(), wg::status_json_fields.end());
    return fields;
}

// The width members leave unused when they follow fields in order, each within
// its budgeted width; otherwise the first way they depart from the contract.
[[nodiscard]] m110::Result<std::size_t> unused_width(std::span<const StatusMember> members, std::span<const waveform_generator::StatusJsonField> fields)
{
    if (members.size() != fields.size())
    {
        return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "the member count differs from the contract"};
    }

    std::size_t unused = 0U;

    for (const auto& [member, field] : std::views::zip(members, fields))
    {
        if (member.name != field.name)
        {
            return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "a member name is out of contract order"};
        }

        if (member.value.size() > field.max_value_chars)
        {
            return m110::Status{.code = m110::StatusCode::buffer_too_small, .message = "a member value is wider than its budget"};
        }

        unused += field.max_value_chars - member.value.size();
    }

    return unused;
}

// JSON-only members whose values are narrower than their budgets.
[[nodiscard]] std::vector<std::string_view> narrow_json_fields(std::span<const StatusMember> members)
{
    std::vector<std::string_view> names;

    for (const auto& field : waveform_generator::status_json_fields)
    {
        if (member_value(members, field.name).size() < field.max_value_chars)
        {
            names.emplace_back(field.name);
        }
    }

    return names;
}

// Maximal inputs fill every JSON-only width except the short test build label
// and, when the build cannot name its commit, the shorter unknown commit.
[[nodiscard]] std::vector<std::string_view> expected_narrow_json_fields()
{
    if (waveform_generator::build_info::fw_commit == waveform_generator::unknown_commit)
    {
        return {"build_id", "fw_commit"};
    }

    return {"build_id"};
}

// fw_commit_hi (index 0) or fw_commit_lo (index 1) as the commit text gives it;
// 0 when the commit is unknown.
[[nodiscard]] m110::Result<std::uint32_t> commit_word(std::string_view commit, std::size_t index)
{
    namespace wg = waveform_generator;

    if (commit == wg::unknown_commit)
    {
        return std::uint32_t{0U};
    }

    const auto first = index * wg::commit_word_hex_digits;

    if (first + wg::commit_word_hex_digits > commit.size())
    {
        return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "the commit is shorter than its words"};
    }

    const auto digits = commit.substr(first, wg::commit_word_hex_digits);
    std::uint32_t word{};
    const auto [end, error] = std::from_chars(std::to_address(digits.begin()), std::to_address(digits.end()), word, hex_base);

    if (error != std::errc{} || end != std::to_address(digits.end()))
    {
        return m110::Status{.code = m110::StatusCode::invalid_argument, .message = "a commit word is not all hex digits"};
    }

    return word;
}

// check() for a Status: a failure also names its reason.
void check_ok(m110::Status status, const char* message)
{
    const auto described = std::string{message} + ": " + status.message;
    check(status.is_ok(), described.c_str());
}

// Text STATUS is one line of status_field_names in order, each a uint32, and
// the width it leaves unused closes status_response_worst_case_bytes() exactly.
void check_text_status(std::string_view text)
{
    namespace wg = waveform_generator;
    const auto members = text_status_members(text);
    const auto unused = unused_width(members, numeric_status_layout());
    const auto is_uint32 = [](const StatusMember& member) { return is_decimal(member.value, wg::max_uint32_decimal_chars); };

    check(text.starts_with(text_status_prefix) && text.ends_with('\n'), "text STATUS is one OK STATUS line");
    check_ok(unused.status(), "text STATUS emits status_field_names in order within their widths");
    check(std::ranges::all_of(members, is_uint32), "every text STATUS value is a decimal uint32");
    check(unused.has_value() && text.size() + 1U + unused.value() == wg::status_response_worst_case_bytes(), "text STATUS worst case counts every byte of the reply and its terminator");
}

// JSON COUNTERS? emits the contract members in order within their widths, and
// the width it leaves unused closes status_json_worst_case_bytes() exactly.
void check_json_status(const RenderedReplies& replies)
{
    namespace wg = waveform_generator;
    const auto members = json_status_members(replies.json_counters);
    const auto unused = unused_width(members, json_status_layout());
    const auto truncated_error = std::string_view{long_tx_error.data(), wg::tx_error_max_chars};
    const auto shorter_key_chars = wg::literal_length("counters") - wg::literal_length("status");
    check(replies.json_counters.ends_with("\"}}\n"), "JSON COUNTERS? ends with a string member and closes both objects");
    check_ok(unused.status(), "JSON COUNTERS? emits the contract members in order within their budgeted widths");
    check(unused.has_value() && replies.json_counters.size() + 1U + unused.value() == wg::status_json_worst_case_bytes(), "JSON STATUS worst case counts every byte of the reply and its terminator");
    check(replies.json_status.size() + shorter_key_chars == replies.json_counters.size(), "JSON STATUS? differs from COUNTERS? only by its shorter object key");
    check(narrow_json_fields(members) == expected_narrow_json_fields(), "maximal live and artifact values fill their budgeted JSON widths exactly");
    check(member_value(members, "tx_error") == quoted(truncated_error), "JSON STATUS truncates an artifact error to its budgeted width");
}

// Every USB? member in emission order: begin_json(), the usb object and its
// format, then usb_field_names, each a uint32.
[[nodiscard]] std::vector<waveform_generator::StatusJsonField> usb_json_layout()
{
    namespace wg = waveform_generator;
    std::vector<wg::StatusJsonField> fields{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "usb", .max_value_chars = wg::literal_length("{")},
        {.name = "format", .max_value_chars = wg::json_string_chars(wg::literal_length("usb-diagnostics"))},
    };

    for (const char* name : wg::usb_field_names)
    {
        fields.push_back({.name = name, .max_value_chars = wg::max_uint32_decimal_chars});
    }

    return fields;
}

// USB? emits usb_field_names in order within their widths, each a decimal
// uint32, and the width it leaves unused closes usb_json_worst_case_bytes()
// exactly.
void check_usb_diagnostics(std::string_view reply)
{
    namespace wg = waveform_generator;
    const auto members = json_status_members(reply);
    const auto layout = usb_json_layout();
    const auto unused = unused_width(members, layout);
    const std::size_t header_members = layout.size() - wg::usb_field_names.size();
    const auto numeric = std::span{members}.subspan(std::min(members.size(), header_members));
    const auto is_uint32 = [](const StatusMember& member) { return is_decimal(member.value, wg::max_uint32_decimal_chars); };

    check(reply.starts_with(R"({"seq":)") && reply.ends_with("}}\n") && member_value(members, "format") == quoted("usb-diagnostics"),
          "USB? is one WFG-LIVE/1 reply whose usb object is in the usb-diagnostics format");
    check_ok(unused.status(), "USB? emits usb_field_names in order within their widths");
    check(numeric.size() == wg::usb_field_names.size() && std::ranges::all_of(numeric, is_uint32), "every USB? field is a decimal uint32");
    check(unused.has_value() && reply.size() + 1U + unused.value() == wg::usb_json_worst_case_bytes(), "USB? worst case counts every byte of the reply and its terminator");
}

// STATUS, STATUS? and INFO? report the identity the generated header holds.
void check_build_identity(const RenderedReplies& replies)
{
    namespace info = waveform_generator::build_info;
    const auto text_members = text_status_members(replies.text_status);
    const auto json_members = json_status_members(replies.json_counters);
    const auto dirty = std::to_string(std::to_underlying(info::fw_dirty));
    const auto commit_hi = commit_word(info::fw_commit, 0U);
    const auto commit_lo = commit_word(info::fw_commit, 1U);
    check_ok(commit_hi.status(), "fw_commit yields its first word");
    check_ok(commit_lo.status(), "fw_commit yields its second word");
    check(commit_hi.has_value() && commit_hi.value() == info::fw_commit_hi, "fw_commit_hi holds the first 8 hex digits of fw_commit");
    check(commit_lo.has_value() && commit_lo.value() == info::fw_commit_lo, "fw_commit_lo holds the next 8 hex digits of fw_commit");
    check(member_value(text_members, "fw_commit_hi") == std::to_string(info::fw_commit_hi), "STATUS reports fw_commit_hi");
    check(member_value(text_members, "fw_commit_lo") == std::to_string(info::fw_commit_lo), "STATUS reports fw_commit_lo");
    check(member_value(text_members, "fw_dirty") == dirty, "STATUS reports the dirty state");
    check(member_value(json_members, "build_id") == quoted(WFG_BUILD_ID), "JSON STATUS names the build label");
    check(member_value(json_members, "fw_commit") == quoted(info::fw_commit), "JSON STATUS names the commit");
    check(replies.info.contains(R"("fw_commit":)" + quoted(info::fw_commit) + R"(,"fw_dirty":)" + dirty + ","), "INFO reports the build commit and dirty state");
}

// The PID this test build's image enumerates with, as the lane's discovery
// knows it (0x4012 writable, 0x4013 read-only), and whether INFO says read-only.
#if defined(WFG_MSC_READ_ONLY)
constexpr std::string_view image_usb_pid{R"("usb_pid":16403,)"};
constexpr bool image_read_only = true;
#else
constexpr std::string_view image_usb_pid{R"("usb_pid":16402,)"};
constexpr bool image_read_only = false;
#endif

// Whether this test build is a WFG_UAC_MONO_ALT image: it offers alternate 3,
// 16-bit mono, and INFO lists the usb-audio-mono capability.
#if defined(WFG_UAC_MONO_ALT)
constexpr bool image_offers_mono = true;
#else
constexpr bool image_offers_mono = false;
#endif

// Whether this test build is a WFG_SD_CLOCK_24MHZ image, whose SD bus runs from
// the 24 MHz root (WFG_SD_USE_BOOTCLOCKRUN_USDHC1): INFO lists sd-clock-24mhz.
#if defined(WFG_SD_USE_BOOTCLOCKRUN_USDHC1)
constexpr bool image_sd_clock_24mhz = true;
#else
constexpr bool image_sd_clock_24mhz = false;
#endif

// INFO keeps the identity the lane checks and names the composite device and output route.
void check_lane_identity(std::string_view info)
{
    check(info.contains(R"("info":{"kind":"waveform-player","board":"MIMXRT1170-EVK",)"), "INFO keeps kind and board exactly");
    check(!info.contains("source_manifest_sha256"), "INFO omits the retired whole-source digest");
    check(info.contains(image_usb_pid), "INFO reports the PID this image enumerates with");
    check(info.contains(R"(,"read_only":true,)") == image_read_only, "INFO says read_only exactly for the read-only image");
    check(info.contains(R"("output":"codec",)") && info.contains(R"(,"usb-audio","usb-diagnostics")"), "INFO reports the route and the usb-audio and usb-diagnostics (USB?) capabilities");
    check(info.contains(R"(,"usb-audio","usb-diagnostics","playlist")"), "INFO lists the playlist capability after usb-diagnostics");
    check(info.contains(R"(,"playlist","usb-audio-mono")") == image_offers_mono && info.contains("usb-audio-mono") == image_offers_mono,
          "INFO lists the usb-audio-mono capability exactly on a WFG_UAC_MONO_ALT image");
    const auto optional_capabilities =
        image_read_only
            ? R"(,"engine-diagnostics","watterson","frequency-shift","wideband-noise","pulsed-cw","swept-cw","markov-cw","fsk-cw","faded-cw","stream-id","second-source","files"],"encoders":[)"
            : R"(,"engine-diagnostics","watterson","frequency-shift","wideband-noise","pulsed-cw","swept-cw","markov-cw","fsk-cw","faded-cw","stream-id","second-source","files","prepare","prepare-play-experimental"],"encoders":[)";
    check(info.contains(optional_capabilities), "INFO lists the optional capabilities in their documented order, with PREPAREPLAY only on writable images");
    check(info.contains(R"(,"sd-clock-24mhz")") == image_sd_clock_24mhz && info.contains("sd-clock-24mhz") == image_sd_clock_24mhz,
          "INFO lists the sd-clock-24mhz capability exactly on a WFG_SD_CLOCK_24MHZ image");
}

// Nothing else ties usb_service.cpp to status_contract.hpp, which sizes the
// reply buffer. The inputs the host can reach are driven to their type maxima;
// the codec counters and ring constants cannot be, so every member must fit its
// budgeted width and the unused width must close the worst case exactly.
void test_status_contract()
{
    const auto sequence_before = waveform_generator::response_sequence;
    const auto replies = render_at_maxima();
    check(waveform_generator::response_sequence == sequence_before && !status_seam().media.has_value(), "the status fixture restores the state it drove");
    check_text_status(replies.text_status);
    check_json_status(replies);
    check_usb_diagnostics(replies.usb);
    check_build_identity(replies);
    check_lane_identity(replies.info);
}

// A file that is not a whole number of 48-frame chunks (an even byte count, so
// the data chunk needs no RIFF pad byte), and a one-second one that is still
// playing when a test interrupts it.
constexpr std::size_t usb_file_frames = 5002U;
constexpr std::size_t usb_long_file_frames = 48000U;
constexpr std::uint32_t usb_idle_milliseconds = 5U;
// The player task starts the run this long after it accepted PLAY.
constexpr std::uint32_t usb_accept_to_run_milliseconds = 7U;
constexpr std::uint32_t usb_trailing_milliseconds = 20U;
constexpr std::uint32_t usb_interrupt_at_milliseconds = 30U;
// STATUS's uac_play_* fields: file frames, two CRCs, the first, last and
// accept indices, the first SOF, missed intervals and the first miss index.
constexpr std::size_t uac_play_field_count = 9U;
// A codec-route file: 0.1 s of a constant PCM16 level.
constexpr std::size_t codec_file_frames = 4800U;
constexpr std::int16_t codec_file_sample = 1000;
// The documented stall rule, written out rather than taken from the firmware's
// usb_stall_timeout_ms so the tests pin its value: more than 100 ms between IN
// completions while armed is error 20.
constexpr std::uint32_t documented_stall_timeout_ms = 100U;

// Full-range PCM24: both extremes, zero and +/-1, then the top 24 bits of a
// 32-bit LCG (Numerical Recipes constants), ending on both extremes again.
[[nodiscard]] std::vector<std::int32_t> full_range_samples(std::size_t count)
{
    namespace audio = waveform_generator::audio;
    constexpr std::uint32_t multiplier = 1664525U;
    constexpr std::uint32_t increment = 1013904223U;
    std::vector<std::int32_t> samples{audio::pcm24_min, audio::pcm24_max, 0, -1, 1};
    std::uint32_t state = 1U;

    while (samples.size() + 2U < count)
    {
        state = state * multiplier + increment;
        const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(state)>>(state);
        samples.push_back(audio::read_pcm24_le(std::span{bytes}.subspan(1U).data()));
    }

    samples.push_back(audio::pcm24_max);
    samples.push_back(audio::pcm24_min);
    return samples;
}

// A full-range file staged on the card as 2:/WG/<name>.
struct UsbFile
{
    std::vector<std::int32_t> samples;
    std::vector<std::uint8_t> wav;

    [[nodiscard]] std::span<const std::uint8_t> data_chunk() const { return std::span{wav}.subspan(wav_header_bytes); }
};

[[nodiscard]] UsbFile stage_usb_file(const std::string& name, std::size_t frames)
{
    UsbFile file{.samples = full_range_samples(frames), .wav = {}};
    file.wav = pcm24_wav(file.samples);
    files["2:/WG/" + name] = file.wav;
    return file;
}

// Silence, then `file` from stream index `first` as L = R = sample << 8, then
// silence again, and nothing else.
[[nodiscard]] bool received_exactly(std::span<const ua::StereoFrame> received, std::size_t first, std::span<const std::int32_t> file)
{
    if (received.size() < first + file.size())
    {
        return false;
    }

    for (std::size_t index = 0U; index < received.size(); ++index)
    {
        const bool in_file = index >= first && index - first < file.size();
        const std::int32_t expected = in_file ? file[index - first] * ua::subslot_scale : 0;

        if (received[index].left != expected || received[index].right != expected)
        {
            return false;
        }
    }

    return true;
}

// The play statistics describe exactly the file prefix the host received:
// zlib's CRC of that much of the data chunk on both channels, and the indices
// of its first frame (leading a chunk) and last.
void check_sent_prefix(const UsbFile& file, const char* what)
{
    const auto play = ua::play_statistics();
    const auto count = std::size_t{play.file_frames.value};
    const auto first = std::size_t{play.first_frame_index.value};
    const bool fits = count <= file.samples.size();
    const auto prefix_crc = fits ? waveform_generator::crc32(file.data_chunk().first(3U * count)) : 0U;
    check(fits && received_exactly(usb_host().received(), first, std::span{file.samples}.first(fits ? count : 0U)) && play.crc32_left == prefix_crc && play.crc32_right == prefix_crc &&
              first % ua::chunk_frame_count == 0U && (count == 0U || play.last_frame_index.value == first + count - 1U),
          what);
}

// Both STATUS forms carry each expected member with its value.
void check_status_members(std::span<const StatusMember> expected, const char* what)
{
    const auto lines = exchange("STATUS\n1 STATUS?\n");
    const auto text = text_status_members(lines.empty() ? std::string{} : lines[0] + "\n");
    const auto json = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    const bool reported =
        std::ranges::all_of(expected, [&text, &json](const StatusMember& member) { return member_value(text, member.name) == member.value && member_value(json, member.name) == member.value; });

    check(lines.size() == 2U && reported, what);
}

// Every uac_play_* member as a PLAY just accepted leaves it: zero, but for the
// stream index at which a USB play was accepted (0 for a codec play).
[[nodiscard]] std::vector<StatusMember> restarted_play_members(std::uint32_t accept_frame_index)
{
    std::vector<StatusMember> members;

    for (const std::string_view field : std::span{waveform_generator::status_field_names})
    {
        if (field.starts_with("uac_play_"))
        {
            members.push_back({.name = std::string{field}, .value = field == "uac_play_accept_frame_index" ? std::to_string(accept_frame_index) : "0"});
        }
    }

    return members;
}

void test_usb_play_needs_a_stream()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    check(file.samples.size() == usb_file_frames && command("OUTPUT USB").ok && command("LOAD:USB.WAV").ok, "the USB route and a file are selected");
    const auto refused = command("PLAY");
    check(!refused.ok && std::string_view{refused.error} == "NO_STREAM" && wg::player_state == wg::PlayerState::waiting_for_command,
          "PLAY on the USB route without an open stream is NO_STREAM and starts nothing");
    const auto lines = exchange("PLAY\n1 PLAY\n");
    check(lines.size() == 2U && lines[0] == "ERR NO_STREAM" && lines[1].contains(R"("error":{"code":"NO_STREAM",)"), "both grammars answer NO_STREAM");
}

// PLAY's refusals (#47): BAD_STATE with the card not Local, while a file
// generation runs, and after a run that has ended without LOAD;
// RUN_ID_REUSED for the last run's ID. A refusal starts nothing: the player
// stays waiting, nothing is requested, and the last run's ID stays.
void test_play_refusals()
{
    namespace wg = waveform_generator;
    constexpr std::size_t run_id_digits = 32U;
    const std::string first_id{"0123456789abcdef0123456789abcdef"};
    const std::string second_id{"fedcba9876543210fedcba9876543210"};
    const auto last_run_id = [] { return std::string_view{std::data(wg::live_snapshot.run_id), run_id_digits}; };
    const auto refused = [](const std::string& text, std::string_view error, wg::PlayerState state)
    {
        const auto reply = command(text.c_str());
        return !reply.ok && std::string_view{reply.error} == error && wg::player_state == state && !wg::play_requested;
    };

    files["2:/WG/REFUSE.WAV"] = wav(codec_file_frames, codec_file_sample);
    check(command("OUTPUT CODEC").ok && command("LOAD:REFUSE.WAV").ok, "a file is selected on the codec route");
    media_local = false;
    check(refused("PLAY", "BAD_STATE", wg::PlayerState::waiting_for_command), "PLAY with the card not Local is BAD_STATE and starts nothing");
    media_local = true;
    status_seam().tx_artifact_busy = true;
    check(refused("PLAY", "BAD_STATE", wg::PlayerState::waiting_for_command), "PLAY while a file generation runs is BAD_STATE and starts nothing");
    status_seam().tx_artifact_busy = false;
    check(command(("PLAY:" + first_id).c_str()).ok, "PLAY with a run ID is accepted");
    run_accepted();
    check(wg::player_state == wg::PlayerState::done, "the run is done");
    check(refused("PLAY:" + second_id, "BAD_STATE", wg::PlayerState::done), "PLAY after a run that has ended, without LOAD, is BAD_STATE");
    check(command("LOAD:REFUSE.WAV").ok, "LOAD selects the file again");
    check(refused("PLAY:" + first_id, "RUN_ID_REUSED", wg::PlayerState::waiting_for_command) && last_run_id() == first_id,
          "PLAY with the last run's ID is RUN_ID_REUSED, starts nothing and keeps that ID");
    check(command(("PLAY:" + second_id).c_str()).ok, "another run ID is accepted");
    run_accepted();
    check(wg::player_state == wg::PlayerState::done && last_run_id() == second_id, "and its run is done under the new ID");
    files.erase("2:/WG/REFUSE.WAV");
}

// A PLAY whose WAV the player refuses: the run ends at the check with the
// WAV's player error, before any output starts.
void test_play_of_a_refused_wav()
{
    namespace wg = waveform_generator;
    auto riff = wav(codec_file_frames, codec_file_sample);
    riff[3] = 'X';
    files["2:/WG/RIFX.WAV"] = riff;
    check(command("OUTPUT CODEC").ok && command("LOAD:RIFX.WAV").ok && command("PLAY").ok, "a PLAY of a WAV the player refuses is accepted");
    run_accepted();
    const auto lines = exchange("STATUS\n");
    const auto text = text_status_members(lines.empty() ? std::string{} : lines[0] + "\n");
    check(wg::player_state == wg::PlayerState::fault && member_value(text, "player_error") == std::to_string(std::to_underlying(wg::PlayerError::invalid_riff)) &&
              member_value(text, "file_frames_total") == "0",
          "the run ends in a fault with invalid_riff and reads no data");
    files.erase("2:/WG/RIFX.WAV");
}

// A PLAY whose card fails: a failed read of the WAV's header ends the run
// with header_io, a failed read of its data with sd_read, a failed seek to
// its data with seek (also with a REFERENCE, which measures nothing).
void test_play_with_card_faults()
{
    namespace wg = waveform_generator;
    files["2:/WG/CARD.WAV"] = wav(codec_file_frames, codec_file_sample);
    constexpr FSIZE_t data_offset = 44U;
    // The header's reads: RIFF, then the chunk headers and the fmt body.
    constexpr unsigned int header_reads = 4U;
    struct Fault
    {
        std::optional<unsigned int> reads;
        std::optional<FSIZE_t> seek;
        bool with_reference;
        wg::PlayerError error;
        const char* what;
    };

    const auto faults = std::to_array<Fault>({
        {.reads = 0U, .seek = std::nullopt, .with_reference = false, .error = wg::PlayerError::header_io, .what = "a failed header read ends the run with header_io"},
        {.reads = header_reads, .seek = std::nullopt, .with_reference = false, .error = wg::PlayerError::sd_read, .what = "a failed data read ends the run with sd_read"},
        {.reads = std::nullopt, .seek = data_offset, .with_reference = false, .error = wg::PlayerError::seek, .what = "a failed seek to the data ends the run with seek"},
        {.reads = std::nullopt, .seek = data_offset, .with_reference = true, .error = wg::PlayerError::seek, .what = "a failed seek to the data ends the run with seek when nothing is measured"},
    });

    for (const Fault& fault : faults)
    {
        check(command("OUTPUT CODEC").ok && command("LOAD:CARD.WAV").ok && (!fault.with_reference || command("REFERENCE:0.125").ok) && command("PLAY").ok, "a PLAY of a good WAV is accepted");
        card_faults() = {.seek = fault.seek, .reads_before_failure = fault.reads};
        run_accepted();
        card_faults() = {};
        const auto lines = exchange("STATUS\n");
        const auto text = text_status_members(lines.empty() ? std::string{} : lines[0] + "\n");
        check(wg::player_state == wg::PlayerState::fault && member_value(text, "player_error") == std::to_string(std::to_underlying(fault.error)), fault.what);
    }

    files.erase("2:/WG/CARD.WAV");
}

// The player's mailbox takes one command at a time: another before the
// player task takes the first is refusal::busy, the code BUSY a reply
// carries; PLAY from the firmware's own start is refused the same way.
void test_mailbox_refusal()
{
    namespace wg = waveform_generator;
    wg::live_protocol::Request request{};
    request.kind = wg::live_protocol::Kind::output_codec;
    check(wg::submit_player_command(request).is_ok(), "the mailbox takes a command");
    const m110::Status refused = wg::submit_player_command(request);
    check(refused.code == m110::StatusCode::busy && std::string_view{refused.message} == "BUSY", "a second command is refused BUSY");
    const m110::Status play = wg::request_player_play();
    check(play.code == m110::StatusCode::busy && std::string_view{play.message} == "player command busy", "PLAY is refused while the mailbox is full");
    wg::service_player_command();
    wg::ControlReply reply;
    check(wg::take_player_reply(reply) && wg::submit_player_command(request).is_ok(), "once served and replied, the mailbox takes a command again");
    wg::service_player_command();
    check(wg::take_player_reply(reply), "and replies to it");
}

// The lane's checks of a USB play, all exact.
void check_usb_play_status(const UsbFile& file)
{
    const auto play = ua::play_statistics();
    const auto crc = std::to_string(waveform_generator::crc32(file.data_chunk()));
    const auto lines = exchange("STATUS\n1 STATUS?\n");
    // sent_lines() strips the newline that ends the last text member.
    const auto text = text_status_members(lines.empty() ? std::string{} : lines[0] + "\n");
    const auto json = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    check(member_value(text, "output_route") == "1" && member_value(text, "pcm_drained") == "1" && member_value(text, "underruns") == "0" && member_value(text, "engine_state") == "0" &&
              member_value(json, "live_events") == "0",
          "STATUS reports a clean, drained, unimpaired play on the USB route");
    check(member_value(text, "uac_play_file_frames") == std::to_string(file.samples.size()) && member_value(text, "file_frames_total") == std::to_string(file.samples.size()) &&
              member_value(text, "uac_play_crc32_l") == crc && member_value(text, "uac_play_crc32_r") == crc && member_value(json, "uac_play_crc32_l") == crc,
          "uac_play_file_frames equals file_frames_total and both CRCs equal zlib.crc32(data chunk)");
    check(member_value(text, "uac_play_first_frame_index") == std::to_string(play.first_frame_index.value) &&
              member_value(text, "uac_play_last_frame_index") == std::to_string(play.last_frame_index.value) &&
              member_value(text, "uac_play_first_frame_sof") == std::to_string(play.first_frame_sof.value) &&
              member_value(text, "uac_play_accept_frame_index") == std::to_string(play.accept_frame_index.value),
          "STATUS reports the play's span in the stream");
    check(member_value(text, "uac_play_missed_intervals") == "0" && member_value(json, "uac_play_missed_intervals") == "0" && member_value(text, "uac_play_first_miss_frame_index") == "0" &&
              member_value(json, "uac_play_first_miss_frame_index") == "0",
          "the play missed no interval (the lane's per-play assertion)");
    check(member_value(text, "uac_stream_open") == "1" && member_value(text, "uac_short_packets") == "0" && member_value(text, "usb_stack_asserts") == "0" &&
              member_value(text, "uac_host_volume") == "0" && member_value(text, "uac_host_mute") == "0",
          "the stream sent no short packet and the stack never asserted");
    check(member_value(text, "uac_stream_frames") == std::to_string(usb_host().received_frames()) && member_value(json, "uac_stream_frames") == std::to_string(ua::stream_counters().frames_sent.value),
          "uac_stream_frames is the exact count of frames sent when the reply is formed");
}

void test_usb_play_is_exact()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:USB.WAV").ok, "the USB route and a full-range file are selected");
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    const auto status = wg::player_snapshot();
    const auto at_done = ua::play_statistics();
    const auto first = std::size_t{at_done.first_frame_index.value};
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U && status.underruns == 0U &&
              status.file_frames_total == file.samples.size(),
          "a USB play drains every file frame without underrun");
    check(at_done.file_frames.value == file.samples.size() && at_done.pending_frames == ua::FrameCount{} && !at_done.armed && host.received().size() >= first + file.samples.size(),
          "done is reported only once the last file frame is confirmed sent");
    check(!host.media_host_allowed_while_armed() && host.armed_milliseconds() != 0U, "MEDIA HOST is refused while the USB play is armed");
    host.run(usb_trailing_milliseconds);
    check_sent_prefix(file, "exact zeros, the file as L = R = sample << 8 including both extremes, then exact zeros");
    // The session's first millisecond completes TinyUSB's zero-length primer.
    check(at_done.accept_frame_index.value == (usb_idle_milliseconds - 1U) * ua::chunk_frame_count &&
              first >= at_done.accept_frame_index.value + usb_accept_to_run_milliseconds * ua::chunk_frame_count,
          "accept_frame_index is the stream index when PLAY was accepted; the first file frame follows the run's start");
    check(at_done.first_frame_sof.value == host.frame_that_sent(first) && at_done.first_frame_sof.value >= ua::usb_frame_number_modulus,
          "the first file frame's USB frame is the completion that carried it, extended across the 11-bit wrap");
    check_usb_play_status(file);
    const std::vector<StatusMember> done{{.name = "player_state", .value = "7"}, {.name = "player_error", .value = "0"}};
    check_status_members(done, "a done play is player_state 7, player_error 0 in both STATUS forms");
    const std::vector<StatusMember> stream_clean{{.name = "uac_missed_intervals", .value = "0"}};
    check_status_members(stream_clean, "a host that polled every frame leaves the stream's own count at zero too");
}

// Windows skips a few frames as a capture stream starts (bench, 2026-10-02).
// uac_missed_intervals, the stream's diagnostic, counts them; the play's own
// count does not: neither a skip that ended before PLAY nor one still going on
// when the play arms, since the play judges only the gaps that end while its
// file frames are queued, and none is queued before its first completion
// after arming.
void test_usb_startup_skips_are_the_streams()
{
    constexpr std::uint32_t ended_skip = 4U;
    constexpr std::uint32_t resume_at_armed_millisecond = 3U;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    const auto missed_before = ua::stream_counters().missed_intervals;
    host.skip_polling(ended_skip);
    host.run(1U);
    const auto last_before_play = host.last_completion_tick();
    host.stop_polling();
    check(command("LOAD:USB.WAV").ok, "a full-range file is selected");
    host.schedule({.resume_polling_at = resume_at_armed_millisecond});
    run_selected(usb_accept_to_run_milliseconds);
    const auto resumed = host.resume_tick();
    const auto spanning_skip = resumed.value_or(last_before_play) - last_before_play - 1U;
    const auto status = waveform_generator::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(waveform_generator::PlayerState::done) && resumed.has_value() && spanning_skip > usb_accept_to_run_milliseconds,
          "the play completes after a skip that began before PLAY and ended after the arming");
    check_sent_prefix(file, "the play after the start-up skips is exact");
    const std::vector<StatusMember> counts{
        {.name = "uac_missed_intervals", .value = std::to_string(missed_before.value + ended_skip + spanning_skip)},
        {.name = "uac_play_missed_intervals", .value = "0"},
        {.name = "uac_play_first_miss_frame_index", .value = "0"},
    };
    check_status_members(counts, "the start-up skips count for the stream and never for the play");
}

// STOP part way: the queued file chunks still go out and count; the
// statistics are final when the run reports aborted, and silence follows.
void test_usb_stop_mid_play()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds});
    run_selected();
    wg::ControlReply stop_reply;
    check(wg::take_player_reply(stop_reply) && stop_reply.ok, "STOP during a USB play is acknowledged");
    const auto status = wg::player_snapshot();
    const auto at_end = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::aborted) && status.pcm_drained == 0U && !at_end.armed && at_end.pending_frames == ua::FrameCount{} &&
              at_end.file_frames.value != 0U && at_end.file_frames.value < file.samples.size(),
          "STOP aborts the USB play once its queued file frames are confirmed sent");
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().file_frames == at_end.file_frames && ua::play_statistics().crc32_left == at_end.crc32_left, "the statistics are final when the run reports its end");
    check_sent_prefix(file, "after STOP the statistics describe the sent prefix and silence follows it");
    const std::vector<StatusMember> aborted{{.name = "player_state", .value = "8"}, {.name = "player_error", .value = "0"}};
    check_status_members(aborted, "a play STOP ended is player_state 8 in both STATUS forms");
}

void test_usb_stream_closed_mid_play()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    host.schedule({.close_stream_at = usb_interrupt_at_milliseconds});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_stream_closed) && play.interrupted && !play.armed,
          "the stream closing during a USB play is error 19");
    const std::vector<StatusMember> closed{{.name = "player_state", .value = "255"}, {.name = "player_error", .value = "19"}};
    check_status_members(closed, "both STATUS forms render it as player_state 255, player_error 19");
    check(play.file_frames.value != 0U && play.file_frames.value < file.samples.size() && play.pending_frames == ua::FrameCount{}, "the chunks the close dropped never count");
    check_sent_prefix(file, "after a close the statistics describe the received prefix");
}

// The stream closes after the last file frame was taken from the FIFO but
// before it was sent: the drain ends with frames dropped, which is error 19,
// not done.
void test_usb_stream_closed_while_draining()
{
    namespace wg = waveform_generator;
    constexpr std::size_t short_file_frames = 3U * ua::chunk_frame_count;
    // Armed with silence in flight and two silent chunks queued, the third file
    // chunk is taken on the 3rd millisecond and the empty FIFO is found on the
    // 4th (draining); on the 5th the first file chunk has gone out.
    constexpr std::uint32_t close_at_millisecond = 5U;
    const auto file = stage_usb_file("SHORT.WAV", short_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:SHORT.WAV").ok, "a short file is selected");
    host.schedule({.close_stream_at = close_at_millisecond});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_stream_closed) && status.eof_count == 1U,
          "a stream closed while the play drains is error 19, not done");
    check(play.file_frames == ua::chunk_frames && status.pcm_drained == 0U, "only the file chunk that went out counts");
    check_sent_prefix(file, "the statistics describe the received prefix of a drain cut short");
}

// The host closes the stream right after the completion that confirms the last
// file frame, before the player's next drain poll: the close dropped no file
// frame, so the run is done, not error 19.
void test_usb_stream_closed_after_delivery()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok, "a full-range file is selected");
    host.schedule({.close_after_file_frames = file.samples.size()});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(play.interrupted && ua::stream_counters().state == ua::StreamState::closed, "the stream closed while the delivered play was still armed");
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U,
          "a close after the last file frame was confirmed sent leaves the run done, not error 19");
    check(play.file_frames.value == file.samples.size() && play.pending_frames == ua::FrameCount{}, "every file frame counts");
    check_sent_prefix(file, "the statistics describe the whole file the host received");
}

// The host stops polling while the play drains: the player's 2 ms drain poll
// declares the stall once more than 100 ms have passed since the last
// completion, and not before. If the host polls again later, the chunks
// still queued go out and join the statistics, and so does the stall: it
// delayed them, so the play counts it although the player disarmed first.
void test_usb_stall()
{
    namespace wg = waveform_generator;
    constexpr std::size_t short_file_frames = 3U * ua::chunk_frame_count;
    // As in test_usb_stream_closed_while_draining: by the 5th armed
    // millisecond the play drains, its first file chunk sent.
    constexpr std::uint32_t stop_at_millisecond = 5U;
    constexpr std::uint32_t drain_poll_milliseconds = 2U;
    const auto file = stage_usb_file("SHORT.WAV", short_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:SHORT.WAV").ok, "a short file is selected");
    host.schedule({.stop_polling_at = stop_at_millisecond});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto fault_tick = host.fault_tick();
    const auto silent_for = fault_tick.value_or(host.last_completion_tick()) - host.last_completion_tick();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_stall) && status.eof_count == 1U &&
              fault_tick.has_value(),
          "a host that stops polling while the play drains is error 20");
    const std::vector<StatusMember> stalled{{.name = "player_state", .value = "255"}, {.name = "player_error", .value = "20"}};
    check_status_members(stalled, "both STATUS forms render it as player_state 255, player_error 20");
    check(silent_for > documented_stall_timeout_ms && silent_for <= documented_stall_timeout_ms + drain_poll_milliseconds,
          "the stall is declared once 100 ms pass without a completion, within one drain poll");
    const auto at_end = ua::play_statistics();
    check(at_end.file_frames == ua::chunk_frames && at_end.pending_frames != ua::FrameCount{} && !at_end.armed, "the run ends with file chunks still queued");
    check(at_end.missed_intervals == ua::IntervalCount{} && at_end.first_miss_frame_index == ua::FrameCount{}, "no completion has ended the stall yet, so the play has counted none of it");
    check_sent_prefix(file, "after a stall the statistics describe the received prefix");
    // The host polls again in the next millisecond.
    const auto stall_missed = host.tick() - host.last_completion_tick() - 1U;
    const auto resume_index = host.received().size();
    host.resume_polling();
    host.run(usb_trailing_milliseconds);
    const auto resumed = ua::play_statistics();
    check(resumed.file_frames.value == file.samples.size() && resumed.pending_frames == ua::FrameCount{}, "if the host polls again, the chunks still queued go out and count");
    check_sent_prefix(file, "chunks a resuming host completes join the prefix, then silence");
    check(stall_missed > documented_stall_timeout_ms && resumed.first_miss_frame_index <= resumed.last_frame_index, "the stall fell before the play's last file frame");
    const std::vector<StatusMember> counted{
        {.name = "uac_play_missed_intervals", .value = std::to_string(stall_missed)},
        {.name = "uac_play_first_miss_frame_index", .value = std::to_string(resume_index)},
    };
    check_status_members(counted, "the play counts the stall that delayed its queued chunks, located at the first frame sent after it");
}

// STOP part way, then the host skips a few milliseconds before the file chunks
// queued at the disarm go out: they still count for the play, and so does the
// gap that delayed them. The run ends aborted once they are confirmed sent.
void test_usb_skip_after_stop()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t skipped = 7U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    const auto stream_before = ua::stream_counters().missed_intervals;
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds, .skip_after_disarm = skipped});
    run_selected();
    wg::ControlReply stop_reply;
    check(wg::take_player_reply(stop_reply) && stop_reply.ok, "STOP during a USB play is acknowledged");
    const auto play = ua::play_statistics();
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::aborted) && host.resume_frame_index().has_value() && play.pending_frames == ua::FrameCount{} &&
              play.file_frames.value != 0U && play.file_frames.value < file.samples.size(),
          "STOP aborts the USB play once its queued file frames are sent after the gap");
    check_sent_prefix(file, "the statistics describe the sent prefix, the chunks sent after the gap included");
    const auto resume_index = host.resume_frame_index().value_or(0U);
    check(resume_index <= play.last_frame_index.value, "the gap fell before the play's last file frame");
    const std::vector<StatusMember> counted{
        {.name = "uac_missed_intervals", .value = std::to_string(stream_before.value + skipped)},
        {.name = "uac_play_missed_intervals", .value = std::to_string(skipped)},
        {.name = "uac_play_first_miss_frame_index", .value = std::to_string(resume_index)},
    };
    check_status_members(counted, "the gap after STOP counts for the play it delayed, located at the first frame sent after it");
}

// STOP part way, then the host polls nothing for longer than the stall
// timeout, the chunks queued at the disarm still waiting: the wait for them
// ends with error 20 once no completion has come for 100 ms, as a stall ends
// an armed play, rather than the run ending aborted with its statistics
// short of what may still go out. Once the host polls again the chunks go out
// and count.
void test_usb_stall_after_stop()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t skipped = 2U * documented_stall_timeout_ms;
    constexpr std::uint32_t stop_poll_milliseconds = 1U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds, .skip_after_disarm = skipped});
    run_selected();
    wg::ControlReply stop_reply;
    check(wg::take_player_reply(stop_reply) && stop_reply.ok, "STOP during a USB play is acknowledged");
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    const auto ended_at = host.fault_tick().value_or(host.tick());
    const auto waited = ended_at - host.last_completion_tick();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_stall),
          "a host that stops polling after STOP, with file frames queued, ends the run with error 20, not aborted");
    check(waited > documented_stall_timeout_ms && waited <= documented_stall_timeout_ms + 2U * stop_poll_milliseconds,
          "the wait ends once no completion has come for 100 ms, within a poll of the player");
    check(play.pending_frames != ua::FrameCount{} && !play.armed, "the run ended with file frames still queued, the play disarmed");
    const std::vector<StatusMember> stalled{{.name = "player_state", .value = "255"}, {.name = "player_error", .value = "20"}};
    check_status_members(stalled, "both STATUS forms render it as player_state 255, player_error 20");
    host.run(skipped + usb_trailing_milliseconds);
    check(ua::play_statistics().pending_frames == ua::FrameCount{} && ua::play_statistics().file_frames > play.file_frames, "once the host polls again, the queued chunks go out and count");
    check_sent_prefix(file, "and the statistics describe the prefix the host received");
}

// What a run ended with once the controller stopped loading the play's
// queued chunks (UsbHost::starve_loads): the tick at which it stopped, and
// the tick of the last completion that confirmed file frames sent.
struct StarvedRun
{
    std::optional<std::uint32_t> starved_at{};
    std::uint32_t last_confirmed_at{};
};

// The play's queued file frames stop falling while completions keep coming:
// a drain whose file has ended (error 26 from the drain loop), or the wait
// after a STOP (from stop_output()). Either wait ends once they have not
// fallen for more than 100 ms, at the player's next poll (2 ms in the drain,
// 1 ms after a STOP). The host's later polls send the chunks, which still
// count.
void check_unconfirmed_end(const UsbFile& file, const StarvedRun& run, std::uint32_t poll_milliseconds, const char* what)
{
    namespace wg = waveform_generator;
    auto& host = usb_host();
    const auto status = wg::player_snapshot();
    // A fault the wait after a STOP latches ends the run before another
    // millisecond can see it: then the run ended at the host's tick.
    const auto ended_at = host.fault_tick().value_or(host.tick());
    const auto play = ua::play_statistics();
    const auto waited = ended_at - run.last_confirmed_at;
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_unconfirmed) && run.starved_at.has_value(), what);
    check(waited > documented_stall_timeout_ms && waited <= documented_stall_timeout_ms + 2U * poll_milliseconds,
          "the wait ends once the queued frames have not fallen for 100 ms, within a poll of the player");
    const std::vector<StatusMember> unconfirmed{{.name = "player_state", .value = "255"}, {.name = "player_error", .value = "26"}};
    check_status_members(unconfirmed, "both STATUS forms render it as player_state 255, player_error 26");
    check(play.pending_frames != ua::FrameCount{} && host.last_completion_tick() + 1U >= ended_at && !play.stalled,
          "completions kept coming and the play never stalled, with file frames still queued");
    check_sent_prefix(file, "the statistics describe the prefix confirmed sent");
    host.feed_loads();
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().pending_frames == ua::FrameCount{} && ua::play_statistics().file_frames > play.file_frames, "once the controller loads again, the queued chunks go out and count");
    check_sent_prefix(file, "and the statistics describe the prefix the host received");
}

// Watches a run on the USB route: starves the controller's loads at the first
// millisecond `starve_when` holds, and records the last completion that
// confirmed file frames sent.
void watch_starved_run(StarvedRun& run, std::function<bool()> starve_when)
{
    auto& host = usb_host();
    host.on_tick(
        [&run, &host, starve_when = std::move(starve_when)]
        {
            if (!run.starved_at.has_value() && starve_when())
            {
                host.starve_loads();
                run.starved_at = host.tick();
            }
        });
    host.on_completion(
        [&run, &host, confirmed = ua::FrameCount{}]() mutable
        {
            const auto play = ua::play_statistics();

            if (play.file_frames != confirmed)
            {
                confirmed = play.file_frames;
                run.last_confirmed_at = host.tick();
            }
        });
}

// The file has ended and the play drains when the controller stops loading
// its last queued chunks: the drain loop ends with error 26.
void test_usb_drain_unconfirmed()
{
    namespace wg = waveform_generator;
    constexpr std::size_t short_file_frames = 3U * ua::chunk_frame_count;
    constexpr std::uint32_t drain_poll_milliseconds = 2U;
    const auto file = stage_usb_file("SHORT.WAV", short_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:SHORT.WAV").ok, "a short file is selected");
    StarvedRun run{};
    watch_starved_run(run, [] { return wg::player_state == wg::PlayerState::draining; });
    host.schedule({});
    run_selected();
    host.on_tick({});
    host.on_completion({});
    check(wg::player_snapshot().eof_count == 1U, "the file had ended");
    check_unconfirmed_end(file, run, drain_poll_milliseconds, "a drain whose queued chunks never go out ends with error 26, not a hang");
}

// STOP part way while the controller stops loading: stop_output() ends its
// wait for the queued chunks with error 26.
void test_usb_stop_unconfirmed()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t stop_poll_milliseconds = 1U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    StarvedRun run{};
    watch_starved_run(run, [] { return wg::stop_requested.load(); });
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds});
    run_selected();
    host.on_tick({});
    host.on_completion({});
    wg::ControlReply stop_reply;
    check(wg::take_player_reply(stop_reply) && stop_reply.ok, "STOP during a USB play is acknowledged");
    check_unconfirmed_end(file, run, stop_poll_milliseconds, "a STOP whose queued chunks never go out ends with error 26, not a hang");
}

// A drain the host stops polling in (test_usb_stall_before_unconfirmed): the
// tick the controller stopped loading, the tick of the player task's first
// check after that, and the tick of the last completion that changed the
// play's queued file frames.
struct StalledDrain
{
    std::optional<std::uint32_t> starved_at{};
    std::optional<std::uint32_t> checked_at{};
    std::uint32_t last_change{};
};

// A host that stops polling while a USB play drains is a stall (20), not
// unconfirmed (26), when both are due at the same drain check. Once the play
// drains, the controller loads nothing more (UsbHost::starve_loads), so the
// play's queued file frames stop falling. The host completes zero-length
// packets up to the tick of the player task's first check after that, then
// polls no more. The stall clock (from the last completion) and the
// unconfirmed watch (from the frames the player last saw fall) start at that
// same tick, so both pass 100 ms at the same check, where the sink's faults
// (check_output()) come before the drain's watch (check_drain()).
void test_usb_stall_before_unconfirmed()
{
    namespace wg = waveform_generator;
    constexpr std::size_t short_file_frames = 3U * ua::chunk_frame_count;
    constexpr std::uint32_t drain_poll_milliseconds = 2U;
    const auto file = stage_usb_file("SHORT.WAV", short_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:SHORT.WAV").ok, "a short file is selected");
    StalledDrain drain{};
    // A wait of the player task since the last millisecond: it checked the
    // output at this tick.
    host.on_tick(
        [&drain, &host, waits = wait_calls]() mutable
        {
            const bool checked = std::exchange(waits, wait_calls) != wait_calls;

            if (!drain.starved_at.has_value() && wg::player_state == wg::PlayerState::draining)
            {
                host.starve_loads();
                drain.starved_at = host.tick();
            }
            else if (drain.starved_at.has_value() && !drain.checked_at.has_value() && checked)
            {
                drain.checked_at = host.tick();
            }
            else if (drain.checked_at == host.tick() - 1U)
            {
                host.stop_polling();
            }
        });
    host.on_completion(
        [&drain, &host, queued = ua::FrameCount{}]() mutable
        {
            const auto pending = ua::play_statistics().pending_frames;

            if (pending != queued)
            {
                queued = pending;
                drain.last_change = host.tick();
            }
        });
    host.schedule({});
    run_selected();
    host.on_tick({});
    host.on_completion({});
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    const auto checked_at = drain.checked_at.value_or(0U);
    const auto ended_at = host.fault_tick().value_or(host.tick());
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_stall),
          "a host that stops polling while the play drains is error 20, though the play's queued frames had stopped falling as long");
    check(drain.checked_at.has_value() && drain.last_change < checked_at && host.last_completion_tick() == checked_at,
          "the frames last fell before the player's first check after the controller stopped loading, and the host's last completion came in that check's tick");
    check(ended_at - checked_at > documented_stall_timeout_ms && ended_at - checked_at <= documented_stall_timeout_ms + drain_poll_milliseconds,
          "both clocks passed 100 ms at the same drain check, which latched the stall");
    check(play.pending_frames != ua::FrameCount{} && !play.armed && !play.stalled, "the run ended with file frames still queued, the gap not yet ended");
    host.feed_loads();
    host.resume_polling();
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().pending_frames == ua::FrameCount{} && ua::play_statistics().file_frames.value == file.samples.size(),
          "once the host polls again and the controller loads, the queued chunks go out and count");
    check_sent_prefix(file, "and the statistics describe the whole file the host received");
}

// The wait after STOP, when the host stops polling at the tick its queued
// frames stop falling: both clocks pass 100 ms at the same check, and the
// stall (20) wins over unconfirmed (26), as in the drain
// (test_usb_stall_before_unconfirmed). Once the play is disarmed the
// controller loads nothing more; the host completes zero-length packets up to
// the tick of the player task's first wait after that, then polls no more.
void test_usb_stall_after_stop_before_unconfirmed()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t stop_poll_milliseconds = 1U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    StalledDrain wait{};
    host.on_tick(
        [&wait, &host, waits = wait_calls]() mutable
        {
            const bool checked = std::exchange(waits, wait_calls) != wait_calls;
            const auto play = ua::play_statistics();

            if (!wait.starved_at.has_value() && wg::stop_requested.load() && !play.armed && play.pending_frames != ua::FrameCount{})
            {
                host.starve_loads();
                wait.starved_at = host.tick();
            }
            else if (wait.starved_at.has_value() && !wait.checked_at.has_value() && checked)
            {
                wait.checked_at = host.tick();
            }
            else if (wait.checked_at == host.tick() - 1U)
            {
                host.stop_polling();
            }
        });
    host.on_completion(
        [&wait, &host, queued = ua::FrameCount{}]() mutable
        {
            const auto pending = ua::play_statistics().pending_frames;

            if (pending != queued)
            {
                queued = pending;
                wait.last_change = host.tick();
            }
        });
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds});
    run_selected();
    host.on_tick({});
    host.on_completion({});
    wg::ControlReply stop_reply;
    check(wg::take_player_reply(stop_reply) && stop_reply.ok, "STOP during a USB play is acknowledged");
    const auto status = wg::player_snapshot();
    const auto checked_at = wait.checked_at.value_or(0U);
    const auto ended_at = host.fault_tick().value_or(host.tick());
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_stall),
          "a host that stops polling in the wait after STOP is error 20, though the queued frames had stopped falling as long");
    check(wait.checked_at.has_value() && wait.last_change < checked_at && host.last_completion_tick() == checked_at,
          "the frames last fell before the player's first wait after the controller stopped loading, and the host's last completion came in that tick");
    check(ended_at - checked_at > documented_stall_timeout_ms && ended_at - checked_at <= documented_stall_timeout_ms + 2U * stop_poll_milliseconds,
          "both clocks passed 100 ms at the same check of the wait, which latched the stall");
    host.feed_loads();
    host.resume_polling();
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().pending_frames == ua::FrameCount{}, "once the host polls again and the controller loads, the queued chunks go out");
    check_sent_prefix(file, "and the statistics describe the prefix the host received");
}

// A polling gap while the producer paces the FIFO: armed at its high
// watermark, it sleeps about 200 ms, so the gap (armed milliseconds 30 to 30 +
// `skipped`) has ended before the player task looks again.
[[nodiscard]] waveform_generator::PlayerSnapshot play_with_polling_gap(std::uint32_t skipped_milliseconds)
{
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    host.schedule({.stop_polling_at = usb_interrupt_at_milliseconds, .resume_polling_at = usb_interrupt_at_milliseconds + skipped_milliseconds});
    run_selected();
    return waveform_generator::player_snapshot();
}

// Skipping n milliseconds puts n + 1 between completions: skipping one less
// than the timeout is a gap of exactly 100 ms, not a stall. The play completes
// exactly and STATUS counts the skipped intervals.
void test_usb_gap_within_the_timeout()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t skipped = documented_stall_timeout_ms - 1U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    const auto missed_before = ua::stream_counters().missed_intervals;
    const auto status = play_with_polling_gap(skipped);
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U && ua::play_statistics().file_frames.value == file.samples.size() &&
              !ua::play_statistics().stalled,
          "a gap of exactly 100 ms between completions is not a stall");
    check_sent_prefix(file, "the play around the gap is exact");
    const auto after_gap = usb_host().resume_frame_index();
    const std::vector<StatusMember> missed{
        {.name = "uac_missed_intervals", .value = std::to_string(missed_before.value + skipped)},
        {.name = "uac_play_missed_intervals", .value = std::to_string(skipped)},
        {.name = "uac_play_first_miss_frame_index", .value = std::to_string(after_gap.value_or(0U))},
    };
    check(after_gap.has_value() && after_gap.value_or(0U) > ua::play_statistics().first_frame_index.value, "the gap fell inside the file");
    check_status_members(missed, "STATUS counts each skipped interval for the stream and the play, and locates the play's first miss");
}

// One millisecond more is a stall, although the host polls again before the
// player task looks: the USB ISR judged the gap when it ended.
void test_usb_gap_past_the_timeout()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    const auto status = play_with_polling_gap(documented_stall_timeout_ms);
    const auto& host = usb_host();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_stall) && ua::play_statistics().stalled,
          "a gap of 101 ms between completions is error 20");
    check(host.resume_tick().has_value() && host.fault_tick().has_value() && host.fault_tick().value_or(0U) > host.resume_tick().value_or(0U),
          "the player found the stall only after the gap had ended");
    check(ua::play_statistics().pending_frames == ua::FrameCount{} && host.tick() - host.last_completion_tick() == 1U, "the host polls again, so the run waits for the chunks it had queued");
    check(ua::play_statistics().missed_intervals.value == documented_stall_timeout_ms && ua::play_statistics().first_miss_frame_index.value == host.resume_frame_index().value_or(0U),
          "the play counts the 100 intervals the stall skipped, located after the gap");
    check_sent_prefix(file, "after the gap the statistics describe the received prefix");
}

// Whether STATUS reports the field called name.
[[nodiscard]] bool status_reports(std::string_view name)
{
    return std::ranges::any_of(std::span{waveform_generator::status_field_names}, [name](std::string_view field) { return field == name; });
}

// The USB? reply's members, or none when lines holds no reply.
[[nodiscard]] std::vector<StatusMember> usb_members(const std::vector<std::string>& lines)
{
    return json_status_members(lines.empty() ? std::string{} : lines[0]);
}

// Every expected member has its value among members.
[[nodiscard]] bool has_members(std::span<const StatusMember> members, std::span<const StatusMember> expected)
{
    return std::ranges::all_of(expected, [members](const StatusMember& member) { return member_value(members, member.name) == member.value; });
}

// USB? reports the stream diagnostics STATUS does, with STATUS's values, and
// the miss attribution STATUS has no room for. A polling gap during a play is
// located at the first frame sent after it, the play's, its length the largest
// gap, and the host's: the service watchdog found the packet waiting in each
// millisecond of it. An ISR held off past the host's polls later, with the
// play done, is the device's: its missed intervals are unprimed, the stream's
// alone.
void test_usb_diagnostics_query()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t skipped = 9U;
    constexpr std::uint32_t late_by = 3U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    const auto before = ua::stream_counters();
    const auto status = play_with_polling_gap(skipped);
    auto& host = usb_host();
    const auto resume = host.resume_frame_index();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && resume.has_value() && ua::play_statistics().file_frames.value == file.samples.size(),
          "the play around the polling gap completes");
    auto lines = exchange("1 USB?\n2 STATUS?\n");
    auto usb = usb_members(lines);
    const auto json_status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    const bool shared_agree =
        std::ranges::all_of(wg::usb_field_names, [&usb, &json_status](std::string_view name) { return !status_reports(name) || member_value(usb, name) == member_value(json_status, name); });

    check(lines.size() == 2U && lines[0].contains(R"("ok":true,)") && usb.size() == usb_json_layout().size() && shared_agree,
          "USB? answers with every field, and each one STATUS reports too has STATUS's value");
    const std::vector<StatusMember> host_gap{
        {.name = "uac_missed_intervals", .value = std::to_string(before.missed_intervals.value + skipped)},
        {.name = "uac_last_miss_frame_index", .value = std::to_string(resume.value_or(0U))},
        {.name = "uac_last_miss_unprimed", .value = "0"},
        {.name = "uac_last_miss_play", .value = "1"},
        {.name = "uac_max_gap_frames", .value = std::to_string(skipped + 1U)},
        {.name = "uac_watchdog_expiries", .value = std::to_string(before.watchdog_expiries + skipped)},
        {.name = "uac_unprimed_intervals", .value = std::to_string(before.unprimed_intervals.value)},
        {.name = "uac_play_unprimed_intervals", .value = "0"},
        {.name = "uac_rejected_chunks", .value = std::to_string(before.rejected_chunks)},
    };
    check(has_members(usb, host_gap), "a gap the host left during the play is located after it, the play's, the largest gap, and not unprimed");
    host.run(usb_idle_milliseconds);
    const auto late_index = ua::stream_counters().frames_sent.value;
    host.complete_with_late_isr(late_by);
    host.run(usb_idle_milliseconds);
    lines = exchange("1 USB?\n");
    usb = usb_members(lines);
    const std::vector<StatusMember> device_gap{
        {.name = "uac_missed_intervals", .value = std::to_string(before.missed_intervals.value + skipped + late_by)},
        {.name = "uac_last_miss_frame_index", .value = std::to_string(late_index)},
        {.name = "uac_last_miss_unprimed", .value = std::to_string(late_by)},
        {.name = "uac_last_miss_play", .value = "0"},
        {.name = "uac_max_gap_frames", .value = std::to_string(skipped + 1U)},
        {.name = "uac_watchdog_expiries", .value = std::to_string(before.watchdog_expiries + skipped + 1U)},
        {.name = "uac_unprimed_intervals", .value = std::to_string(before.unprimed_intervals.value + late_by)},
        {.name = "uac_play_unprimed_intervals", .value = "0"},
    };
    check(lines.size() == 1U && has_members(usb, device_gap), "an ISR held off past the host's polls after the play is located there, not the play's, and its missed intervals are unprimed");
}

// #45: twenty IN completions part way through a play report 0 bytes for
// packets the host received whole, as the ChipIdea driver reports a transfer
// that ended in error. Each still confirms the packet TinyUSB loaded, so no
// chunk record is left behind and the play runs to its end. The records used
// to fill instead and freeze the stream: frames_sent stuck, only zero-length
// packets, and the drain (or a STOP) waiting forever. The board cannot tell
// such a completion from one whose packet never reached the host
// (test_usb_lost_packet), so once the play is over the run ends with
// usb_unconfirmed (26): its statistics count every file frame TinyUSB took,
// which here the host did receive.
void test_usb_short_completions()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t short_completions = 20U;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok, "a full-range file is selected");
    const auto before = ua::stream_counters();
    host.schedule({.short_reports_at = usb_interrupt_at_milliseconds, .short_reports = short_completions});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    const auto after = ua::stream_counters();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_unconfirmed),
          "a USB play whose completions are reported short part way ends with usb_unconfirmed");
    check(play.file_frames.value == file.samples.size() && play.pending_frames == ua::FrameCount{} && after.short_packets == before.short_packets + short_completions,
          "it ran to its end: every file frame is counted sent, and each completion reported short is a short packet");
    host.run(usb_trailing_milliseconds);
    check_sent_prefix(file, "the statistics describe the whole file, which this host received");
    const std::vector<StatusMember> mismatches{{.name = "uac_sent_mismatches", .value = std::to_string(before.sent_mismatches + short_completions)}};
    check(has_members(usb_members(exchange("1 USB?\n")), mismatches), "USB? counts each completion reported short of its packet");
}

// A completion part way through a play reports 0 bytes for a packet the host
// never received: the transfer ended in error before its data went out. The
// board counts the packet sent, as TinyUSB took it from the FIFO, so the
// play's statistics are one chunk ahead of the host's capture, and only the
// run's error, usb_unconfirmed, says so. The play runs to its end, so the
// host's capture shows what went missing. The next play, on a reopened
// stream, starts with its delivery confirmed again.
void test_usb_lost_packet()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok, "a full-range file is selected");
    host.schedule({.lost_reports_at = usb_interrupt_at_milliseconds, .lost_reports = 1U});
    run_selected();
    host.run(usb_trailing_milliseconds);
    const auto status = wg::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_unconfirmed) &&
              ua::play_statistics().file_frames.value == file.samples.size(),
          "a play with a packet the host never received runs to its end and ends with usb_unconfirmed");
    check(ua::stream_counters().frames_sent.value == host.received_frames() + ua::chunk_frames.value, "the board counts the lost packet sent: the host received one chunk fewer");
    host.schedule({});
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok, "the file is selected again");
    run_selected();
    host.run(usb_trailing_milliseconds);
    const auto again = wg::player_snapshot();
    check(again.state == static_cast<std::uint32_t>(wg::PlayerState::done) && again.error == 0U && again.pcm_drained == 1U, "the next play's delivery is confirmed afresh");
    check_sent_prefix(file, "the next play is exact");
}

// The mark a completion reported short leaves on a play while it has file
// frames queued stays to the run's end: a second one reported short after
// the play's last file frame is confirmed, with nothing queued, must not
// clear it, or the run would end done. The completion that confirms the last
// file frames is a late read, so the player task waits for the next
// completion to settle the play's misses (settle_play_misses()); that
// completion is the second one reported short, so it comes before the
// verdict (judge_usb_delivery()).
void test_usb_mismatch_after_the_window()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok, "a full-range file is selected");
    const auto before = ua::stream_counters();
    const auto late_reads_before = host.late_reads();
    std::optional<std::uint32_t> confirmed_at{};
    host.on_completion(
        [&host, &confirmed_at, frames = file.samples.size()]
        {
            const auto play = ua::play_statistics();

            if (!confirmed_at.has_value() && play.file_frames.value == frames && play.pending_frames == ua::FrameCount{})
            {
                confirmed_at = host.tick();
                host.report_short(1U);
            }
        });
    host.schedule({.late_read_last = true, .short_reports_at = usb_interrupt_at_milliseconds, .short_reports = 1U});
    run_selected();
    host.run(usb_trailing_milliseconds);
    host.on_completion({});
    const auto status = wg::player_snapshot();
    const auto last_confirmed = confirmed_at.value_or(0U);
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::usb_unconfirmed),
          "a play reported short inside its window and again after it ends with usb_unconfirmed, not done");
    check(confirmed_at.has_value() && host.late_reads() == late_reads_before + 1U && ua::stream_counters().sent_mismatches == before.sent_mismatches + 2U,
          "the completion that confirmed the last file frames was a late read, and both completions reported short are mismatches");
    check(host.fault_tick().has_value() && host.fault_tick().value_or(0U) > last_confirmed + 1U, "the verdict came after the completion reported short once nothing was queued");
    check(ua::play_statistics().file_frames.value == file.samples.size() && ua::play_statistics().missed_intervals == ua::IntervalCount{},
          "every file frame counts sent, and the late read is no missed interval");
    check_sent_prefix(file, "the statistics describe the whole file, which this host received");
}

// A play whose USB ISR is held off past the host's polls: STATUS counts the
// missed intervals for the play, and USB? says they were the device's, for the
// play and at the latest miss.
void test_usb_diagnostics_play_device_late()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t late_by = 3U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    const auto before = ua::stream_counters();
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LONG.WAV").ok, "a long file is selected");
    host.schedule({.late_isr_at = usb_interrupt_at_milliseconds, .late_isr_by = late_by});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto late_index = host.resume_frame_index();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && late_index.has_value() && ua::play_statistics().file_frames.value == file.samples.size(),
          "the play around the held-off ISR completes");
    check_sent_prefix(file, "the play around the held-off ISR is exact");
    const auto lines = exchange("1 USB?\n2 STATUS?\n");
    const auto usb = usb_members(lines);
    const auto json_status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    const std::vector<StatusMember> device_late{
        {.name = "uac_unprimed_intervals", .value = std::to_string(before.unprimed_intervals.value + late_by)},
        {.name = "uac_last_miss_frame_index", .value = std::to_string(late_index.value_or(0U))},
        {.name = "uac_last_miss_unprimed", .value = std::to_string(late_by)},
        {.name = "uac_last_miss_play", .value = "1"},
        {.name = "uac_play_unprimed_intervals", .value = std::to_string(late_by)},
    };
    check(lines.size() == 2U && has_members(usb, device_late) && member_value(json_status, "uac_play_missed_intervals") == std::to_string(late_by),
          "the play's missed intervals are all unprimed, for the play and at the latest miss, which is the play's");
}

// USB? reports the media layer's MSC transfer maxima and SD bus clock as its
// last members, after the play's capture format, with the media snapshot's
// values; STATUS carries none of them.
void test_usb_diagnostics_report_msc_transfers()
{
    constexpr std::uint32_t write_us = 2817U;
    constexpr std::uint32_t read_us = 1433U;
    constexpr std::uint32_t blocks = 32U;
    constexpr std::uint32_t bus_clock_hz = 49500000U;
    wfg_media_snapshot_t media{};
    media.state = kWFG_MediaHost;
    media.msc_max_write_us = write_us;
    media.msc_max_read_us = read_us;
    media.msc_max_blocks_per_call = blocks;
    media.sd_bus_clock_hz = bus_clock_hz;
    status_seam().media = media;
    const auto lines = exchange("1 USB?\n2 STATUS?\n");
    status_seam().media.reset();
    const auto usb = usb_members(lines);
    const auto json_status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    const std::vector<StatusMember> transfers{
        {.name = "msc_max_write_us", .value = std::to_string(write_us)},
        {.name = "msc_max_read_us", .value = std::to_string(read_us)},
        {.name = "msc_max_blocks_per_call", .value = std::to_string(blocks)},
        {.name = "sd_bus_clock_hz", .value = std::to_string(bus_clock_hz)},
    };
    // The transfer fields follow the play's format; the play's cuts follow them.
    const auto follows_format = [&usb, &transfers]()
    {
        const auto format = std::ranges::find(usb, std::string_view{"uac_play_stream_channels"}, &StatusMember::name);

        if (format == usb.end() || static_cast<std::size_t>(std::distance(format, usb.end())) <= transfers.size())
        {
            return false;
        }

        return std::ranges::equal(std::span{std::next(format), transfers.size()}, transfers,
                                  [](const StatusMember& got, const StatusMember& want) { return got.name == want.name && got.value == want.value; });
    };

    check(lines.size() == 2U && follows_format(), "USB? reports msc_max_write_us, msc_max_read_us, msc_max_blocks_per_call and sd_bus_clock_hz right after the play's format");
    check(lines.size() == 2U && std::ranges::none_of(json_status, [](const StatusMember& member) { return member.name.starts_with("msc_max_") || member.name.starts_with("sd_bus_"); }),

          "STATUS does not carry the MSC transfer maxima or the SD bus clock");
}

// The stream diagnostics reach STATUS each in its own field: the host's volume
// and mute, TinyUSB assertion failures, short packets, missed intervals, and
// whether the stream is open and how often it opened. USB? adds where the
// assertion failed: its call site, the stream frame index and the tick.
void test_usb_stream_diagnostics()
{
    constexpr std::array<std::uint8_t, 2> minus_10_db{0x00, 0xF6};
    constexpr std::array<std::uint8_t, 2> zero_db{};
    constexpr std::array<std::uint8_t, 1> muted{1U};
    constexpr std::array<std::uint8_t, 1> unmuted{};
    // -10 dB is -2560 in 1/256 dB, reported as its uint16 two's complement.
    constexpr std::string_view minus_10_db_reported{"62976"};
    constexpr std::uint32_t skipped_intervals = 3U;
    const wfg_uac_request_t volume{.code = wfg_uac_request_cur, .entity = wfg_uac_entity_feature_unit, .selector = wfg_uac_feature_volume, .channel = wfg_uac_master_channel};
    const wfg_uac_request_t mute{.code = wfg_uac_request_cur, .entity = wfg_uac_entity_feature_unit, .selector = wfg_uac_feature_mute, .channel = wfg_uac_master_channel};
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    const auto before = ua::stream_counters();
    const auto asserts = ua::stack_asserts();
    check(wfg_usb_audio_set_request(volume, minus_10_db.data(), to_u16(minus_10_db.size())) == kWFG_UacRequestAccepted &&
              wfg_usb_audio_set_request(mute, muted.data(), to_u16(muted.size())) == kWFG_UacRequestAccepted,
          "the host sets -10 dB and mutes");
    // Two assertions at two call sites, a millisecond apart, and a chunk the
    // FIFO refuses.
    wfg_usb_count_stack_assert();
    const auto first_site = ua::stack_asserts().last_pc;
    host.refuse_next_write();
    host.run(1U);
    const auto assert_frame_index = ua::stream_counters().frames_sent;
    const auto assert_tick = host.tick();
    wfg_usb_count_stack_assert();
    const auto recorded = ua::stack_asserts();
    const auto first_pc = asserts.count == 0U ? first_site : asserts.first_pc;
    check(assert_frame_index != ua::FrameCount{} && recorded.last_pc != ua::CodeAddress{} && first_pc != ua::CodeAddress{} && recorded.last_pc != first_site && recorded.first_pc == first_pc,
          "the assertions fail on a streaming stream, at two call sites; the record keeps the first's");
    check(ua::stream_counters().rejected_chunks == before.rejected_chunks + 1U, "the FIFO refused one chunk");
    // A data packet, then a zero-length one; then three intervals without a poll.
    host.hold_next_load();
    host.run(2U);
    host.skip_polling(skipped_intervals);
    host.run(1U);
    const std::vector<StatusMember> diagnostics{
        {.name = "uac_host_volume", .value = std::string{minus_10_db_reported}},
        {.name = "uac_host_mute", .value = "1"},
        {.name = "usb_stack_asserts", .value = std::to_string(asserts.count + 2U)},
        {.name = "uac_short_packets", .value = std::to_string(before.short_packets + 1U)},
        {.name = "uac_missed_intervals", .value = std::to_string(before.missed_intervals.value + skipped_intervals)},
        {.name = "uac_stream_open", .value = "1"},
        {.name = "uac_stream_opens", .value = std::to_string(before.opens)},
    };
    check_status_members(diagnostics, "STATUS reports -10 dB, mute, two stack asserts, one short packet and three missed intervals, each in its own field");
    // The record keeps the assertion's moment while the stream runs on.
    const std::vector<StatusMember> assert_record{
        {.name = "usb_stack_asserts", .value = std::to_string(asserts.count + 2U)},
        {.name = "usb_assert_count", .value = std::to_string(asserts.count + 2U)},
        {.name = "usb_first_assert_pc", .value = std::to_string(first_pc.value)},
        {.name = "usb_last_assert_pc", .value = std::to_string(recorded.last_pc.value)},
        {.name = "usb_last_assert_frame_index", .value = std::to_string(assert_frame_index.value)},
        {.name = "usb_last_assert_tick", .value = std::to_string(assert_tick)},
        {.name = "uac_rejected_chunks", .value = std::to_string(before.rejected_chunks + 1U)},
    };
    const auto usb_lines = exchange("1 USB?\n");
    check(usb_lines.size() == 1U && has_members(usb_members(usb_lines), assert_record) && ua::stream_counters().frames_sent != assert_frame_index && host.tick() != assert_tick,
          "USB? reports the count, the first and latest call sites, the latest's stream frame index and tick (not the reply's), and the refused chunk");
    host.close_stream();
    const std::vector<StatusMember> closed{{.name = "uac_stream_open", .value = "0"}, {.name = "uac_stream_opens", .value = std::to_string(before.opens)}};
    check_status_members(closed, "STATUS reports the closed stream and keeps its open count");
    host.open_stream();
    const std::vector<StatusMember> reopened{{.name = "uac_stream_open", .value = "1"}, {.name = "uac_stream_opens", .value = std::to_string(before.opens + 1U)}};
    check_status_members(reopened, "STATUS counts the reopening");
    check(wfg_usb_audio_set_request(volume, zero_db.data(), to_u16(zero_db.size())) == kWFG_UacRequestAccepted &&
              wfg_usb_audio_set_request(mute, unmuted.data(), to_u16(unmuted.size())) == kWFG_UacRequestAccepted,
          "the host restores 0 dB and unmutes");
    const std::vector<StatusMember> restored{{.name = "uac_host_volume", .value = "0"}, {.name = "uac_host_mute", .value = "0"}};
    check_status_members(restored, "STATUS follows the host back to 0 dB, unmuted");
}

// PLAY on the USB route after a USB play that stalled and missed intervals:
// as it is accepted every uac_play_* field restarts (the accept index at the
// stream's frames sent), and the new play reports only its own.
void test_usb_play_restarts_the_statistics()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    const auto previous = ua::play_statistics();
    check(previous.missed_intervals != ua::IntervalCount{} && previous.first_miss_frame_index != ua::FrameCount{} && previous.file_frames != ua::FrameCount{},
          "the previous USB play missed intervals and sent file frames");
    host.run(usb_idle_milliseconds);
    check(command("LOAD:USB.WAV").ok && command("PLAY").ok, "PLAY on the USB route is accepted");
    const auto restarted = restarted_play_members(ua::stream_counters().frames_sent.value);
    check(restarted.size() == uac_play_field_count, "the contract names every uac_play_* field");
    check_status_members(restarted, "every uac_play_* field restarts as PLAY is accepted on the USB route");
    host.schedule({});
    run_accepted(usb_accept_to_run_milliseconds);
    const auto play = ua::play_statistics();
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && play.file_frames.value == file.samples.size() && play.missed_intervals == ua::IntervalCount{} &&
              play.first_miss_frame_index == ua::FrameCount{},
          "the new play reports only its own frames and misses");
    check_sent_prefix(file, "the new play is exact");
}

// A codec PLAY after a USB play: the open stream stays exact digital silence,
// and every uac_play_* field restarts at zero (no USB play) as PLAY is
// accepted, rather than keeping the last USB play's values.
void test_codec_route_clears_the_usb_play()
{
    namespace wg = waveform_generator;
    files["2:/WG/CODEC.WAV"] = wav(codec_file_frames, codec_file_sample);
    auto& host = usb_host();
    host.open_stream();
    const auto before = ua::play_statistics();
    check(before.file_frames != ua::FrameCount{} && before.accept_frame_index != ua::FrameCount{}, "the last USB play left its statistics");
    check(command("OUTPUT CODEC").ok && command("LOAD:CODEC.WAV").ok && command("PLAY").ok, "PLAY on the codec route is accepted");
    check_status_members(restarted_play_members(0U), "a codec PLAY zeroes every uac_play_* field as it is accepted");
    run_accepted();
    const auto received = host.received();
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && wg::player_snapshot().file_frames_submitted == codec_file_frames, "the codec route plays the file");
    check(!received.empty() && received_exactly(received, 0U, {}), "the USB stream stays exact digital silence during a codec play");
    check_status_members(restarted_play_members(0U), "after the codec play STATUS still reports no USB play");
}

// A short USB play whose host stops polling while it drains: error 20 with
// file chunks still queued, as in test_usb_stall. The stream stays open and
// the host silent; the scheduled events are cleared.
[[nodiscard]] UsbFile stall_a_usb_play()
{
    namespace wg = waveform_generator;
    constexpr std::size_t short_file_frames = 3U * ua::chunk_frame_count;
    constexpr std::uint32_t stop_at_millisecond = 5U;
    auto file = stage_usb_file("SHORT.WAV", short_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:SHORT.WAV").ok, "the USB route and a short file are selected");
    host.schedule({.stop_polling_at = stop_at_millisecond});
    run_selected();
    host.schedule({});
    const auto play = ua::play_statistics();
    check(wg::player_snapshot().error == static_cast<std::uint32_t>(wg::PlayerError::usb_stall) && play.pending_frames != ua::FrameCount{} &&
              play.file_frames.value + play.pending_frames.value == short_file_frames,
          "the stalled play ends with file chunks still queued");
    return file;
}

// PLAY on the USB route while a stalled play's chunks are still queued: the
// statistics restart with nothing pending. When the host polls again the
// leftovers reach the wire after the new accept index and count for no play,
// the stall gap counts only for the stream, and the new play's statistics
// describe exactly its own file.
void test_usb_play_after_a_stall()
{
    namespace wg = waveform_generator;
    const auto stalled = stall_a_usb_play();
    const auto left = ua::play_statistics();
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    check(command("LOAD:USB.WAV").ok && command("PLAY").ok, "PLAY on the USB route is accepted while the stalled play's chunks are queued");
    const auto accept_index = ua::stream_counters().frames_sent.value;
    const std::size_t accept = accept_index;
    check(ua::play_statistics().pending_frames == ua::FrameCount{}, "the new play has nothing pending: the leftovers are not its frames");
    check_status_members(restarted_play_members(accept_index), "every uac_play_* field restarts although the stalled play's chunks are queued");
    const auto missed_before = ua::stream_counters().missed_intervals;
    host.resume_polling();
    run_accepted(usb_accept_to_run_milliseconds);
    const auto play = ua::play_statistics();
    const auto crc = waveform_generator::crc32(file.data_chunk());
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && play.file_frames.value == file.samples.size() && play.pending_frames == ua::FrameCount{} &&
              play.crc32_left == crc && play.crc32_right == crc,
          "the new play's statistics describe exactly its own file");
    check(play.missed_intervals == ua::IntervalCount{} && ua::stream_counters().missed_intervals > missed_before,
          "the stall gap, ended after the new PLAY, counts for the stream and not for the new play");
    const auto received = host.received();
    const auto leftovers = std::span{stalled.samples}.subspan(left.file_frames.value);
    const auto after_leftovers = accept + leftovers.size();
    const std::size_t first = play.first_frame_index.value;
    const bool before_accept = received.size() >= after_leftovers && first >= after_leftovers &&
                               received_exactly(std::span{received}.first(accept), left.first_frame_index.value, std::span{stalled.samples}.first(left.file_frames.value));
    check(before_accept && received_exactly(std::span{received}.subspan(accept, leftovers.size()), 0U, leftovers) &&
              received_exactly(std::span{received}.subspan(after_leftovers), first - after_leftovers, file.samples),
          "the wire: the stalled play's sent prefix, its leftovers from the accept index, silence, then the new file");
}

// A codec PLAY while a stalled USB play's chunks are still queued: every
// uac_play_* field is zero from acceptance on, and the leftovers, which reach
// the wire during the codec play once the host polls again, count for no play.
void test_codec_play_after_a_stall()
{
    namespace wg = waveform_generator;
    const auto stalled = stall_a_usb_play();
    const auto left = ua::play_statistics();
    files["2:/WG/CODEC.WAV"] = wav(codec_file_frames, codec_file_sample);
    auto& host = usb_host();
    check(command("OUTPUT CODEC").ok && command("LOAD:CODEC.WAV").ok && command("PLAY").ok, "PLAY on the codec route is accepted while the stalled play's chunks are queued");
    const std::size_t at_play = ua::stream_counters().frames_sent.value;
    check_status_members(restarted_play_members(0U), "a codec PLAY zeroes every uac_play_* field although the stalled play's chunks are queued");
    host.resume_polling();
    run_accepted();
    const auto received = host.received();
    const auto leftovers = std::span{stalled.samples}.subspan(left.file_frames.value);
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && wg::player_snapshot().file_frames_submitted == codec_file_frames, "the codec route plays the file");
    check(received.size() >= at_play + leftovers.size() && received_exactly(std::span{received}.subspan(at_play), 0U, leftovers), "the leftovers reach the wire as the host polls again, then silence");
    check_status_members(restarted_play_members(0U), "the leftovers count for no play: STATUS still reports no USB play");
}

// The drain's watch of queued file frames (check_drain(), error 26) is the
// USB route's. A codec play drains its EOF silence blocks, and the USB play
// statistics it cleared have nothing queued that could fall. Its drain here
// lasts twice the watch's 100 ms (the codec's blocks held meanwhile), and the
// run still ends done.
void test_codec_drain_outside_the_watch()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t held_milliseconds = 2U * documented_stall_timeout_ms;
    files["2:/WG/CODEC.WAV"] = wav(codec_file_frames, codec_file_sample);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT CODEC").ok && command("LOAD:CODEC.WAV").ok, "the codec route and a file are selected");
    std::optional<std::uint32_t> drain_began{};
    std::uint32_t last_draining = 0U;
    host.on_tick(
        [&host, &drain_began, &last_draining]
        {
            if (wg::player_state != wg::PlayerState::draining)
            {
                return;
            }

            last_draining = host.tick();

            if (!drain_began.has_value())
            {
                drain_began = host.tick();
            }

            codec_blocks_held() = host.tick() - drain_began.value_or(0U) < held_milliseconds;
        });
    run_selected();
    host.on_tick({});
    codec_blocks_held() = false;
    const auto status = wg::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U && status.file_frames_submitted == codec_file_frames,
          "a codec play whose drain outlasts the watch's 100 ms ends done, not with usb_unconfirmed");
    check(drain_began.has_value() && last_draining - drain_began.value_or(0U) >= held_milliseconds, "its drain lasted the time the codec's blocks were held");
}

// The 16-bit sample at `index` (little-endian int16) of the bytes received.
[[nodiscard]] std::int16_t pcm16_sample(std::span<const std::uint8_t> bytes, std::size_t index)
{
    std::array<std::uint8_t, sizeof(std::int16_t)> raw{};
    std::ranges::copy(bytes.subspan(index * raw.size(), raw.size()), raw.begin());
    return std::bit_cast<std::int16_t>(raw);
}

// Silence, then `file` from stream index `first` with each of the frame's
// `channels` 16-bit samples the 24-bit sample >> 8 (truncated; L = R), then
// silence again, and nothing else.
[[nodiscard]] bool received_truncated(std::span<const std::uint8_t> bytes, std::size_t channels, std::size_t first, std::span<const std::int32_t> file)
{
    constexpr std::int32_t truncated_bits = 8;
    const std::size_t frames = bytes.size() / (channels * sizeof(std::int16_t));

    if (frames < first + file.size())
    {
        return false;
    }

    for (std::size_t index = 0U; index < frames; ++index)
    {
        const bool in_file = index >= first && index - first < file.size();
        const std::int32_t expected = in_file ? file[index - first] >> truncated_bits : 0;

        for (std::size_t channel = 0U; channel < channels; ++channel)
        {
            if (pcm16_sample(bytes, index * channels + channel) != expected)
            {
                return false;
            }
        }
    }

    return true;
}

// USB?'s capture-format members: the stream's, then the play's.
[[nodiscard]] std::vector<StatusMember> format_members(const ua::StreamFormat& stream, const ua::StreamFormat& play)
{
    return {
        {.name = "uac_stream_alt", .value = std::to_string(stream.alternate)},        {.name = "uac_stream_bits", .value = std::to_string(stream.bits)},
        {.name = "uac_stream_channels", .value = std::to_string(stream.channels)},    {.name = "uac_play_stream_bits", .value = std::to_string(play.bits)},
        {.name = "uac_play_stream_channels", .value = std::to_string(play.channels)},
    };
}

// A USB play in 16-bit stereo (alternate 2): each channel carries the 24-bit
// sample's top 16 bits, sample >> 8 (truncated), between exact zeros. The
// per-play CRCs stay zlib.crc32(data chunk), over the 24-bit source, and every
// lane check of the play holds; USB? reports the stream's format and the one
// the play went out in, STATUS neither.
void test_usb_play_16_bit()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB16.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream(ua::pcm16_stereo);
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:USB16.WAV").ok, "the USB route and a full-range file are selected for 16-bit capture");
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U && play.file_frames.value == file.samples.size() &&
              play.pending_frames == ua::FrameCount{},
          "a USB play in 16-bit stereo delivers every file frame");
    host.run(usb_trailing_milliseconds);
    check(received_truncated(host.received_bytes(), wfg_uac_channels, play.first_frame_index.value, file.samples),
          "exact zeros, the file as L = R = sample >> 8 including both extremes, then exact zeros");
    const auto crc = std::to_string(waveform_generator::crc32(file.data_chunk()));
    const std::vector<StatusMember> lane_checks{
        {.name = "uac_play_file_frames", .value = std::to_string(file.samples.size())},
        {.name = "uac_play_crc32_l", .value = crc},
        {.name = "uac_play_crc32_r", .value = crc},
        {.name = "uac_play_missed_intervals", .value = "0"},
        {.name = "uac_stream_frames", .value = std::to_string(host.received_frames())},
    };
    check_status_members(lane_checks, "uac_play_file_frames is the file, both CRCs equal zlib.crc32(data chunk), no interval missed, "
                                      "and uac_stream_frames counts 16-bit frames");
    check(!status_reports("uac_stream_alt") && !status_reports("uac_play_stream_bits"), "STATUS gains no format field");
    const auto lines = exchange("1 USB?\n");
    check(has_members(usb_members(lines), format_members(ua::pcm16_stereo, ua::pcm16_stereo)), "USB? reports alternate 2, 16 bits and 2 channels for the stream and for the play");
    host.open_stream();
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// The host selects alternate 2 straight from alternate 1 while a USB play is
// armed, never passing alternate 0: the play ends as on a stream close, error
// 19, its statistics the exact 24-bit prefix the host received before the
// switch; the stream goes on in 16 bits with exact zeros, and USB? tells the
// stream's format (alternate 2) from the play's (24-bit stereo).
void test_usb_format_switch_mid_play()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:LONG.WAV").ok, "the USB route and a long file are selected");
    host.schedule({.switch_format_at = usb_interrupt_at_milliseconds, .switch_to = ua::pcm16_stereo});
    run_selected();
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == static_cast<std::uint32_t>(wg::PlayerError::usb_stream_closed) && play.interrupted && !play.armed,
          "a direct switch from alternate 1 to alternate 2 during a USB play is error 19");
    const auto count = std::size_t{play.file_frames.value};
    const bool fits = count != 0U && count < file.samples.size();
    const auto prefix_crc = fits ? waveform_generator::crc32(file.data_chunk().first(3U * count)) : 0U;
    check(fits && received_exactly(host.received_before_switch(), play.first_frame_index.value, std::span{file.samples}.first(fits ? count : 0U)) && play.crc32_left == prefix_crc &&
              play.crc32_right == prefix_crc && play.pending_frames == ua::FrameCount{} && play.format == ua::pcm24_stereo,
          "the statistics are the exact 24-bit prefix the host received before the switch");
    host.run(usb_trailing_milliseconds);
    const auto& after = host.received_bytes();
    check(!after.empty() && std::ranges::all_of(after, [](std::uint8_t byte) { return byte == 0U; }) && ua::stream_counters().format == ua::pcm16_stereo,

          "the stream goes on in 16 bits with exact zeros");
    const auto lines = exchange("1 USB?\n");
    check(has_members(usb_members(lines), format_members(ua::pcm16_stereo, ua::pcm24_stereo)), "USB? reports the stream in alternate 2 and the play in 24-bit stereo");
    host.open_stream();
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// An ordinary USB play in 24-bit stereo (alternate 1) that ends done: USB?
// reports alternate 1, 24 bits and 2 channels for the stream, and 24 bits and
// 2 channels for the play, the reading a bit-exact verdict needs (the CRCs are
// over the 24-bit source in every format). Once the stream closes its format
// reads 0, while the play's stays latched.
void test_usb_format_of_a_24_bit_play()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("USB24.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:USB24.WAV").ok, "the USB route and a full-range file are selected for 24-bit capture");
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && ua::play_statistics().file_frames.value == file.samples.size(),
          "a USB play in 24-bit stereo delivers every file frame");
    auto lines = exchange("1 USB?\n");
    check(lines.size() == 1U && has_members(usb_members(lines), format_members(ua::pcm24_stereo, ua::pcm24_stereo)),
          "after a done 24-bit play USB? reports alternate 1, 24 bits and 2 channels for the stream and for the play");
    host.close_stream();
    lines = exchange("1 USB?\n");
    const auto closed = usb_members(lines);
    check(lines.size() == 1U && has_members(closed, format_members(ua::StreamFormat{}, ua::pcm24_stereo)) && member_value(closed, "uac_stream_open") == "0",
          "with the stream closed USB? reports its alternate, bits and channels as 0, and the play's format stays latched");
    host.open_stream();
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A USB play in 16-bit mono (alternate 3, a WFG_UAC_MONO_ALT image only): the
// one channel on the wire is the left one, each 24-bit sample >> 8
// (truncated), between exact zeros. Both CRCs stay zlib.crc32(data chunk),
// over the 24-bit source; USB? reports alternate 3, 16 bits and 1 channel for
// the stream and for the play, the only format whose channel count is not 2.
// Any other image stalls alternate 3 and leaves its stream as it was.
void test_usb_play_16_bit_mono()
{
    namespace wg = waveform_generator;

    if constexpr (!image_offers_mono)
    {
        const auto before = ua::stream_counters();
        check(wfg_usb_audio_open_stream(ua::pcm16_mono.alternate) == kWFG_UacRequestStall && ua::stream_counters().opens == before.opens && ua::stream_counters().format == before.format,
              "an image without WFG_UAC_MONO_ALT stalls alternate 3 and opens nothing");
        return;
    }

    const auto file = stage_usb_file("USBMONO.WAV", usb_file_frames);
    auto& host = usb_host();
    host.open_stream(ua::pcm16_mono);
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok && command("LOAD:USBMONO.WAV").ok, "the USB route and a full-range file are selected for 16-bit mono capture");
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    const auto status = wg::player_snapshot();
    const auto play = ua::play_statistics();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.error == 0U && status.pcm_drained == 1U && play.file_frames.value == file.samples.size() &&
              play.pending_frames == ua::FrameCount{},
          "a USB play in 16-bit mono delivers every file frame");
    host.run(usb_trailing_milliseconds);
    check(received_truncated(host.received_bytes(), wfg_uac_mono_channels, play.first_frame_index.value, file.samples),
          "exact zeros, the file's left channel as sample >> 8 including both extremes, then exact zeros");
    const auto crc = std::to_string(waveform_generator::crc32(file.data_chunk()));
    const std::vector<StatusMember> lane_checks{
        {.name = "uac_play_file_frames", .value = std::to_string(file.samples.size())},
        {.name = "uac_play_crc32_l", .value = crc},
        {.name = "uac_play_crc32_r", .value = crc},
        {.name = "uac_play_missed_intervals", .value = "0"},
        {.name = "uac_stream_frames", .value = std::to_string(host.received_frames())},
    };
    check_status_members(lane_checks, "uac_play_file_frames is the file, both CRCs equal zlib.crc32(data chunk), no interval missed, "
                                      "and uac_stream_frames counts mono frames");
    const auto lines = exchange("1 USB?\n");
    check(lines.size() == 1U && has_members(usb_members(lines), format_members(ua::pcm16_mono, ua::pcm16_mono)), "USB? reports alternate 3, 16 bits and 1 channel for the stream and for the play");
    host.open_stream();
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// The codec's output while a test records it: one sample per frame, taken
// from the left I2S word (L = R), as the stub's audio hook fills its blocks.
struct CodecCapture
{
    bool recording{};
    std::vector<std::int32_t> samples;
};

[[nodiscard]] CodecCapture& codec_capture()
{
    static CodecCapture capture;
    return capture;
}

void record_codec_block(std::span<const std::uint32_t> words)
{
    auto& capture = codec_capture();

    if (!capture.recording)
    {
        return;
    }

    for (std::size_t word = 0U; word < words.size(); word += waveform_generator::codec_channels)
    {
        capture.samples.push_back(std::bit_cast<std::int32_t>(words[word]) / ua::subslot_scale);
    }
}

// The CUT injector on the player (common/cut_schedule.hpp): the CUT command,
// plays on both routes with dropped frames (DROP) and inserted zeros (ZERO),
// the statistics of a play that ends part way, the per-play latch, a play
// without cuts that sends exactly what it sent before the injector, and cuts
// after the channel: scenario plays against the host render, and live
// controls whose positions run on through dropped frames.
namespace cut = waveform_generator::cut;

void check(bool condition, const std::string& message)
{
    check(condition, message.c_str());
}

// Half a second of full-range PCM24: room for 4800-frame cuts at the start,
// in the middle and at the end.
constexpr std::size_t cut_card_frames = 24000U;
// ZERO only: at the start, in the middle (one frame), and before the last frame.
constexpr std::string_view zero_only_cuts{"CUT:0:4800:zero,12000:1:zero,23999:100:zero"};
// DROP only: at the start, in the middle (4800 frames), and one clipped at the end.
constexpr std::string_view drop_only_cuts{"CUT:0:100:drop,12000:4800:drop,23950:100:drop"};
// An erasure, adjacent entries, one frame, and two entries past the end.
constexpr std::string_view mixed_cuts{"CUT:5000:480:zero,5000:480:drop,6000:50:drop,6050:50:zero,6050:1:drop,23999:1:zero,24000:7:zero,30000:1:drop"};
// The widest offset of the 16-entry request: the 32-bit maximum.
constexpr std::uint32_t widest_offset = std::numeric_limits<std::uint32_t>::max();

// A count the tests hold in a container, as the player counts frames.
[[nodiscard]] std::uint32_t count_of(std::size_t value) noexcept
{
    return static_cast<std::uint32_t>(value);
}

// One cut of the expected stream: where it takes effect, its frames, its kind.
struct ExpectedCut
{
    std::uint32_t position{};
    std::uint32_t frames{};
    bool zero{};
};

// What a play of a card (or of a scenario's output) with a CUT schedule
// sends, built the plain way.
struct ExpectedStream
{
    std::vector<std::int32_t> samples;
    std::vector<ExpectedCut> cuts;
    std::uint32_t rejected{};
};

// The engine's output frames up to each entry, then its zeros, or the output
// past its dropped frames (clipped at the output's end); entries past the end
// ignored. A clean card's output is the card.
[[nodiscard]] ExpectedStream expected_stream(std::span<const std::int32_t> card, std::string_view command_text)
{
    ExpectedStream expected;
    const auto entries = cut::parse_command(command_text);
    check(entries.has_value(), "the test's CUT command parses");

    if (!entries.has_value())
    {
        return expected;
    }

    std::size_t at = 0U;

    for (const auto& entry : entries.value().entries())
    {
        const std::size_t offset = entry.offset.value;

        if (offset >= card.size())
        {
            ++expected.rejected;
            continue;
        }

        const auto kept = card.subspan(at, offset - at);
        expected.samples.insert(expected.samples.end(), kept.begin(), kept.end());
        at = offset;
        const auto position = count_of(expected.samples.size());

        if (entry.kind == cut::CutKind::zero)
        {
            expected.samples.insert(expected.samples.end(), entry.length, 0);
            expected.cuts.push_back({.position = position, .frames = entry.length, .zero = true});
            continue;
        }

        const auto skipped = std::min<std::size_t>(entry.length, card.size() - at);
        at += skipped;
        expected.cuts.push_back({.position = position, .frames = count_of(skipped), .zero = false});
    }

    const auto rest = card.subspan(at);
    expected.samples.insert(expected.samples.end(), rest.begin(), rest.end());
    return expected;
}

// The cuts within the first `sent` frames of the stream by the documented
// rule (a cut counts once a frame at or after its place went out; one at the
// stream's end once the whole stream did; a ZERO with the zeros sent of it;
// the entries past the output's end once the whole stream went out), and the
// output frames those frames cover.
struct CutFields
{
    std::uint32_t count{};
    std::uint32_t drop_frames{};
    std::uint32_t zero_frames{};
    std::optional<std::uint32_t> first_position{};
    std::uint32_t rejected{};
    std::uint32_t output_frames{};
};

[[nodiscard]] CutFields expected_cut_fields(const ExpectedStream& expected, std::uint32_t sent)
{
    const auto end = count_of(expected.samples.size());
    CutFields fields{.rejected = sent == end ? expected.rejected : 0U};

    for (const auto& item : expected.cuts)
    {
        if (sent <= item.position && !(sent == end && item.position == end))
        {
            continue;
        }

        ++fields.count;
        fields.first_position = fields.first_position.value_or(item.position);

        if (item.zero)
        {
            fields.zero_frames += std::min(item.frames, sent - item.position);
        }
        else
        {
            fields.drop_frames += item.frames;
        }
    }

    fields.output_frames = sent - fields.zero_frames + fields.drop_frames;
    return fields;
}

// zlib's CRC-32 of samples as 3-byte little-endian PCM24, as a WAV holds them.
[[nodiscard]] std::uint32_t pcm24_crc(std::span<const std::int32_t> samples)
{
    waveform_generator::Crc32 crc;
    std::array<std::uint8_t, waveform_generator::audio::pcm24_bytes_per_sample> bytes{};

    for (const auto sample : samples)
    {
        waveform_generator::audio::write_pcm24_le(bytes.data(), sample);
        crc.update(bytes);
    }

    return crc.value();
}

// USB?'s cut members for the first `sent` frames, the first sent at stream
// index `first_index` (0 for a codec play, whose frames count from its first).
[[nodiscard]] std::vector<StatusMember> cut_members(const ExpectedStream& expected, std::uint32_t sent, std::uint32_t first_index)
{
    const auto fields = expected_cut_fields(expected, sent);
    const auto first_cut = fields.first_position.has_value() ? first_index + *fields.first_position : 0U;
    return {
        {.name = "uac_play_cut_count", .value = std::to_string(fields.count)},
        {.name = "uac_play_cut_drop_frames", .value = std::to_string(fields.drop_frames)},
        {.name = "uac_play_cut_zero_frames", .value = std::to_string(fields.zero_frames)},
        {.name = "uac_play_cut_first_index", .value = std::to_string(first_cut)},
        {.name = "uac_play_cut_rejected", .value = std::to_string(fields.rejected)},
        {.name = "uac_play_sent_frames", .value = std::to_string(sent)},
    };
}

// A USB play with cuts, whole or ended part way: the host received silence,
// the frames the play confirmed sent (a prefix of the cut stream) and silence
// again; the CRCs and the first and last indices are those frames'; STATUS
// counts the output frames they cover; USB? the cuts within them; and the
// cuts are no missed intervals.
void check_cut_prefix(const ExpectedStream& expected, const std::string& what)
{
    const auto play = ua::play_statistics();
    const auto sent = play.file_frames.value;
    const bool fits = sent <= expected.samples.size();
    const auto prefix = std::span{expected.samples}.first(fits ? sent : 0U);
    const auto crc = pcm24_crc(prefix);
    const auto first = play.first_frame_index.value;
    check(fits && received_exactly(usb_host().received(), first, prefix), what + ": the host received the cut stream exactly");
    check(play.crc32_left == crc && play.crc32_right == crc && (sent == 0U || play.last_frame_index.value - first + 1U == sent),
          what + ": the CRCs and the first and last indices are the sent frames'");
    const auto lines = exchange("1 USB?\n2 STATUS?\n");
    const auto usb = usb_members(lines);
    const auto status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    check(lines.size() == 2U && has_members(usb, cut_members(expected, sent, first)), what + ": USB? reports the cuts within the frames sent");
    check(member_value(status, "uac_play_file_frames") == std::to_string(expected_cut_fields(expected, sent).output_frames) && member_value(status, "uac_play_crc32_l") == std::to_string(crc) &&
              member_value(status, "uac_play_missed_intervals") == "0" && member_value(status, "uac_play_first_miss_frame_index") == "0",
          what + ": STATUS counts the output frames covered, and no cut is a missed interval");
}

// A USB play of the selected file with `schedule`, from a freshly opened stream.
void play_on_usb(std::string_view schedule)
{
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    const auto reply = command(std::string{schedule}.c_str());

    if (!reply.ok)
    {
        const std::string bounded_schedule{schedule.substr(0U, 128U)};
        std::fprintf(stderr, "CUT schedule rejection diagnostic: `%s` -> %s\n", bounded_schedule.c_str(), reply.error == nullptr ? "(no error text)" : reply.error);
    }

    check(reply.ok, "the schedule is accepted for the next PLAY");
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    host.run(usb_trailing_milliseconds);
}

// CUT over WFG-LIVE/1: an accepted list answers with its entries, a refused
// one with the rule it breaks and changes nothing, CUT alone clears, one
// request carries 16 entries with 32-bit offsets, and a run makes it BUSY.
// INFO reports the capability and the larger request capacity.
void test_cut_command()
{
    namespace wg = waveform_generator;
    // Each refused request after the accepted first one, with its reply code.
    constexpr auto refusals = std::to_array<std::pair<std::string_view, std::string_view>>({
        {"CUT:200:10:zero,100:10:drop", "CUT_UNSORTED"},
        {"CUT:100:10:drop,105:1:zero", "CUT_OVERLAP"},
        {"CUT:1:0:drop", "CUT_RANGE"},
        {"CUT:1:1:DROP", "BAD_REQUEST"},
        {"CUT:100:5:zero,100:1:zero", "CUT_OVERLAP"},
    });
    std::string requests{"1 CUT:100:10:zero,200:10:drop\n"};

    for (std::size_t index = 0U; index < refusals.size(); ++index)
    {
        requests += std::to_string(index + 2U) + " " + std::string{refusals.at(index).first} + "\n";
    }

    auto lines = exchange(requests);
    bool all_refused = lines.size() == refusals.size() + 1U;

    for (std::size_t index = 0U; all_refused && index < refusals.size(); ++index)
    {
        all_refused = lines.at(index + 1U).contains(R"("code":")" + std::string{refusals.at(index).second} + R"(")");
    }

    check(!lines.empty() && lines[0].contains(R"("accepted":true,"apply_frame":0,"events":2})"), "an accepted CUT answers with its entries");
    check(all_refused, "a refused CUT answers with the rule it breaks");
    check(wg::player_cuts().pending.size() == 2U, "a refused CUT keeps the list accepted before it");
    std::string widest{"7 CUT:"};

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        widest += (index == 0U ? "" : ",") + std::to_string(widest_offset - (cut::max_entries - 1U) + index) + ":4800:zero";
    }

    lines = exchange("1 CUT\n" + widest + "\n8 INFO?\n");
    check(widest.size() + 1U <= wg::live_protocol::wire_capacity && widest.size() > wg::live_protocol::wire_capacity / 2U,
          "the widest 16-entry CUT is one request, longer than the former 192-byte limit");
    check(lines.size() == 3U && lines[0].contains(R"("events":0})") && lines[1].contains(R"("events":16})") && wg::player_cuts().pending.size() == cut::max_entries,
          "CUT alone clears; one request sets 16 entries");
    check(lines.size() == 3U && lines[2].contains(R"("reusable-playback","cut-injection","usb-audio")") &&
              lines[2].contains(R"(,"request_capacity":384,"response_capacity":4096,"watterson_algorithm_version":1,"frequency_shift_algorithm_version":1})"),
          "INFO lists cut-injection and reports the request capacity");
    wg::run_busy = true;
    const auto busy = command("CUT");
    wg::run_busy = false;
    check(!busy.ok && std::string_view{busy.error} == "BUSY" && wg::player_cuts().pending.size() == cut::max_entries, "CUT during a run is BUSY");
    check(command("CUT").ok && wg::player_cuts().pending.empty(), "CUT alone empties the list");
}

// USB plays with ZERO only, DROP only and a mix (an erasure, adjacent
// entries, one frame, two entries past the end) and with 16 entries: each
// sends the cut stream exactly, and the counters match the schedule.
void test_usb_play_with_cuts()
{
    namespace wg = waveform_generator;
    const auto file = stage_usb_file("CUT.WAV", cut_card_frames);
    check(command("OUTPUT USB").ok, "the USB route is selected");
    std::string most{cut::entries_prefix};
    constexpr std::uint32_t spacing = 1000U;

    for (std::uint32_t index = 0U; index < cut::max_entries; ++index)
    {
        most += (index == 0U ? "" : ",") + std::to_string(index * spacing) + ":" + std::to_string(index + 1U) + (index % 2U == 0U ? ":zero" : ":drop");
    }

    unaligned_sector_reads() = 0U;

    for (const std::string_view schedule : {zero_only_cuts, drop_only_cuts, mixed_cuts, std::string_view{most}})
    {
        const std::string name{schedule};
        check(command("LOAD:CUT.WAV").ok, "the file is selected");
        play_on_usb(schedule);
        const auto expected = expected_stream(file.samples, schedule);
        const auto status = wg::player_snapshot();
        const auto play = ua::play_statistics();
        check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.underruns == 0U && status.file_frames_total == file.samples.size() &&
                  status.file_frames_submitted == expected.samples.size() && play.file_frames.value == expected.samples.size(),
              name + ": the play completes, every frame of the cut stream sent");
        check_cut_prefix(expected, name);
        const auto fields = expected_cut_fields(expected, count_of(expected.samples.size()));
        check(fields.output_frames == file.samples.size() && fields.count == expected.cuts.size(), name + ": a whole play covers the whole card and applies every entry before its end");
    }

    // The cuts act on the engine's output, at frames that are not multiples
    // of 4 too (23999, 6051, 1002, ...); the card is read in whole blocks from
    // its data chunk on, never seeked, so every read lands word-aligned with
    // the file pointer and the SD driver keeps its multi-block DMA path.
    check(unaligned_sector_reads() == 0U, "with cuts at any frame, every read lands word-aligned with the file pointer");
}

// A DROP costs the producer engine time with nothing for the output FIFO.
// validate() keeps a schedule's drop balance within 14400 frames (a fourth
// adjacent DROP of 4800 frames is CUT_DROP_RUN), and the FIFO's margin above
// its critical level covers that balance at a producer twice as fast as
// real time (player.cpp, min_producer_speed). A producer that takes 21 ms
// per 2048-frame block (2.03 times real time) plays the longest runs
// validate() accepts, one where the FIFO holds no more than its wake
// watermark and three more, without a deadline miss or an underrun: the
// FIFO never falls below its critical level. (Five adjacent DROPs there,
// which validate() refuses, drain it to about 1900 frames.)
void test_drop_runs_within_the_fifo_margin()
{
    namespace wg = waveform_generator;
    constexpr std::size_t card_frames = 480000U;
    constexpr std::uint32_t block_ms = 21U;
    // The first run begins at the first block after the prefill (twelve
    // 2048-frame blocks reach the high watermark): the producer, which then
    // sleeps until the FIFO has drained to its wake watermark, starts it
    // there, the worst place for a DROP. The others fall elsewhere in its
    // cycle.
    constexpr std::string_view runs{"CUT:24576:4800:drop,29376:4800:drop,34176:4800:drop,96000:4800:drop,100800:4800:drop,105600:4800:drop,"
                                    "170000:4800:drop,174800:4800:drop,179600:4800:drop,250001:4800:drop,254801:4800:drop,259601:4800:drop"};
    const auto file = stage_usb_file("RUNS.WAV", card_frames);
    check(command("OUTPUT USB").ok && command("LOAD:RUNS.WAV").ok, "the USB route and a 10 s card");
    const auto refused = exchange("1 CUT:96000:4800:drop,100800:4800:drop,105600:4800:drop,110400:4800:drop\n");
    check(refused.size() == 1U && refused[0].contains(R"("code":"CUT_DROP_RUN")"), "a fourth adjacent DROP of 4800 frames is CUT_DROP_RUN");
    producer_block_ms() = block_ms;
    play_on_usb(runs);
    producer_block_ms() = 0U;
    const auto expected = expected_stream(file.samples, runs);
    const auto status = wg::player_snapshot();
    const auto usb = usb_members(exchange("1 USB?\n"));
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.underruns == 0U && member_value(usb, "engine_deadline_misses") == "0" &&
              status.ring_min_pre_eof_frames >= wg::ring_critical_frames && status.ring_min_pre_eof_frames < wg::ring_wake_watermark_frames,
          "a producer at twice real time rides out each longest DROP run: no deadline miss, the FIFO between its critical and wake levels, lowest " + std::to_string(status.ring_min_pre_eof_frames));
    check_cut_prefix(expected, "the longest DROP runs");
    check(ua::play_statistics().file_frames.value == expected.samples.size(), "every frame of the cut stream is sent");
}

// How a USB play with cuts ends part way, its schedule, how the run ends,
// and the card frame from which a ZERO's zeros run when the end must fall
// inside them.
struct PartWayCase
{
    std::string_view name;
    ArmedPlayEvents events;
    std::string_view schedule;
    waveform_generator::PlayerState state;
    std::optional<std::uint32_t> zeros_from;
};

// STOP, a stream close and a stall part way: the statistics stay the exact
// prefix the host received, with the cuts applied so far and no later one.
// The simulated player sleeps its whole pacing interval before it reads the
// STOP (the stub's task notification wakes nothing), so STOP at the 30th
// armed millisecond ends the play after about 9800 frames; a stall then has
// sent about 1500.
void test_usb_cuts_part_way()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t stop_zeros_from = 8000U;
    constexpr std::uint32_t stall_zeros_from = 1000U;
    const auto file = stage_usb_file("LONG.WAV", usb_long_file_frames);
    auto& host = usb_host();
    const auto cases = std::to_array<PartWayCase>({
        {.name = "STOP inside a ZERO",
         .events = {.send_stop_at = usb_interrupt_at_milliseconds},
         .schedule = "CUT:8000:4800:zero,30000:4800:drop",
         .state = wg::PlayerState::aborted,
         .zeros_from = stop_zeros_from},
        {.name = "a stream close after a DROP",
         .events = {.close_stream_at = usb_interrupt_at_milliseconds},
         .schedule = "CUT:500:100:drop,40000:10:zero",
         .state = wg::PlayerState::fault,
         .zeros_from = std::nullopt},
        {.name = "a stall inside a ZERO",
         .events = {.stop_polling_at = usb_interrupt_at_milliseconds},
         .schedule = "CUT:1000:4800:zero,20000:4800:drop",
         .state = wg::PlayerState::fault,
         .zeros_from = stall_zeros_from},
    });

    for (const auto& item : cases)
    {
        const std::string name{item.name};
        host.open_stream();
        host.run(usb_idle_milliseconds);
        check(command("LOAD:LONG.WAV").ok && command(std::string{item.schedule}.c_str()).ok, name + ": the file and its schedule");
        host.schedule(item.events);
        run_selected();
        // Only STOP has a reply waiting; a close or a stall ends the run unasked.
        wg::ControlReply stop_reply;
        const bool replied = wg::take_player_reply(stop_reply);
        check(replied == item.events.send_stop_at.has_value() && (!replied || stop_reply.ok), name + ": STOP, and only STOP, is acknowledged");
        const auto sent = ua::play_statistics().file_frames.value;
        check(wg::player_snapshot().state == static_cast<std::uint32_t>(item.state) && sent != 0U && sent < file.samples.size(), name + ": the play ends part way");
        const auto zeros_from = item.zeros_from.value_or(0U);
        check(!item.zeros_from.has_value() || (sent > zeros_from && sent < zeros_from + cut::max_length), name + ": it ends inside the zeros, after " + std::to_string(sent) + " frames");
        check_cut_prefix(expected_stream(file.samples, item.schedule), name);
    }
}

// Without cuts the player sends what it sent before the injector: a play
// with no CUT, one after a list was set and cleared, and one whose entries
// all lie past the file's end leave the same player snapshot, the same play
// statistics and the same stream; only the last counts its entries.
void test_usb_play_without_cuts_is_unchanged()
{
    namespace wg = waveform_generator;
    struct Outcome
    {
        std::array<std::uint32_t, sizeof(wg::PlayerSnapshot) / sizeof(std::uint32_t)> status{};
        ua::PlayStatistics play{};
        std::vector<ua::StereoFrame> received;
    };

    static_assert(sizeof(wg::PlayerSnapshot) % sizeof(std::uint32_t) == 0U, "the snapshot is uint32 fields");
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    const auto play_after = [](std::string_view schedule)
    {
        auto& host = usb_host();
        check(command("LOAD:USB.WAV").ok, "the file is selected");
        host.open_stream();
        host.run(usb_idle_milliseconds);

        if (!schedule.empty())
        {
            check(command(std::string{schedule}.c_str()).ok, "the CUT command is accepted");
        }

        host.schedule({});
        run_selected(usb_accept_to_run_milliseconds);
        host.run(usb_trailing_milliseconds);
        return Outcome{.status = std::bit_cast<decltype(Outcome::status)>(wg::player_snapshot()), .play = ua::play_statistics(), .received = host.received()};
    };

    const auto same_frame = [](const ua::StereoFrame& one, const ua::StereoFrame& other) { return one.left == other.left && one.right == other.right; };

    const auto same = [&same_frame](const Outcome& one, const Outcome& other)
    {
        return one.status == other.status && std::ranges::equal(one.received, other.received, same_frame) && one.play.file_frames == other.play.file_frames &&
               one.play.crc32_left == other.play.crc32_left && one.play.crc32_right == other.play.crc32_right && one.play.first_frame_index == other.play.first_frame_index &&
               one.play.last_frame_index == other.play.last_frame_index && one.play.accept_frame_index == other.play.accept_frame_index && one.play.missed_intervals == other.play.missed_intervals;
    };

    check(command("OUTPUT USB").ok && command("CUT").ok, "the USB route, no cuts pending");
    const auto plain = play_after({});
    check(command("CUT:100:1:zero").ok, "a list is set");
    const auto cleared = play_after("CUT");
    const auto past_end = play_after("CUT:5002:1:zero,6000:4800:drop");
    check(same(plain, cleared) && same(plain, past_end), "no cuts, cleared cuts and cuts past the end send and report the same");
    check_sent_prefix(file, "the play without cuts is the card exactly");
    const auto usb = usb_members(exchange("1 USB?\n"));
    ExpectedStream ignored{.samples = file.samples, .cuts = {}, .rejected = 2U};
    check(has_members(usb, cut_members(ignored, count_of(file.samples.size()), ua::play_statistics().first_frame_index.value)), "entries past the end are counted, nothing else");
}

// The list belongs to the next PLAY: LOAD keeps it, a refused PLAY
// (NO_STREAM) keeps it, an accepted one takes it, and the PLAY after that has
// no cuts unless CUT is sent again.
void test_cut_latching()
{
    constexpr std::string_view schedule{"CUT:100:10:zero"};
    const auto file = stage_usb_file("USB.WAV", usb_file_frames);
    auto& host = usb_host();
    host.close_stream();
    check(command("OUTPUT USB").ok && command(std::string{schedule}.c_str()).ok && command("LOAD:USB.WAV").ok, "CUT, then LOAD");
    const auto refused = command("PLAY");
    check(!refused.ok && std::string_view{refused.error} == "NO_STREAM", "PLAY without a stream is refused");
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    host.run(usb_trailing_milliseconds);
    check_cut_prefix(expected_stream(file.samples, schedule), "the list survives LOAD and a refused PLAY");
    check(command("LOAD:USB.WAV").ok, "the file again");
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    run_selected(usb_accept_to_run_milliseconds);
    host.run(usb_trailing_milliseconds);
    check_cut_prefix(expected_stream(file.samples, cut::command_name), "the next PLAY has no cuts");
}

// The codec route takes the engine's output through the same cut stage: the
// codec plays the cut stream, STATUS counts the frames it took (file_frames_submitted)
// against the card's (file_frames_total), USB? reports the cuts with frames
// counted from the play's first, and the uac_play_* STATUS fields stay zero.
void test_codec_play_with_cuts()
{
    namespace wg = waveform_generator;
    constexpr std::string_view schedule{"CUT:0:480:zero,12000:4800:drop,20000:4800:zero"};
    const auto file = stage_usb_file("CODEC.WAV", cut_card_frames);
    check(command("OUTPUT CODEC").ok && command("LOAD:CODEC.WAV").ok && command(std::string{schedule}.c_str()).ok, "the codec route, a file and its schedule");
    auto& capture = codec_capture();
    capture = CodecCapture{.recording = true, .samples = {}};
    run_selected();
    capture.recording = false;
    const auto expected = expected_stream(file.samples, schedule);
    const auto status = wg::player_snapshot();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::done) && status.underruns == 0U && status.pcm_drained == 1U && status.file_frames_total == file.samples.size() &&
              status.file_frames_submitted == expected.samples.size(),
          "the codec play completes with every frame of the cut stream");
    const auto played = std::span{capture.samples};
    check(played.size() >= expected.samples.size() && std::ranges::equal(played.first(std::min(played.size(), expected.samples.size())), expected.samples) &&
              std::ranges::all_of(played.subspan(std::min(played.size(), expected.samples.size())), [](std::int32_t sample) { return sample == 0; }),

          "the codec plays the cut stream exactly, then silence");
    const auto usb = usb_members(exchange("1 USB?\n"));
    check(has_members(usb, cut_members(expected, count_of(expected.samples.size()), 0U)), "USB? reports the codec play's cuts");
    check_status_members(restarted_play_members(0U), "the uac_play_* STATUS fields stay zero: no USB play");
}

// Cuts after the channel (#19). The engine and the live controls process
// every card frame, and the cuts act on their output. A scenario play with a
// DROP and a ZERO sends the host render of its card (signal_lab_render's
// engine, in the player's blocks) with the schedule applied, sample for
// sample, on both routes; the engine's digest and frame counts are the uncut
// render's, as it saw every frame. With a sample slip the output is longer
// than the card and the offsets count its frames: a DROP past the card's end
// but inside the output applies. A live control at output frame F applies at
// frame F of the output whatever a DROP before it discards.

// A fade, AWGN and a CW carrier: every output frame depends on where it lies
// in the engine's time.
constexpr std::string_view channel_scenario{R"({"schema":"signal-lab.scenario/1","test_id":"cut-channel","seed":7,"reference":"auto","reference_rms":0.5,"source_gain_db":-12,)"
                                            R"("clip_policy":"saturate","impairments":[{"type":"fade","depth_db":6,"duration_ms":50,"first_seconds":0.1,"period_seconds":0.2},)"
                                            R"({"type":"awgn","snr_db":20},{"type":"cw","frequency_hz":900,"ci_db":12}]})"};
// AWGN, then 100 samples duplicated at 0.1 s: the output is 100 frames longer
// than the card.
constexpr std::string_view slip_scenario{R"({"schema":"signal-lab.scenario/1","test_id":"cut-slip","seed":3,"reference":"auto","reference_rms":0.5,"source_gain_db":-12,)"
                                         R"("clip_policy":"saturate","impairments":[{"type":"awgn","snr_db":30},)"
                                         R"({"type":"sample_slip","kind":"duplicate","length_samples":100,"placement":"single","at_seconds":0.1}]})"};
constexpr std::size_t slip_extra_frames = 100U;
// DROPs inside an engine block and across two, and ZEROs.
constexpr std::string_view channel_cuts{"CUT:3000:480:drop,9000:480:zero,12000:4800:drop,20000:100:zero"};
// A DROP that starts past the card's last frame and runs to the output's end,
// where it is cut short (50 of its 100 frames).
constexpr std::string_view slip_cuts{"CUT:4000:50:zero,10000:480:drop,24050:100:drop"};

// signal_lab_render's engine, a second instance beside the player's, static
// as the player's is (its arena and buffers are large), with the scenario it
// was configured for, which it keeps a pointer to.
struct HostRender
{
    signal_lab::Engine engine;
    signal_lab::Scenario scenario;
    signal_lab::WattersonChannel channel;
};

[[nodiscard]] HostRender& host_render()
{
    static HostRender render;
    return render;
}

// What the host render of a card puts out, and its engine's statistics.
struct Rendered
{
    std::vector<std::int32_t> samples;
    signal_lab::EngineStats stats;
};

// `card` through the scenario `text`, as signal_lab_render renders it with
// the scenario's reference_rms: the same engine in engine blocks, each output
// sample quantized as the player quantizes it.
[[nodiscard]] Rendered render_card(std::span<const std::int32_t> card, std::string_view text, std::optional<std::uint32_t> seed = std::nullopt, signal_lab::SecondarySource* secondary_source = nullptr)
{
    auto& render = host_render();
    const bool parsed = signal_lab::parse_scenario(text.data(), text.size(), render.scenario).is_ok();

    if (seed.has_value())
    {
        render.scenario.seed = *seed;
    }

    const bool configured =
        parsed && render.engine.configure(render.scenario, render.scenario.reference_rms, card.size(), signal_lab::StageResources{&render.channel, nullptr, secondary_source}).is_ok();
    check(configured, "the host render takes the test scenario");
    std::vector<float> input(card.size());
    std::ranges::transform(card, input.begin(), [](std::int32_t sample) { return waveform_generator::audio::pcm24_to_float(sample); });

    std::array<float, signal_lab::engine_capacity_frames> block{};
    Rendered rendered;

    for (std::size_t at = 0U; configured && at < input.size(); at += signal_lab::engine_block_frames)
    {
        const auto frames = std::min(signal_lab::engine_block_frames, input.size() - at);
        const auto produced = render.engine.process(std::span{input}.subspan(at, frames), std::span{block});
        check(produced.has_value(), "the host render processes every block");
        std::ranges::transform(std::span{block}.first(produced.has_value() ? produced.value() : 0U), std::back_inserter(rendered.samples),
                               [](float sample) { return signal_lab::det::quantize_pcm24(sample); });
    }

    rendered.stats = render.engine.stats();
    return rendered;
}

// The upper and lower 32 bits of a 64-bit digest, as STATUS reports them.
[[nodiscard]] constexpr std::uint32_t high_word(std::uint64_t value) noexcept
{
    constexpr unsigned int word_bits = 32U;
    return static_cast<std::uint32_t>(value >> word_bits);
}

[[nodiscard]] constexpr std::uint32_t low_word(std::uint64_t value) noexcept
{
    return static_cast<std::uint32_t>(value & std::numeric_limits<std::uint32_t>::max());
}

// The engine of the last play processed every card frame: its digest and
// frame counts are those of the uncut render.
void check_engine_saw_every_frame(const Rendered& render, std::size_t card_frames, const std::string& what)
{
    const auto status = waveform_generator::player_snapshot();
    const auto digest = render.stats.output_digest;
    check(status.engine_state == 1U && status.engine_digest_hi == high_word(digest) && status.engine_digest_lo == low_word(digest) && status.engine_frames_in == card_frames &&
              status.engine_frames_out == render.samples.size(),
          what + ": the engine saw every card frame: its digest and frame counts are the uncut render's");
}

// The frames the host received of the last USB play, as card samples.
[[nodiscard]] std::vector<std::int32_t> received_play()
{
    const auto play = ua::play_statistics();
    const auto received = usb_host().received();
    const auto first = std::min<std::size_t>(play.first_frame_index.value, received.size());
    const auto frames = std::span{received}.subspan(first, std::min<std::size_t>(play.file_frames.value, received.size() - first));
    std::vector<std::int32_t> samples;
    samples.reserve(frames.size());

    for (const auto& frame : frames)
    {
        samples.push_back(frame.left / ua::subslot_scale);
    }

    return samples;
}

// A scenario play of SCN.WAV (`card`, with `scenario` as SCN.SCN) with
// `schedule`, on the USB route and then the codec route.
void check_scenario_play_with_cuts(const UsbFile& card, std::string_view scenario, std::string_view schedule, const std::string& name)
{
    namespace wg = waveform_generator;
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
    const auto render = render_card(card.samples, scenario);
    const auto expected = expected_stream(render.samples, schedule);
    check(command("OUTPUT USB").ok && command("LOAD:SCN.WAV").ok, name + ": the USB route and the scenario's card");
    play_on_usb(schedule);
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && ua::play_statistics().file_frames.value == expected.samples.size(),
          name + ": the USB play completes, every frame of the cut render sent");
    check_cut_prefix(expected, name + " on USB");
    check_engine_saw_every_frame(render, card.samples.size(), name + " on USB");
    check(command("OUTPUT CODEC").ok && command("LOAD:SCN.WAV").ok && command(std::string{schedule}.c_str()).ok, name + ": the codec route, the card and its schedule");
    auto& capture = codec_capture();
    capture = CodecCapture{.recording = true, .samples = {}};
    run_selected();
    capture.recording = false;
    const auto played = std::span{capture.samples};
    const auto heard = played.first(std::min(played.size(), expected.samples.size()));
    check(wg::player_snapshot().file_frames_submitted == expected.samples.size() && std::ranges::equal(heard, expected.samples), name + ": the codec plays the cut render exactly");
    check(has_members(usb_members(exchange("1 USB?\n")), cut_members(expected, count_of(expected.samples.size()), 0U)), name + ": USB? reports the codec play's cuts");
    check_engine_saw_every_frame(render, card.samples.size(), name + " on the codec");
}

void test_scenario_plays_with_cuts()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    check_scenario_play_with_cuts(card, channel_scenario, channel_cuts, "a fade, AWGN and CW with DROPs and ZEROs");
    check_scenario_play_with_cuts(card, slip_scenario, slip_cuts, "a sample slip with a DROP past the card's end");
    const auto slipped = render_card(card.samples, slip_scenario);
    const auto expected = expected_stream(slipped.samples, slip_cuts);
    check(slipped.samples.size() == card.samples.size() + slip_extra_frames && expected.rejected == 0U && expected.cuts.size() == 3U && expected.cuts.back().frames == slip_extra_frames / 2U,
          "the slip lengthens the output past the card, and the last DROP applies there, cut short at the output's end");
    files.erase("2:/WG/SCN.SCN");
}

// Every ENGINE? member in emission order: begin_json(), the engine object and
// its format, then engine_json_fields.
[[nodiscard]] std::vector<waveform_generator::StatusJsonField> engine_json_layout()
{
    namespace wg = waveform_generator;
    std::vector<wg::StatusJsonField> fields{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "engine", .max_value_chars = wg::literal_length("{")},
        {.name = "format", .max_value_chars = wg::json_string_chars(wg::literal_length("engine-diagnostics"))},
    };
    std::ranges::copy(wg::engine_json_fields, std::back_inserter(fields));
    return fields;
}

[[nodiscard]] std::vector<waveform_generator::StatusJsonField> prepare_json_layout()
{
    namespace wg = waveform_generator;
    std::vector<wg::StatusJsonField> fields{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "prepare", .max_value_chars = wg::literal_length("{")},
    };
    std::ranges::copy(wg::prepare_json_fields, std::back_inserter(fields));
    return fields;
}

// The ENGINE? reply on a fresh connection, without its newline.
[[nodiscard]] std::string engine_reply()
{
    const auto lines = exchange("1 ENGINE?\n");
    return lines.size() == 1U ? lines[0] : std::string{};
}

// A refused or failed scenario keeps its reason (#50): player_engine_reason()
// is the Status that said why, beside STATUS's engine_state and engine_error,
// ENGINE? reports the three, and the next PLAY restarts the reason.
constexpr std::string_view watterson_scenario = R"({"seed":7,"reference_rms":0.25,"source_gain_db":-24,"clip_policy":"reject","impairments":[
    {"type":"watterson","paths":[{"spread_hz":0.1,"shift_hz":-75},{"delay_ms":20,"spread_hz":10},
    {"fading":"fixed","power_db":-6},{"spread_hz":2,"shift_hz":75}]},{"type":"awgn","snr_db":24}]})";

[[nodiscard]] waveform_generator::playlist::WattersonIdentity host_channel_identity()
{
    const auto& report = host_render().channel.report();
    return {.version = static_cast<std::uint32_t>(signal_lab::watterson_algorithm_version),
            .digest_hi = high_word(report.parameter_digest),
            .digest_lo = low_word(report.parameter_digest),
            .latency_frames = static_cast<std::uint32_t>(report.latency.value()),
            .paths = report.path_count};
}

void test_watterson_player()
{
    namespace wg = waveform_generator;
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view fixed = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed","delay_ms":2}]}]})";
    // Model uninitialized NOLOAD OCRAM; configure must overwrite it before use.
    std::ranges::fill(std::as_writable_bytes(std::span{&wg::player_watterson(), 1U}), std::byte{0xFF});

    for (const auto scenario : {fixed, watterson_scenario, fixed})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "Watterson with post-channel cuts");
        const auto identity = host_channel_identity();
        check(wg::player_watterson_identity() == identity, "player identity equals the host's after configure and reconfigure");
        const auto members = json_status_members(engine_reply());
        check(member_value(members, "watterson_version") == std::to_string(identity.version) && member_value(members, "watterson_digest_hi") == std::to_string(identity.digest_hi) &&
                  member_value(members, "watterson_digest_lo") == std::to_string(identity.digest_lo) && member_value(members, "watterson_latency_frames") == std::to_string(identity.latency_frames) &&
                  member_value(members, "watterson_paths") == std::to_string(identity.paths),
              "ENGINE? carries the configured identity and latency");
    }

    files.erase("2:/WG/SCN.SCN");
    check(command("LOAD:SCN.WAV").ok, "load the card without a scenario");
    run_selected();
    check(wg::player_watterson_identity() == wg::playlist::WattersonIdentity{}, "a clean play clears the earlier channel identity");
    files.erase("2:/WG/SCN.WAV");
}

void test_frequency_shift_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view positive = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"frequency_shift","mode":"offset","offset_hz":75}]})";
    constexpr std::string_view negative = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"frequency_shift","mode":"offset","offset_hz":-75}]})";
    constexpr std::string_view sweep = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"watterson","paths":[{"spread_hz":1},{"spread_hz":1,"delay_ms":2}]},
        {"type":"frequency_shift","mode":"sweep"},{"type":"awgn","snr_db":24}]})";

    for (const auto scenario : {positive, negative, sweep, positive})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "global Doppler with post-channel cuts");
    }

    check(waveform_generator::player_watterson_identity() == waveform_generator::playlist::WattersonIdentity{}, "standalone global Doppler clears the previous Watterson identity");
    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void stage_text(const std::string& name, std::string_view text);
void reset_playlist_state();
void run_accepted_playlist(bool deferred_stop_pending = false);
[[nodiscard]] std::vector<waveform_generator::playlist::ResultBlock> pending_blocks();
[[nodiscard]] waveform_generator::playlist::Progress playlist_progress();

[[nodiscard]] std::vector<float> pcm24_as_float(std::span<const std::int32_t> samples)
{
    std::vector<float> converted(samples.size());
    std::ranges::transform(samples, converted.begin(), [](std::int32_t sample) { return waveform_generator::audio::pcm24_to_float(sample); });

    return converted;
}

[[nodiscard]] waveform_generator::SecondarySourceIdentity secondary_identity(const UsbFile& source, std::uint64_t excerpt_start, std::uint64_t excerpt_frames, std::uint32_t latency)
{
    const auto converted = pcm24_as_float(source.samples);
    const auto excerpt = std::span{converted}.subspan(static_cast<std::size_t>(excerpt_start), static_cast<std::size_t>(excerpt_frames));
    double energy{};

    for (const float sample : excerpt)
    {
        energy += static_cast<double>(sample) * static_cast<double>(sample);
    }

    signal_lab::det::StreamDigest digest;
    digest.update(excerpt.data(), excerpt.size());
    const auto value = digest.value();
    constexpr auto word_bits = std::numeric_limits<std::uint32_t>::digits;
    return {
        .version = static_cast<std::uint32_t>(signal_lab::second_source_algorithm_version),
        .source_frames = static_cast<std::uint32_t>(source.samples.size()),
        .excerpt_start_frame = static_cast<std::uint32_t>(excerpt_start),
        .excerpt_frames = static_cast<std::uint32_t>(excerpt_frames),
        .digest_hi = static_cast<std::uint32_t>(value >> word_bits),
        .digest_lo = low_word(value),
        .latency_frames = latency,
        .reference_rms = std::sqrt(energy / static_cast<double>(excerpt.size())),
    };
}

void test_second_source_player()
{
    namespace wg = waveform_generator;
    const auto reason_is = [](std::string_view expected)
    {
        const auto reason = wg::player_engine_reason();
        return reason.message != nullptr && std::string_view{reason.message} == expected;
    };

    constexpr std::uint64_t destination_start = 2300U;
    constexpr std::uint64_t source_start = 700U;
    constexpr std::uint64_t excerpt_frames = 4600U;
    const auto card = stage_usb_file("SCN.WAV", 12000U);
    const auto source = stage_usb_file("SECOND.WAV", 9000U);
    files["2:/WG/SILENT.WAV"] = wav(10000U, 0);
    const auto source_float = pcm24_as_float(source.samples);
    const auto source_excerpt = std::span{source_float}.subspan(static_cast<std::size_t>(source_start), static_cast<std::size_t>(excerpt_frames));

    for (const double offset : {0.0, 75.0, -75.0})
    {
        secondary_card_faults() = SecondaryCardFaults{};
        const auto scenario = std::string{"{\"reference_rms\":0.25,\"source_gain_db\":0,\"impairments\":[{\"type\":\"second_source\",\"source_path\":\"SECOND.WAV\",\"si_db\":6,\"start_frame\":"} +
                              std::to_string(destination_start) + ",\"source_start_frame\":" + std::to_string(source_start) + ",\"duration_frames\":" + std::to_string(excerpt_frames) +
                              ",\"frequency_offset_hz\":" + std::to_string(offset) + "}]}";
        double energy{};

        for (const float sample : source_excerpt)
        {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }

        signal_lab::FixedSecondarySource provider{source_float, std::sqrt(energy / static_cast<double>(excerpt_frames))};
        const auto expected = render_card(card.samples, scenario, std::nullopt, &provider);
        const auto primary_only = render_card(card.samples, R"({"reference_rms":0.25,"source_gain_db":0,"impairments":[]})");
        bool primary_unchanged_outside_window = expected.samples.size() == primary_only.samples.size();

        for (std::size_t frame = 0U; primary_unchanged_outside_window && frame < expected.samples.size(); ++frame)
        {
            if (frame < destination_start || frame >= destination_start + excerpt_frames)
            {
                primary_unchanged_outside_window = expected.samples[frame] == primary_only.samples[frame];
            }
        }

        check(primary_unchanged_outside_window, "the secondary source leaves primary samples outside its destination window unchanged");
        files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
        check(command("OUTPUT USB").ok && command("LOAD:SCN.WAV").ok, "second-source scenario selects its primary card");
        play_on_usb("CUT");
        check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && received_play() == expected.samples,
              "the SD-backed second source matches FixedSecondarySource host rendering across offset modes");
        check_engine_saw_every_frame(expected, card.samples.size(), "second source");
        const auto latency = offset == 0.0 ? 0U : signal_lab::hilbert_delay;
        const auto identity = secondary_identity(source, source_start, excerpt_frames, latency);
        const auto reported_identity = wg::player_secondary_source_identity();
        check(reported_identity == identity, "the player reports the measured second-source excerpt RMS, PCM digest, range and latency");
        check(secondary_card_faults().successful_opens == 1U && secondary_card_faults().close_calls == 1U && open_files == 0U,
              "a normal play opens and closes its separate secondary file handle exactly once");
        const auto members = json_status_members(engine_reply());
        const auto reported_rms = member_value(members, "second_source_reference_rms");
        check(member_value(members, "second_source_version") == std::to_string(identity.version) && member_value(members, "second_source_source_frames") == std::to_string(identity.source_frames) &&
                  member_value(members, "second_source_excerpt_start_frame") == std::to_string(identity.excerpt_start_frame) &&
                  member_value(members, "second_source_excerpt_frames") == std::to_string(identity.excerpt_frames) &&
                  member_value(members, "second_source_digest_hi") == std::to_string(identity.digest_hi) && member_value(members, "second_source_digest_lo") == std::to_string(identity.digest_lo) &&
                  member_value(members, "second_source_latency_frames") == std::to_string(identity.latency_frames) && !reported_rms.empty() &&
                  std::abs(std::stod(reported_rms) - identity.reference_rms) < 1.0e-14,
              "ENGINE? carries the calibrated second-source identity");
    }

    for (const double offset : {0.0, 75.0, -75.0})
    {
        secondary_card_faults() = SecondaryCardFaults{};
        const auto fading_scenario =
            std::string{"{\"reference_rms\":0.25,\"source_gain_db\":0,\"impairments\":[{\"type\":\"second_source\",\"source_path\":\"SECOND.WAV\",\"si_db\":6,\"start_frame\":"} +
            std::to_string(destination_start) + ",\"source_start_frame\":" + std::to_string(source_start) + ",\"duration_frames\":" + std::to_string(excerpt_frames) +
            ",\"frequency_offset_hz\":" + std::to_string(offset) + R"(,"fading":{"spread_hz":1}}]})";
        double energy{};

        for (const float sample : source_excerpt)
        {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }

        signal_lab::FixedSecondarySource provider{source_float, std::sqrt(energy / static_cast<double>(excerpt_frames))};
        const auto expected = render_card(card.samples, fading_scenario, std::nullopt, &provider);
        files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(fading_scenario.begin(), fading_scenario.end());
        check(command("OUTPUT USB").ok && command("LOAD:SCN.WAV").ok, "a faded second-source case selects its primary card");
        play_on_usb("CUT");
        check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && received_play() == expected.samples,
              "the SD-backed Rayleigh source matches host rendering at zero and signed frequency offsets");
        signal_lab::SecondSourceParams identity_params{};
        identity_params.frequency_offset = signal_lab::units::FrequencyDelta{offset, signal_lab::units::Hz};
        identity_params.fading.enabled = true;
        identity_params.fading.spread = signal_lab::units::FrequencyDelta{1.0, signal_lab::units::Hz};
        const auto expected_latency = static_cast<std::uint32_t>(signal_lab::second_source_filter_latency_frames(identity_params).value());
        check(wg::player_secondary_source_identity().latency_frames == expected_latency && expected_latency == signal_lab::hilbert_delay,
              "Rayleigh fading reports the fixed analytic-filter latency even at zero frequency offset");
    }

    constexpr std::uint32_t markov_stream_id = 213U;
    constexpr std::uint64_t markov_seed = 41U;
    const signal_lab::MarkovGateParams markov_params{.enabled = true, .mean_on_seconds = 0.001, .mean_off_seconds = 0.001};
    const auto random_key = signal_lab::resolve_stage_random_key(markov_seed, 0U, markov_stream_id);
    const auto primary_only = render_card(card.samples, R"({"reference_rms":0.25,"source_gain_db":0,"impairments":[]})");

    for (const double offset : {0.0, 75.0, -75.0})
    {
        signal_lab::MarkovGate gate{};
        check(gate.configure(markov_params, random_key.seed, random_key.index).is_ok(), "the independent destination-window oracle configures a deterministic gate");
        secondary_card_faults() = SecondaryCardFaults{};
        const auto markov_scenario = std::string{"{\"seed\":"} + std::to_string(markov_seed) + R"(,"reference_rms":0.25,"source_gain_db":0,"impairments":[{"type":"second_source","stream_id":)" +
                                     std::to_string(markov_stream_id) + R"(,"source_path":"SECOND.WAV","si_db":6,"start_frame":)" + std::to_string(destination_start) + R"(,"source_start_frame":)" +
                                     std::to_string(source_start) + R"(,"duration_frames":)" + std::to_string(excerpt_frames) + R"(,"frequency_offset_hz":)" + std::to_string(offset) +
                                     R"(,"fading":{"spread_hz":1},"markov_on_off":{"mean_on_seconds":0.001,"mean_off_seconds":0.001}}]})";
        double energy{};

        for (const float sample : source_excerpt)
        {
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }

        signal_lab::FixedSecondarySource provider{source_float, std::sqrt(energy / static_cast<double>(excerpt_frames))};
        const auto baseline_scenario = std::string{"{\"seed\":"} + std::to_string(markov_seed) + R"(,"reference_rms":0.25,"source_gain_db":0,"impairments":[{"type":"second_source","stream_id":)" +
                                       std::to_string(markov_stream_id) + R"(,"source_path":"SECOND.WAV","si_db":6,"start_frame":)" + std::to_string(destination_start) + R"(,"source_start_frame":)" +
                                       std::to_string(source_start) + R"(,"duration_frames":)" + std::to_string(excerpt_frames) + R"(,"frequency_offset_hz":)" + std::to_string(offset) +
                                       R"(,"fading":{"spread_hz":1}}]})";
        const auto baseline = render_card(card.samples, baseline_scenario, std::nullopt, &provider);
        const auto expected = render_card(card.samples, markov_scenario, std::nullopt, &provider);
        files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(markov_scenario.begin(), markov_scenario.end());
        check(command("OUTPUT USB").ok && command("LOAD:SCN.WAV").ok, "a faded and gated second-source case selects the primary card");
        play_on_usb("CUT");
        check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && received_play() == expected.samples,
              "the SD-backed gated source matches host rendering at zero and signed offsets");
        std::uint64_t on_frames{};
        std::uint64_t off_frames{};
        bool destination_window_matches = true;

        for (std::uint64_t frame = 0U; frame < excerpt_frames; ++frame)
        {
            const std::size_t destination = static_cast<std::size_t>(destination_start + frame);

            if (gate.on())
            {
                ++on_frames;
                destination_window_matches = destination_window_matches && expected.samples[destination] == baseline.samples[destination];
            }
            else
            {
                ++off_frames;
                destination_window_matches = destination_window_matches && expected.samples[destination] == primary_only.samples[destination];
            }

            (void)gate.advance();
        }

        check(on_frames > 100U && off_frames > 100U && destination_window_matches,
              "the family-10 gate uses the destination-window mask, emits exact primary samples while OFF, and does not pause source/filter/fading state");
        signal_lab::SecondSourceParams gated_identity_params{};
        gated_identity_params.frequency_offset = signal_lab::units::FrequencyDelta{offset, signal_lab::units::Hz};
        gated_identity_params.fading.enabled = true;
        gated_identity_params.fading.spread = signal_lab::units::FrequencyDelta{1.0, signal_lab::units::Hz};
        check(wg::player_secondary_source_identity().latency_frames == signal_lab::second_source_filter_latency_frames(gated_identity_params).value(),
              "Markov gating leaves the shared analytic-filter latency unchanged");
    }

    reset_playlist_state();
    secondary_card_faults() = SecondaryCardFaults{};
    const auto scenario =
        std::string{"{\"reference_rms\":0.25,\"source_gain_db\":0,\"impairments\":[{\"type\":\"second_source\",\"source_path\":\"SECOND.WAV\",\"si_db\":6,\"duration_frames\":4600}]}"};
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
    stage_text("SECOND.PL", "SCN.WAV 0.05 USB scn=SCN.SCN\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    const auto previous_identity = wg::player_secondary_source_identity();
    check(command("PLAYLIST:SECOND.PL").ok, "second-source playlist passes preflight");
    check(secondary_card_faults().successful_opens == 1U && secondary_card_faults().close_calls == 1U && secondary_card_faults().read_calls == 4U &&
              wg::player_secondary_source_identity() == previous_identity,
          "structural preflight checks the secondary WAV header and closes it without RMS measurement or identity replacement");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    const auto playlist_identity = secondary_identity(source, 0U, excerpt_frames, 0U);
    check(blocks.size() == 1U && blocks[0].play.secondary_source == playlist_identity, "the playlist result retains the calibrated second-source identity from its actual play");
    check(secondary_card_faults().successful_opens == 2U && secondary_card_faults().close_calls == 2U && secondary_card_faults().read_calls > 8U && open_files == 0U,
          "accepted playback reopens the provider and measures the selected excerpt in bounded reads");
    check(wg::player_secondary_source_identity() == playlist_identity, "the last playlist item keeps its source identity");
    check(command("PLAYLIST ACK:1").ok, "the prior playlist result is acknowledged before the STOP case");
    secondary_card_faults() = SecondaryCardFaults{};
    const std::string_view long_measurement = R"({"reference_rms":0.25,"source_gain_db":0,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":6,"duration_frames":9000}]})";
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(long_measurement.begin(), long_measurement.end());
    stage_text("STOP.PL", "SCN.WAV 0.05 USB scn=SCN.SCN\n");
    bool stop_queued{};
    secondary_card_faults().after_read = [&stop_queued]
    {
        if (!stop_queued && secondary_card_faults().read_calls == 9U)
        {
            stop_queued = true;
            wg::live_protocol::Request request{};
            request.kind = wg::live_protocol::Kind::stop;
            check(wg::submit_player_command(request).is_ok(), "STOP enters the player mailbox after one secondary measurement block");
        }
    };
    host.run(usb_idle_milliseconds);
    host.schedule({});
    const auto stopped_playlist = command("PLAYLIST:STOP.PL");
    check(stopped_playlist.ok && secondary_card_faults().read_calls == 4U && secondary_card_faults().successful_opens == 1U && secondary_card_faults().close_calls == 1U,
          "PLAYLIST ACK follows structural validation before any RMS measurement");
    run_accepted_playlist(true);
    wg::service_player_command();
    wg::ControlReply deferred_stop{};
    check(wg::take_player_reply(deferred_stop) && deferred_stop.ok, "STOP during accepted-play calibration is acknowledged after the interrupted item closes");
    wg::service_player_command();
    wg::ControlReply duplicate_stop{};
    check(!wg::take_player_reply(duplicate_stop), "the deferred STOP receives no duplicate acknowledgement");
    const auto stopped_blocks = pending_blocks();
    const bool stopped_cleanly = stop_queued && secondary_card_faults().read_calls == 9U && secondary_card_faults().successful_opens == 2U && secondary_card_faults().close_calls == 2U &&
                                 open_files == 0U && wg::player_playlist_snapshot().progress.state == wg::playlist::State::aborted && stopped_blocks.size() == 1U &&
                                 stopped_blocks[0].outcome == wg::playlist::Outcome::aborted && stopped_blocks[0].play.sent_frames == 0U;

    if (!stopped_cleanly)
    {
        const auto progress = wg::player_playlist_snapshot().progress;
        const auto& faults = secondary_card_faults();
        std::fprintf(stderr, "secondary STOP diagnostic: queued=%u reads=%u opens=%u closes=%u open_files=%u state=%u owns=%u blocks=%zu outcome=%u sent=%u stop_requested=%u command_pending=%u\n",
                     static_cast<unsigned>(stop_queued), faults.read_calls, faults.successful_opens, faults.close_calls, open_files, static_cast<unsigned>(progress.state),
                     static_cast<unsigned>(wg::playlist_owns_player()), stopped_blocks.size(), stopped_blocks.empty() ? 255U : static_cast<unsigned>(stopped_blocks[0].outcome),
                     stopped_blocks.empty() ? 0U : stopped_blocks[0].play.sent_frames, static_cast<unsigned>(wg::player_playlist().stop_requested), static_cast<unsigned>(wg::command_pending.load()));
    }

    check(stopped_cleanly, "STOP after PLAYLIST ACK interrupts the first calibration block, closes the provider, and sends no item frames");

    if (!stopped_blocks.empty())
    {
        check(command(("PLAYLIST ACK:" + std::to_string(stopped_blocks.back().seq)).c_str()).ok, "the interrupted playlist result is acknowledged before the next case");
    }

    reset_playlist_state();
    secondary_card_faults() = SecondaryCardFaults{};
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(long_measurement.begin(), long_measurement.end());
    stage_text("BUSY.PL", "SCN.WAV 0.05 USB scn=SCN.SCN\n");
    bool busy_command_queued{};
    secondary_card_faults().after_read = [&busy_command_queued]
    {
        if (!busy_command_queued && secondary_card_faults().read_calls == 1U)
        {
            busy_command_queued = true;
            std::uint32_t previous{};
            const auto parsed = wg::live_protocol::parse("1 LOAD:SECOND.WAV", previous);
            check(parsed.has_value() && wg::submit_player_command(parsed.value()).is_ok(), "a mutating command queues during second-source preflight");
        }
    };
    host.run(usb_idle_milliseconds);
    host.schedule({});
    std::uint32_t previous{};
    const auto playlist_request = wg::live_protocol::parse("1 PLAYLIST:BUSY.PL", previous);
    check(playlist_request.has_value() && wg::submit_player_command(playlist_request.value()).is_ok(), "a second-source playlist request enters the player mailbox");
    wg::service_player_command();
    check(wg::reply_pending.load() && wg::command_pending.load() && secondary_card_faults().read_calls == 4U && secondary_card_faults().successful_opens == 1U &&
              secondary_card_faults().close_calls == 1U,
          "the outer reply is pending while structural preflight performs only bounded header reads");
    wg::service_player_command();
    check(wg::reply_pending.load() && wg::command_pending.load(), "reply backpressure does not overwrite the playlist response");
    wg::ControlReply outer_reply{};
    check(wg::take_player_reply(outer_reply) && outer_reply.ok, "the accepted playlist response is consumed before the deferred BUSY response");
    wg::service_player_command();
    wg::ControlReply deferred_busy{};
    check(wg::take_player_reply(deferred_busy) && !deferred_busy.ok && deferred_busy.error != nullptr && std::string_view{deferred_busy.error} == "BUSY",
          "the queued mutation receives exactly one deferred BUSY response");
    wg::service_player_command();
    wg::ControlReply duplicate_busy{};
    check(!wg::take_player_reply(duplicate_busy), "the deferred BUSY response is not published a second time");
    check(busy_command_queued && std::string_view{wg::player_live_snapshot().selected_file} == "SCN.WAV" && wg::player_playlist().run_requested,
          "the deferred LOAD does not change the selected source or cancel the accepted playlist");
    run_accepted_playlist();
    check(secondary_card_faults().successful_opens == 2U && secondary_card_faults().close_calls == 2U && secondary_card_faults().read_calls > 8U && open_files == 0U,
          "the accepted item performs source calibration only after playlist acceptance");
    const auto busy_blocks = pending_blocks();

    if (!busy_blocks.empty())
    {
        check(command(("PLAYLIST ACK:" + std::to_string(busy_blocks.back().seq)).c_str()).ok, "the BUSY case result is acknowledged before calibration-fault cases");
    }

    // A STOP submitted while the structural preflight reads the provider is
    // deferred until the outer PLAYLIST reply. If provider close then fails,
    // the cleanup refusal must win over STOPPED, and STOP must still be
    // answered once after the outer response is consumed.
    reset_playlist_state();
    secondary_card_faults() = SecondaryCardFaults{};
    secondary_card_faults().fail_close = true;
    const std::string_view close_failure_scenario = R"({"reference_rms":0.25,"source_gain_db":0,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":6,"duration_frames":9000}]})";
    files["2:/WG/CLFAIL.SCN"] = std::vector<std::uint8_t>(close_failure_scenario.begin(), close_failure_scenario.end());
    stage_text("CLFAIL.PL", "SCN.WAV 0 CODEC scn=CLFAIL.SCN\n");
    bool close_failure_stop_queued{};
    secondary_card_faults().after_read = [&close_failure_stop_queued]
    {
        if (!close_failure_stop_queued && secondary_card_faults().read_calls == 1U)
        {
            close_failure_stop_queued = true;
            wg::live_protocol::Request request{};
            request.kind = wg::live_protocol::Kind::stop;
            check(wg::submit_player_command(request).is_ok(), "STOP queues during secondary structural preflight");
        }
    };
    const auto close_failure_reply = command("PLAYLIST:CLFAIL.PL");
    check(!close_failure_reply.ok && close_failure_reply.error != nullptr && std::string_view{close_failure_reply.error} == "PLAYLIST_SCN_INVALID" && close_failure_reply.detail != nullptr &&
              std::string_view{close_failure_reply.detail} == "configure_failed",
          "secondary close failure is returned as a configuration refusal instead of STOPPED");
    const bool failed_close_holds_media = open_files == 1U && mounted && !wg::player_allows_media_host();
    const bool no_play_started = !wg::player_playlist().run_requested && !wg::playlist_owns_player() && pending_blocks().empty();
    // The failed structural-preflight close is retried while detaching the
    // provider from the rejected item, before ownership remains held for STOP.
    check(no_play_started && failed_close_holds_media && secondary_card_faults().successful_opens == 1U && secondary_card_faults().close_calls == 2U && secondary_card_faults().read_calls == 4U,
          "preflight close failure starts no item and retains ownership of the still-open provider and card");
    secondary_card_faults().fail_close = false;
    wg::service_player_command();
    wg::ControlReply close_failure_stop_reply{};
    check(close_failure_stop_queued && wg::take_player_reply(close_failure_stop_reply) && close_failure_stop_reply.ok && !wg::command_pending.load() && wg::player_allows_media_host(),
          "the deferred STOP retries the failed close once and releases media access");
    wg::service_player_command();
    wg::ControlReply duplicate_close_failure_stop{};
    check(!wg::take_player_reply(duplicate_close_failure_stop), "the preflight STOP does not receive a duplicate reply");
    // Structural preflight accepts a well-formed source even when calibration
    // will later fail. The playlist records that failure as an impairment and
    // applies its selected stop/continue policy to the following item.
    enum class CalibrationFault : std::uint8_t
    {
        silent,
        read,
        seek
    };

    for (const auto fault : {CalibrationFault::silent, CalibrationFault::read, CalibrationFault::seek})
    {
        for (const bool continuing : {false, true})
        {
            reset_playlist_state();
            secondary_card_faults() = SecondaryCardFaults{};
            const std::string source_path = fault == CalibrationFault::silent ? "SILENT.WAV" : "SECOND.WAV";
            constexpr auto default_source_start = std::uint64_t{};
            const std::uint64_t source_start_frame = fault == CalibrationFault::seek ? 1U : default_source_start;
            const std::uint64_t duration_frames = fault == CalibrationFault::seek ? 8000U : 9000U;
            const std::string fault_scenario = std::string{"{\"reference_rms\":0.25,\"source_gain_db\":0,\"impairments\":[{\"type\":\"second_source\",\"source_path\":\""} + source_path +
                                               "\",\"si_db\":6,\"source_start_frame\":" + std::to_string(source_start_frame) + ",\"duration_frames\":" + std::to_string(duration_frames) + "}]}";
            files["2:/WG/FAIL.SCN"] = std::vector<std::uint8_t>(fault_scenario.begin(), fault_scenario.end());
            stage_text("FAIL.PL", "SCN.WAV 0 CODEC scn=FAIL.SCN\nSCN.WAV 0 CODEC\n");
            host.open_stream();
            host.run(usb_idle_milliseconds);
            host.schedule({});
            check(command(continuing ? "PLAYLIST:FAIL.PL:CONTINUE" : "PLAYLIST:FAIL.PL").ok, "a playlist with a structurally valid second source is accepted before calibration");
            check(secondary_card_faults().read_calls == 4U && secondary_card_faults().successful_opens == 1U && secondary_card_faults().close_calls == 1U,
                  "fault-policy preflight reads only the source header and excerpt geometry");

            if (fault == CalibrationFault::read)
            {
                secondary_card_faults().reads_before_failure = 4U;
            }
            else if (fault == CalibrationFault::seek)
            {
                secondary_card_faults().seek = static_cast<FSIZE_t>(wav_header_bytes + waveform_generator::audio::pcm24_bytes_per_sample);
            }

            run_accepted_playlist();
            const auto fault_blocks = pending_blocks();
            const auto progress = playlist_progress();
            const auto expected_read_error = std::to_underlying(wg::PlayerError::impairment);
            const bool error_recorded = !fault_blocks.empty() && fault_blocks[0].outcome == wg::playlist::Outcome::error && fault_blocks[0].player_error == expected_read_error;
            const bool policy_followed =
                continuing ? progress.state == wg::playlist::State::done && progress.failed == 1U && fault_blocks.size() == 2U && fault_blocks[1].outcome == wg::playlist::Outcome::done
                           : progress.state == wg::playlist::State::error && progress.failed == 1U && fault_blocks.size() == 1U;
            check(error_recorded && policy_followed, "silent, seek, and read calibration failures become impairment errors and obey stop/CONTINUE policy");
            check(secondary_card_faults().successful_opens == 2U && secondary_card_faults().close_calls == 2U && open_files == 0U,
                  "a failed calibration closes the actual-play source and leaves no provider handle open");

            if (!fault_blocks.empty())
            {
                check(command(("PLAYLIST ACK:" + std::to_string(fault_blocks.back().seq)).c_str()).ok, "the calibration-fault result is acknowledged before the next policy case");
            }
        }
    }

    reset_playlist_state();
    files.erase("2:/WG/SCN.SCN");
    check(command("LOAD:SCN.WAV").ok, "a clean play reloads the primary card without an impairment file");
    run_selected();
    check(wg::player_secondary_source_identity() == wg::SecondarySourceIdentity{}, "a clean play clears the earlier second-source identity");
    // A FatFs failure is attached to SECOND.WAV, not the primary card handle.
    secondary_card_faults() = SecondaryCardFaults{};
    card_faults() = CardFaults{};
    secondary_card_faults().fail_open = true;
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
    check(command("LOAD:SCN.WAV").ok, "reload the primary for the provider-open fault");
    run_selected();
    check(wg::player_snapshot().engine_state == 2U && reason_is("second_source: SD WAV open failed") && secondary_card_faults().open_attempts == 1U &&
              card_faults().reads_before_failure == std::nullopt,
          "a secondary open fault fails calibration without affecting primary reads");
    secondary_card_faults() = SecondaryCardFaults{};
    secondary_card_faults().reads_before_failure = 4U;
    check(command("LOAD:SCN.WAV").ok, "reload for the secondary measurement read fault");
    run_selected();
    check(reason_is("second_source: SD WAV read failed or was short") && secondary_card_faults().read_calls == 5U && open_files == 0U,
          "a short/failing secondary measurement read is reported and its handle closes");
    secondary_card_faults() = SecondaryCardFaults{};
    secondary_card_faults().reads_before_short = 4U;
    check(command("LOAD:SCN.WAV").ok, "reload for the secondary short-read fault");
    run_selected();
    check(reason_is("second_source: SD WAV read failed or was short") && open_files == 0U, "a short secondary read is refused instead of calibrating partial data");
    secondary_card_faults() = SecondaryCardFaults{};
    constexpr std::string_view seek_scenario =
        R"({"reference_rms":0.25,"source_gain_db":0,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":6,"source_start_frame":1,"duration_frames":4600}]})";
    secondary_card_faults().seek = static_cast<FSIZE_t>(wav_header_bytes + waveform_generator::audio::pcm24_bytes_per_sample);
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(seek_scenario.begin(), seek_scenario.end());
    check(command("LOAD:SCN.WAV").ok, "reload for the secondary seek fault");
    run_selected();
    const auto seek_reason = wg::player_engine_reason();
    const bool seek_failed = reason_is("second_source: SD WAV seek failed") && open_files == 0U;

    if (!seek_failed)
    {
        std::fprintf(stderr, "secondary seek diagnostic: target=%u calls=%u last=%u reads=%u opens=%u closes=%u open_files=%u reason=%s\n",
                     static_cast<unsigned>(secondary_card_faults().seek.value_or(0U)), secondary_card_faults().seek_calls, static_cast<unsigned>(secondary_card_faults().last_seek),
                     secondary_card_faults().read_calls, secondary_card_faults().successful_opens, secondary_card_faults().close_calls, open_files,
                     seek_reason.message == nullptr ? "(null)" : seek_reason.message);
    }

    check(seek_failed, "a secondary file seek failure is reported and closed");
    secondary_card_faults() = SecondaryCardFaults{};
    secondary_card_faults().fail_close = true;
    files["2:/WG/SCN.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
    check(command("LOAD:SCN.WAV").ok, "reload for secondary close failure");
    run_selected(0U, true);
    check(secondary_card_faults().close_calls == 1U && open_files == 1U && mounted && !wg::player_allows_media_host(), "a secondary close failure retains the actual open handle and media ownership");
    secondary_card_faults().fail_close = false;
    check(command("STOP").ok && open_files == 0U && !mounted && wg::player_allows_media_host(), "STOP retries secondary cleanup and releases media only after close succeeds");
    secondary_card_faults() = SecondaryCardFaults{};
    reset_playlist_state();
    check(command("OUTPUT CODEC").ok, "restore the codec route after second-source player tests");
    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
    files.erase("2:/WG/SECOND.WAV");
    files.erase("2:/WG/SILENT.WAV");
    files.erase("2:/WG/SECOND.PL");
    files.erase("2:/WG/STOP.PL");
    files.erase("2:/WG/BUSY.PL");
    files.erase("2:/WG/FAIL.PL");
    files.erase("2:/WG/FAIL.SCN");
    files.erase("2:/WG/CLFAIL.PL");
    files.erase("2:/WG/CLFAIL.SCN");
}

void test_wideband_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);

    for (const unsigned int bandwidth : {3000U, 6000U, 9000U, 12000U, 3000U})
    {
        const std::string scenario = std::string{"{\"noise_bandwidth_hz\":"} + std::to_string(bandwidth) +
                                     R"(,"reference_rms":0.25,"source_gain_db":-24,"impairments":[
              {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
              {"type":"frequency_shift","mode":"sweep"},{"type":"awgn","snr_db":24}]})";
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "selected wideband AWGN with global Doppler and cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_pulsed_cw_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view pulsed = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75},{"type":"awgn","snr_db":24},
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"pulse_period_seconds":0.01,
         "pulse_duty_cycle":0.4,"ramp_seconds":0.0005}]})";
    constexpr std::string_view continuous = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"ramp_seconds":0.0005}]})";

    for (const auto scenario : {pulsed, continuous, pulsed})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "pulsed CW reconfiguration with channel and post-channel cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_swept_cw_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view swept = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75},{"type":"awgn","snr_db":24},
        {"type":"cw","frequency_sweep":{"low_hz":300,"high_hz":3300,"slope_hz_per_second":30000},
         "ci_db":12,"phase_degrees":90,"pulse_period_seconds":0.01,
         "pulse_duty_cycle":0.4,"ramp_seconds":0.0005}]})";
    constexpr std::string_view continuous = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"ramp_seconds":0.0005}]})";

    for (const auto scenario : {swept, continuous, swept})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "swept pulsed CW reconfiguration with channel and cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_markov_cw_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view gated = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75},{"type":"awgn","snr_db":24},
        {"type":"cw","frequency_sweep":{"low_hz":300,"high_hz":3300,"slope_hz_per_second":30000},
         "ci_db":12,"phase_degrees":90,"markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},
         "ramp_seconds":0.0005}]})";
    constexpr std::string_view fixed = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,
         "markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},"ramp_seconds":0.0005}]})";
    constexpr std::string_view continuous = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"ramp_seconds":0.0005}]})";

    for (const auto scenario : {gated, fixed, continuous, gated})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "Markov CW reconfiguration with channel and cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_fsk_cw_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view gated = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75},{"type":"awgn","snr_db":24},
        {"type":"cw","frequency_hz":1900,"fsk":{"baud":300,"shift_hz":170,"pattern":"random"},
         "ci_db":12,"phase_degrees":90,"markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},
         "ramp_seconds":0.0005}]})";
    constexpr std::string_view pulsed = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"cw","frequency_hz":1900,"fsk":{"baud":300,"shift_hz":170,"pattern":"alternating"},
         "ci_db":12,"phase_degrees":90,"pulse_period_seconds":0.01,"pulse_duty_cycle":0.4,
         "ramp_seconds":0.0005}]})";
    constexpr std::string_view continuous = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"ramp_seconds":0.0005}]})";

    for (const auto scenario : {gated, pulsed, continuous, gated})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "FSK CW reconfiguration with channel and cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_faded_cw_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view gated = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"watterson","paths":[{"fading":"fixed"},{"fading":"fixed","delay_ms":1.5}]},
        {"type":"frequency_shift","mode":"offset","offset_hz":75},{"type":"awgn","snr_db":24},
        {"type":"cw","frequency_hz":1900,"fsk":{"baud":300,"shift_hz":170,"pattern":"random"},
         "fading":{"spread_hz":10},"ci_db":12,"phase_degrees":90,
         "markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},"ramp_seconds":0.0005}]})";
    constexpr std::string_view pulsed = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"cw","frequency_sweep":{"low_hz":300,"high_hz":3300,"slope_hz_per_second":300},
         "fading":{"spread_hz":1},"ci_db":12,"phase_degrees":90,
         "pulse_period_seconds":0.01,"pulse_duty_cycle":0.4,"ramp_seconds":0.0005}]})";
    constexpr std::string_view fixed = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"cw","frequency_hz":1900,"fading":{"spread_hz":0.1},"ci_db":12,"ramp_seconds":0.0005}]})";
    constexpr std::string_view continuous = R"({"reference_rms":0.25,"source_gain_db":-24,"impairments":[
        {"type":"cw","frequency_hz":1900,"ci_db":12,"phase_degrees":90,"ramp_seconds":0.0005}]})";

    for (const auto scenario : {gated, pulsed, fixed, continuous, gated})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "faded CW reconfiguration with channel and cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_stream_identity_player()
{
    const auto card = stage_usb_file("SCN.WAV", cut_card_frames);
    constexpr std::string_view base = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"watterson","stream_id":101,"paths":[{"fading":"rayleigh","spread_hz":1}]},
        {"type":"awgn","stream_id":102,"snr_db":24},
        {"type":"cw","stream_id":103,"frequency_hz":1900,"phase_degrees":"random",
         "fsk":{"baud":300,"shift_hz":170,"pattern":"random"},"fading":{"spread_hz":10},"ci_db":12,
         "markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},"ramp_seconds":0.0005}]})";
    constexpr std::string_view inserted = R"({"reference_rms":0.25,"source_gain_db":-24,"seed":17,"impairments":[
        {"type":"watterson","stream_id":101,"paths":[{"fading":"rayleigh","spread_hz":1}]},
        {"type":"cw","frequency_hz":1000,"ci_db":12,"start_seconds":3600,"duration_seconds":1},
        {"type":"awgn","stream_id":102,"snr_db":24},
        {"type":"cw","stream_id":103,"frequency_hz":1900,"phase_degrees":"random",
         "fsk":{"baud":300,"shift_hz":170,"pattern":"random"},"fading":{"spread_hz":10},"ci_db":12,
         "markov_on_off":{"mean_on_seconds":0.005,"mean_off_seconds":0.008},"ramp_seconds":0.0005}]})";
    const auto before = render_card(card.samples, base);
    const auto after = render_card(card.samples, inserted);
    check(before.samples == after.samples && before.stats.output_digest == after.stats.output_digest,
          "explicit stream IDs preserve the random channel and interferers after an inactive stage is inserted");

    for (const auto scenario : {base, inserted, base})
    {
        check_scenario_play_with_cuts(card, scenario, channel_cuts, "explicit random stream identities with cuts");
    }

    files.erase("2:/WG/SCN.SCN");
    files.erase("2:/WG/SCN.WAV");
}

void test_engine_reasons()
{
    namespace wg = waveform_generator;
    constexpr std::size_t reason_card_frames = 10000U;
    constexpr std::int16_t reason_card_level = -2000;
    files["2:/WG/REASON.WAV"] = wav(reason_card_frames, reason_card_level);
    const auto play_with = [](std::string_view text)
    {
        files["2:/WG/REASON.SCN"] = std::vector<std::uint8_t>(text.begin(), text.end());
        check(command("LOAD:REASON.WAV").ok, "LOAD of the reason card");
        run_selected();
        return wg::player_snapshot();
    };

    const auto message_is = [](m110::Status status, std::string_view expected) { return status.message != nullptr && std::string_view{status.message} == expected; };

    const std::string_view unparsable = "{bad scenario}";
    const m110::Status parse_verdict = signal_lab::parse_scenario(unparsable.data(), unparsable.size(), host_render().scenario);
    auto status = play_with(unparsable);
    auto reason = wg::player_engine_reason();
    check(status.engine_state == 2U && status.engine_error == std::to_underlying(wg::EngineError::scenario_parse) && reason.code == m110::StatusCode::invalid_argument &&
              message_is(reason, parse_verdict.message),
          "a scenario the parser refuses keeps the parser's reason");
    // Parsing accepts the finite reference, but scaling it overflows at configure.
    status = play_with(R"({"reference_rms":1e308,"source_gain_db":30,"impairments":[]})");
    reason = wg::player_engine_reason();
    check(status.engine_state == 2U && status.engine_error == std::to_underlying(wg::EngineError::configure_failed) && reason.code == m110::StatusCode::invalid_argument &&
              message_is(reason, "scaled reference RMS is not finite"),
          "a scenario the engine refuses at configure keeps the engine's reason");
    const std::string refused = engine_reply();
    const auto members = json_status_members(refused);
    const auto unused = unused_width(members, engine_json_layout());
    check(refused.starts_with(R"({"seq":)") && refused.ends_with("}}") && member_value(members, "format") == quoted("engine-diagnostics") && member_value(members, "engine_state") == "2" &&
              member_value(members, "engine_error") == "5" && member_value(members, "engine_reason") == quoted("scaled reference RMS is not finite"),
          "ENGINE? reports the refusal's engine_state, engine_error and reason");
    check_ok(unused.status(), "ENGINE? emits engine_json_fields in order within their widths");
    constexpr std::size_t newline_and_terminator = 2U;
    check(unused.has_value() && refused.size() + newline_and_terminator + unused.value() == wg::engine_json_worst_case_bytes(), "ENGINE? worst case counts every byte of the reply and its terminator");
    // A reason with quotes reaches the host as a JSON string.
    play_with(R"({"reference_rms":0.1,"impairments":[{"type":"cw","frequency_hz":900,"ci_db":3,"phase_degrees":"x"}]})");
    check(engine_reply().contains(R"(,"engine_state":2,"engine_error":3,"engine_reason":"cw.phase_degrees must be a number or \"random\""}})"), "ENGINE? escapes the quotes of a reason");
    status = play_with(R"({"impairments":[]})");
    reason = wg::player_engine_reason();
    check(status.engine_state == 2U && status.engine_error == std::to_underlying(wg::EngineError::missing_reference_rms) && reason.code == m110::StatusCode::invalid_configuration &&
              message_is(reason, "missing_reference_rms"),
          "a refusal with no other reason is the engine error's own Status");
    files.erase("2:/WG/SECOND.WAV");
    status = play_with(R"({"reference_rms":0.1,"impairments":[{"type":"second_source","source_path":"SECOND.WAV","si_db":12}]})");
    reason = wg::player_engine_reason();
    check(status.engine_state == 2U && status.engine_error == std::to_underlying(wg::EngineError::configure_failed) && message_is(reason, "second_source: SD WAV open failed"),
          "a missing secondary SD WAV keeps the provider's specific open failure reason");
    // The card is at -24 dBFS; 30 dB of gain clips it, and the scenario rejects a clip.
    status = play_with(R"({"reference_rms":0.1,"source_gain_db":30,"clip_policy":"reject","impairments":[]})");
    reason = wg::player_engine_reason();
    check(status.engine_state == 3U && status.engine_error == std::to_underlying(wg::EngineError::clipping_rejected) && status.error == std::to_underlying(wg::PlayerError::impairment) &&
              signal_lab::same_failure(reason, signal_lab::engine_failure::clipping),
          "an engine that stops on a clip keeps the engine's clipping Status");
    status = play_with(R"({"reference_rms":0.1,"impairments":[]})");
    check(status.engine_state == 1U && wg::player_engine_reason().is_ok(), "the next PLAY restarts the reason");
    check(engine_reply().contains(R"(,"engine_state":1,"engine_error":0,"engine_reason":null}})"), "ENGINE? reports no reason as null");
    files.erase("2:/WG/REASON.SCN");
    files.erase("2:/WG/REASON.WAV");
    // The widest reason: more than the budget, every character a quote. ENGINE?
    // carries engine_reason_max_chars of it, each escaped, within the worst case.
    const std::string quotes(wg::engine_reason_max_chars + 1U, '"');
    wg::play_watch().engine_reason = m110::Status{.code = m110::StatusCode::internal_error, .message = quotes.c_str()};
    const std::string widest = engine_reply();
    std::string expected_tail{R"(,"engine_reason":")"};

    for (std::size_t index = 0U; index < wg::engine_reason_max_chars; ++index)
    {
        expected_tail += R"(\")";
    }

    expected_tail += "\"}}";
    check(widest.ends_with(expected_tail) && widest.size() + newline_and_terminator <= wg::engine_json_worst_case_bytes(),
          "ENGINE? cuts a reason at engine_reason_max_chars and escapes every quote within its budget");
    // The other escapes: a backslash is escaped as a quote is, a control
    // character goes out as '?', and a byte above 0x7F (UTF-8) as it is, char
    // being signed on this host.
    const auto reason_reply = [](const char* message)
    {
        wg::play_watch().engine_reason = m110::Status{.code = m110::StatusCode::internal_error, .message = message};
        return engine_reply();
    };

    check(reason_reply(R"(C:\WG\PLAY.SCN)").ends_with(R"(,"engine_reason":"C:\\WG\\PLAY.SCN"}})"), "ENGINE? escapes each backslash of a reason");
    check(reason_reply("tab\there, bell\a, unit\x1F.").ends_with(R"(,"engine_reason":"tab?here, bell?, unit?."}})"), "ENGINE? sends each control character of a reason as '?'");
    check(reason_reply("caf\xC3\xA9 \x7F").ends_with(",\"engine_reason\":\"caf\xC3\xA9 \x7F\"}}"), "ENGINE? sends the bytes of a UTF-8 reason as they are");
    wg::play_watch().engine_reason = m110::Status::success();
    // engine_reason_max_chars holds the engine's own messages, the error map's
    // names and the longest parser and stage messages, so ENGINE? cuts none.
    constexpr auto longest_messages = std::to_array<std::string_view>({
        "watterson: must be the first stage (noise and interference follow the channel, E.4.1)",
        "watterson.paths[].spread_hz: required for a rayleigh path, number in [0.1, 10]",
        "sample_slip duplicates exceed 256 output frames in a 2048-input-frame window",
        "sample_slip length_samples must be in 1..1024 for the streaming engine",
        "watterson: needs a WattersonChannel (StageResources.watterson)",
    });
    const auto fits = [](std::string_view message) { return message.size() <= wg::engine_reason_max_chars; };

    check(std::ranges::all_of(signal_lab::engine_failure::all, [&fits](const m110::Status& failure) { return fits(failure.message); }) &&

              std::ranges::all_of(wg::engine_error_names, [&fits](const auto& row) { return fits(row.name); }) && std::ranges::all_of(longest_messages, fits) &&

              fits(wg::live_fault::no_reference.message),
          "engine_reason_max_chars holds every engine reason whole, and the live controls' own");
}

// The live controls' fault keeps its reason too (QA's check of #50 step 2,
// gap 4): a run the live controls stop (player_error 18, live_control) keeps
// the Status that said why in the watch, beside the engine's, and ENGINE?
// reports it as engine_reason, also after a refused scenario (QA on 2e92802),
// with STATUS's engine_state and engine_error as they are. The next PLAY
// restarts it.
void test_live_fault_reasons()
{
    namespace wg = waveform_generator;
    constexpr std::size_t quiet_card_frames = 10000U;
    constexpr std::string_view no_reference_reason{"live_control: a control queued before PLAY needs a reference RMS in (0, 1] (REFERENCE, or a card that is not silent)"};
    const auto is_status = [](m110::Status status, m110::StatusCode code, std::string_view message)
    { return status.code == code && status.message != nullptr && std::string_view{status.message} == message; };

    files["2:/WG/QUIET.WAV"] = wav(quiet_card_frames, 0);
    const auto play_quiet_with_preset = []
    {
        check(command("LOAD:QUIET.WAV").ok && command("CW ON").ok, "a silent card and a CW preset, no REFERENCE");
        run_selected();
        return wg::player_snapshot();
    };
    // A control queued before PLAY on a silent card has no reference to scale by.
    auto status = play_quiet_with_preset();
    check(status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::live_control) && status.engine_state == 0U &&
              status.engine_error == 0U,
          "a preset on a silent card stops the run with live_control, no scenario");
    check(is_status(wg::player_live_reason(), wg::live_fault::no_reference.code, no_reference_reason) && wg::player_engine_reason().is_ok(),
          "the watch keeps the live controls' reason, and the engine has none");
    check(engine_reply().ends_with(R"(,"engine_state":0,"engine_error":0,"engine_reason":)" + quoted(no_reference_reason) + "}}"),
          "ENGINE? reports why the live controls stopped the run, with STATUS's engine_state and engine_error");
    // A refused scenario keeps its own reason in the watch, but the play
    // stopped for the live controls' reason, which ENGINE? reports.
    const std::string_view unparsable = "{bad scenario}";
    files["2:/WG/QUIET.SCN"] = std::vector<std::uint8_t>(unparsable.begin(), unparsable.end());
    const m110::Status parse_verdict = signal_lab::parse_scenario(unparsable.data(), unparsable.size(), host_render().scenario);
    status = play_quiet_with_preset();
    check(status.error == std::to_underlying(wg::PlayerError::live_control) && status.engine_state == 2U && status.engine_error == std::to_underlying(wg::EngineError::scenario_parse),
          "a refused scenario, then the live controls stop the run");
    check(is_status(wg::player_live_reason(), wg::live_fault::no_reference.code, no_reference_reason) && is_status(wg::player_engine_reason(), parse_verdict.code, parse_verdict.message),
          "the watch keeps both reasons");
    check(engine_reply().ends_with(R"(,"engine_state":2,"engine_error":3,"engine_reason":)" + quoted(no_reference_reason) + "}}"),
          "ENGINE? reports why the live controls stopped the run, after a refused scenario too");
    files.erase("2:/WG/QUIET.SCN");
    // The block path, as the producer calls it: the live controller refuses a
    // block holding a NaN, process_block() returns its Status, and the run
    // latches live_control.
    check(command("LOAD:PLAY.WAV").ok, "LOAD of a card with a level");
    run_selected();
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && engine_reply().ends_with(R"(,"engine_state":0,"engine_error":0,"engine_reason":null}})"),
          "the next PLAY restarts the reason");
    check(wg::player_live_reason().is_ok(), "the next PLAY restarts the live controls' reason");
    constexpr std::size_t nan_index = 2U;
    constexpr float card_level = 0.25F;
    std::array<float, 4U> block{card_level, card_level, card_level, card_level};
    block.at(nan_index) = std::numeric_limits<float>::quiet_NaN();
    const auto processed = wg::process_block(block);
    check(!processed.has_value() && processed.status().code == m110::StatusCode::invalid_argument && std::string_view{processed.status().message} == "nonfinite live sample" &&
              wg::player_snapshot().error == std::to_underlying(wg::PlayerError::live_control),
          "process_block() returns the live controller's Status for a NaN, and the run latches live_control");
    check(is_status(wg::player_live_reason(), m110::StatusCode::invalid_argument, "nonfinite live sample"), "the watch keeps the live controller's Status");
    check(engine_reply().ends_with(R"(,"engine_state":0,"engine_error":0,"engine_reason":"nonfinite live sample"}})"), "ENGINE? reports the live controller's reason for the block");
    check(command("LOAD:PLAY.WAV").ok, "LOAD after the block's fault");
    run_selected();
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && engine_reply().ends_with(R"(,"engine_reason":null}})"),
          "a clean run after the block's fault reports no reason");
    check(wg::player_live_reason().is_ok(), "a clean run restarts the live controls' reason");
    files.erase("2:/WG/QUIET.WAV");
}

// The start of a run's live controls (start_live_controls()) on the paths no
// PLAY reaches on its own, since the run resets the controller for a
// reference in (0, 1] and queues again only presets the controller took
// before PLAY: a refused reset, a preset refused after the reset, and two
// presets refused. Each stops the run with live_control, and the watch keeps
// the FIRST refusal, as the run keeps its first fault, so ENGINE? names the
// refusal that stopped it (QA on batch 3: the last one used to win). A clean
// run between them restarts the state.
void test_live_start_refusals()
{
    namespace wg = waveform_generator;
    namespace sl = signal_lab;
    constexpr std::string_view reset_refusal{"live reference RMS must be finite and in (0, 32768]"};
    constexpr double controlled_reference = 1.0;
    constexpr double beyond_ci_db = 500.0; // a CW C/I outside [-120, 120] dB: invalid_event
    constexpr double ci_db = 3.0;
    const auto is_status = [](m110::Status status, m110::StatusCode code, std::string_view message)
    { return status.code == code && status.message != nullptr && std::string_view{status.message} == message; };

    const auto clean_run = []
    {
        check(command("LOAD:PLAY.WAV").ok, "LOAD of a card with a level");
        run_selected();
        check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && wg::player_live_reason().is_ok(), "a clean run, with no live reason");
    };
    // The run stopped with live_control, and ENGINE? gives `reason`, the one the watch keeps.
    const auto stopped_for = [&is_status](m110::StatusCode code, std::string_view reason)
    {
        const auto status = wg::player_snapshot();
        return status.state == static_cast<std::uint32_t>(wg::PlayerState::fault) && status.error == std::to_underlying(wg::PlayerError::live_control) &&
               is_status(wg::player_live_reason(), code, reason) && engine_reply().ends_with(R"(,"engine_reason":)" + quoted(reason) + "}}");
    };

    clean_run();
    const auto reset = wg::start_live_controls(std::numeric_limits<double>::quiet_NaN(), {});
    check(is_status(reset, m110::StatusCode::invalid_argument, reset_refusal) && stopped_for(m110::StatusCode::invalid_argument, reset_refusal),
          "a refused reset stops the run with live_control, and ENGINE? gives the controller's reason");
    clean_run();
    const sl::ControlEvent refused_preset{.frame = 0U, .kind = sl::ControlKind::cw_ci, .value = beyond_ci_db};
    const auto one = wg::start_live_controls(controlled_reference, std::span{&refused_preset, 1U});
    check(is_status(one, m110::StatusCode::internal_error, "invalid_event") && stopped_for(m110::StatusCode::internal_error, "invalid_event"),
          "a preset refused after the reset is internal_error with the refusal's name, and stops the run with live_control");
    // invalid_event first; then pending_full, for the control past the 64 that wait.
    clean_run();
    std::vector<sl::ControlEvent> presets{refused_preset};

    for (std::uint64_t frame = 0U; frame <= sl::live_pending_capacity; ++frame)
    {
        presets.push_back({.frame = frame, .kind = sl::ControlKind::cw_ci, .value = ci_db});
    }

    const auto two = wg::start_live_controls(controlled_reference, presets);
    check(is_status(two, m110::StatusCode::internal_error, "invalid_event") && stopped_for(m110::StatusCode::internal_error, "invalid_event"),
          "of two refused presets the first (invalid_event), not the last (pending_full), is the reason, as live_control is the run's first fault");
    clean_run();
}

// AT: positions count the engine's output frames, the dropped ones included:
// a CW carrier switched on at output frame 6000 starts 1000 frames earlier in
// the stream sent once a DROP of 1000 frames at 2000 discards some before it,
// and the cut play is the uncut one with those frames taken out.
void test_live_positions_run_through_drops()
{
    constexpr std::string_view drop_before{"CUT:2000:1000:drop"};
    const auto file = stage_usb_file("LIVE.WAV", cut_card_frames);
    const auto play_with = [](std::string_view schedule)
    {
        check(command("OUTPUT USB").ok && command("LOAD:LIVE.WAV").ok && command("AT:6000 CW ON").ok, "the card and a CW carrier from output frame 6000");
        play_on_usb(schedule);
        return received_play();
    };

    const auto plain = play_with(cut::command_name);
    const auto dropped = play_with(drop_before);
    const auto expected = expected_stream(plain, drop_before);
    check(plain.size() == file.samples.size() && plain != file.samples && dropped == expected.samples,
          "the carrier starts at output frame 6000 with or without the DROP before it: AT: runs on through dropped frames");
}

// Image A's CUT rows (build/main/bringup/w4/CHECKLIST.md row 6) on the player
// itself, when WFG_FSRAND_WAV names FSRAND.WAV, the card they ran on: the host
// receives the card with each schedule applied, and USB? and STATUS report
// the values the bench measured on 2057673 (2026-10-09), where the cuts acted
// on the card before the engine. A clean card's output is the card, so the
// rows' values hold after the channel unchanged.
struct ImageARow
{
    std::string_view name;
    std::string_view schedule;
    std::uint32_t count;
    std::uint32_t drop_frames;
    std::uint32_t zero_frames;
    std::uint32_t rejected;
    std::uint32_t sent;
    std::uint32_t first_position;
    std::uint32_t crc;
};

constexpr std::uint32_t fsrand_frames = 2880000U;
constexpr std::uint32_t fsrand_data_crc = 0x93B97321U;
constexpr auto image_a_rows = std::to_array<ImageARow>({
    {.name = "6.1 ZERO only",
     .schedule = "CUT:480000:144:zero,1440000:2400:zero,2400000:4800:zero",
     .count = 3U,
     .drop_frames = 0U,
     .zero_frames = 7344U,
     .rejected = 0U,
     .sent = 2887344U,
     .first_position = 480000U,
     .crc = 0xF8EA55BFU},

    {.name = "6.2 DROP only",
     .schedule = "CUT:480000:144:drop,1440000:2400:drop,2400000:4800:drop",
     .count = 3U,
     .drop_frames = 7344U,
     .zero_frames = 0U,
     .rejected = 0U,
     .sent = 2872656U,
     .first_position = 480000U,
     .crc = 0xD4A76BBCU},
    {.name = "6.3 mixed, 16",
     .schedule = "CUT:48000:1:zero,96001:1:drop,240000:4800:zero,240000:4800:drop,480003:4800:drop,484803:480:zero,"
                 "720000:2400:zero,960007:2400:drop,962407:144:drop,1200000:144:zero,1440000:4800:zero,1680001:4800:drop,1920000:1:zero,1920000:1:drop,"
                 "2400000:999:zero,2879999:4800:drop",
     .count = 16U,
     .drop_frames = 16947U,
     .zero_frames = 13625U,
     .rejected = 0U,
     .sent = 2876678U,
     .first_position = 48000U,
     .crc = 0x0AD9F805U},
    {.name = "6.4 past the end",
     .schedule = "CUT:960000:2400:zero,2880000:4800:drop",
     .count = 1U,
     .drop_frames = 0U,
     .zero_frames = 2400U,
     .rejected = 1U,
     .sent = 2882400U,
     .first_position = 960000U,
     .crc = 0x5ED0E17AU},
});

// The card WFG_FSRAND_WAV names, or none without it.
[[nodiscard]] std::optional<std::vector<std::uint8_t>> fsrand_card()
{
    const char* const path = std::getenv("WFG_FSRAND_WAV");

    if (path == nullptr || std::string_view{path}.empty())
    {
        return std::nullopt;
    }

    std::ifstream file{path, std::ios::binary};
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
}

void test_image_a_cut_rows()
{
    const auto card = fsrand_card();

    if (!card.has_value())
    {
        std::puts("image A CUT rows: skipped (WFG_FSRAND_WAV names no card)");
        return;
    }

    const auto data = std::span{*card}.subspan(std::min(card->size(), wav_header_bytes));
    check(data.size() == std::size_t{fsrand_frames} * waveform_generator::audio::pcm24_bytes_per_sample && waveform_generator::crc32(data) == fsrand_data_crc, "FSRAND.WAV is image A's card");
    std::vector<std::int32_t> samples;
    samples.reserve(fsrand_frames);

    for (std::size_t at = 0U; at + waveform_generator::audio::pcm24_bytes_per_sample <= data.size(); at += waveform_generator::audio::pcm24_bytes_per_sample)
    {
        samples.push_back(waveform_generator::audio::read_pcm24_le(data.subspan(at).data()));
    }

    files["2:/WG/FSRAND.WAV"] = *card;
    check(command("OUTPUT USB").ok, "the USB route");

    for (const auto& row : image_a_rows)
    {
        const std::string name{row.name};
        check(command("LOAD:FSRAND.WAV").ok, name + ": FSRAND is selected");
        play_on_usb(row.schedule);
        const auto play = ua::play_statistics();
        const auto first = play.first_frame_index.value;
        const std::vector<StatusMember> cuts{
            {.name = "uac_play_cut_count", .value = std::to_string(row.count)},
            {.name = "uac_play_cut_drop_frames", .value = std::to_string(row.drop_frames)},
            {.name = "uac_play_cut_zero_frames", .value = std::to_string(row.zero_frames)},
            {.name = "uac_play_cut_first_index", .value = std::to_string(first + row.first_position)},
            {.name = "uac_play_cut_rejected", .value = std::to_string(row.rejected)},
            {.name = "uac_play_sent_frames", .value = std::to_string(row.sent)},
        };
        const auto lines = exchange("1 USB?\n2 STATUS?\n");
        const auto status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
        check(has_members(usb_members(lines), cuts) && play.crc32_left == row.crc && play.crc32_right == row.crc && play.last_frame_index.value == first + row.sent - 1U &&
                  member_value(status, "uac_play_file_frames") == std::to_string(fsrand_frames) && member_value(status, "uac_play_missed_intervals") == "0",
              name + ": USB? and STATUS report the values image A's bench row measured");
        check(received_play() == expected_stream(samples, row.schedule).samples, name + ": the host receives FSRAND with the schedule applied");
    }

    files.erase("2:/WG/FSRAND.WAV");
}

// USB? ends with the play's cut fields, in their order, after the SD bus
// clock, then the play's engine deadline misses, the USB interrupt's EP0
// order and the completions reported short of their packet.
void test_usb_diagnostics_cut_order()
{
    constexpr auto tail = std::to_array<std::string_view>({"sd_bus_clock_hz", "uac_play_cut_count", "uac_play_cut_drop_frames", "uac_play_cut_zero_frames", "uac_play_cut_first_index",
                                                           "uac_play_cut_rejected", "uac_play_sent_frames", "engine_deadline_misses", "usb_ep0_reordered", "usb_ep0_unordered", "uac_sent_mismatches"});
    const auto usb = usb_members(exchange("1 USB?\n"));
    const auto names = std::span{waveform_generator::usb_field_names}.last(tail.size());
    check(usb.size() >= tail.size() && std::ranges::equal(std::span{usb}.last(std::min(usb.size(), tail.size())), tail, {}, &StatusMember::name) && std::ranges::equal(names, tail),
          "USB? ends with sd_bus_clock_hz, the play's six cut fields, its engine deadline misses, the EP0 order's counters and uac_sent_mismatches, "
          "in the contract's order");
}

// PLAYLIST batch mode on the player (player.cpp's sequencer, usb_service.cpp's
// PLAYLIST?): the start's file and card checks and their refusals, the lane's
// six-item battery played unattended on the USB route with exact device-time
// gaps, STOP in an item, in a gap and while an item waits for its stream
// index, the stop and continue policies with an item that finds no stream
// (NO_STREAM), the ring that pauses when full until PLAYLIST ACK, route
// switching, each item's CUT list and scenario seed, the BUSY rule, and
// PLAYLIST?'s members, order and budget.
namespace pl = waveform_generator::playlist;

constexpr unsigned int playlist_wait_call_limit = 4000000U;
constexpr std::uint32_t frames_per_ms = 48U;
constexpr std::uint32_t five_second_gap_frames = 5000U * frames_per_ms;
// The lane's battery: six items, and FSRAND's erasure-free cuts, a 480-frame
// ZERO at card frame 1000 and a 480-frame DROP at 3000.
constexpr std::size_t battery_items = 6U;
constexpr std::uint32_t fsrand_cut_frames = 480U;
constexpr std::uint32_t fsrand_zero_at = 1000U;
// A clean signal-lab scenario with AWGN, as a lane scenario card would be.
constexpr std::string_view noise_scenario{R"({"schema":"signal-lab.scenario/1","test_id":"noise","seed":42,"reference":"auto","reference_rms":0.25,)"
                                          R"("source_gain_db":-12,"clip_policy":"saturate","impairments":[{"type":"awgn","snr_db":20}]})"};

void stage_text(const std::string& name, std::string_view text)
{
    files["2:/WG/" + name] = std::vector<std::uint8_t>(text.begin(), text.end());
}

[[nodiscard]] std::uint32_t text_crc(std::string_view text)
{
    waveform_generator::Crc32 crc;

    for (const char character : text)
    {
        crc.update(std::array{std::bit_cast<std::uint8_t>(character)});
    }

    return crc.value();
}

// The commands the tick hook sent, and the replies it took to send them.
[[nodiscard]] std::vector<waveform_generator::ControlReply>& hook_replies()
{
    static std::vector<waveform_generator::ControlReply> replies;
    return replies;
}

// Sends `text` from the tick hook, while a run or a playlist is under way:
// a reply still waiting is taken first, as the CDC service would.
void send_from_hook(const std::string& text)
{
    namespace wg = waveform_generator;
    wg::ControlReply waiting{};

    if (wg::take_player_reply(waiting))
    {
        hook_replies().push_back(waiting);
    }

    std::uint32_t previous{};
    const auto parsed = wg::live_protocol::parse("1 " + text, previous);
    check(parsed.has_value(), "the hook's command parses");
    const auto request = parsed.has_value() ? parsed.value() : wg::live_protocol::Request{};
    check(wg::submit_player_command(request).is_ok(), "the player's mailbox takes the hook's command");
}

// The playlist state as at boot, no tick hook, no reply kept.
void reset_playlist_state()
{
    waveform_generator::reset_playlist();
    usb_host().on_tick({});
    hook_replies().clear();
}

// A refused live control's reply names the live controller's refusal: a value
// out of range is invalid_event, a control waiting for its frame when 64
// already wait is pending_full, and one for a frame the play has produced is
// late_event.
void test_live_control_refusals()
{
    namespace wg = waveform_generator;
    constexpr std::size_t waiting_controls = signal_lab::live_pending_capacity;
    constexpr std::uint64_t first_frame = 6000U;
    const auto at_frame = [](std::uint64_t frame) { return "AT:" + std::to_string(frame) + " CW CI 3"; };

    const auto refused_with = [](const wg::ControlReply& reply, std::string_view error) { return !reply.ok && std::string_view{reply.error} == error; };

    static_cast<void>(stage_usb_file("REFUSE.WAV", cut_card_frames));
    check(command("OUTPUT USB").ok && command("LOAD:REFUSE.WAV").ok, "a card on the USB route for the refused controls");
    check(refused_with(command("CW FREQ 30000"), "invalid_event"), "a CW frequency above Nyquist is invalid_event");
    bool queued = true;

    for (std::size_t index = 0U; index < waiting_controls; ++index)
    {
        queued = command(at_frame(first_frame + index).c_str()).ok && queued;
    }

    check(queued && refused_with(command(at_frame(first_frame + waiting_controls).c_str()), "pending_full"), "64 controls wait for their frames, and one more is pending_full");
    bool sent = false;
    usb_host().on_tick(
        [&sent]
        {
            if (!sent && wg::live_controller.frame() > first_frame + waiting_controls)
            {
                sent = true;
                send_from_hook("AT:0 CW CI 3");
            }
        });
    play_on_usb(cut::command_name);
    usb_host().on_tick({});
    wg::ControlReply late{};
    check(sent && wg::take_player_reply(late) && refused_with(late, "late_event"), "a control for a frame the play has produced is late_event");
    hook_replies().clear();
    wg::live_protocol::Request query{};
    query.kind = wg::live_protocol::Kind::info;
    // A sweep that would be taken but for its one step.
    constexpr double sweep_hz = 1000.0;
    constexpr std::uint32_t sweep_interval_ms = 100U;
    wg::live_protocol::Request one_step{};
    one_step.kind = wg::live_protocol::Kind::sweep_frequency;
    one_step.steps = 1U;
    one_step.value = sweep_hz;
    one_step.end_value = sweep_hz;
    one_step.interval_ms = sweep_interval_ms;
    std::uint64_t frame = 0U;
    std::uint32_t events = 0U;
    check(signal_lab::same_failure(wg::live_protocol::apply_control(query, wg::live_controller, frame, events), signal_lab::control_refusal::invalid_event) &&
              signal_lab::same_failure(wg::live_protocol::apply_control(one_step, wg::live_controller, frame, events), signal_lab::control_refusal::invalid_event),
          "apply_control() refuses a request that is no live control, and a sweep of one step, as invalid_event");
}

// The playlist the player task accepted, run as the task would run it, with
// room for the simulated waits of its gaps; then the replies to commands the
// hook sent are taken.
void run_accepted_playlist(bool deferred_stop_pending)
{
    namespace wg = waveform_generator;
    check(wg::player_playlist().run_requested && wg::playlist_owns_player() && !wg::player_allows_media_host() && wg::player_live_snapshot().busy,
          "an accepted PLAYLIST holds the player and the card until it has run");
    wait_calls = 0U;
    wait_call_limit() = playlist_wait_call_limit;
    wg::run_playlist();
    wait_call_limit() = default_wait_call_limit;
    usb_host().on_tick({});
    wg::ControlReply waiting{};

    if (wg::take_player_reply(waiting))
    {
        hook_replies().push_back(waiting);
    }

    const bool player_and_card_free = !mounted && open_files == 0U && !wg::playlist_owns_player() && !wg::player_live_snapshot().busy && wg::command_pending.load() == deferred_stop_pending &&
                                      wg::player_allows_media_host() == !deferred_stop_pending;

    if (!player_and_card_free)
    {
        const auto live = wg::player_live_snapshot();
        const auto progress = wg::player_playlist_snapshot().progress;
        std::fprintf(stderr, "playlist cleanup diagnostic: state=%u run_requested=%u owns=%u busy=%u mounted=%u open_files=%u reply_pending=%u command_pending=%u expected_stop=%u\n",
                     static_cast<unsigned>(progress.state), static_cast<unsigned>(wg::player_playlist().run_requested), static_cast<unsigned>(wg::playlist_owns_player()),
                     static_cast<unsigned>(live.busy), static_cast<unsigned>(mounted), open_files, static_cast<unsigned>(wg::reply_pending.load()), static_cast<unsigned>(wg::command_pending.load()),
                     static_cast<unsigned>(deferred_stop_pending));
    }

    check(player_and_card_free, "a playlist that ended leaves the player and the card free");
}

void test_card_listing_player_protocol()
{
    namespace wg = waveform_generator;
    const auto original_route = wg::player_output_route();
    const bool original_media_local = media_local;
    const bool original_tx_busy = status_seam().tx_artifact_busy;
    reset_directory_fixture();
    mounted = false;
    media_local = true;
    status_seam().tx_artifact_busy = false;

    for (unsigned int index = 0U; index < 26U; ++index)
    {
        directory_entries.push_back({.name = std::string{"P"} + (index < 10U ? "0" : "") + std::to_string(index) + ".WAV", .alias = "", .size = static_cast<FSIZE_t>(index * 101U)});
    }

    const auto pages = exchange("1 FILES?\n2 FILES? 24\n");
    check(pages.size() == 2U && pages[0].contains(R"("files":{"offset":0,"total":26,"next_offset":24,"entries":[{"name":"P00.WAV","size_bytes":0})") &&
              pages[0].contains(R"END({"name":"P23.WAV","size_bytes":2323}]}})END"),
          "FILES? returns the first 24 player-owned directory entries in protocol order");
    check(pages.size() == 2U && pages[1].contains(R"("files":{"offset":24,"total":26,"next_offset":null,"entries":[{"name":"P24.WAV","size_bytes":2424},{"name":"P25.WAV","size_bytes":2525}]}})"),
          "FILES? offset pages resume at the exact next FatFs entry and terminate with null");
    check(directory_faults().mount_calls == 2U && directory_faults().unmount_calls == 2U && directory_faults().successful_opens == 2U && directory_faults().close_calls == 2U &&
              directory_faults().open_directories == 0U && !mounted && !wg::file_system_owned && wg::player_allows_media_host(),
          "each successful FILES? request mounts, closes and unmounts the card before releasing ownership");
    reset_directory_fixture();
    media_local = false;
    const auto host_owned = exchange("3 FILES?\n");
    check(host_owned.size() == 1U && host_owned[0].contains(R"("code":"HOST_OWNED")") && directory_faults().mount_calls == 0U && directory_faults().unmount_calls == 0U &&
              directory_faults().open_attempts == 0U && !wg::file_system_owned,
          "FILES? refuses Host-owned media before making any FatFs call");
    media_local = true;
    reset_directory_fixture();
    wg::play_requested = true;
    const auto play_busy = exchange("4 FILES?\n");
    wg::play_requested = false;
    check(play_busy.size() == 1U && play_busy[0].contains(R"("code":"BUSY")") && directory_faults().mount_calls == 0U && directory_faults().open_attempts == 0U,
          "FILES? is BUSY while playback is pending and does not claim the card");
    reset_directory_fixture();
    wg::player_playlist().owns_player = true;
    const auto playlist_busy = exchange("41 FILES?\n");
    wg::player_playlist().owns_player = false;
    check(playlist_busy.size() == 1U && playlist_busy[0].contains(R"("code":"BUSY")") && directory_faults().mount_calls == 0U && directory_faults().open_attempts == 0U,
          "FILES? is BUSY while a playlist owns the player");
    reset_directory_fixture();
    status_seam().tx_artifact_busy = true;
    const auto tx_busy = exchange("5 FILES?\n");
    status_seam().tx_artifact_busy = false;
    check(tx_busy.size() == 1U && tx_busy[0].contains(R"("code":"BUSY")") && directory_faults().mount_calls == 0U && directory_faults().open_attempts == 0U,
          "FILES? is BUSY during TX artifact generation without opening the card");
    reset_directory_fixture();
    directory_faults().fail_mount = true;
    const auto mount_failure = exchange("6 FILES?\n");
    check(mount_failure.size() == 1U && mount_failure[0].contains(R"("code":"FILES_MOUNT")") && directory_faults().mount_calls == 1U && directory_faults().unmount_calls == 1U &&
              directory_faults().open_attempts == 0U && !mounted && !wg::file_system_owned,
          "FILES? attempts unmount cleanup even when the initial mount fails");
    const auto check_reader_error = [](const char* request, const char* expected, auto configure)
    {
        reset_directory_fixture();
        directory_entries.push_back({.name = "ONE.WAV", .alias = "", .size = 10U});
        configure(directory_faults());
        const auto lines = exchange(std::string{request} + "\n");
        check(lines.size() == 1U && lines[0].contains(std::string{R"("code":")"} + expected + R"(")"), std::string{"FILES? publishes "} + expected + " from the player mailbox");
        check(directory_faults().unmount_calls == 1U && !mounted && !wg::file_system_owned, std::string{"FILES? releases the mount after "} + expected);
    };

    check_reader_error("7 FILES?", "FILES_OPEN", [](DirectoryFaults& faults) { faults.fail_open = true; });

    check_reader_error("8 FILES?", "FILES_READ", [](DirectoryFaults& faults) { faults.reads_before_failure = 0U; });

    check_reader_error("9 FILES?", "FILES_CLOSE", [](DirectoryFaults& faults) { faults.fail_close = true; });

    check_reader_error("10 FILES?", "FILES_NAME", [](DirectoryFaults&) { directory_entries[0].name = std::string(37U, 'A') + ".WAV"; });

    check_reader_error("11 FILES? 2", "FILES_RANGE", [](DirectoryFaults&) {});

    reset_directory_fixture();
    directory_entries.push_back({.name = "RECOVER.WAV", .alias = "", .size = 45U});
    directory_faults().fail_unmount = true;
    const auto unmount_failure = exchange("12 FILES?\n");
    const unsigned int mounts_after_failure = directory_faults().mount_calls;
    const unsigned int unmounts_after_failure = directory_faults().unmount_calls;
    check(unmount_failure.size() == 1U && unmount_failure[0].contains(R"("code":"FILES_UNMOUNT")") && mounted && wg::file_system_owned && !wg::player_allows_media_host() &&
              mounts_after_failure == 1U && unmounts_after_failure == 1U,
          "FILES? retains ownership after unmount failure so the card cannot pass to the host");
    media_local = false;
    const auto pending_host_owned = exchange("13 FILES?\n");
    check(pending_host_owned.size() == 1U && pending_host_owned[0].contains(R"("code":"HOST_OWNED")") && directory_faults().mount_calls == mounts_after_failure &&
              directory_faults().unmount_calls == unmounts_after_failure && directory_faults().open_attempts == 1U && wg::file_system_owned,
          "Host-owned media refuses a pending cleanup retry without touching FatFs");
    media_local = true;
    directory_faults().fail_unmount = false;
    const auto recovered = exchange("14 FILES?\n");
    check(recovered.size() == 1U && recovered[0].contains(R"("files":{"offset":0,"total":1,"next_offset":null,"entries":[{"name":"RECOVER.WAV","size_bytes":45}]}})") &&
              directory_faults().mount_calls == mounts_after_failure + 1U && directory_faults().unmount_calls == unmounts_after_failure + 2U && !mounted && !wg::file_system_owned &&
              wg::player_allows_media_host(),
          "the next Local FILES? retries unmount before scanning, then unmounts again and releases ownership");
    reset_directory_fixture();
    mounted = false;
    files["2:/WG/GOOD.WAV"] = wav(10000U, 1000);
    const std::string playlist_text{"GOOD.WAV 0 CODEC\n"};
    files["2:/WG/ONE.PL"] = {playlist_text.begin(), playlist_text.end()};
    check(command("OUTPUT CODEC").ok, "the old playback regression uses the codec route so it needs no USB stream");
    const auto old_commands = exchange("15 LOAD:GOOD.WAV\n16 PLAY\n");
    check(old_commands.size() == 2U && old_commands[0].contains(R"("ok":true)") && old_commands[1].contains(R"("ok":true)") && !wg::file_system_owned,
          "LOAD and PLAY still route through the owner after FILES? queries");
    run_accepted();
    const auto playlist = exchange("17 PLAYLIST:ONE.PL\n");
    check(playlist.size() == 1U && playlist[0].contains(R"("ok":true)") && wg::playlist_owns_player(), "PLAYLIST still routes through the owner after FILES? queries");
    run_accepted_playlist(false);
    reset_playlist_state();
    files.erase("2:/WG/GOOD.WAV");
    files.erase("2:/WG/ONE.PL");

    if (original_route == wg::OutputRoute::usb)
    {
        check(command("OUTPUT USB").ok, "FILES tests restore the pre-existing USB output route");
    }

    check(wg::player_output_route() == original_route, "FILES tests leave the output route as they found it");
    status_seam().tx_artifact_busy = original_tx_busy;
    media_local = original_media_local;
    reset_directory_fixture();
    mounted = false;
}

void test_prepare_player_protocol()
{
    namespace wg = waveform_generator;
#if defined(WFG_MSC_READ_ONLY)
    const unsigned int mounts_before = directory_faults().mount_calls;
    const auto refusal = exchange("1 PREPARE:PREPOUT.WAV:PREPSRC.WAV:PREP.SCN:KEEP:23\n");
    check(refusal.size() == 1U && refusal[0].contains(R"("code":"READ_ONLY_IMAGE")") && directory_faults().mount_calls == mounts_before && open_files == 0U && !mounted,
          "a read-only image refuses PREPARE before opening or mounting the card");
    const auto status = exchange("2 PREPARE?\n");
    check(status.size() == 1U && status[0].contains(R"("prepare":{"phase":10)") && status[0].contains(R"("error":"READ_ONLY_IMAGE")"),
          "PREPARE? remains available and reports why a read-only image cannot prepare files");
#else
    files["2:/WG/PREPSRC.WAV"] = wav(512U, 1200);
    constexpr std::string_view scenario = R"({"seed":7,"reference_rms":0.1,"source_gain_db":0,"impairments":[]})";
    files["2:/WG/PREP.SCN"] = std::vector<std::uint8_t>(scenario.begin(), scenario.end());
    const auto admission = exchange("1 PREPARE:PREPOUT.WAV:PREPSRC.WAV:PREP.SCN:KEEP:23\n");
    check(admission.size() == 1U && admission[0].contains(R"("ok":true)") && !wg::player_allows_media_host() && wg::player_prepare_snapshot().phase == wg::prepare::Phase::starting,
          "PREPARE admits an asynchronous render and takes Local media ownership");
    const auto busy = exchange("2 LOAD:PREPSRC.WAV\n");
    check(busy.size() == 1U && busy[0].contains(R"("code":"BUSY")"), "PREPARE keeps other media-changing player commands BUSY while the job runs");
    const auto in_progress = exchange("3 PREPARE?\n");
    check(in_progress.size() == 1U && in_progress[0].contains(R"("prepare":{"phase":1)") && in_progress[0].contains(R"("source":"PREPSRC.WAV")"),
          "PREPARE? reports the accepted request while rendering is pending");
    unsigned int steps{};

    while (wg::player_prepare_snapshot().phase != wg::prepare::Phase::complete && steps < 1000U)
    {
        wg::service_preparation();
        ++steps;
    }

    const auto complete = wg::player_prepare_snapshot();
    const auto final_status = exchange("4 PREPARE?\n");
    check(steps < 1000U && complete.phase == wg::prepare::Phase::complete && !wg::player_live_snapshot().busy && wg::player_allows_media_host() && complete.seed == 23U &&
              complete.input_frames == 512U && complete.output_frames == 512U && std::char_traits<char>::length(complete.wav_sha256.data()) == 64U &&
              std::char_traits<char>::length(complete.pcm_sha256.data()) == 64U,
          "PREPARE completes with frame counts, requested seed, hashes and released media ownership");
    check(final_status.size() == 1U && final_status[0].contains(R"("prepare":{"phase":8)") && final_status[0].contains(R"("retention":"keep")") &&
              final_status[0].contains(std::string{R"("wav_sha256":")"} + complete.wav_sha256.data() + '"') &&
              final_status[0].contains(std::string{R"("pcm_sha256":")"} + complete.pcm_sha256.data() + '"') && files.contains("2:/WG/PREPOUT.WAV") && files.contains("2:/WG/PREPOUT.JSN") &&
              !files.contains("2:/WG/PREPOUT.TMP") && !files.contains("2:/WG/PREPOUT.JMP"),
          "PREPARE? publishes complete output identity and only final artifact names");
    files.erase("2:/WG/PREPSRC.WAV");
    files.erase("2:/WG/PREP.SCN");
    files.erase("2:/WG/PREPOUT.WAV");
    files.erase("2:/WG/PREPOUT.JSN");
#endif
}

#if !defined(WFG_MSC_READ_ONLY)
[[nodiscard]] std::vector<std::int32_t> prepared_wav_samples(std::span<const std::uint8_t> wav_bytes)
{
    namespace audio = waveform_generator::audio;

    if (wav_bytes.size() < wav_header_bytes)
    {
        return {};
    }

    const std::uint32_t payload_bytes = std::uint32_t{wav_bytes[40U]} | (std::uint32_t{wav_bytes[41U]} << 8U) | (std::uint32_t{wav_bytes[42U]} << 16U) | (std::uint32_t{wav_bytes[43U]} << 24U);
    const std::size_t expected_size = wav_header_bytes + payload_bytes + (payload_bytes & 1U);

    if (payload_bytes % audio::pcm24_bytes_per_sample != 0U || wav_bytes.size() != expected_size)
    {
        return {};
    }

    std::vector<std::int32_t> samples;
    samples.reserve(payload_bytes / audio::pcm24_bytes_per_sample);

    for (std::size_t offset = wav_header_bytes; offset < wav_header_bytes + payload_bytes; offset += audio::pcm24_bytes_per_sample)
    {
        samples.push_back(audio::read_pcm24_le(wav_bytes.subspan(offset).data()));
    }

    return samples;
}

#endif

// Read the nested scheduler state rather than the envelope's player state.
[[nodiscard]] std::vector<StatusMember> prepared_playback_members(std::string_view response)
{
    constexpr std::string_view header{R"("prepare_play":)"};
    const auto at = response.find(header);
    return at == std::string_view::npos ? std::vector<StatusMember>{} : direct_json_object_members(response.substr(at + header.size()));
}

#if !defined(WFG_MSC_READ_ONLY)
[[nodiscard]] std::string timing_phase_value(std::string_view timing_json, std::string_view phase, std::string_view field)
{
    std::string marker{"\""};
    marker.append(phase);
    marker += R"(":)";
    const auto phase_at = timing_json.find(marker);

    if (phase_at == std::string_view::npos)
    {
        return {};
    }

    const auto object_at = timing_json.find('{', phase_at + marker.size());

    if (object_at == std::string_view::npos)
    {
        return {};
    }

    const auto members = direct_json_object_members(timing_json.substr(object_at));
    return member_value(members, field);
}

[[nodiscard]] bool timing_phase_has_measurements(std::string_view timing_json, std::string_view phase)
{
    const std::string calls = timing_phase_value(timing_json, phase, "calls");
    const std::string cycles = timing_phase_value(timing_json, phase, "total_cycles");
    const std::string maximum = timing_phase_value(timing_json, phase, "max_cycles");
    const std::string rejected = timing_phase_value(timing_json, phase, "rejected");
    const std::string saturated = timing_phase_value(timing_json, phase, "saturated");
    return is_decimal(calls, waveform_generator::max_uint64_decimal_chars) && calls != "0" && is_decimal(cycles, waveform_generator::max_uint64_decimal_chars) && cycles != "0" &&
           is_decimal(maximum, waveform_generator::max_uint32_decimal_chars) && maximum != "0" && is_decimal(rejected, waveform_generator::max_uint64_decimal_chars) && rejected == "0" &&
           saturated == "false";
}

[[nodiscard]] bool timing_phase_is_empty(std::string_view timing_json, std::string_view phase)
{
    return timing_phase_value(timing_json, phase, "calls") == "0" && timing_phase_value(timing_json, phase, "total_cycles") == "0" && timing_phase_value(timing_json, phase, "max_cycles") == "0" &&
           timing_phase_value(timing_json, phase, "rejected") == "0" && timing_phase_value(timing_json, phase, "saturated") == "false";
}

#endif

void test_prepare_playback_protocol()
{
    namespace wg = waveform_generator;
    const auto initial_query = exchange("1 PREPAREPLAY?\n");
    const auto initial_json = prepared_playback_members(initial_query.size() == 1U ? initial_query[0] : std::string{});
    check(initial_query.size() == 1U && member_value(initial_json, "state") == "0" && member_value(initial_json, "active") == "false" && member_value(initial_json, "status") == "0" &&
              member_value(initial_json, "operation_id") == "0" && member_value(initial_json, "reason") == quoted("ok") && member_value(initial_json, "total_frames") == "0",
          "PREPAREPLAY? starts with a coherent empty snapshot and zero operation ID before admission");
    const auto initial_timing = direct_json_object_members(member_value(initial_json, "timing"));
    check(member_value(initial_timing, "cycle_hz") == "0" && member_value(initial_timing, "tick_hz") == "0", "PREPAREPLAY? begins with unconfigured timing clocks before its first admission");
    auto& host = usb_host();
    const auto route_before = wg::player_output_route();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok, "PREPAREPLAY selects a live USB capture route");
#if defined(WFG_MSC_READ_ONLY)
    const unsigned int mounts_before = directory_faults().mount_calls;
    const auto refusal = exchange("1 PREPAREPLAY:PPROUT.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:48000:2048:23\n");
    check(refusal.size() == 1U && refusal[0].contains(R"("code":"READ_ONLY_IMAGE")") && directory_faults().mount_calls == mounts_before && open_files == 0U && !mounted,
          "a read-only image refuses PREPAREPLAY without opening or mounting the card");
#else
    constexpr std::size_t source_frames = 240000U;
    const auto original = full_range_samples(source_frames);
    files["2:/WG/PPSRC.WAV"] = pcm24_wav(original);
    constexpr std::string_view scenario = R"({"seed":7,"reference_rms":0.1,"source_gain_db":0,"impairments":[{"type":"awgn","snr_db":20}]})";
    stage_text("PPCASE.SCN", scenario);
    const auto admission = exchange("1 PREPAREPLAY:PPROUT.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:48000:2048:23\n");
    check(admission.size() == 1U && admission[0].contains(R"("ok":true)") && !wg::player_allows_media_host() && wg::preparation_busy,
          "PREPAREPLAY admits bounded preparation and takes Local media ownership");
    const auto admitted_query = exchange("2 PREPAREPLAY?\n");
    const auto admitted_json = prepared_playback_members(admitted_query.size() == 1U ? admitted_query[0] : std::string{});
    check(admitted_query.size() == 1U && member_value(admitted_json, "active") == "true" && member_value(admitted_json, "operation_id") == "1" && member_value(admitted_json, "state") == "0" &&
              member_value(admitted_json, "rate") == "48000" && member_value(admitted_json, "margin_frames") == "2048" && member_value(admitted_json, "total_frames") == "0",
          "PREPAREPLAY? reports its first operation ID, accepted rate and margin before the first bounded step");
    const std::string admitted_timing = member_value(admitted_json, "timing");
    const auto busy = exchange("2 LOAD:PPSRC.WAV\n");
    check(busy.size() == 1U && busy[0].contains(R"("code":"BUSY")"), "other media-changing commands remain BUSY during PREPAREPLAY");
    const auto overlapping = command("PREPAREPLAY:PPBUSY.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:48000:2048:24");
    const auto overlap_query = exchange("23 PREPAREPLAY?\n");
    const auto overlap_json = prepared_playback_members(overlap_query.size() == 1U ? overlap_query[0] : std::string{});
    check(!overlapping.ok && member_value(overlap_json, "operation_id") == "1" && member_value(overlap_json, "timing") == admitted_timing && !files.contains("2:/WG/PPBUSY.WAV"),
          "a refused overlapping PREPAREPLAY does not allocate or advance the operation ID");
    bool saw_playback_while_rendering = false;
    bool saw_final_names_absent_during_playback = false;
    bool queried_progress = false;
    constexpr unsigned int max_steps = 20000U;
    unsigned int steps{};

    while (wg::preparation_busy && steps < max_steps)
    {
        wg::service_preparation();
        host.run(1U);
        const auto player = wg::player_snapshot();
        const auto progress = wg::prepared_playback.snapshot();

        if ((player.state == static_cast<std::uint32_t>(wg::PlayerState::playing) || progress.started) && progress.written_frames < source_frames)
        {
            saw_playback_while_rendering = true;
            saw_final_names_absent_during_playback = !files.contains("2:/WG/PPROUT.WAV") && !files.contains("2:/WG/PPROUT.JSN");

            if (!queried_progress)
            {
                const auto progress_query = exchange("3 PREPAREPLAY?\n");
                const auto progress_json = prepared_playback_members(progress_query.size() == 1U ? progress_query[0] : std::string{});
                check(progress_query.size() == 1U && member_value(progress_json, "active") == "true" && member_value(progress_json, "total_frames") == std::to_string(source_frames) &&
                          member_value(progress_json, "started") == "true" && member_value(progress_json, "committed_frames") == std::to_string(progress.committed_frames) &&
                          member_value(progress_json, "written_frames") == std::to_string(progress.written_frames) &&
                          member_value(progress_json, "queued_frames") == std::to_string(progress.read_frames) &&
                          member_value(progress_json, "consumed_frames") == std::to_string(progress.consumed_frames) && progress.committed_frames <= progress.written_frames &&
                          progress.consumed_frames <= progress.read_frames,
                      "PREPAREPLAY? exposes committed, queued and consumed progress during playback");
                queried_progress = true;
            }
        }

        ++steps;
    }

    wg::service_preparation();
    host.run(usb_trailing_milliseconds);
    const auto completed = wg::player_prepare_snapshot();
    const auto completed_query = exchange("4 PREPAREPLAY?\n");
    const auto completed_json = prepared_playback_members(completed_query.size() == 1U ? completed_query[0] : std::string{});
    const auto player = wg::player_snapshot();
    const auto play = ua::play_statistics();
    const auto output_it = files.find("2:/WG/PPROUT.WAV");
    const auto rendered = output_it == files.end() ? std::vector<std::int32_t>{} : prepared_wav_samples(output_it->second);
    const auto manifest_it = files.find("2:/WG/PPROUT.JSN");
    const std::string manifest = manifest_it == files.end() ? std::string{} : std::string{manifest_it->second.begin(), manifest_it->second.end()};
    const bool differs_from_source = rendered.size() == original.size() && rendered != original;
    check(steps < max_steps && !wg::preparation_busy && completed.phase == wg::prepare::Phase::complete && player.state == static_cast<std::uint32_t>(wg::PlayerState::done) &&
              player.pcm_drained == 1U && play.file_frames.value == source_frames,
          "PREPAREPLAY drains the finite USB playback before completing the prepared artifact");
    check(completed_query.size() == 1U && member_value(completed_json, "active") == "false" && member_value(completed_json, "operation_id") == "1" && member_value(completed_json, "state") == "4" &&
              member_value(completed_json, "status") == "0" && member_value(completed_json, "artifact_finalized") == "true" && member_value(completed_json, "owner_released") == "true" &&
              member_value(completed_json, "consumed_frames") == std::to_string(source_frames),
          "PREPAREPLAY? retains its successful terminal state after playback drains");
    const std::string completed_timing = member_value(completed_json, "timing");
    const auto completed_timing_top = direct_json_object_members(completed_timing);
    check(member_value(completed_timing_top, "cycle_hz") == "996000000" && member_value(completed_timing_top, "tick_hz") == "1000" && timing_phase_has_measurements(completed_timing, "setup") &&
              timing_phase_has_measurements(completed_timing, "source_read") && timing_phase_has_measurements(completed_timing, "render") &&
              timing_phase_has_measurements(completed_timing, "append") && timing_phase_has_measurements(completed_timing, "commit") && timing_phase_has_measurements(completed_timing, "read") &&
              timing_phase_has_measurements(completed_timing, "finalize"),
          "PREPAREPLAY? measures each successful operation phase and publishes its clock rates");
    check(saw_playback_while_rendering && saw_final_names_absent_during_playback, "the player starts from the committed prefix while rendering continues, before final names are published");
    check(rendered.size() == source_frames && differs_from_source && received_exactly(host.received(), play.first_frame_index.value, rendered),
          "USB receives the exact nontrivial PCM24 samples stored in the completed prepared WAV");
    check(completed.seed == 23U && completed.input_frames == source_frames && completed.output_frames == source_frames &&
              manifest.find(R"("schema":"waveform-generator.prepared/2")") != std::string::npos && manifest.find(R"("complete":true)") != std::string::npos &&
              manifest.find(R"("seed":23)") != std::string::npos && manifest.find(std::string{R"("wav_sha256":")"} + completed.wav_sha256.data() + '"') != std::string::npos &&
              manifest.find(std::string{R"("pcm_sha256":")"} + completed.pcm_sha256.data() + '"') != std::string::npos,
          "the retained manifest binds seed, frame counts and the exact playback WAV/PCM hashes");
    host.open_stream();
    host.run(usb_idle_milliseconds);
    const std::array<std::int32_t, 7U> odd_source{-0x123456, -1, 0, 1, 0x123456, 0x456789, -0x456789};
    auto odd_source_wav = pcm24_wav(odd_source);
    odd_source_wav.push_back(0U);
    const std::uint32_t odd_riff_size = static_cast<std::uint32_t>(odd_source_wav.size() - 8U);

    for (unsigned int byte = 0U; byte < 4U; ++byte)
    {
        odd_source_wav[4U + byte] = static_cast<std::uint8_t>(odd_riff_size >> (8U * byte));
    }

    files["2:/WG/PPODD.WAV"] = std::move(odd_source_wav);
    constexpr std::string_view odd_scenario = R"({"seed":31,"reference_rms":0.1,"source_gain_db":0,"impairments":[]})";
    stage_text("PPODD.SCN", odd_scenario);
    const auto odd_admission = command("PREPAREPLAY:PPODDOUT.WAV:PPODD.WAV:PPODD.SCN:KEEP:48000:2048:31");
    const auto odd_start_query = exchange("24 PREPAREPLAY?\n");
    const auto odd_start_json = prepared_playback_members(odd_start_query.size() == 1U ? odd_start_query[0] : std::string{});
    const auto odd_start_timing = member_value(odd_start_json, "timing");
    const auto odd_start_timing_members = direct_json_object_members(odd_start_timing);
    bool timing_reset =
        odd_admission.ok && odd_start_query.size() == 1U && member_value(odd_start_timing_members, "cycle_hz") == "996000000" && member_value(odd_start_timing_members, "tick_hz") == "1000";

    for (const char* phase : wg::prepared_timing_phase_names)
    {
        timing_reset = timing_reset && timing_phase_is_empty(odd_start_timing, phase);
    }

    check(timing_reset, "a new admitted PREPAREPLAY resets every timing bucket before its first action");
    unsigned int odd_steps{};

    while (wg::preparation_busy && odd_steps < 1000U)
    {
        wg::service_preparation();
        host.run(1U);
        ++odd_steps;
    }

    host.run(usb_trailing_milliseconds);
    const auto odd_play = ua::play_statistics();
    const auto odd_it = files.find("2:/WG/PPODDOUT.WAV");
    const auto odd_rendered = odd_it == files.end() ? std::vector<std::int32_t>{} : prepared_wav_samples(odd_it->second);
    const auto odd_query = exchange("5 PREPAREPLAY?\n");
    const auto odd_json = prepared_playback_members(odd_query.size() == 1U ? odd_query[0] : std::string{});
    check(odd_admission.ok && odd_steps < 1000U && odd_it != files.end() && odd_it->second.size() == wav_header_bytes + 22U && std::ranges::equal(odd_rendered, odd_source) &&
              received_exactly(host.received(), odd_play.first_frame_index.value, odd_rendered),
          "a seven-frame prepared WAV preserves exact PCM identity and its RIFF odd-byte pad during playback");
    check(member_value(odd_json, "operation_id") == "2", "a second accepted PREPAREPLAY increments the boot-local operation ID exactly once");
    check(files.contains("2:/WG/PPROUT.JSN") && !files.contains("2:/WG/PPROUT.TMP") && !files.contains("2:/WG/PPROUT.JMP") && wg::player_allows_media_host(),
          "the WAV and identity manifest publish only after drain and release media ownership");
    const std::string odd_timing = member_value(odd_json, "timing");
    const auto last_prepared = wg::player_prepared_playback_snapshot();
    const auto ordinary_admission = exchange("5 PREPARE:PPORDOUT.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:37\n");
    unsigned int ordinary_steps{};

    while (wg::preparation_busy && ordinary_steps < 1000U)
    {
        wg::service_preparation();
        ++ordinary_steps;
    }

    const auto retained_query = exchange("6 PREPAREPLAY?\n");
    const auto retained_json = prepared_playback_members(retained_query.size() == 1U ? retained_query[0] : std::string{});
    check(ordinary_admission.size() == 1U && ordinary_admission[0].contains(R"("ok":true)") && ordinary_steps < 1000U && retained_query.size() == 1U &&
              member_value(retained_json, "operation_id") == "2" && member_value(retained_json, "state") == std::to_string(std::to_underlying(last_prepared.progress.state)) &&
              member_value(retained_json, "total_frames") == std::to_string(odd_source.size()) &&
              member_value(retained_json, "consumed_frames") == std::to_string(last_prepared.progress.consumed_frames) &&
              member_value(retained_json, "status") == std::to_string(std::to_underlying(last_prepared.status.code)),
          "an ordinary PREPARE leaves the last PREPAREPLAY? snapshot intact");
    check(member_value(retained_json, "timing") == odd_timing, "the last admitted PREPAREPLAY retains its timing counters across unrelated PREPARE");
    const auto saved_operation_id = wg::prepared_playback_operation_id;
    wg::prepared_playback_operation_id = UINT64_MAX;
    const unsigned int files_before_exhaustion = open_files;
    const bool mounted_before_exhaustion = mounted;
    const auto exhausted = command("PREPAREPLAY:PPMAXOUT.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:48000:2048:25");
    const auto exhausted_query = exchange("7 PREPAREPLAY?\n");
    const auto exhausted_json = prepared_playback_members(exhausted_query.size() == 1U ? exhausted_query[0] : std::string{});
    const bool exhausted_refused_without_claim = !exhausted.ok && open_files == files_before_exhaustion && mounted == mounted_before_exhaustion && !wg::preparation_busy &&
                                                 wg::player_allows_media_host() && !files.contains("2:/WG/PPMAXOUT.TMP") && !files.contains("2:/WG/PPMAXOUT.JMP") &&
                                                 !files.contains("2:/WG/PPMAXOUT.WAV") && !files.contains("2:/WG/PPMAXOUT.JSN") && member_value(exhausted_json, "operation_id") == "2" &&
                                                 member_value(exhausted_json, "state") == "4" && member_value(exhausted_json, "timing") == odd_timing;
    wg::prepared_playback_operation_id = saved_operation_id;
    check(exhausted_refused_without_claim, "UINT64_MAX refuses PREPAREPLAY before claiming files and preserves the prior terminal snapshot");
    const auto verify_admission = command("PREPAREPLAY:PPVERIFY.WAV:PPSRC.WAV:PPCASE.SCN:KEEP:48000:2048:26");
    bool verification_fault_armed = false;

    for (unsigned int step = 0U; step < 20000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);

        if (!verification_fault_armed && wg::player_prepare_snapshot().phase == wg::prepare::Phase::verifying)
        {
            card_faults().reads_before_failure = 0U;
            verification_fault_armed = true;
        }
    }

    card_faults().reads_before_failure.reset();
    const auto verification_query = exchange("25 PREPAREPLAY?\n");
    const auto verification_json = prepared_playback_members(verification_query.size() == 1U ? verification_query[0] : std::string{});
    const std::string verification_timing = member_value(verification_json, "timing");
    check(verify_admission.ok && verification_fault_armed && !wg::preparation_busy && wg::player_prepare_snapshot().phase == wg::prepare::Phase::failed && !files.contains("2:/WG/PPVERIFY.WAV") &&
              !files.contains("2:/WG/PPVERIFY.JSN"),
          "a prepared-WAV verification read fault fails before final artifact publication");
    check(timing_phase_has_measurements(verification_timing, "finalize") && timing_phase_has_measurements(verification_timing, "cleanup"),
          "a finalization error is timed as finalize, then subsequent cleanup steps use the cleanup bucket");
    files.erase("2:/WG/PPSRC.WAV");
    files.erase("2:/WG/PPCASE.SCN");
    files.erase("2:/WG/PPROUT.WAV");
    files.erase("2:/WG/PPROUT.JSN");
    files.erase("2:/WG/PPORDOUT.WAV");
    files.erase("2:/WG/PPORDOUT.JSN");
    files.erase("2:/WG/PPODD.WAV");
    files.erase("2:/WG/PPODD.SCN");
    files.erase("2:/WG/PPODDOUT.WAV");
    files.erase("2:/WG/PPODDOUT.JSN");
#endif
    host.close_stream();

    if (route_before == wg::OutputRoute::codec)
    {
        check(command("OUTPUT CODEC").ok, "PREPAREPLAY restores the original codec route");
    }
    else
    {
        host.open_stream();
        host.run(usb_idle_milliseconds);
        check(wg::player_output_route() == wg::OutputRoute::usb, "PREPAREPLAY restores the original USB route and open stream");
    }
}

void test_prepare_playback_refusals_and_stop()
{
    namespace wg = waveform_generator;
#if !defined(WFG_MSC_READ_ONLY)
    auto& host = usb_host();
    constexpr std::size_t source_frames = 120000U;
    const auto source = full_range_samples(source_frames);
    files["2:/WG/PPSTOP.WAV"] = pcm24_wav(source);
    constexpr std::string_view clean_scenario = R"({"seed":17,"reference_rms":0.1,"source_gain_db":0,"impairments":[]})";
    stage_text("PPSTOP.SCN", clean_scenario);
    const auto route_before = wg::player_output_route();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("OUTPUT USB").ok, "the PREPAREPLAY refusal fixture selects USB");
    check(command("CUT:0:1:zero").ok, "the refusal fixture queues an output cut");
    const auto cut_busy = command("PREPAREPLAY:PPNOCUT.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    check(!cut_busy.ok && !wg::preparation_busy && !files.contains("2:/WG/PPNOCUT.TMP"), "PREPAREPLAY refuses a queued CUT rather than silently ignoring it");
    check(command("CUT").ok, "the refusal fixture clears its queued CUT");
    check(wg::live_controller.reset(17U, 0.1).is_ok() && wg::live_controller.enqueue({.frame = 0U, .kind = signal_lab::ControlKind::cw_enable, .value = 1.0}).is_ok(),
          "the refusal fixture creates a captured live control");
    const auto capture_busy = command("PREPAREPLAY:PPNOCAP.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    check(!capture_busy.ok && !wg::preparation_busy && !files.contains("2:/WG/PPNOCAP.TMP"), "PREPAREPLAY refuses queued live controls rather than silently discarding their capture");
    check(wg::live_controller.reset(17U, 0.1).is_ok(), "the refusal fixture clears its live-control capture");
    host.close_stream();
    const unsigned int mounts_before = directory_faults().mount_calls;
    const auto closed_usb = command("PREPAREPLAY:PPNOUSB.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    check(!closed_usb.ok && !wg::preparation_busy && directory_faults().mount_calls == mounts_before && !files.contains("2:/WG/PPNOUSB.TMP") && !files.contains("2:/WG/PPNOUSB.WAV"),
          "a closed USB stream refuses PREPAREPLAY before opening output files or mounting");
    check(command("OUTPUT CODEC").ok, "the refusal fixture restores codec output before its other cases");

    if (route_before == wg::OutputRoute::usb)
    {
        host.open_stream();
        host.run(usb_idle_milliseconds);
        check(command("OUTPUT USB").ok, "the refusal fixture restores its initial USB route");
    }
    else
    {
        host.open_stream();
        host.run(usb_idle_milliseconds);
        check(command("OUTPUT USB").ok, "the stop fixtures open USB output for finite playback");
    }

    const auto begin_stream_close = command("PREPAREPLAY:PPCLOSE.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    bool streaming = false;

    for (unsigned int step = 0U; step < 10000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);
        streaming = wg::prepared_playback.snapshot().started && ua::play_statistics().file_frames.value != 0U;

        if (streaming)
        {
            break;
        }
    }

    host.close_stream();

    for (unsigned int step = 0U; step < 1000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
    }

    const auto stream_closed_player = wg::player_snapshot();
    const auto stream_closed_prepare = wg::player_prepare_snapshot();
    check(begin_stream_close.ok, "the stream-close fixture is admitted");
    check(streaming, "the stream-close fixture is actively sending prepared frames before close");
    check(!wg::preparation_busy && stream_closed_prepare.phase == wg::prepare::Phase::failed, "closing USB during PREPAREPLAY terminates preparation instead of waiting forever");
    const auto stream_closed_query = exchange("7 PREPAREPLAY?\n");
    const auto stream_closed_json = prepared_playback_members(stream_closed_query.size() == 1U ? stream_closed_query[0] : std::string{});
    check(stream_closed_query.size() == 1U && member_value(stream_closed_json, "state") == "5" && member_value(stream_closed_json, "active") == "false" &&
              member_value(stream_closed_json, "status") != "0" && member_value(stream_closed_json, "owner_released") == "true" &&
              timing_phase_has_measurements(member_value(stream_closed_json, "timing"), "cleanup"),
          "PREPAREPLAY? retains cleanup timing for a failed stream-close outcome after ownership releases");
    check(stream_closed_player.error == std::to_underlying(wg::PlayerError::usb_stream_closed) && !files.contains("2:/WG/PPCLOSE.WAV") && !files.contains("2:/WG/PPCLOSE.JSN"),
          "a mid-stream USB close is reported and never publishes a partial final artifact");
    host.open_stream();
    host.run(usb_idle_milliseconds);
    const auto begin_production = command("PREPAREPLAY:PPPROD.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    bool reached_production = false;

    for (unsigned int step = 0U; step < 1000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);
        const auto progress = wg::prepared_playback.snapshot();
        reached_production = reached_production || (wg::player_prepare_snapshot().phase == wg::prepare::Phase::rendering && !progress.started && progress.total_frames != 0U);

        if (reached_production)
        {
            break;
        }
    }

    card_faults().close_path = "2:/WG/PPPROD.TMP";
    card_faults().fail_close = true;
    const auto stop_production = reached_production ? command("STOP") : wg::ControlReply{};

    for (unsigned int step = 0U; step < 1000U && wg::preparation_busy && wg::player_prepare_snapshot().phase != wg::prepare::Phase::cleanup_failed; ++step)
    {
        wg::service_preparation();
        host.run(1U);
    }

    check(wg::preparation_busy && !wg::player_allows_media_host() && wg::player_prepare_snapshot().phase == wg::prepare::Phase::cleanup_failed,
          "a failed prepared-file close retains media ownership and a retryable cleanup state");
    card_faults().fail_close = false;
    wg::service_preparation();
    check(wg::preparation_busy && wg::player_prepare_snapshot().phase == wg::prepare::Phase::cleanup_failed, "clearing a close fault alone does not silently retry failed cleanup");
    check(command("STOP").ok, "an explicit STOP retries prepared-file cleanup");

    for (unsigned int step = 0U; step < 1000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);
    }

    check(begin_production.ok, "the producer-stop fixture is admitted");
    check(reached_production, "the producer-stop fixture reaches active rendering before output starts");
    check(stop_production.ok, "STOP acknowledges cancellation during production");
    check(!wg::preparation_busy, "STOP during production completes bounded cleanup");
    check(!files.contains("2:/WG/PPPROD.WAV") && !files.contains("2:/WG/PPPROD.JSN"), "STOP during production publishes neither final output name");
    const auto cancelled_query = exchange("8 PREPAREPLAY?\n");
    const auto cancelled_json = prepared_playback_members(cancelled_query.size() == 1U ? cancelled_query[0] : std::string{});
    check(cancelled_query.size() == 1U && member_value(cancelled_json, "state") == "6" && member_value(cancelled_json, "active") == "false" && member_value(cancelled_json, "status") != "0" &&
              member_value(cancelled_json, "owner_released") == "true",
          "PREPAREPLAY? retains the cancellation outcome after STOP cleanup");
    card_faults().close_path.clear();
    files.erase("2:/WG/PPPROD.TMP");
    files.erase("2:/WG/PPPROD.JMP");
    const auto begin_drain = command("PREPAREPLAY:PPDRAIN.WAV:PPSTOP.WAV:PPSTOP.SCN:KEEP:48000:2048:17");
    bool reached_drain = false;

    for (unsigned int step = 0U; step < 10000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);
        const auto progress = wg::prepared_playback.snapshot();
        reached_drain = progress.started && progress.total_frames == source_frames && progress.read_frames == progress.total_frames && progress.consumed_frames < progress.read_frames;

        if (reached_drain)
        {
            break;
        }
    }

    const auto stop_drain = reached_drain ? command("STOP") : wg::ControlReply{};

    for (unsigned int step = 0U; step < 1000U && wg::preparation_busy; ++step)
    {
        wg::service_preparation();
        host.run(1U);
    }

    check(begin_drain.ok, "the drain-stop fixture is admitted");
    check(reached_drain, "the drain-stop fixture reaches a queued, unread output tail");
    check(stop_drain.ok, "STOP acknowledges cancellation while the prepared FIFO drains");
    check(!wg::preparation_busy, "STOP during drain completes bounded cleanup");
    check(!files.contains("2:/WG/PPDRAIN.WAV") && !files.contains("2:/WG/PPDRAIN.JSN"), "STOP during drain publishes neither final output name");
    files.erase("2:/WG/PPDRAIN.TMP");
    files.erase("2:/WG/PPDRAIN.JMP");
    host.close_stream();
    check(command("OUTPUT CODEC").ok, "sample-slip failure runs over the codec without a USB stream");
    constexpr std::string_view slip_scenario = R"({"seed":17,"reference_rms":0.1,"source_gain_db":0,"impairments":[{"type":"sample_slip","kind":"delete","length_samples":1,"at_seconds":0}]})";
    stage_text("PPSLIP.SCN", slip_scenario);
    const auto begin_slip = command("PREPAREPLAY:PPSLIP.WAV:PPSTOP.WAV:PPSLIP.SCN:KEEP:48000:2048:17");
    unsigned int slip_steps{};

    while (wg::preparation_busy && slip_steps < 1000U)
    {
        wg::service_preparation();
        ++slip_steps;
    }

    const auto failed_slip = wg::player_prepare_snapshot();
    check(begin_slip.ok && slip_steps < 1000U && !wg::preparation_busy && failed_slip.phase == wg::prepare::Phase::failed && ua::play_statistics().file_frames.value == 0U &&
              !files.contains("2:/WG/PPSLIP.WAV") && !files.contains("2:/WG/PPSLIP.JSN"),
          "a sample-slip output-length mismatch fails asynchronously before any prepared frames reach output");
    const auto failed_query = exchange("9 PREPAREPLAY?\n");
    const auto failed_json = prepared_playback_members(failed_query.size() == 1U ? failed_query[0] : std::string{});
    check(failed_query.size() == 1U && member_value(failed_json, "state") == "0" && member_value(failed_json, "active") == "false" &&
              member_value(failed_json, "status") == std::to_string(std::to_underlying(failed_slip.status.code)) && member_value(failed_json, "owner_released") == "false" &&
              member_value(failed_json, "reason") == quoted(failed_slip.status.message),
          "PREPAREPLAY? retains an early failure even when the scheduler was never configured");
    files.erase("2:/WG/PPSLIP.TMP");
    files.erase("2:/WG/PPSLIP.JMP");
    files.erase("2:/WG/PPSLIP.SCN");

    if (route_before == wg::OutputRoute::usb)
    {
        host.open_stream();
        host.run(usb_idle_milliseconds);
        check(command("OUTPUT USB").ok, "PREPAREPLAY stop cases restore the original USB route");
    }

#endif
    files.erase("2:/WG/PPSTOP.WAV");
    files.erase("2:/WG/PPSTOP.SCN");
}

void test_prepare_status_reply_budget()
{
    namespace wg = waveform_generator;
    const auto saved = wg::preparation_snapshot;
    auto& snapshot = wg::preparation_snapshot;
    snapshot.phase = static_cast<wg::prepare::Phase>(UINT32_MAX);
    static std::array<char, wg::prepare_reason_max_chars + 1U> escaped_reason{};
    std::ranges::fill(std::span{escaped_reason}.first(wg::prepare_reason_max_chars), '\x01');
    snapshot.status = {.code = m110::StatusCode::internal_error, .message = escaped_reason.data()};
    const auto fill_name = [](auto& name, char value)
    {
        std::ranges::fill(std::span{name}.first(name.size() - 1U), value);
        name.back() = '\0';
    };

    fill_name(snapshot.request.source, 'A');
    fill_name(snapshot.request.scenario, 'B');
    fill_name(snapshot.request.output, 'C');
    snapshot.request.retention = wg::prepare::Retention::temporary;
    snapshot.seed = UINT64_MAX;
    snapshot.input_frames = UINT64_MAX;
    snapshot.output_frames = UINT64_MAX;
    snapshot.total_input_frames = UINT64_MAX;
    std::ranges::fill(std::span{snapshot.wav_sha256}.first(snapshot.wav_sha256.size() - 1U), 'a');
    std::ranges::fill(std::span{snapshot.pcm_sha256}.first(snapshot.pcm_sha256.size() - 1U), 'b');
    snapshot.wav_sha256.back() = '\0';
    snapshot.pcm_sha256.back() = '\0';
    const auto lines = exchange("1 PREPARE?\n");
    const auto reply = lines.size() == 1U ? lines[0] : std::string{};
    const auto members = json_status_members(reply);
    const auto unused = unused_width(members, prepare_json_layout());
    constexpr std::size_t newline_and_terminator = 2U;
    check_ok(unused.status(), "PREPARE? emits snapshot fields in documented order and within their widths");
    check(unused.has_value() && reply.size() + newline_and_terminator + unused.value() == wg::prepare_json_worst_case_bytes() && reply.size() + newline_and_terminator < wg::cdc_response_capacity,
          "the widest PREPARE? snapshot with escaped static error and uint64 counts fits the response buffer exactly");
    snapshot = saved;
}

void test_prepared_playback_status_reply_budget()
{
    namespace wg = waveform_generator;
    const auto saved = wg::prepared_playback_diagnostics;
    auto& snapshot = wg::prepared_playback_diagnostics;
    snapshot.progress.state = wg::finite_playback::State::cancelled;
    snapshot.progress.total_frames = UINT64_MAX;
    snapshot.progress.required_lead_frames = UINT64_MAX;
    snapshot.progress.produced_frames = UINT64_MAX;
    snapshot.progress.written_frames = UINT64_MAX;
    snapshot.progress.committed_frames = UINT64_MAX;
    snapshot.progress.read_frames = UINT64_MAX;
    snapshot.progress.consumed_frames = UINT64_MAX;
    snapshot.progress.started = true;
    snapshot.progress.artifact_finalized = true;
    snapshot.progress.action_pending = true;
    snapshot.progress.owner_released = true;
    snapshot.operation_id = UINT64_MAX;
    snapshot.timing.cycle_hz = UINT32_MAX;
    snapshot.timing.tick_hz = UINT32_MAX;

    for (auto& phase : snapshot.timing.phases)
    {
        phase.calls = UINT64_MAX;
        phase.total_cycles = UINT64_MAX;
        phase.max_cycles = UINT32_MAX;
        phase.rejected = UINT64_MAX;
        phase.saturated = false;
    }

    snapshot.measured_rate_frames_per_second = UINT32_MAX;
    snapshot.margin_frames = UINT32_MAX;
    snapshot.active = true;
    static std::array<char, wg::prepare_reason_max_chars + 1U> escaped_reason{};
    std::ranges::fill(std::span{escaped_reason}.first(wg::prepare_reason_max_chars), '\x01');
    snapshot.status = {.code = m110::StatusCode::internal_error, .message = escaped_reason.data()};
    const auto lines = exchange("1 PREPAREPLAY?\n");
    const auto reply = lines.size() == 1U ? lines[0] : std::string{};
    auto members = direct_json_object_members(reply);
    auto prepare_play = std::ranges::find(members, "prepare_play", &StatusMember::name);

    if (prepare_play != members.end())
    {
        prepare_play->value = "{";
        const auto prepared_fields = prepared_playback_members(reply);
        members.insert(std::next(prepare_play), prepared_fields.begin(), prepared_fields.end());
    }

    std::vector<wg::StatusJsonField> layout{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "prepare_play", .max_value_chars = wg::literal_length("{")},
    };
    std::ranges::copy(wg::prepared_playback_json_fields, std::back_inserter(layout));
    const auto unused = unused_width(members, layout);
    constexpr std::size_t newline_and_terminator = 2U;
    const bool widths_valid = unused.has_value() && reply.size() + newline_and_terminator + unused.value() == wg::prepared_playback_json_worst_case_bytes() &&
                              reply.size() + newline_and_terminator < wg::cdc_response_capacity && member_value(members, "operation_id") == std::to_string(UINT64_MAX);
    snapshot = saved;
    check_ok(unused.status(), "PREPAREPLAY? emits each watermarked field within its documented width");
    check(widths_valid, "the widest PREPAREPLAY? JSON with escaped reason and uint64 watermarks fits exactly");
}

// The result blocks the ring holds, oldest first.
[[nodiscard]] std::vector<pl::ResultBlock> pending_blocks()
{
    std::array<pl::ResultBlock, pl::result_capacity> blocks{};
    const auto count = waveform_generator::player_playlist().results.copy_oldest(blocks);
    std::vector<pl::ResultBlock> kept(blocks.begin(), blocks.end());
    kept.resize(count);
    return kept;
}

[[nodiscard]] pl::Progress playlist_progress()
{
    return waveform_generator::player_playlist_snapshot().progress;
}

// A USB item's frames as the host should receive them from its first frame
// index; an item whose scenario changed them is skipped (checked by its CRC).
struct ExpectedItem
{
    std::size_t first{};
    std::vector<std::int32_t> samples;
    bool exact{true};
};

// The host received exact silence everywhere but each item's frames.
[[nodiscard]] bool capture_is(std::span<const ua::StereoFrame> received, std::span<const ExpectedItem> items)
{
    for (std::size_t index = 0U; index < received.size(); ++index)
    {
        const auto covering = std::ranges::find_if(items, [index](const ExpectedItem& item) { return index >= item.first && index - item.first < item.samples.size(); });

        if (covering != items.end() && !covering->exact)
        {
            continue;
        }

        const std::int32_t expected = covering == items.end() ? 0 : covering->samples[index - covering->first] * ua::subslot_scale;

        if (received[index].left != expected || received[index].right != expected)
        {
            return false;
        }
    }

    return true;
}

[[nodiscard]] std::string text_or_null(std::string_view text)
{
    return text.empty() ? std::string{"null"} : quoted(text);
}

[[nodiscard]] std::string number_or_null(std::optional<std::uint32_t> value)
{
    return value.has_value() ? std::to_string(*value) : std::string{"null"};
}

// A result block's PLAYLIST? members as the contract orders them.
[[nodiscard]] std::vector<StatusMember> block_members(const pl::ResultBlock& block)
{
    const auto& play = block.play;
    const auto number = [](std::uint32_t value) { return std::to_string(value); };

    const auto real_number = [](double value)
    {
        std::array<char, 32U> buffer{};
        const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general);
        return formatted.ec == std::errc{} ? std::string{buffer.data(), formatted.ptr} : std::string{};
    };

    return {
        {.name = "seq", .value = number(block.seq)},
        {.name = "run", .value = number(block.run)},
        {.name = "item", .value = number(block.item)},
        {.name = "label", .value = text_or_null(block.label.view())},
        {.name = "file", .value = quoted(block.file.view())},
        {.name = "route", .value = quoted(pl::route_name(block.route))},
        {.name = "result", .value = quoted(pl::outcome_name(block.outcome))},
        {.name = "player_error", .value = number(block.player_error)},
        {.name = "file_frames_total", .value = number(play.file_frames_total)},
        {.name = "scn", .value = text_or_null(block.scenario.view())},
        {.name = "seed", .value = number_or_null(block.seed)},
        {.name = "scn_crc32", .value = number(block.scenario_crc32)},
        {.name = "engine_state", .value = number(play.engine_state)},
        {.name = "engine_error", .value = number(play.engine_error)},
        {.name = "uac_play_file_frames", .value = number(play.usb_file_frames)},
        {.name = "uac_play_sent_frames", .value = number(play.sent_frames)},
        {.name = "uac_play_crc32_l", .value = number(play.crc32_left)},
        {.name = "uac_play_crc32_r", .value = number(play.crc32_right)},
        {.name = "uac_play_first_frame_index", .value = number(play.first_frame_index)},
        {.name = "uac_play_last_frame_index", .value = number(play.last_frame_index)},
        {.name = "uac_play_first_frame_sof", .value = number(play.first_frame_sof)},
        {.name = "uac_play_missed_intervals", .value = number(play.missed_intervals)},
        {.name = "uac_play_first_miss_frame_index", .value = number(play.first_miss_frame_index)},
        {.name = "uac_play_stream_bits", .value = number(play.stream_bits)},
        {.name = "uac_play_stream_channels", .value = number(play.stream_channels)},
        {.name = "uac_play_cut_count", .value = number(play.cut_count)},
        {.name = "uac_play_cut_drop_frames", .value = number(play.cut_drop_frames)},
        {.name = "uac_play_cut_zero_frames", .value = number(play.cut_zero_frames)},
        {.name = "uac_play_cut_first_index", .value = number(play.cut_first_index)},
        {.name = "uac_play_cut_rejected", .value = number(play.cut_rejected)},
        {.name = "underruns", .value = number(play.underruns)},
        {.name = "pcm_drained", .value = number(play.pcm_drained)},
        {.name = "gap_frames", .value = number(block.gap.value)},
        {.name = "scheduled_index", .value = number_or_null(block.scheduled_index)},
        {.name = "start_late", .value = block.start_late() ? "1" : "0"},
        {.name = "start_tick", .value = number(block.start_tick)},
        {.name = "end_tick", .value = number(block.end_tick)},
        {.name = "att_tenths_db", .value = number(block.attenuation.value())},
        {.name = "dur_frames", .value = block.duration.has_value() ? number(block.duration->value) : std::string{"null"}},
        {.name = "loops", .value = number(block.loops)},
        {.name = "end_cause", .value = quoted(pl::end_cause_name(block.end_cause))},
        {.name = "engine_deadline_misses", .value = number(play.deadline_misses)},
        {.name = "watterson_version", .value = number(play.watterson.version)},
        {.name = "watterson_digest_hi", .value = number(play.watterson.digest_hi)},
        {.name = "watterson_digest_lo", .value = number(play.watterson.digest_lo)},
        {.name = "watterson_latency_frames", .value = number(play.watterson.latency_frames)},
        {.name = "watterson_paths", .value = number(play.watterson.paths)},
        {.name = "second_source_version", .value = number(play.secondary_source.version)},
        {.name = "second_source_source_frames", .value = number(play.secondary_source.source_frames)},
        {.name = "second_source_excerpt_start_frame", .value = number(play.secondary_source.excerpt_start_frame)},
        {.name = "second_source_excerpt_frames", .value = number(play.secondary_source.excerpt_frames)},
        {.name = "second_source_digest_hi", .value = number(play.secondary_source.digest_hi)},
        {.name = "second_source_digest_lo", .value = number(play.secondary_source.digest_lo)},
        {.name = "second_source_latency_frames", .value = number(play.secondary_source.latency_frames)},
        {.name = "second_source_reference_rms", .value = real_number(play.secondary_source.reference_rms)},
        {.name = "engine_digest_hi", .value = number(play.engine_digest_hi)},
        {.name = "engine_digest_lo", .value = number(play.engine_digest_lo)},
    };
}

constexpr std::string_view blocks_key{R"(,"blocks":[)"};
constexpr std::string_view tick_key{R"(,"tick":)"};

// A PLAYLIST? reply's tick, the harness clock when the reply was read, or
// none when the reply has no tick member.
[[nodiscard]] std::optional<std::uint32_t> reply_tick(std::string_view reply)
{
    const auto at = reply.find(tick_key);

    if (at == std::string_view::npos)
    {
        return std::nullopt;
    }

    const auto digits = reply.substr(at + tick_key.size());
    std::uint32_t value{};
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), value);
    return parsed.ec == std::errc{} ? std::optional<std::uint32_t>{value} : std::nullopt;
}

// A PLAYLIST? reply without its tick member, which the harness clock sets:
// the rest of the reply is fixed by the playlist alone.
[[nodiscard]] std::string without_tick(std::string_view reply)
{
    std::string rest{reply};
    const auto at = rest.find(tick_key);

    if (at != std::string::npos)
    {
        const auto end = rest.find_first_not_of("0123456789", at + tick_key.size());
        rest.erase(at, (end == std::string::npos ? rest.size() : end) - at);
    }

    return rest;
}

// A PLAYLIST? reply's members before its blocks: the begin_json() prefix and
// the progress.
[[nodiscard]] std::vector<StatusMember> reply_progress(std::string_view reply)
{
    return json_status_members(reply.substr(0U, reply.find(blocks_key)));
}

// A PLAYLIST? reply's blocks, each as its members; block values never hold a
// brace.
[[nodiscard]] std::vector<std::vector<StatusMember>> reply_blocks(std::string_view reply)
{
    std::vector<std::vector<StatusMember>> blocks;
    const auto open = reply.find(blocks_key);

    if (open == std::string_view::npos)
    {
        return blocks;
    }

    auto rest = reply.substr(open + blocks_key.size());

    while (rest.starts_with("{"))
    {
        const auto end = rest.find('}');
        blocks.push_back(json_status_members(rest.substr(0U, end + 1U)));
        rest.remove_prefix(std::min(rest.size(), end + 1U));

        if (rest.starts_with(","))
        {
            rest.remove_prefix(1U);
        }
    }

    return blocks;
}

[[nodiscard]] bool same_members(std::span<const StatusMember> members, std::span<const StatusMember> expected)
{
    return std::ranges::equal(members, expected, [](const StatusMember& member, const StatusMember& wanted) { return member.name == wanted.name && member.value == wanted.value; });
}

// A PLAYLIST file refused, and what the reply says.
struct PlaylistRefusalCase
{
    std::string_view name;
    std::string_view text;
    std::string_view message;
};

// The current or last play's SD read counters, as STATUS reports them: calls,
// bytes, short reads, errors and the longest read's cycles.
constexpr std::size_t sd_read_counter_count = 5U;

[[nodiscard]] std::array<std::uint32_t, sd_read_counter_count> sd_read_counters()
{
    const auto snapshot = waveform_generator::player_snapshot();
    return {snapshot.sd_read_calls, snapshot.sd_bytes_read, snapshot.sd_short_reads, snapshot.sd_read_errors, snapshot.max_sd_read_cycles};
}

// STATUS sd_bytes_read holds at 2^32 - 1 rather than wrap: a looped item
// reads its card pass after pass, past 4 GiB after about 8.28 h.
void test_sd_bytes_read_holds_at_its_maximum()
{
    namespace wg = waveform_generator;
    constexpr auto most = std::numeric_limits<std::uint32_t>::max();
    constexpr UINT block = wg::read_buffer_bytes;
    const auto saved = wg::counters;
    wg::counters.sd_bytes_read = most - block + 1U;
    wg::record_read(FR_OK, block, block, 0U);
    const auto held = sd_read_counters();
    wg::record_read(FR_OK, block, block, 0U);
    const auto still = sd_read_counters();
    wg::counters = saved;
    check(held[1] == most && still[1] == most && still[0] == held[0] + 1U, "sd_bytes_read holds at 2^32 - 1 once the reads pass it, and the calls count on");
}

// Every refusal names its code and line, and what the card check found; a
// refused start leaves the player, the card, the playlist state and the last
// play's STATUS counters (its SD reads too) as they were. As PLAY: the card
// Local, no run pending.
void test_playlist_refusals()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    const auto good = stage_usb_file("GOOD.WAV", usb_file_frames);
    auto riff = good.wav;
    riff[3] = 'X';
    files["2:/WG/RIFX.WAV"] = riff;
    stage_text("NOISE.SCN", noise_scenario);
    stage_text("BAD.SCN", "{bad scenario}");
    stage_text("NOREF.SCN", R"({"schema":"signal-lab.scenario/1","test_id":"noref","seed":1,"reference":"auto","source_gain_db":-12,)"
                            R"("clip_policy":"saturate","impairments":[{"type":"awgn","snr_db":20}]})");
    const auto cases = std::to_array<PlaylistRefusalCase>({
        {.name = "NOPE.PL", .text = {}, .message = "PLAYLIST_OPEN"},
        {.name = "SYNTAX.PL", .text = "GOOD.WAV 5 USB\n# then\nGOOD.WAV 5s USB\n", .message = "PLAYLIST_GAP line 3"},
        {.name = "MISSING.PL", .text = "GOOD.WAV 0 USB\n\nNONE.WAV 0 USB\n", .message = "PLAYLIST_WAV_MISSING line 3"},
        {.name = "RIFX.PL", .text = "GOOD.WAV 0 CODEC\nRIFX.WAV 0 CODEC\n", .message = "PLAYLIST_WAV_INVALID line 2: invalid_riff"},
        {.name = "NOSCN.PL", .text = "GOOD.WAV 0 USB scn=NONE.SCN\n", .message = "PLAYLIST_SCN_MISSING line 1"},
        {.name = "BADSCN.PL", .text = "GOOD.WAV 0 USB scn=NOISE.SCN\nGOOD.WAV 0 USB scn=BAD.SCN\n", .message = "PLAYLIST_SCN_INVALID line 2: scenario_parse"},
        {.name = "NOREF.PL", .text = "GOOD.WAV 0 USB scn=NOREF.SCN seed=3\n", .message = "PLAYLIST_SCN_INVALID line 1: missing_reference_rms"},
        {.name = "EMPTY.PL", .text = "# nothing to play\n", .message = "PLAYLIST_EMPTY"},
        {.name = "LABELS.PL", .text = "GOOD.WAV 0 USB label=a\nGOOD.WAV 0 USB label=a\n", .message = "PLAYLIST_LABEL_DUPLICATE line 2"},
    });

    for (const auto& refusal : cases)
    {
        const std::string name{refusal.name};

        if (!refusal.text.empty() || name != "NOPE.PL")
        {
            stage_text(name, refusal.text);
        }

        const auto state_before = wg::player_state.load();
        const auto reads_before = sd_read_counters();
        const auto lines = exchange("1 PLAYLIST:" + name + "\n");
        const auto code = refusal.message.substr(0U, refusal.message.find(' '));
        check(lines.size() == 1U && lines[0].contains(R"("error":{"code":")" + std::string{code} + R"(","message":")" + std::string{refusal.message} + R"("}})"),
              name + " is refused with " + std::string{refusal.message});
        check(!mounted && open_files == 0U && wg::player_state.load() == state_before && playlist_progress().state == pl::State::idle && !wg::playlist_owns_player() &&
                  wg::player_allows_media_host() && sd_read_counters() == reads_before,
              name + ": the refusal leaves the player, the card, the playlist and the last play's SD read counters as they were");
    }

    stage_text("ONE.PL", "GOOD.WAV 0 USB\n");
    media_local = false;
    const auto not_local = command("PLAYLIST:ONE.PL");
    media_local = true;
    check(!not_local.ok && std::string_view{not_local.error} == "BAD_STATE", "PLAYLIST needs the card Local, as PLAY does");
    check(command("LOAD:GOOD.WAV").ok && command("PLAY").ok, "a PLAY is pending");
    const auto busy = command("PLAYLIST:ONE.PL");
    check(!busy.ok && std::string_view{busy.error} == "BUSY", "PLAYLIST while a run is pending is BUSY");
    wg::play_requested = false;
    wait_calls = 0U;
    wg::run_player();
    check(command("PLAYLIST:ONE.PL").ok && playlist_progress().state == pl::State::running && playlist_progress().items == 1U, "once the player has stopped a PLAYLIST is accepted");
    wg::reset_playlist();
}

// A primary FIL close error during either structural-preflight phase must
// keep the card owned until STOP successfully retries the close.
void test_playlist_primary_close_failures()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    static_cast<void>(stage_usb_file("PRIM.WAV", 2048U));
    stage_text("PFILE.PL", "PRIM.WAV 0 CODEC\n");
    card_faults() = {};
    card_faults().close_path = "2:/WG/PFILE.PL";
    card_faults().fail_close = true;
    const auto playlist_close = command("PLAYLIST:PFILE.PL");
    check(!playlist_close.ok && playlist_close.error != nullptr && std::string_view{playlist_close.error} == "PLAYER_CLOSE" && card_faults().close_calls == 1U && open_files == 1U && mounted &&
              !wg::player_allows_media_host() && !wg::playlist_owns_player() && pending_blocks().empty(),
          "a playlist-file close failure refuses before playback and keeps the primary FIL and card owned");
    const auto playlist_retry = command("STOP");
    check(!playlist_retry.ok && playlist_retry.error != nullptr && std::string_view{playlist_retry.error} == "PLAYER_CLOSE" && card_faults().close_calls == 2U && open_files == 1U && mounted &&
              !wg::player_allows_media_host(),
          "a failed STOP close retry keeps playlist preflight media ownership held");
    card_faults().fail_close = false;
    const auto playlist_recovered = command("STOP");
    check(playlist_recovered.ok && card_faults().close_calls == 3U && open_files == 0U && !mounted && wg::player_allows_media_host(),
          "STOP releases the playlist-file handle and card only after the close retry succeeds");
    reset_playlist_state();
    stage_text("PWAV.PL", "PRIM.WAV 0 CODEC\n");
    card_faults() = {};
    card_faults().close_path = "2:/WG/PRIM.WAV";
    card_faults().fail_close = true;
    const auto wav_close = command("PLAYLIST:PWAV.PL");
    check(!wav_close.ok && wav_close.error != nullptr && std::string_view{wav_close.error} == "PLAYER_CLOSE" && card_faults().close_calls == 1U && open_files == 1U && mounted &&
              !wg::player_allows_media_host() && !wg::playlist_owns_player() && pending_blocks().empty(),
          "an item-WAV close failure refuses preflight and retains the primary FIL and card ownership");
    card_faults().fail_close = false;
    const auto wav_recovered = command("STOP");
    check(wav_recovered.ok && card_faults().close_calls == 2U && open_files == 0U && !mounted && wg::player_allows_media_host(),
          "STOP retries and releases the item-WAV handle before returning media access");
    card_faults() = {};
    files.erase("2:/WG/PFILE.PL");
    files.erase("2:/WG/PWAV.PL");
    files.erase("2:/WG/PRIM.WAV");
}

// Drain every page before acknowledging it: no result may be skipped when
// the per-reply capacity changes. A future sequence cannot be acknowledged.
void check_playlist_drain(std::span<const pl::ResultBlock> blocks)
{
    namespace wg = waveform_generator;
    check(blocks.size() == battery_items, "six battery results to drain");

    for (std::size_t offset = 0U; offset < blocks.size(); offset += pl::blocks_per_reply)
    {
        const auto lines = exchange("1 PLAYLIST?\n");
        check(lines.size() == 1U, "one playlist reply per query");

        if (lines.empty())
        {
            return;
        }

        const auto page = reply_blocks(lines[0]);
        const auto count = std::min(pl::blocks_per_reply, blocks.size() - offset);
        check(page.size() == count && reply_tick(lines[0]).has_value(), "the next bounded page and snapshot tick");

        for (std::size_t index = 0U; index < std::min(page.size(), count); ++index)
        {
            check(same_members(page[index], block_members(blocks[offset + index])), "every retained result is delivered in order");
        }

        const auto ack = command(("PLAYLIST ACK:" + std::to_string(blocks[offset + count - 1U].seq)).c_str());
        check(ack.ok && ack.events == count, "acknowledge exactly the page just read");
    }

    const auto lines = exchange("1 PLAYLIST?\n2 PLAYLIST ACK:7\n");
    check(lines.size() == 2U && without_tick(lines[0]).contains(R"("pending":0,"oldest_seq":0,"next_seq":7,"blocks":[]}})"), "all six results were delivered and the ring is empty");
    check(lines.size() == 2U && lines[1].contains(R"("error":{"code":"PLAYLIST_ACK_RANGE",)") && wg::player_playlist().results.next_seq() == battery_items + 1U,
          "a future sequence is refused and numbers are not reused");
}

// The lane's acceptance battery: six items on the USB route with 5 s gaps,
// one with a CUT schedule and one with a scenario and seed, played unattended
// while the host records. Each item is exact where its block says; each USB
// item starts exactly next_start_index() frames into the stream (the packet
// that ends the item before it, then its gap); every block holds the item's
// per-play statistics, the last equal to what STATUS? and USB? report; and
// the stream carries exact silence between the items and for the last gap.
void test_playlist_battery()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    struct Card
    {
        std::string name;
        std::size_t frames;
        std::string options;
    };

    constexpr std::string_view fsrand_cuts{"CUT:1000:480:zero,3000:480:drop"};
    constexpr std::size_t fsrand = 3U;
    constexpr std::size_t noisy = 4U;
    constexpr std::uint32_t noisy_seed = 7U;
    const std::vector<Card> cards{
        {.name = "C24L05.WAV", .frames = 5002U, .options = "label=C24L05"},
        {.name = "C12L05.WAV", .frames = 4800U, .options = "label=C12L05"},
        {.name = "C06L05.WAV", .frames = 3002U, .options = "label=C06L05"},
        {.name = "FSRAND.WAV", .frames = 6000U, .options = "label=FSRAND cut=1000:480:zero,3000:480:drop"},
        {.name = "C06L05N.WAV", .frames = 2400U, .options = "label=C06L05N scn=NOISE.SCN seed=7"},
        {.name = "C24L10.WAV", .frames = 2000U, .options = "label=C24L10"},
    };
    std::string text{"# the lane's acceptance battery: 5 s gaps on the USB route\n"};
    std::vector<UsbFile> staged;

    for (const auto& card : cards)
    {
        staged.push_back(stage_usb_file(card.name, card.frames));
        text += card.name + " 5 USB " + card.options + "\n";
    }

    stage_text("NOISE.SCN", noise_scenario);
    stage_text("BATTERY.PL", text);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    const auto accepted = command("PLAYLIST:BATTERY.PL");
    check(accepted.ok && accepted.events == cards.size() && !mounted && open_files == 0U, "PLAYLIST checks the six items, answers with them and releases the card");
    run_accepted_playlist();
    const auto progress = playlist_progress();
    check(progress.state == pl::State::done && progress.items == cards.size() && progress.completed == cards.size() && progress.failed == 0U && progress.index == cards.size() &&
              progress.name.view() == "BATTERY.PL" && progress.run == 1U && progress.policy == pl::Policy::stop,
          "the battery is done: six items completed, none failed");
    const auto blocks = pending_blocks();
    check(blocks.size() == cards.size(), "every item's block is kept until the host reads it");

    if (blocks.size() != cards.size())
    {
        return;
    }

    std::vector<ExpectedItem> expected;

    for (std::size_t index = 0U; index < cards.size(); ++index)
    {
        const auto& block = blocks[index];
        const auto& file = staged[index];
        const auto& card = cards[index];
        const auto stream = index == fsrand ? expected_stream(file.samples, fsrand_cuts) : ExpectedStream{.samples = file.samples, .cuts = {}, .rejected = 0U};
        const auto sent = count_of(stream.samples.size());
        const auto& play = block.play;
        check(block.seq == index + 1U && block.item == index && block.run == 1U && block.outcome == pl::Outcome::done && block.player_error == 0U && block.route == pl::Route::usb &&
                  block.file.view() == card.name && block.gap.value == five_second_gap_frames && block.label.view() == card.name.substr(0U, card.name.find('.')),
              card.name + ": its block names the item and says it is done");
        check(play.sent_frames == sent && play.usb_file_frames == file.samples.size() && play.file_frames_total == file.samples.size() && play.last_frame_index == play.first_frame_index + sent - 1U &&
                  play.missed_intervals == 0U && play.underruns == 0U && play.pcm_drained == 1U && play.stream_bits == wfg_uac_pcm24_resolution_bits && play.stream_channels == wfg_uac_channels &&
                  block.end_tick > block.start_tick,
              card.name + ": every frame confirmed sent, none missed, in 24-bit stereo");
        check(index == noisy || (play.crc32_left == pcm24_crc(stream.samples) && play.crc32_right == play.crc32_left && play.engine_state == 0U),
              card.name + ": both CRCs are those of the card (after its cuts)");

        if (index == 0U)
        {
            check(!block.scheduled_index.has_value() && !block.start_late(), "the first item starts when it is ready");
        }
        else
        {
            const auto& before = blocks[index - 1U].play;
            const auto start = pl::next_start_index(before.first_frame_index, before.sent_frames, blocks[index - 1U].gap);
            check(block.scheduled_index == start && play.first_frame_index == start && !block.start_late(),
                  card.name + ": starts exactly the packet after the item before it, then its 5 s gap of 240000 frames, into the stream");
        }

        expected.push_back({.first = play.first_frame_index, .samples = stream.samples, .exact = index != noisy});
    }

    const auto& cut_play = blocks[fsrand].play;
    check(cut_play.cut_count == 2U && cut_play.cut_drop_frames == fsrand_cut_frames && cut_play.cut_zero_frames == fsrand_cut_frames && cut_play.cut_rejected == 0U &&
              cut_play.cut_first_index == cut_play.first_frame_index + fsrand_zero_at,
          "FSRAND plays its own CUT schedule");
    const auto& noise = blocks[noisy];
    check(noise.play.engine_state == 1U && noise.scenario.view() == "NOISE.SCN" && noise.seed == noisy_seed && noise.scenario_crc32 == text_crc(noise_scenario) &&
              noise.play.crc32_left != pcm24_crc(staged[noisy].samples),
          "C06L05N plays its scenario with its seed; the block names both and the scenario's CRC-32");
    check(blocks[0].scenario.empty() && !blocks[0].seed.has_value() && blocks[0].scenario_crc32 == 0U && blocks[0].play.cut_count == 0U, "an item without scn=, seed= or cut= plays clean");
    check(capture_is(host.received(), expected), "the host recorded exact silence, each item exactly where its block says, and silence between");
    const auto& last = blocks.back();
    const auto end = pl::next_start_index(last.play.first_frame_index, last.play.sent_frames, last.gap);
    const auto stream_frames = ua::stream_counters().frames_sent.value;
    check(stream_frames >= end && stream_frames - end <= 2U * ua::chunk_frame_count, "the playlist is done once its last gap has gone out");
    const auto lines = exchange("1 USB?\n2 STATUS?\n");
    const auto usb = usb_members(lines);
    const auto status = json_status_members(lines.size() < 2U ? std::string{} : lines[1]);
    const auto block = block_members(last);
    const auto reported = [&usb, &status](const StatusMember& member)
    {
        const auto in_usb = member_value(usb, member.name);
        return (in_usb.empty() ? member_value(status, member.name) : in_usb) == member.value;
    };

    // The block's members STATUS or USB? also report: the player's error,
    // the card's frames, the engine's state, the codec counters, every
    // uac_play_* statistic the block keeps and the engine deadline misses.
    constexpr std::size_t reported_members = 25U;
    std::vector<StatusMember> per_play;
    std::ranges::copy_if(block, std::back_inserter(per_play),
                         [](const StatusMember& member) { return std::ranges::contains(wg::status_field_names, member.name) || std::ranges::contains(wg::usb_field_names, member.name); });

    check(lines.size() == 2U && per_play.size() == reported_members && std::ranges::all_of(per_play, reported), "the last block holds what STATUS? and USB? report for the current or last play");
    check_playlist_drain(blocks);
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// STOP ends the item it reaches and the playlist: in an item (the item is
// aborted, its block the exact prefix sent), in a gap before a codec item
// (no further item runs), and while a USB item waits for its stream index
// (that item is aborted with nothing sent).
void test_playlist_stop()
{
    namespace wg = waveform_generator;
    auto& host = usb_host();
    const auto long_file = stage_usb_file("S1LONG.WAV", usb_long_file_frames);
    const auto short_file = stage_usb_file("S2.WAV", usb_file_frames);
    stage_text("STOPA.PL", "S1LONG.WAV 1 USB\nS2.WAV 0 USB\n");
    stage_text("STOPB.PL", "S2.WAV 2 USB\nS2.WAV 0 CODEC\n");
    stage_text("STOPC.PL", "S2.WAV 2 USB\nS2.WAV 0 USB\n");
    reset_playlist_state();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({.send_stop_at = usb_interrupt_at_milliseconds});
    check(command("PLAYLIST:STOPA.PL").ok, "a playlist whose first item is long");
    run_accepted_playlist();
    auto blocks = pending_blocks();
    check(!hook_replies().empty() && hook_replies().back().ok, "STOP in an item is acknowledged");
    check(playlist_progress().state == pl::State::aborted && playlist_progress().index == 0U && blocks.size() == 1U && blocks[0].outcome == pl::Outcome::aborted && blocks[0].play.sent_frames != 0U &&
              blocks[0].play.sent_frames < long_file.samples.size() && blocks[0].play.crc32_left == pcm24_crc(std::span{long_file.samples}.first(blocks[0].play.sent_frames)),
          "STOP in an item aborts it and the playlist; its block is the exact prefix sent");
    reset_playlist_state();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    bool stopped = false;
    host.on_tick(
        [&stopped]
        {
            if (!stopped && playlist_progress().state == pl::State::gap && wg::player_playlist().results.size() == 1U)
            {
                stopped = true;
                send_from_hook("STOP");
            }
        });
    check(command("PLAYLIST:STOPB.PL").ok, "a USB item, then a codec item");
    run_accepted_playlist();
    blocks = pending_blocks();
    check(stopped && playlist_progress().state == pl::State::aborted && blocks.size() == 1U && blocks[0].outcome == pl::Outcome::done && !hook_replies().empty() && hook_replies().back().ok,
          "STOP in a gap ends the playlist before its next item");
    reset_playlist_state();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    stopped = false;
    host.on_tick(
        [&stopped]
        {
            if (!stopped && wg::player_playlist().results.size() == 1U && ua::play_statistics().armed && ua::play_statistics().file_frames == ua::FrameCount{})
            {
                stopped = true;
                send_from_hook("STOP");
            }
        });
    check(command("PLAYLIST:STOPC.PL").ok, "two USB items");
    run_accepted_playlist();
    blocks = pending_blocks();
    check(stopped && playlist_progress().state == pl::State::aborted && blocks.size() == 2U && blocks[1].outcome == pl::Outcome::aborted && blocks[1].play.sent_frames == 0U &&
              blocks[1].scheduled_index.has_value() && !blocks[1].start_late(),
          "STOP while a USB item waits for its stream index aborts it with nothing sent, and it did not start late");
    const std::vector<ExpectedItem> first_only{{.first = blocks.empty() ? 0U : blocks[0].play.first_frame_index, .samples = short_file.samples, .exact = true}};
    check(capture_is(host.received(), first_only), "the capture holds the first item and silence");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// An item error: under the stop policy the playlist ends in error after its
// block; under CONTINUE the next item runs and the playlist ends done with
// one failure. The error here is a USB item that finds the capture stream
// closed at its start: it plays nothing and its block says usb_no_stream (21).
void test_playlist_error_policy()
{
    namespace wg = waveform_generator;
    auto& host = usb_host();
    const auto first = stage_usb_file("E1.WAV", usb_file_frames);
    static_cast<void>(stage_usb_file("E2.WAV", 3U * ua::chunk_frame_count));
    static_cast<void>(stage_usb_file("E3.WAV", codec_file_frames));
    const std::string items{"E1.WAV 0.5 USB label=e1\nE2.WAV 0 USB label=e2 cut=10:5:zero\nE3.WAV 0 CODEC label=e3\n"};
    stage_text("ERRORS.PL", items);

    for (const bool continuing : {false, true})
    {
        const std::string name = continuing ? "CONTINUE" : "the stop policy";
        reset_playlist_state();
        host.open_stream();
        host.run(usb_idle_milliseconds);
        // The host closes the stream right after the first item's last frame.
        host.schedule({.close_after_file_frames = first.samples.size()});
        check(command(continuing ? "PLAYLIST:ERRORS.PL:CONTINUE" : "PLAYLIST:ERRORS.PL").ok, name + ": the playlist is accepted");
        run_accepted_playlist();
        const auto blocks = pending_blocks();
        const auto progress = playlist_progress();
        check(blocks.size() >= 2U && blocks[0].outcome == pl::Outcome::done && blocks[1].outcome == pl::Outcome::error &&
                  blocks[1].player_error == std::to_underlying(wg::PlayerError::usb_no_stream) && blocks[1].play == pl::PlayRecord{} && !blocks[1].scheduled_index.has_value(),
              name + ": the first item is done; the second finds no stream and plays nothing");
        check(wg::player_cuts().pending.empty(), name + ": the item that played nothing leaves no CUT list pending");

        if (!continuing)
        {
            // The item that found no stream selected nothing: the first item's
            // file stays selected, its run's end state stays, and a PLAY needs
            // LOAD as after every other item.
            const auto play = command("PLAY");
            const auto live = wg::player_live_snapshot();
            check(std::string_view{std::data(live.selected_file)} == "E1.WAV" && wg::player_state.load() == wg::PlayerState::done && !play.ok && std::string_view{play.error} == "BAD_STATE",
                  name + ": the item without a stream leaves the run before it as it ended; PLAY without LOAD is BAD_STATE");
        }

        if (continuing)
        {
            check(progress.state == pl::State::done && progress.completed == 3U && progress.failed == 1U && blocks.size() == 3U && blocks[2].outcome == pl::Outcome::done &&
                      blocks[2].route == pl::Route::codec && progress.policy == pl::Policy::continue_on_error,
                  name + ": the next item runs, and the playlist ends done with one failure");
        }
        else
        {
            check(progress.state == pl::State::error && progress.index == 1U && progress.completed == 2U && progress.failed == 1U && blocks.size() == 2U,
                  name + ": the playlist ends in error after the failed item's block");
        }
    }

    host.schedule({});
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A full ring pauses the playlist before its next item: no block is dropped.
// PLAYLIST? shows paused_ring_full; PLAYLIST ACK frees blocks and the
// playlist goes on.
void test_playlist_ring_full()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    constexpr std::size_t items = pl::result_capacity + 2U;
    constexpr std::uint32_t freed = 10U;
    static_cast<void>(stage_usb_file("TINY.WAV", ua::chunk_frame_count));
    std::string text;

    for (std::size_t index = 0U; index < items; ++index)
    {
        text += "TINY.WAV 0 CODEC label=t" + std::to_string(index) + "\n";
    }

    stage_text("RING.PL", text);
    bool paused = false;
    std::string pause_reply;
    usb_host().on_tick(
        [&paused, &pause_reply]
        {
            const auto progress = playlist_progress();

            if (!paused && progress.state == pl::State::paused_ring_full)
            {
                paused = true;
                const auto lines = exchange("1 PLAYLIST?\n");
                pause_reply = lines.empty() ? std::string{} : lines.front();
                send_from_hook("PLAYLIST ACK:" + std::to_string(freed));
            }
        });
    check(command("PLAYLIST:RING.PL").ok, "a playlist of 34 items");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    const auto progress = playlist_progress();
    check(paused && pause_reply.contains(R"("state":"paused_ring_full","name":"RING.PL","run":1,"policy":"stop","items":34,"index":32,"completed":32,)") &&
              pause_reply.contains(R"("pending":32,"oldest_seq":1,"next_seq":33,"tick":)"),
          "the 33rd item waits for room: PLAYLIST? shows the pause and the full ring");
    check(!hook_replies().empty() && hook_replies().front().ok && hook_replies().front().events == freed, "PLAYLIST ACK during the pause frees ten blocks");
    check(progress.state == pl::State::done && progress.completed == items && blocks.size() == pl::result_capacity - freed + 2U && blocks.front().seq == freed + 1U && blocks.back().seq == items &&
              blocks.back().item == items - 1U,
          "the playlist goes on once there is room, and every item's block is kept in order");
}

// Items switch routes between them: USB, codec, USB. The codec item's gap
// passes on the tick clock and the USB item after it starts when it is
// ready; a codec item's block has no USB play and counts the frames the codec
// took. The route stays where the last item left it.
void test_playlist_route_switching()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    constexpr std::uint32_t codec_gap_ms = 300U;
    const auto one = stage_usb_file("RT1.WAV", usb_file_frames);
    const auto two = stage_usb_file("RT2.WAV", codec_file_frames);
    const auto three = stage_usb_file("RT3.WAV", 2U * ua::chunk_frame_count + 8U);
    stage_text("ROUTES.PL", "RT1.WAV 0.2 USB\nRT2.WAV 0.3 CODEC\nRT3.WAV 0.1 USB\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    auto& capture = codec_capture();
    capture = CodecCapture{.recording = true, .samples = {}};
    check(command("PLAYLIST:ROUTES.PL").ok, "USB, codec, USB");
    run_accepted_playlist();
    capture.recording = false;
    const auto blocks = pending_blocks();
    check(blocks.size() == 3U && blocks[0].route == pl::Route::usb && blocks[1].route == pl::Route::codec && blocks[2].route == pl::Route::usb && wg::player_output_route() == wg::OutputRoute::usb,
          "each item plays on its own route, and the route stays the last item's");

    if (blocks.size() != 3U)
    {
        return;
    }

    const auto& codec = blocks[1];
    check(codec.outcome == pl::Outcome::done && codec.play.sent_frames == two.samples.size() && codec.play.usb_file_frames == 0U && codec.play.crc32_left == 0U && codec.play.first_frame_index == 0U &&
              codec.play.pcm_drained == 1U && !codec.scheduled_index.has_value(),
          "the codec item drains every frame and has no USB play");
    check(std::ranges::search(capture.samples, two.samples).size() == two.samples.size(), "the codec played its card");
    check(blocks[2].start_tick >= codec.end_tick + codec_gap_ms && !blocks[2].scheduled_index.has_value(), "after a codec item the gap passes on the tick clock, then the USB item starts when ready");
    const std::vector<ExpectedItem> expected{{.first = blocks[0].play.first_frame_index, .samples = one.samples, .exact = true},
                                             {.first = blocks[2].play.first_frame_index, .samples = three.samples, .exact = true}};
    check(capture_is(host.received(), expected), "the stream carries the two USB items and silence");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// Each item plays exactly its own cut=, scn= and seed=. A CUT list set before
// PLAYLIST does not reach an item; the same scenario and seed give the same
// samples and another seed others; a PLAY after the playlist has no cuts and
// no seed of an item's.
void test_watterson_playlist()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    const auto card = stage_usb_file("WAT.WAV", 8000U);
    stage_text("WAT.SCN", watterson_scenario);
    stage_text("WAT.PL", "WAT.WAV 0.05 USB scn=WAT.SCN seed=1\nWAT.WAV 0.05 USB scn=WAT.SCN seed=1 cut=200:48:drop\n"
                         "WAT.WAV 0.05 USB scn=WAT.SCN seed=2\nWAT.WAV 0.05 USB\nWAT.WAV 0.05 USB scn=WAT.SCN seed=1 dur=1\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    check(command("PLAYLIST:WAT.PL").ok, "Watterson playlist preflight accepts all items");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    check(blocks.size() == 5U && playlist_progress().failed == 0U, "all Watterson and clean items finish");

    if (blocks.size() != 5U)
    {
        return;
    }

    std::vector<ExpectedItem> expected;

    for (std::size_t index = 0U; index < blocks.size(); ++index)
    {
        const auto& block = blocks[index];
        auto input = card.samples;

        if (index == 4U)
        {
            input.resize(48000U);

            for (std::size_t frame = 0U; frame < input.size(); ++frame)
            {
                input[frame] = card.samples[frame % card.samples.size()];
            }
        }

        const bool clean = index == 3U;
        const auto render = clean ? Rendered{.samples = input, .stats = {}} : render_card(input, watterson_scenario, index == 2U ? 2U : 1U);
        const auto cut = index == 1U ? std::string_view{"CUT:200:48:drop"} : std::string_view{"CUT:CLEAR"};
        const auto stream = index == 1U ? expected_stream(render.samples, cut) : ExpectedStream{.samples = render.samples, .cuts = {}, .rejected = 0U};
        expected.push_back({.first = block.play.first_frame_index, .samples = stream.samples, .exact = true});
        check(block.play.watterson == (clean ? pl::WattersonIdentity{} : host_channel_identity()), "each retained item has its own channel identity");
        check(block.play.engine_digest_hi == high_word(render.stats.output_digest) && block.play.engine_digest_lo == low_word(render.stats.output_digest),
              "each retained engine digest is the host render before CUT (zero for clean)");
    }

    check(capture_is(host.received(), expected), "Watterson playlist output, including a continuous loop and CUT, equals host rendering");
    check(blocks[0].play.watterson == blocks[1].play.watterson && blocks[0].play.watterson == blocks[4].play.watterson && blocks[0].play.watterson != blocks[2].play.watterson,
          "seed overrides repeat the channel; another seed changes its identity");
    check(blocks[4].loops == 6U, "a looped Watterson scenario runs continuously across six source passes");
    const auto identity_before = wg::player_watterson_identity();
    stage_text("BAD.PL", "WAT.WAV 0 USB scn=WAT.SCN seed=42\nABSENT.WAV 0 USB\n");
    check(!command("PLAYLIST:BAD.PL").ok && wg::player_watterson_identity() == identity_before, "a later failed preflight does not replace the last play's reported identity");
    reset_playlist_state();
    check(command("OUTPUT CODEC").ok, "restore codec after the Watterson playlist");
}

void test_playlist_items_own_settings()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    const auto tone = stage_usb_file("TONE.WAV", usb_file_frames);
    stage_text("NOISE.SCN", noise_scenario);
    stage_text("OWN.PL", "TONE.WAV 0.05 USB label=a\nTONE.WAV 0.05 USB label=b scn=NOISE.SCN seed=1 cut=200:48:drop\n"
                         "TONE.WAV 0.05 USB label=c scn=NOISE.SCN seed=1\nTONE.WAV 0.05 USB label=d scn=NOISE.SCN seed=2\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    check(command("OUTPUT USB").ok && command("CUT:100:10:zero").ok, "a CUT list is pending before the playlist");
    check(command("PLAYLIST:OWN.PL").ok, "four items with their own settings");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    check(blocks.size() == 4U, "four blocks");

    if (blocks.size() != 4U)
    {
        return;
    }

    check(blocks[0].play.cut_count == 0U && blocks[0].play.cut_rejected == 0U && blocks[0].play.crc32_left == pcm24_crc(tone.samples),
          "the list set before PLAYLIST reaches no item: the first plays the card");
    check(blocks[1].play.cut_count == 1U && blocks[1].play.cut_drop_frames == ua::chunk_frame_count && blocks[2].play.cut_count == 0U &&
              blocks[1].play.sent_frames == tone.samples.size() - ua::chunk_frame_count && blocks[2].play.sent_frames == tone.samples.size(),
          "an item's cut= is its own, and the next item has none");
    check(blocks[2].play.engine_state == 1U && blocks[3].play.engine_state == 1U && blocks[2].seed == 1U && blocks[3].seed == 2U && blocks[2].play.crc32_left != blocks[3].play.crc32_left &&
              blocks[2].play.crc32_left != blocks[0].play.crc32_left,
          "the scenario plays with each item's seed: another seed gives other samples");
    check(wg::player_cuts().pending.empty() && !wg::player_playlist().scenario_seed.has_value(), "nothing of an item's is left for a later PLAY");
    check(command("LOAD:TONE.WAV").ok && command("PLAY").ok, "a plain PLAY after the playlist");
    run_accepted();
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().crc32_left == pcm24_crc(tone.samples) && wg::player_snapshot().engine_state == 0U, "it plays the card clean, with no cuts");
    reset_playlist_state();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    stage_text("SAME.PL", "TONE.WAV 0.05 USB scn=NOISE.SCN seed=1\n");
    check(command("PLAYLIST:SAME.PL").ok, "the seeded item again, after a new stream");
    run_accepted_playlist();
    const auto again = pending_blocks();
    check(again.size() == 1U && again[0].play.crc32_left == blocks[2].play.crc32_left, "the same scenario and seed give the same samples again");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// An item plays what its line says: without scn= its WAV's own <FILE>.SCN,
// which LOAD and PLAY would load, plays no part, so the block's clean CRC and
// engine_state 0 match its scn null. With scn= naming that file it plays it.
void test_playlist_ignores_the_wav_scenario()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    const auto tone = stage_usb_file("SIB.WAV", usb_file_frames);
    stage_text("SIB.SCN", noise_scenario);
    stage_text("SIB.PL", "SIB.WAV 0.05 USB label=plain\nSIB.WAV 0.05 USB label=named scn=SIB.SCN\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    check(command("PLAYLIST:SIB.PL").ok, "a card with a scenario of its own name, played without and with scn=");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    check(blocks.size() == 2U && blocks[0].play.engine_state == 0U && blocks[0].scenario.empty() && blocks[0].scenario_crc32 == 0U && blocks[0].play.crc32_left == pcm24_crc(tone.samples),
          "without scn= the item plays its card clean, whatever <FILE>.SCN the card holds");
    check(blocks.size() == 2U && blocks[1].play.engine_state == 1U && blocks[1].scenario.view() == "SIB.SCN" && blocks[1].scenario_crc32 == text_crc(noise_scenario) &&
              blocks[1].play.crc32_left != pcm24_crc(tone.samples),
          "scn= naming that file plays it");
    files.erase("2:/WG/SIB.SCN");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A PCM24 sample attenuated by `tenths` tenths of a dB, as the host predicts
// it (host/attenuation.py): the table's Q1.31 gain times the sample, then
// division by 2^31 rounded half to even, written out with integer division
// rather than the firmware's shift.
[[nodiscard]] std::int32_t reference_attenuation(std::int32_t sample, std::uint32_t tenths)
{
    constexpr std::int64_t unity = std::int64_t{1} << 31U;
    const std::int64_t product = std::int64_t{sample} * std::int64_t{std::span{waveform_generator::attenuation::gains_q31}[tenths]};
    std::int64_t quotient = product / unity;
    std::int64_t remainder = product % unity;

    if (remainder < 0)
    {
        remainder += unity;
        --quotient;
    }

    const bool up = 2 * remainder > unity || (2 * remainder == unity && quotient % 2 != 0);
    return static_cast<std::int32_t>(quotient + (up ? 1 : 0));
}

[[nodiscard]] std::vector<std::int32_t> attenuated(std::span<const std::int32_t> samples, std::uint32_t tenths)
{
    std::vector<std::int32_t> result;
    result.reserve(samples.size());
    std::ranges::transform(samples, std::back_inserter(result), [tenths](std::int32_t sample) { return reference_attenuation(sample, tenths); });

    return result;
}

// att= after the engine, on both routes: 0 dB passes the card through bit
// for bit; -20.0 dB sends each sample times the table's gain, rounded half to
// even, as the host predicts it; on the codec route with cuts the stream is
// the cut card attenuated (the ZERO's zeros stay zero), the same as the
// attenuated card cut; and a scenario item's render is attenuated after the
// engine, whose digest stays the unattenuated render's.
void test_playlist_attenuation()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t twenty_db = 200U;
    constexpr std::uint32_t codec_tenths = 65U;
    constexpr std::uint32_t forty_db = 400U;
    constexpr std::string_view codec_cuts{"CUT:100:480:zero,2000:300:drop"};
    reset_playlist_state();
    const auto card = stage_usb_file("ATT.WAV", usb_file_frames);
    stage_text("NOISE.SCN", noise_scenario);
    stage_text("ATT.PL", "ATT.WAV 0.05 USB label=unity att=0\nATT.WAV 0.05 USB label=twenty att=-20.0\n"
                         "ATT.WAV 0.05 CODEC label=codec att=-6.5 cut=100:480:zero,2000:300:drop\nATT.WAV 0.05 USB label=noise scn=NOISE.SCN att=-40\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    auto& capture = codec_capture();
    capture = CodecCapture{.recording = true, .samples = {}};
    check(command("PLAYLIST:ATT.PL").ok, "four attenuated items");
    run_accepted_playlist();
    capture.recording = false;
    const auto blocks = pending_blocks();
    check(blocks.size() == 4U && std::ranges::all_of(blocks, [](const pl::ResultBlock& block) { return block.outcome == pl::Outcome::done; }), "four blocks, every item done");

    if (blocks.size() != 4U)
    {
        return;
    }

    const auto twenty = attenuated(card.samples, twenty_db);
    const auto codec = expected_stream(attenuated(card.samples, codec_tenths), codec_cuts).samples;
    const auto render = render_card(card.samples, noise_scenario);
    const auto noise = attenuated(render.samples, forty_db);
    check(blocks[0].attenuation.value() == 0U && blocks[0].play.crc32_left == pcm24_crc(card.samples) && blocks[0].play.crc32_right == blocks[0].play.crc32_left,
          "att=0 passes the card through bit for bit");
    check(blocks[1].attenuation.value() == twenty_db && twenty != card.samples && blocks[1].play.crc32_left == pcm24_crc(twenty) && blocks[1].play.sent_frames == card.samples.size(),
          "att=-20.0 sends the card times the table's gain, rounded half to even: the host's prediction");
    check(codec == attenuated(expected_stream(card.samples, codec_cuts).samples, codec_tenths) && blocks[2].attenuation.value() == codec_tenths && blocks[2].play.sent_frames == codec.size() &&
              std::ranges::search(capture.samples, codec).size() == codec.size(),
          "the codec plays the cut card attenuated: the gain and the cuts commute, and the ZERO's zeros stay zero");
    check(blocks[3].attenuation.value() == forty_db && blocks[3].play.crc32_left == pcm24_crc(noise) && blocks[3].play.engine_state == 1U, "a scenario item sends its render attenuated by 40 dB");
    check_engine_saw_every_frame(render, card.samples.size(), "the attenuated scenario item");
    const std::vector<ExpectedItem> expected{{.first = blocks[0].play.first_frame_index, .samples = card.samples, .exact = true},
                                             {.first = blocks[1].play.first_frame_index, .samples = twenty, .exact = true},
                                             {.first = blocks[3].play.first_frame_index, .samples = noise, .exact = true}};
    check(capture_is(host.received(), expected), "the host received each USB item attenuated exactly, and silence between");
    const auto reply = exchange("1 PLAYLIST?\n");
    const auto shown = reply_blocks(reply.empty() ? std::string{} : reply.front());
    check(shown.size() == pl::blocks_per_reply && member_value(shown[1], "att_tenths_db") == std::to_string(twenty_db), "PLAYLIST? reports each item's attenuation in tenths of a dB");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// dur= (#19 "Duration"): the frames a looped item feeds the engine, its card
// from frame 0 again after its last frame, until `frames` frames.
[[nodiscard]] std::vector<std::int32_t> looped(std::span<const std::int32_t> card, std::size_t frames)
{
    std::vector<std::int32_t> stream;
    stream.reserve(frames);

    while (!card.empty() && stream.size() < frames)
    {
        const auto pass = card.first(std::min(card.size(), frames - stream.size()));
        stream.insert(stream.end(), pass.begin(), pass.end());
    }

    return stream;
}

// A USB item's block against the stream it was to send: every frame it
// confirmed sent is that stream's prefix, in both CRCs and its indices, and
// the host received exactly that prefix from its first frame index.
void check_item_prefix(const pl::ResultBlock& block, std::span<const std::int32_t> stream, const std::string& what)
{
    const auto sent = std::size_t{block.play.sent_frames};
    const auto prefix = stream.first(std::min(sent, stream.size()));
    check(sent <= stream.size() && block.play.crc32_left == pcm24_crc(prefix) && block.play.crc32_right == block.play.crc32_left &&
              (sent == 0U || block.play.last_frame_index - block.play.first_frame_index + 1U == sent),
          what + ": its frames sent are an exact prefix of its stream, by both CRCs and its indices");
    const auto received = usb_host().received();
    const auto first = std::size_t{block.play.first_frame_index};
    const bool held = first + prefix.size() <= received.size();
    const auto window = held ? std::span{received}.subspan(first, prefix.size()) : std::span<const ua::StereoFrame>{};
    check(held && std::ranges::equal(window, prefix, [](const ua::StereoFrame& frame, std::int32_t sample) { return frame.left == sample * ua::subslot_scale && frame.right == frame.left; }),
          what + ": the host received that prefix exactly from the item's first frame index");
}

// Looped items: a clean card shorter than its duration loops seamlessly (each
// seam's frame 0 follows the last frame, nothing repeated or zeroed); a cut
// across a seam and a ZERO at one land on the looped stream's absolute
// frames; a codec item loops attenuated; a scenario item's engine sees one
// input across the seams, its fades, noise and carrier going on as in the
// host render of the looped card; and an item without dur= plays its card
// once as before. Every read lands word-aligned: a seam seeks back to the data
// chunk's first frame only.
void test_playlist_looped_items()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t one_second = 48000U;
    constexpr std::uint32_t one_and_a_half = 72000U;
    constexpr std::string_view seam_cuts{"CUT:4900:4800:drop,10004:480:zero,71990:100:drop"};
    constexpr std::uint32_t seam_drop_at = 4900U;
    constexpr std::uint32_t seam_drop_frames = 4800U;
    constexpr std::uint32_t seam_zero_frames = 480U;
    constexpr std::uint32_t clipped_drop_frames = 10U;
    constexpr std::uint32_t codec_tenths = 60U;
    constexpr std::size_t item_count = 5U;
    reset_playlist_state();
    const auto card = stage_usb_file("LOOP.WAV", usb_file_frames);
    stage_text("CHAN.SCN", channel_scenario);
    stage_text("LOOPS.PL", "LOOP.WAV 0.05 USB label=loop dur=1\nLOOP.WAV 0.05 USB label=seam dur=1.5 cut=4900:4800:drop,10004:480:zero,71990:100:drop\n"
                           "LOOP.WAV 0.05 CODEC label=codec dur=1 att=-6\nLOOP.WAV 0.05 USB label=scn scn=CHAN.SCN dur=1\nLOOP.WAV 0.05 USB label=once\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    auto& capture = codec_capture();
    capture = CodecCapture{.recording = true, .samples = {}};
    unaligned_sector_reads() = 0U;
    check(command("PLAYLIST:LOOPS.PL").ok, "five items, four of them looped");
    run_accepted_playlist();
    capture.recording = false;
    const auto blocks = pending_blocks();
    check(blocks.size() == item_count &&
              std::ranges::all_of(blocks, [](const pl::ResultBlock& block) { return block.outcome == pl::Outcome::done && block.end_cause == pl::EndCause::end && block.play.deadline_misses == 0U; }),
          "five blocks, every item played to its end without a deadline miss");

    if (blocks.size() != item_count)
    {
        return;
    }

    const auto loop = looped(card.samples, one_second);
    check(loop.size() == one_second && loop[usb_file_frames] == card.samples.front() && loop[usb_file_frames - 1U] == card.samples.back(),
          "the test's looped stream: frame 0 of each pass right after the last of the one before");
    check(blocks[0].duration == pl::DurationFrames{.value = one_second} && blocks[0].play.sent_frames == one_second && blocks[0].play.usb_file_frames == one_second &&
              blocks[0].play.file_frames_total == usb_file_frames && blocks[0].loops == one_second / usb_file_frames && blocks[0].play.cut_count == 0U,
          "a 1 s item of a 5002-frame card sends 48000 frames: 9 whole passes and part of a tenth");
    check_item_prefix(blocks[0], loop, "the looped clean item");
    check(blocks[0].play.sent_frames == loop.size(), "the looped clean item sends its whole stream, every seam exact");
    const auto seam = expected_stream(looped(card.samples, one_and_a_half), seam_cuts);
    const auto& cut = blocks[1].play;
    // The DROP spans the first seam (frames 4900 to 9699 of the looped
    // stream), the ZERO sits at the second (10004) and the last DROP is cut
    // short by the duration's end. The loops count the engine output the
    // frames sent cover (14 passes), not the frames sent (13 passes' worth).
    check(cut.sent_frames == seam.samples.size() && cut.cut_count == 3U && cut.cut_drop_frames == seam_drop_frames + clipped_drop_frames && cut.cut_zero_frames == seam_zero_frames &&
              cut.cut_first_index == cut.first_frame_index + seam_drop_at && cut.usb_file_frames == one_and_a_half && blocks[1].loops == one_and_a_half / usb_file_frames &&
              cut.sent_frames / usb_file_frames != blocks[1].loops,
          "a DROP across the first seam, a ZERO at the second and a DROP the duration's end cuts short land on the looped stream's frames");
    check_item_prefix(blocks[1], seam.samples, "the item cut across a seam");
    const auto codec = attenuated(loop, codec_tenths);
    check(blocks[2].play.sent_frames == one_second && blocks[2].attenuation.value() == codec_tenths && blocks[2].loops == one_second / usb_file_frames &&
              std::ranges::search(capture.samples, codec).size() == codec.size(),
          "the codec loops the card attenuated");
    const auto render = render_card(loop, channel_scenario);
    check(blocks[3].play.engine_state == 1U && blocks[3].play.sent_frames == render.samples.size() && blocks[3].play.crc32_left == pcm24_crc(render.samples),
          "a looped scenario item sends the host render of its looped card: the fades, noise and carrier run on across the seams");
    check_item_prefix(blocks[3], render.samples, "the looped scenario item");
    check(!blocks[4].duration.has_value() && blocks[4].play.sent_frames == usb_file_frames && blocks[4].loops == 1U && blocks[4].play.crc32_left == pcm24_crc(card.samples),
          "an item without dur= plays its card once, one pass");
    const std::vector<ExpectedItem> expected{{.first = blocks[0].play.first_frame_index, .samples = loop, .exact = true},
                                             {.first = blocks[1].play.first_frame_index, .samples = seam.samples, .exact = true},
                                             {.first = blocks[3].play.first_frame_index, .samples = render.samples, .exact = true},
                                             {.first = blocks[4].play.first_frame_index, .samples = card.samples, .exact = true}};
    check(capture_is(host.received(), expected), "the host received the four USB items exactly, and silence between them");
    check(unaligned_sector_reads() == 0U, "every read of every pass lands word-aligned with the file pointer");
    const auto reply = exchange("1 PLAYLIST?\n");
    const auto shown = reply_blocks(reply.empty() ? std::string{} : reply.front());
    check(shown.size() == pl::blocks_per_reply && member_value(shown[0], "dur_frames") == std::to_string(one_second) && member_value(shown[0], "loops") == "9" &&
              member_value(shown[0], "end_cause") == R"("end")" && member_value(shown[0], "engine_deadline_misses") == "0",
          "PLAYLIST? reports each item's duration, loops, end cause and deadline misses");
    // The last item ran clean: STATUS shows its engine idle; the scenario
    // item's engine is checked against the render here, as it was the play
    // before.
    check(render.stats.frames_in == one_second, "the host render's engine saw the looped item's 48000 input frames");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A slow block for the deadline-miss tests: the next read of a whole card
// block once the output runs above its wake watermark takes until the host
// has drained the output FIFO below `fill_below` frames, as a slow SD read or
// engine block would.
struct SlowRead
{
    bool armed{};
    std::uint32_t fill_below{};
    unsigned int taken{};
};

[[nodiscard]] SlowRead& slow_read()
{
    static SlowRead slow;
    return slow;
}

void take_slow_read(UINT requested)
{
    namespace wg = waveform_generator;
    constexpr unsigned int longest_ms = 2000U;

    if (producer_block_ms() != 0U && requested == wg::read_buffer_bytes && wg::player_state.load() == wg::PlayerState::playing)
    {
        usb_host().run(producer_block_ms());
    }

    auto& slow = slow_read();

    if (!slow.armed || requested != wg::read_buffer_bytes || wg::player_state.load() != wg::PlayerState::playing || wg::ring_fill() < wg::ring_wake_watermark_frames)
    {
        return;
    }

    slow.armed = false;
    ++slow.taken;

    for (unsigned int elapsed = 0U; elapsed < longest_ms && wg::ring_fill() >= slow.fill_below; ++elapsed)
    {
        usb_host().run(1U);
    }
}

// One playlist of `text` (staged as `name`) on a freshly opened stream with
// `events` and the tick hook `hook`: its blocks.
[[nodiscard]] std::vector<pl::ResultBlock> run_playlist_on_usb(const std::string& name, std::string_view text, const ArmedPlayEvents& events, std::function<void()> hook = {})
{
    reset_playlist_state();
    stage_text(name, text);
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule(events);
    host.on_tick(std::move(hook));
    check(command(("PLAYLIST:" + name).c_str()).ok, name + " is accepted");
    run_accepted_playlist();
    host.schedule({});
    return pending_blocks();
}

// A looped item that ended part way at `cause` (player error `error`): its
// block says so, with the loops its frames sent cover, and those frames are
// an exact prefix of its looped stream.
void check_ended_early(const std::vector<pl::ResultBlock>& blocks, std::span<const std::int32_t> stream, pl::EndCause cause, std::uint32_t error, const std::string& what)
{
    const bool one = blocks.size() == 1U;
    const auto& block = one ? blocks.front() : pl::ResultBlock{};
    const auto expected_outcome = cause == pl::EndCause::stop ? pl::Outcome::aborted : pl::Outcome::error;
    check(one && block.outcome == expected_outcome && block.player_error == error && block.end_cause == cause && block.play.sent_frames != 0U && block.play.sent_frames < stream.size() &&
              block.loops == block.play.usb_file_frames / usb_file_frames,
          what + ": the looped item ends early with its cause, error and loops in its block, after " + std::to_string(block.play.sent_frames) + " frames");

    if (one)
    {
        check_item_prefix(block, stream, what);
        const auto prefix = stream.first(std::min<std::size_t>(block.play.sent_frames, stream.size()));
        const std::vector<ExpectedItem> expected{{.first = block.play.first_frame_index, .samples = {prefix.begin(), prefix.end()}, .exact = true}};
        check(capture_is(usb_host().received(), expected), what + ": the stream carries silence around the prefix");
    }
}

// STOP in an item, then a host that stops polling while the chunks queued at
// STOP wait to go out: the wait ends with error 20 as it does for a plain PLAY
// (test_usb_stall_after_stop), so the item's block says error, player_error 20
// and end_cause stall, not aborted. The playlist still ends aborted, no later
// item runs, and the block is the exact prefix confirmed sent; once the host
// polls again the queued chunks go out.
void test_playlist_stop_then_stall()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t skipped = 2U * documented_stall_timeout_ms;
    const auto long_file = stage_usb_file("S1LONG.WAV", usb_long_file_frames);
    static_cast<void>(stage_usb_file("S2.WAV", usb_file_frames));
    const auto blocks = run_playlist_on_usb("STOPD.PL", "S1LONG.WAV 1 USB\nS2.WAV 0 USB\n", {.send_stop_at = usb_interrupt_at_milliseconds, .skip_after_disarm = skipped});
    const bool one = blocks.size() == 1U;
    const auto& block = one ? blocks.front() : pl::ResultBlock{};
    check(!hook_replies().empty() && hook_replies().back().ok, "STOP in the item is acknowledged");
    check(one && block.outcome == pl::Outcome::error && block.player_error == std::to_underlying(wg::PlayerError::usb_stall) && block.end_cause == pl::EndCause::stall,
          "a host that stops polling after STOP, with the item's frames queued, ends the item with error 20 and end_cause stall, not aborted");
    check(playlist_progress().state == pl::State::aborted && playlist_progress().index == 0U && playlist_progress().failed == 1U,
          "the playlist still ends aborted at that item, which counts as failed, and no later item runs");
    check(one && block.play.sent_frames != 0U && block.play.sent_frames < long_file.samples.size() && block.play.crc32_left == pcm24_crc(std::span{long_file.samples}.first(block.play.sent_frames)),
          "the block is the exact prefix confirmed sent when the wait ended");
    const auto reply = exchange("1 PLAYLIST?\n");
    const auto shown = reply_blocks(reply.empty() ? std::string{} : reply.front());
    check(shown.size() == 1U && member_value(shown[0], "result") == quoted("error") && member_value(shown[0], "player_error") == "20" && member_value(shown[0], "end_cause") == quoted("stall"),
          R"(PLAYLIST? reports the item as "result":"error", "player_error":20, "end_cause":"stall")");
    auto& host = usb_host();
    host.resume_polling();
    host.run(usb_trailing_milliseconds);
    check(ua::play_statistics().pending_frames == ua::FrameCount{}, "once the host polls again, the queued chunks go out");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// What ends a looped item early, the first anomaly its block records: a
// stream close, STOP, a missed USB interval, usb_stack_asserts rising, and an
// engine deadline miss. A play without dur= rides out the missed interval
// and the deadline miss and only counts them.
void test_playlist_loop_anomalies()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t two_seconds = 96000U;
    constexpr std::uint32_t mid_loop_ms = 500U;
    constexpr std::uint32_t gap_ms = 5U;
    constexpr std::uint32_t slow_until_fill = 4000U;
    const auto card = stage_usb_file("LOOP.WAV", usb_file_frames);
    const auto stream = looped(card.samples, two_seconds);
    constexpr std::string_view looped_item{"LOOP.WAV 0 USB label=long dur=2\n"};
    check_ended_early(run_playlist_on_usb("CLOSE.PL", looped_item, {.close_stream_at = mid_loop_ms}), stream, pl::EndCause::stream_closed, std::to_underlying(wg::PlayerError::usb_stream_closed),
                      "a stream close mid-loop");
    check_ended_early(run_playlist_on_usb("STOP.PL", looped_item, {.send_stop_at = mid_loop_ms}), stream, pl::EndCause::stop, 0U, "STOP mid-loop");
    check_ended_early(run_playlist_on_usb("MISS.PL", looped_item, {.stop_polling_at = mid_loop_ms, .resume_polling_at = mid_loop_ms + gap_ms}), stream, pl::EndCause::missed_interval,
                      std::to_underlying(wg::PlayerError::missed_interval), "a missed interval mid-loop");
    const auto once = run_playlist_on_usb("MISS1.PL", "LOOP.WAV 0 USB label=once\nLOOP.WAV 0 USB label=again\n", {.stop_polling_at = 50U, .resume_polling_at = 50U + gap_ms});
    check(once.size() == 2U && once[0].outcome == pl::Outcome::done && once[0].play.missed_intervals != 0U && once[0].end_cause == pl::EndCause::end,
          "an item without dur= rides out a missed interval, as a PLAY does, and its block counts it");
    bool asserted = false;
    const auto assert_mid_loop = [&asserted]
    {
        if (!asserted && ua::play_statistics().armed && ua::play_statistics().file_frames.value > two_seconds / 4U)
        {
            asserted = true;
            wfg_usb_count_stack_assert();
        }
    };
    const auto assert_blocks = run_playlist_on_usb("ASSERT.PL", looped_item, {}, assert_mid_loop);
    check(asserted, "a TinyUSB assertion failed mid-loop");
    check_ended_early(assert_blocks, stream, pl::EndCause::usb_assert, std::to_underlying(wg::PlayerError::usb_assert), "usb_stack_asserts rising mid-loop");
    // A completion reported short of its packet: the host may not have
    // received it, so a looped item ends there, as at its other anomalies.
    check_ended_early(run_playlist_on_usb("SHORT.PL", looped_item, {.short_reports_at = mid_loop_ms, .short_reports = 1U}), stream, pl::EndCause::fault,
                      std::to_underlying(wg::PlayerError::usb_unconfirmed), "a completion reported short mid-loop");
    // A slow block: one deadline miss. A play without dur= counts it and
    // plays on, every frame sent; a looped item ends there.
    const auto long_card = stage_usb_file("DL.WAV", usb_long_file_frames);
    slow_read() = SlowRead{.armed = true, .fill_below = slow_until_fill, .taken = 0U};
    const auto counted = run_playlist_on_usb("SLOW1.PL", "DL.WAV 0 USB label=slow\n", {});
    const auto usb = usb_members(exchange("1 USB?\n"));
    check(slow_read().taken == 1U && counted.size() == 1U && counted[0].outcome == pl::Outcome::done && counted[0].play.deadline_misses == 1U &&
              counted[0].play.sent_frames == long_card.samples.size() && counted[0].play.crc32_left == pcm24_crc(long_card.samples) && counted[0].play.underruns == 0U &&
              member_value(usb, "engine_deadline_misses") == "1",
          "a slow block in an item without dur= is one deadline miss, which USB? and the block count; the item plays every frame");
    slow_read() = SlowRead{.armed = true, .fill_below = slow_until_fill, .taken = 0U};
    const auto ended = run_playlist_on_usb("SLOW2.PL", "DL.WAV 0 USB label=loop dur=2\n", {});
    check(slow_read().taken == 1U && !ended.empty() && ended[0].play.deadline_misses == 1U, "the looped item meets the same slow block");
    const auto long_stream = looped(long_card.samples, two_seconds);
    check(ended.size() == 1U && ended[0].outcome == pl::Outcome::error && ended[0].player_error == std::to_underlying(wg::PlayerError::deadline_miss) &&
              ended[0].end_cause == pl::EndCause::deadline_miss && ended[0].play.sent_frames < long_stream.size(),
          "an engine deadline miss ends a looped item, with its cause in the block");

    if (ended.size() == 1U)
    {
        check_item_prefix(ended[0], long_stream, "the looped item ended by a deadline miss");
    }

    slow_read() = SlowRead{};
    check(command("LOAD:DL.WAV").ok && command("PLAY").ok, "a PLAY after them");
    run_accepted(usb_accept_to_run_milliseconds);
    usb_host().run(usb_trailing_milliseconds);
    check(member_value(usb_members(exchange("1 USB?\n")), "engine_deadline_misses") == "0", "every PLAY restarts the count");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A late read is no anomaly: a completion handled just past the end of its
// frame reads as a gap of 2 that the next completion's gap of 0 takes back.
// A looped item plays on through one mid-loop and through one on the
// completion that confirms its last frames, although the player task looks
// before the next completion each time, and its block counts no missed
// interval. One real missed interval, which the next completion confirms,
// still ends it. Once its last frame is confirmed sent, a TinyUSB assertion
// ends nothing more: the item has reached its end.
void test_playlist_loop_late_reads()
{
    namespace wg = waveform_generator;
    constexpr std::uint32_t two_seconds = 96000U;
    constexpr std::uint32_t mid_loop_ms = 500U;
    constexpr std::uint32_t late_reads = 2U;
    const auto card = stage_usb_file("LOOP.WAV", usb_file_frames);
    const auto stream = looped(card.samples, two_seconds);
    constexpr std::string_view looped_item{"LOOP.WAV 0 USB label=long dur=2\n"};
    const auto stream_missed = ua::stream_counters().missed_intervals;
    const auto blocks = run_playlist_on_usb("LATE.PL", looped_item, {.late_read_at = mid_loop_ms, .late_read_last = true});
    const bool one = blocks.size() == 1U;
    const auto& block = one ? blocks.front() : pl::ResultBlock{};
    check(usb_host().late_reads() == late_reads, "a late read mid-loop and one on the completion that confirms the item's last frames");
    check(one && block.outcome == pl::Outcome::done && block.end_cause == pl::EndCause::end && block.player_error == 0U && block.play.sent_frames == stream.size() &&
              block.play.missed_intervals == 0U && block.play.first_miss_frame_index == 0U,
          "late reads do not end a looped item: it plays to its end, and its block counts no missed interval");

    if (one)
    {
        check_item_prefix(block, stream, "the looped item with late reads");
    }

    check(ua::stream_counters().missed_intervals == stream_missed, "the next completion took each late read back from the stream's count too");
    bool asserted = false;
    usb_host().on_completion(
        [&asserted]
        {
            const auto play = ua::play_statistics();

            if (!asserted && play.armed && play.file_frames != ua::FrameCount{} && play.pending_frames == ua::FrameCount{})
            {
                asserted = true;
                wfg_usb_count_stack_assert();
            }
        });
    const auto delivered = run_playlist_on_usb("LATE2.PL", looped_item, {});
    usb_host().on_completion({});
    check(asserted && delivered.size() == 1U && delivered[0].outcome == pl::Outcome::done && delivered[0].end_cause == pl::EndCause::end && delivered[0].play.sent_frames == stream.size(),
          "a TinyUSB assertion right after the completion that confirms the item's last frames ends nothing: the item has reached its end");
    check_ended_early(run_playlist_on_usb("MISS2.PL", looped_item, {.stop_polling_at = mid_loop_ms, .resume_polling_at = mid_loop_ms + 1U}), stream, pl::EndCause::missed_interval,
                      std::to_underlying(wg::PlayerError::missed_interval), "one missed interval mid-loop, which the next completion confirms");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// A gap counted in stream frames ends even if the host stops polling without
// closing the stream (a suspended bus): once the stream has sent nothing for
// the stall timeout, the gap passes on the tick clock from its start, so the
// playlist ends rather than wait for frames that never come.
void test_playlist_gap_on_a_stalled_stream()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    constexpr std::uint32_t gap_ms = 1000U;
    static_cast<void>(stage_usb_file("STALL.WAV", usb_file_frames));
    stage_text("STALL.PL", "STALL.WAV 1 USB\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    bool stopped_polling = false;
    host.on_tick(
        [&stopped_polling, &host]
        {
            if (!stopped_polling && playlist_progress().state == pl::State::gap)
            {
                stopped_polling = true;
                host.stop_polling();
            }
        });
    check(command("PLAYLIST:STALL.PL").ok, "one USB item and its last gap");
    run_accepted_playlist();
    host.resume_polling();
    const auto blocks = pending_blocks();
    const auto progress = playlist_progress();
    check(stopped_polling && progress.state == pl::State::done && blocks.size() == 1U && blocks[0].outcome == pl::Outcome::done,
          "the host stopped polling in the last gap, and the playlist still ends done");
    const auto waited = host.tick() - (blocks.empty() ? 0U : blocks[0].end_tick);
    check(waited >= gap_ms && waited <= gap_ms + 2U * wg::playlist_poll_ms, "the stalled gap passes on the tick clock: its length from the item's end");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// #40: a gap on a stream that keeps moving is counted in its frames, not on
// the RTOS tick, even when the two clocks disagree: with the tick 10 % fast,
// the last gap (1 s) ends when the stream has sent the item's frames, whole
// packets, and 1 s of frames, within a packet, where the tick clock would
// have ended it some 90 ms of frames early.
void test_playlist_gap_follows_the_stream()
{
    reset_playlist_state();
    constexpr std::uint32_t skew_ms = 10U;
    constexpr std::uint32_t gap_ms = 1000U;
    constexpr std::uint32_t gap_frames = gap_ms * frames_per_ms;
    static_cast<void>(stage_usb_file("SKEW.WAV", usb_file_frames));
    stage_text("SKEW.PL", "SKEW.WAV 1 USB\n");
    auto& host = usb_host();
    host.open_stream();
    host.skew_ticks(skew_ms);
    host.run(usb_idle_milliseconds);
    host.schedule({});
    check(command("PLAYLIST:SKEW.PL").ok, "one USB item and its last gap, on a tick 10 % fast");
    run_accepted_playlist();
    const auto ended_at = ua::stream_counters().frames_sent.value;
    host.skew_ticks(0U);
    const auto blocks = pending_blocks();
    check(playlist_progress().state == pl::State::done && blocks.size() == 1U && blocks[0].outcome == pl::Outcome::done, "the playlist ends done");

    if (blocks.size() != 1U)
    {
        return;
    }

    const auto gap_end = pl::next_start_index(blocks[0].play.first_frame_index, blocks[0].play.sent_frames, pl::GapFrames{.value = gap_frames});
    const auto ticks = host.tick() - blocks[0].end_tick;
    check(ended_at >= gap_end && ended_at <= gap_end + 2U * ua::chunk_frames.value, "the gap ends at the stream index the item's frames and the gap's frames give, within a packet");
    check(ticks >= gap_ms + gap_ms / (2U * skew_ms), "and the tick clock, 10 % fast, counted some 1100 ticks meanwhile, past the 1000 it would have stopped at");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// #40: a CUT list pending before a playlist whose one USB item finds no
// stream: the item plays nothing, and the list does not stay pending for a
// later PLAY, which the playlist was to have consumed.
void test_playlist_no_stream_drops_a_pending_cut()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    static_cast<void>(stage_usb_file("NOSTR.WAV", usb_file_frames));
    stage_text("NOSTR.PL", "NOSTR.WAV 0 USB\n");
    usb_host().close_stream();
    check(command("OUTPUT CODEC").ok && command("CUT:100:10:zero").ok && wg::player_cuts().pending.size() == 1U, "a CUT list is pending, and no stream is open");
    check(command("PLAYLIST:NOSTR.PL").ok, "a playlist of one USB item");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    check(blocks.size() == 1U && blocks[0].outcome == pl::Outcome::error && blocks[0].player_error == std::to_underlying(wg::PlayerError::usb_no_stream), "the item finds no stream and plays nothing");
    check(wg::player_cuts().pending.empty(), "and the CUT list pending before the playlist is not left for a later PLAY");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// 7af1c4c and 1981524: the last item's att= and dur= end with it. After a
// playlist whose last item is attenuated and looped, LOAD and PLAY play the
// card once, at full level, bit for bit.
void test_playlist_settings_end_with_it()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    const auto card = stage_usb_file("LASTATT.WAV", usb_file_frames);
    stage_text("LASTATT.PL", "LASTATT.WAV 0 USB label=last att=-20 dur=1\n");
    auto& host = usb_host();
    host.open_stream();
    host.run(usb_idle_milliseconds);
    host.schedule({});
    check(command("PLAYLIST:LASTATT.PL").ok, "a playlist whose last item is attenuated and looped");
    run_accepted_playlist();
    const auto blocks = pending_blocks();
    check(blocks.size() == 1U && blocks[0].outcome == pl::Outcome::done && blocks[0].play.sent_frames > card.samples.size(), "the item looped its card, attenuated");
    // A new capture session, so the host's capture holds the PLAY alone.
    host.open_stream();
    host.run(usb_idle_milliseconds);
    check(command("LOAD:LASTATT.WAV").ok, "LOAD selects the card");
    host.schedule({});
    run_selected();
    const auto play = ua::play_statistics();
    const auto crc = wg::crc32(card.data_chunk());
    check(wg::player_snapshot().state == static_cast<std::uint32_t>(wg::PlayerState::done) && play.file_frames.value == card.samples.size() && play.crc32_left == crc && play.crc32_right == crc,
          "PLAY then plays the card once, at full level: its frames and zlib.crc32 of its data chunk");
    host.run(usb_trailing_milliseconds);
    check_sent_prefix(card, "the host receives the card once, bit for bit");
    check(command("OUTPUT CODEC").ok, "the codec route is selected again");
}

// While a playlist holds the player, every command that would change a run is
// BUSY, the live controls included, and MEDIA HOST is refused; STOP and
// PLAYLIST ACK act, and STATUS? and PLAYLIST? answer as always.
void test_playlist_holds_the_player()
{
    namespace wg = waveform_generator;
    reset_playlist_state();
    wg::player_playlist().owns_player = true;
    wg::publish_live();

    for (const char* text : {"LOAD:GOOD.WAV", "PLAY", "CUT", "CUT:1:1:zero", "OUTPUT CODEC", "OUTPUT USB", "SEED:1", "REFERENCE:0.5", "CW ON", "PLAYLIST:ONE.PL"})
    {
        const auto reply = command(text);
        check(!reply.ok && std::string_view{reply.error} == "BUSY", std::string{text} + " while a playlist holds the player is BUSY");
    }

    check(command("PLAYLIST ACK:0").ok, "PLAYLIST ACK acts while a playlist runs");
    const auto lines = exchange("MEDIA HOST\n1 MEDIA HOST\n2 STATUS?\n3 PLAYLIST?\n");
    check(lines.size() == 4U && lines[0] == "ERR BUSY" && lines[1].contains(R"("code":"BUSY")") && lines[2].contains(R"("ok":true)") && lines[3].contains(R"("ok":true)") &&
              !wg::player_allows_media_host() && wg::player_live_snapshot().busy,
          "MEDIA HOST is refused, so no file generation starts; STATUS? and PLAYLIST? answer");
    check(command("STOP").ok && wg::player_playlist().stop_requested, "STOP reaches the playlist");
    wg::reset_playlist();
    wg::publish_live();
}

// PLAYLIST? at its widest: the progress and two blocks with every member at
// its widest, in the contract's order (status_contract.hpp); the width they
// leave unused closes playlist_json_worst_case_bytes() exactly. And the reply
// of a board that has run no playlist.
void test_playlist_status_contract()
{
    namespace wg = waveform_generator;
    constexpr auto max = std::numeric_limits<std::uint32_t>::max();
    pl::Snapshot widest{};
    widest.progress = {.state = pl::State::paused_ring_full,
                       .name = pl::ShortName::from("ABCDEFGH.XYZ"),
                       .run = max,
                       .policy = pl::Policy::continue_on_error,
                       .items = pl::max_items,
                       .index = pl::max_items,
                       .completed = pl::max_items,
                       .failed = pl::max_items};
    widest.pending = pl::result_capacity;
    widest.oldest_seq = max;
    widest.next_seq = max;
    widest.tick = max;
    pl::ResultBlock block{.seq = max,
                          .run = max,
                          .item = pl::max_items - 1U,
                          .label = pl::Label::from(std::string(pl::label_max_chars, 'x')),
                          .file = pl::ShortName::from("ABCDEFGH.WAV"),
                          .route = pl::Route::codec,
                          .outcome = pl::Outcome::aborted,
                          .player_error = max,
                          .scenario = pl::ShortName::from("ABCDEFGH.SCN"),
                          .seed = max,
                          .scenario_crc32 = max,
                          .play = {},
                          .gap = pl::max_gap,
                          .scheduled_index = max - 1U,
                          .start_tick = max,
                          .end_tick = max,
                          .attenuation = waveform_generator::attenuation::most,
                          .duration = pl::max_duration,
                          .loops = max,
                          .end_cause = pl::EndCause::missed_interval};
    block.play = {.file_frames_total = max,
                  .usb_file_frames = max,
                  .sent_frames = max,
                  .crc32_left = max,
                  .crc32_right = max,
                  .first_frame_index = max,
                  .last_frame_index = max,
                  .first_frame_sof = max,
                  .missed_intervals = max,
                  .first_miss_frame_index = max,
                  .stream_bits = max,
                  .stream_channels = max,
                  .cut_count = max,
                  .cut_drop_frames = max,
                  .cut_zero_frames = max,
                  .cut_first_index = max,
                  .cut_rejected = max,
                  .underruns = max,
                  .pcm_drained = max,
                  .engine_state = max,
                  .engine_error = max,
                  .deadline_misses = max,
                  .watterson = {.version = max, .digest_hi = max, .digest_lo = max, .latency_frames = max, .paths = max},
                  .engine_digest_hi = max,
                  .engine_digest_lo = max};
    widest.blocks.fill(block);
    widest.block_count = pl::blocks_per_reply;
    std::string reply;
    {
        const ServiceStateGuard saved;
        check(wg::response_size == 0U, "the PLAYLIST? fixture starts with no reply queued");
        set_max(wg::response_sequence);
        fill_text(wg::live_snapshot.run_id, 'R');
        wg::queue_playlist_reply(widest);
        reply = rendered_reply();
    }

    std::vector<wg::StatusJsonField> progress_layout{
        {.name = "seq", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "protocol", .max_value_chars = wg::json_string_chars(wg::literal_length("WFG-LIVE/1"))},
        {.name = "ok", .max_value_chars = wg::literal_length("true")},
        {.name = "state", .max_value_chars = wg::max_uint32_decimal_chars},
        {.name = "run_id", .max_value_chars = wg::json_string_chars(wg::run_id_max_chars)},
        {.name = "playlist", .max_value_chars = wg::literal_length("{")},
        {.name = "format", .max_value_chars = wg::json_string_chars(wg::literal_length("playlist"))},
    };
    progress_layout.insert(progress_layout.end(), wg::playlist_progress_fields.begin(), wg::playlist_progress_fields.end());
    const auto progress_unused = unused_width(reply_progress(reply), progress_layout);
    check_ok(progress_unused.status(), "PLAYLIST? emits the progress members in the contract's order within their widths");
    const auto blocks = reply_blocks(reply);
    std::size_t unused = progress_unused.has_value() ? progress_unused.value() : 0U;
    bool blocks_in_order = blocks.size() == pl::blocks_per_reply;

    for (const auto& members : blocks)
    {
        const auto block_unused = unused_width(members, wg::playlist_block_fields);
        blocks_in_order = blocks_in_order && block_unused.has_value();
        unused += block_unused.has_value() ? block_unused.value() : 0U;
    }

    check(blocks_in_order && reply.starts_with(R"({"seq":)") && reply.ends_with("]}}\n"), "two blocks, each in the contract's order within its widths");
    check(reply.size() + 1U + unused == wg::playlist_json_worst_case_bytes() && reply.size() < wg::cdc_response_capacity, "PLAYLIST? worst case counts every byte of the reply and its terminator");
    check(std::ranges::equal(block_members(block), wg::playlist_block_fields, {}, &StatusMember::name, &wg::StatusJsonField::name), "the test's block members follow the contract");
    reset_playlist_state();
    const auto tick_before = usb_host().tick();
    const auto idle = exchange("1 PLAYLIST?\n");
    check(idle.size() == 1U && without_tick(idle[0]).contains(R"(,"playlist":{"format":"playlist","state":"idle","name":"","run":0,"policy":"stop","items":0,)"
                                                              R"("index":0,"completed":0,"failed":0,"pending":0,"oldest_seq":0,"next_seq":1,"blocks":[]}})"),
          "a board that has run no playlist reports it idle");
    const auto idle_tick = idle.empty() ? std::nullopt : reply_tick(idle[0]);
    check(idle_tick.has_value() && idle_tick.value() >= tick_before && idle_tick.value() <= usb_host().tick(), "PLAYLIST? reports the RTOS tick when the reply was read");
}
} // namespace

TestOcotp* OCOTP = &test_ocotp;

extern "C"
{
    TaskHandle_t xTaskCreateStatic(void (*)(void*), const char* name, std::uint32_t, void*, UBaseType_t, StackType_t*, StaticTask_t*)
    {
        if (std::string_view{name} == "wfg_player")
        {
            ++player_task_creations;
            return player_task_created ? reinterpret_cast<void*>(1) : nullptr;
        }

        if (std::string_view{name} == "wfg_usb")
        {
            ++usb_bring_up().task_creations;

            if (!usb_bring_up().task_created)
            {
                return nullptr;
            }
        }

        return reinterpret_cast<void*>(1);
    }

    void xTaskNotifyGive(TaskHandle_t) {}

    void vTaskSuspend(TaskHandle_t task)
    {
        if (usb_task_run() != UsbTaskRun::none)
        {
            end_usb_task_run(usb_task_run() == UsbTaskRun::stack_fails && task == nullptr, "the USB task suspended itself");
        }

        std::abort();
    }

    std::uint32_t ulTaskNotifyTake(BaseType_t, TickType_t ticks)
    {
        if (++wait_calls > wait_call_limit())
        {
            std::fputs("simulated player stalled\n", stderr);
            std::abort();
        }

        if (stop_on_next_wait)
        {
            stop_on_next_wait = false;
            waveform_generator::live_protocol::Request request;
            request.kind = waveform_generator::live_protocol::Kind::stop;
            check(waveform_generator::submit_player_command(request).is_ok(), "STOP enters owner mailbox during pacing");
            return 1U;
        }

        if (m110::imxrt1170::wm8960_codec().is_running() && audio_hook != nullptr && !codec_blocks_held())
        {
            std::uint32_t playback[256]{};
            const auto frames = std::min<std::uint32_t>(ticks, 1000U) * 48U;

            for (std::uint32_t done = 0U; done < frames; done += 128U)
            {
                audio_hook(audio_context, nullptr, playback, 128U);
                record_codec_block(playback);
            }
        }

        // The wait lets that much USB time pass, at most a second as above:
        // ticks, frames and, while the stream is open and the host polls, one
        // packet per millisecond.
        constexpr std::uint32_t longest_simulated_wait_ms = 1000U;
        usb_host().run(std::min<std::uint32_t>(ticks, longest_simulated_wait_ms));
        return 0U;
    }

    TickType_t xTaskGetTickCount()
    {
        return usb_host().tick();
    }

    TickType_t xTaskGetTickCountFromISR()
    {
        return usb_host().tick();
    }

    std::uint16_t m110_tinyusb_audio_in_queued_bytes()
    {
        return usb_host().queued();
    }

    std::uint16_t m110_tinyusb_audio_in_free_bytes()
    {
        return usb_host().free_space();
    }

    std::uint16_t m110_tinyusb_audio_in_write(const void* bytes, std::uint16_t count)
    {
        // The C shim passes the chunk as const void*; this is that boundary's one cast.
        return usb_host().write({static_cast<const std::uint8_t*>(bytes), count});
    }

    void vTaskDelay(TickType_t ticks)
    {
        (void)ulTaskNotifyTake(pdTRUE, ticks);
    }

    FRESULT f_mount(FATFS* filesystem, const char*, unsigned char)
    {
        auto& faults = directory_faults();

        if (filesystem != nullptr)
        {
            ++faults.mount_calls;

            if (faults.fail_mount)
            {
                return FR_DISK_ERR;
            }
        }
        else
        {
            ++faults.unmount_calls;

            if (faults.fail_unmount)
            {
                return FR_DISK_ERR;
            }
        }

        mounted = filesystem != nullptr;
        return FR_OK;
    }

    FRESULT f_open(FIL* file, const char* path, unsigned char mode)
    {
        if (!mounted || file == nullptr || path == nullptr)
        {
            return FR_DISK_ERR;
        }

        const bool create_new = (mode & FA_CREATE_NEW) != 0U;
        auto found = files.find(path);

        if (create_new)
        {
            if ((mode & FA_WRITE) == 0U || found != files.end())
            {
                return found != files.end() ? FR_EXIST : FR_DISK_ERR;
            }

            found = files.emplace(path, std::vector<std::uint8_t>{}).first;
        }

        const bool secondary = is_secondary_path(path);

        if (secondary)
        {
            auto& faults = secondary_card_faults();
            ++faults.open_attempts;

            if (faults.fail_open)
            {
                return FR_DISK_ERR;
            }
        }

        if (found == files.end())
        {
            return FR_NO_FILE;
        }

        file->data = found->second.data();
        file->size = static_cast<FSIZE_t>(found->second.size());
        file->position = 0U;
        file->opened = 1;
        file->context = secondary ? &secondary_file_tag() : nullptr;
        file->mode = mode;
        opened_file_paths[file] = path;
        ++open_files;

        if (secondary)
        {
            ++secondary_card_faults().successful_opens;
        }

        return FR_OK;
    }

    FRESULT f_read(FIL* file, void* destination, UINT requested, UINT* actual)
    {
        if (!file->opened || (file->mode & FA_READ) == 0U || actual == nullptr || (requested != 0U && destination == nullptr))
        {
            return FR_DISK_ERR;
        }

        const auto read_fails = [](auto& reads)
        {
            if (!reads.has_value())
            {
                return false;
            }

            if (*reads == 0U)
            {
                return true;
            }

            --*reads;
            return false;
        };

        if (is_secondary_file(file))
        {
            auto& faults = secondary_card_faults();
            ++faults.read_calls;

            if (read_fails(faults.reads_before_failure))
            {
                *actual = 0U;
                return FR_DISK_ERR;
            }
        }
        else if (read_fails(card_faults().reads_before_failure))
        {
            *actual = 0U;
            return FR_DISK_ERR;
        }

        if (requested >= sector_bytes && !word_aligned_with(destination, file->position))
        {
            ++unaligned_sector_reads();
        }

        take_slow_read(requested);
        bool short_read = false;

        if (is_secondary_file(file))
        {
            auto& short_after = secondary_card_faults().reads_before_short;

            if (short_after.has_value())
            {
                if (*short_after == 0U)
                {
                    short_read = true;
                    short_after.reset();
                }
                else
                {
                    --*short_after;
                }
            }
        }

        *actual = file->position > file->size ? 0U : std::min<UINT>(short_read && requested != 0U ? requested - 1U : requested, file->size - file->position);
        std::memcpy(destination, file->data + file->position, *actual);
        file->position += *actual;

        if (is_secondary_file(file) && secondary_card_faults().after_read)
        {
            secondary_card_faults().after_read();
        }

        return FR_OK;
    }

    FRESULT f_lseek(FIL* file, FSIZE_t offset)
    {
        if (is_secondary_file(file))
        {
            ++secondary_card_faults().seek_calls;
            secondary_card_faults().last_seek = offset;
        }

        const bool failed_seek = is_secondary_file(file) ? secondary_card_faults().seek == offset : card_faults().seek == offset;

        if (offset > file->size || failed_seek)
        {
            return FR_DISK_ERR;
        }

        file->position = offset;
        return FR_OK;
    }

    FRESULT f_close(FIL* file)
    {
        const bool secondary = is_secondary_file(file);

        if (secondary)
        {
            auto& faults = secondary_card_faults();
            ++faults.close_calls;

            if (faults.fail_close)
            {
                return FR_DISK_ERR;
            }
        }
        else
        {
            const auto opened = opened_file_paths.find(file);

            if (opened != opened_file_paths.end() && opened->second == card_faults().close_path)
            {
                ++card_faults().close_calls;

                if (card_faults().fail_close)
                {
                    return FR_DISK_ERR;
                }
            }
        }

        if (file->opened)
        {
            --open_files;
        }

        opened_file_paths.erase(file);
        file->opened = 0;
        file->context = nullptr;
        file->mode = 0U;
        return FR_OK;
    }

    FRESULT f_write(FIL* file, const void* source, UINT requested, UINT* actual)
    {
        const auto opened = opened_file_paths.find(file);

        if (file == nullptr || !file->opened || (file->mode & FA_WRITE) == 0U || actual == nullptr || (requested != 0U && source == nullptr) || opened == opened_file_paths.end())
        {
            return FR_DISK_ERR;
        }

        auto found = files.find(opened->second);

        if (found == files.end() || file->position > std::numeric_limits<std::size_t>::max() - requested ||
            static_cast<std::uint64_t>(file->position) + requested > std::numeric_limits<FSIZE_t>::max())
        {
            return FR_DISK_ERR;
        }

        const std::size_t end = static_cast<std::size_t>(file->position) + requested;

        if (found->second.size() < end)
        {
            found->second.resize(end);
        }

        if (requested != 0U)
        {
            std::memcpy(found->second.data() + file->position, source, requested);
        }

        file->position = static_cast<FSIZE_t>(end);
        file->size = std::max(file->size, file->position);
        file->data = found->second.data();
        *actual = requested;
        return FR_OK;
    }

    FRESULT f_sync(FIL* file)
    {
        return file != nullptr && file->opened && (file->mode & FA_WRITE) != 0U ? FR_OK : FR_DISK_ERR;
    }

    FRESULT f_stat(const char* path, FILINFO* info)
    {
        if (!mounted || path == nullptr || info == nullptr)
        {
            return FR_DISK_ERR;
        }

        const auto found = files.find(path);

        if (found == files.end())
        {
            return FR_NO_FILE;
        }

        *info = {};
        info->fsize = static_cast<FSIZE_t>(found->second.size());
        return FR_OK;
    }

    FRESULT f_rename(const char* old_path, const char* new_path)
    {
        if (!mounted || old_path == nullptr || new_path == nullptr || files.contains(new_path))
        {
            return FR_DISK_ERR;
        }

        const auto found = files.find(old_path);

        if (found == files.end())
        {
            return FR_NO_FILE;
        }

        files.emplace(new_path, std::move(found->second));
        files.erase(found);
        return FR_OK;
    }

    FRESULT f_opendir(DIR* directory, const char* path)
    {
        auto& faults = directory_faults();
        ++faults.open_attempts;
        faults.last_path = path == nullptr ? "<null>" : path;

        if (!mounted || path == nullptr || faults.fail_open || faults.last_path != "2:/WG")
        {
            return faults.fail_open ? FR_DISK_ERR : FR_NO_PATH;
        }

        directory->opened = 1;
        directory->position = 0U;
        directory->context = nullptr;
        ++faults.successful_opens;
        ++faults.open_directories;
        return FR_OK;
    }

    FRESULT f_readdir(DIR* directory, FILINFO* info)
    {
        auto& faults = directory_faults();
        ++faults.read_calls;

        if (directory == nullptr || info == nullptr || directory->opened == 0)
        {
            return FR_INVALID_OBJECT;
        }

        if (faults.reads_before_failure.has_value())
        {
            if (*faults.reads_before_failure == 0U)
            {
                return FR_DISK_ERR;
            }

            --*faults.reads_before_failure;
        }

        *info = {};

        if (directory->position >= directory_entries.size())
        {
            return FR_OK;
        }

        const auto& entry = directory_entries[directory->position++];
        info->fsize = entry.size;
        info->fattrib = entry.attributes;
        const auto copy_name = [](char* destination, std::size_t capacity, const std::string& source)
        {
            if (source.size() < capacity)
            {
                std::memcpy(destination, source.data(), source.size());
                destination[source.size()] = '\0';
            }
        };

        copy_name(info->fname, sizeof(info->fname), entry.name);
        copy_name(info->altname, sizeof(info->altname), entry.alias);
        return FR_OK;
    }

    FRESULT f_closedir(DIR* directory)
    {
        auto& faults = directory_faults();
        ++faults.close_calls;

        if (directory == nullptr || directory->opened == 0)
        {
            return FR_INVALID_OBJECT;
        }

        directory->opened = 0;

        if (faults.open_directories != 0U)
        {
            --faults.open_directories;
        }

        return faults.fail_close ? FR_DISK_ERR : FR_OK;
    }

    bool WFG_MediaIsLocal()
    {
        return media_local;
    }

    // Answers as waveform_msc.c does for the media snapshot the stub below reports.
    wfg_media_local_request_result WFG_MediaRequestLocal()
    {
        const auto media = WFG_MediaGetSnapshot();

        if ((media.msc_host_hold & kWFG_MediaHostHoldWriting) != 0U)
        {
            return kWFG_MediaLocalHostWriting;
        }

        if ((media.msc_host_hold & kWFG_MediaHostHoldRemovalPrevented) != 0U)
        {
            return kWFG_MediaLocalHostLocked;
        }

        if (media.state != kWFG_MediaHost && media.state != kWFG_MediaLocal)
        {
            return kWFG_MediaLocalFailed;
        }

        media_local = true;
        return kWFG_MediaLocalGranted;
    }

    wfg_status WFG_MediaSetHost()
    {
        media_local = false;
        return kWFG_StatusOk;
    }

    wfg_media_host_request_result_t WFG_MediaRequestHost()
    {
        media_local = false;
        return kWFG_MediaHostRequestReady;
    }

    wfg_media_snapshot_t WFG_MediaGetSnapshot()
    {
        wfg_media_snapshot_t idle{};
        idle.state = media_local ? 3U : 2U;
        return status_seam().media.value_or(idle);
    }

    wfg_status m110_usb_hardware_initialize()
    {
        ++usb_bring_up().hardware_starts;
        return usb_bring_up().hardware;
    }

    wfg_status m110_tinyusb_stack_initialize()
    {
        return usb_bring_up().stack;
    }

    void m110_tinyusb_task(std::uint32_t)
    {
        if (usb_task_run() != UsbTaskRun::none)
        {
            end_usb_task_run(usb_task_run() == UsbTaskRun::stack_starts, "the USB task ran TinyUSB's task");
        }
    }

    void m110_tinyusb_cdc_flush() {}

    bool m110_tinyusb_cdc_connected()
    {
        return cdc_connected;
    }

    std::uint32_t m110_tinyusb_cdc_available()
    {
        return static_cast<std::uint32_t>(rx.size());
    }

    std::uint32_t m110_tinyusb_cdc_read(void* buffer, std::uint32_t size)
    {
        const auto count = std::min<std::uint32_t>(size, static_cast<std::uint32_t>(rx.size()));
        auto* output = static_cast<char*>(buffer);

        for (std::uint32_t index = 0U; index < count; ++index)
        {
            output[index] = rx.front();
            rx.pop_front();
        }

        return count;
    }

    std::uint32_t m110_tinyusb_cdc_write(const void* buffer, std::uint32_t size)
    {
        const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(size, write_budget));
        tx.append(static_cast<const char*>(buffer), count);
        write_budget -= count;
        return count;
    }
}

namespace m110
{
std::uint32_t platform_cycle_counter() noexcept
{
    cycles += 100U;
    return cycles;
}

std::uint32_t platform_cycle_counter_frequency_hz() noexcept
{
    return 996000000U;
}
} // namespace m110

namespace m110::imxrt1170
{
Wm8960Codec& wm8960_codec() noexcept
{
    static Wm8960Codec codec;
    return codec;
}

Status Wm8960Codec::configure(const CodecConfig& config, CodecBlockHook hook, void* context) noexcept
{
    configured_ = true;
    frames_ = config.frames_per_block;
    audio_hook = hook;
    audio_context = context;
    return Status::success();
}

Status Wm8960Codec::start() noexcept
{
    running_ = true;
    return Status::success();
}

Status Wm8960Codec::stop() noexcept
{
    running_ = false;
    return Status::success();
}

Result<std::uint8_t> Wm8960Codec::set_transmit_level_percent(std::uint8_t value) noexcept
{
    return value;
}
} // namespace m110::imxrt1170

namespace waveform_generator
{
bool tx_artifact_busy() noexcept
{
    return status_seam().tx_artifact_busy;
}

m110::Status submit_tx_request(const tx_protocol::Request&) noexcept
{
    return refusal::busy;
}

bool take_tx_reply(tx_protocol::Reply&) noexcept
{
    return false;
}

TxArtifactSnapshot tx_artifact_snapshot() noexcept
{
    return status_seam().tx_artifact;
}

void service_tx_artifact() noexcept {}

void abort_tx_artifact() noexcept {}

bool tx_usb_active() noexcept
{
    return false;
}

bool tx_usb_blocked() noexcept
{
    return false;
}

void tx_usb_feed(std::uint8_t) noexcept {}

void tx_usb_service() noexcept {}

void tx_usb_disconnect() noexcept {}
} // namespace waveform_generator

TEST_CASE("Player and USB host lifecycle")
{
    test_owner_lifecycle();
    test_usb_service();
    test_start_usb_service();
    test_request_corpus();
    test_output_route();
    test_play_refusals();
    test_play_of_a_refused_wav();
    test_play_with_card_faults();
    test_mailbox_refusal();
    test_status_reports_eject();
    test_media_local_waits_for_host();
    test_usb_play_needs_a_stream();
    test_usb_play_is_exact();
    test_usb_startup_skips_are_the_streams();
    test_card_listing_reader();
    test_card_listing_player_protocol();
    test_prepare_player_protocol();
    test_prepare_playback_protocol();
    test_prepare_playback_refusals_and_stop();
    test_prepare_status_reply_budget();
    test_prepared_playback_status_reply_budget();
    test_card_listing_reply_budget();
    test_usb_stop_mid_play();
    test_usb_stream_closed_mid_play();
    test_usb_stream_closed_while_draining();
    test_usb_stream_closed_after_delivery();
    test_usb_stall();
    test_usb_skip_after_stop();
    test_usb_stall_after_stop();
    test_usb_stall_after_stop_before_unconfirmed();
    test_usb_short_completions();
    test_usb_lost_packet();
    test_usb_mismatch_after_the_window();
    test_usb_drain_unconfirmed();
    test_usb_stop_unconfirmed();
    test_usb_stall_before_unconfirmed();
    test_usb_gap_within_the_timeout();
    test_usb_gap_past_the_timeout();
    test_usb_diagnostics_query();
    test_usb_diagnostics_play_device_late();
    test_usb_diagnostics_report_msc_transfers();
    test_usb_stream_diagnostics();
    test_usb_play_restarts_the_statistics();
    test_codec_route_clears_the_usb_play();
    test_usb_play_after_a_stall();
    test_codec_play_after_a_stall();
    test_codec_drain_outside_the_watch();
    test_usb_play_16_bit();
    test_usb_format_switch_mid_play();
    test_usb_format_of_a_24_bit_play();
    test_usb_play_16_bit_mono();
    test_cut_command();
    test_usb_play_with_cuts();
    test_drop_runs_within_the_fifo_margin();
    test_usb_cuts_part_way();
    test_usb_play_without_cuts_is_unchanged();
    test_cut_latching();
    test_codec_play_with_cuts();
    test_scenario_plays_with_cuts();
    test_watterson_player();
    test_frequency_shift_player();
    test_wideband_player();
    test_pulsed_cw_player();
    test_swept_cw_player();
    test_markov_cw_player();
    test_fsk_cw_player();
    test_faded_cw_player();
    test_stream_identity_player();
    test_second_source_player();
    test_engine_reasons();
    test_live_fault_reasons();
    test_live_start_refusals();
    test_live_positions_run_through_drops();
    test_live_control_refusals();
    test_image_a_cut_rows();
    test_usb_diagnostics_cut_order();
    test_playlist_refusals();
    test_playlist_primary_close_failures();
    test_playlist_battery();
    test_playlist_stop();
    test_playlist_stop_then_stall();
    test_playlist_error_policy();
    test_playlist_ring_full();
    test_playlist_route_switching();
    test_watterson_playlist();
    test_playlist_items_own_settings();
    test_playlist_ignores_the_wav_scenario();
    test_playlist_attenuation();
    test_playlist_looped_items();
    test_playlist_loop_anomalies();
    test_playlist_loop_late_reads();
    test_sd_bytes_read_holds_at_its_maximum();
    test_playlist_gap_on_a_stalled_stream();
    test_playlist_gap_follows_the_stream();
    test_playlist_no_stream_drops_a_pending_cut();
    test_playlist_settings_end_with_it();
    test_playlist_holds_the_player();
    test_playlist_status_contract();
    test_status_contract();
}

int main(int argc, char** argv)
{
    const std::span<char* const> args{argv, static_cast<std::size_t>(argc)};
    constexpr std::string_view usb_task_flag{"--usb-task"};
    constexpr std::size_t usb_task_args = 3U;

    if (args.size() == usb_task_args && std::string_view{args[1]} == usb_task_flag)
    {
        return run_usb_task(args[2]);
    }

    Catch::Session session;
    return session.run(argc, argv);
}
