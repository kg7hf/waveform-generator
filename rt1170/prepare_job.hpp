// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: 2026 Paul R. Decker

#pragma once

#include "common/pcm24_artifact.hpp"
#include "common/prepare_request.hpp"
#include "prepared_stream.hpp"

extern "C"
{
#include "ff.h"
}

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace signal_lab
{
class Engine;
struct Scenario;
class WattersonChannel;
} // namespace signal_lab

namespace waveform_generator
{
class Rt1170SecondaryWav;

// Incremental player-task-owned prepare/render/verify/publish job. Resources
// and all referenced signal state must remain alive until the job is no longer
// busy. No other task may use FatFs or the engine while a job is active.
class PrepareJob
{
public:
    struct Resources
    {
        signal_lab::Engine* engine{};
        signal_lab::WattersonChannel* watterson{};
        Rt1170SecondaryWav* secondary{};
        signal_lab::Scenario* scenario{};
        std::span<float> input;
        std::span<float> output;
        std::span<char> scenario_text;
        std::span<std::uint8_t> packed;
        // Separate from packed production staging; prefix reads may occur
        // between processing and appending that staged block.
        std::span<std::uint8_t> stream_read_scratch;
    };

    PrepareJob() noexcept = default;
    ~PrepareJob() = default;
    PrepareJob(const PrepareJob&) = delete;
    PrepareJob& operator=(const PrepareJob&) = delete;
    PrepareJob(PrepareJob&&) = delete;
    PrepareJob& operator=(PrepareJob&&) = delete;

    // Admission and fixed-field validation only. Filesystem work begins in step().
    [[nodiscard]] m110::Status begin(const prepare::Request& request, Resources resources) noexcept;
    // Managed admission leaves rendering under the transport owner's control.
    [[nodiscard]] m110::Status begin_managed(const prepare::Request& request, Resources resources) noexcept;
    // Advances one bounded I/O or render substep. Terminal snapshots remain available.
    [[nodiscard]] m110::Status step() noexcept;
    // Managed rendering has one decode call and one DSP/pack call per block.
    [[nodiscard]] m110::Status produce_step(std::uint32_t frames) noexcept;
    [[nodiscard]] m110::Status append_produced(std::uint32_t frames) noexcept;
    // Call after all produced frames have been appended and playback is drained.
    [[nodiscard]] m110::Status finish_managed() noexcept;
    [[nodiscard]] std::uint64_t managed_produced_frames() const noexcept { return managed_produced_frames_; }

    // Requests cancellation; cleanup advances through later step() calls.
    [[nodiscard]] m110::Status cancel() noexcept;

    // Player-owner-only access to the private growing file during rendering.
    // Neither method starts audio or publishes an ordinary playable WAV.
    [[nodiscard]] m110::Status commit_prefix() noexcept;
    // Destination belongs to the playback owner and must not alias engine or
    // production staging resources. Copy it to the FIFO before reusing it.
    [[nodiscard]] m110::Status read_prefix(std::uint64_t first_frame, std::span<std::uint8_t> output) noexcept;
    [[nodiscard]] std::uint64_t committed_frames() const noexcept { return output_open_ ? sink_.committed_frames() : 0U; }

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] prepare::Snapshot snapshot() const noexcept { return snapshot_; }

private:
    enum class Work : std::uint8_t;

    [[nodiscard]] m110::Status validate_admission(const prepare::Request& request, const Resources& resources) const noexcept;
    [[nodiscard]] m110::Status begin_internal(const prepare::Request& request, Resources resources, bool managed) noexcept;
    [[nodiscard]] m110::Status advance() noexcept;
    [[nodiscard]] m110::Status dispatch_work() noexcept;
    [[nodiscard]] m110::Status dispatch_secondary_work() noexcept;
    [[nodiscard]] m110::Status dispatch_output_work() noexcept;
    [[nodiscard]] m110::Status dispatch_verify_work() noexcept;
    [[nodiscard]] m110::Status dispatch_manifest_work() noexcept;
    [[nodiscard]] m110::Status open_source_step() noexcept;
    [[nodiscard]] m110::Status open_scenario_step() noexcept;
    [[nodiscard]] m110::Status parse_scenario_step() noexcept;
    [[nodiscard]] m110::Status configure_step() noexcept;
    [[nodiscard]] m110::Status finish_output_step() noexcept;
    [[nodiscard]] m110::Status close_secondary_step() noexcept;
    [[nodiscard]] m110::Status publish_step() noexcept;
    [[nodiscard]] m110::Status fail(m110::Status failure) noexcept;
    [[nodiscard]] m110::Status cleanup_step() noexcept;
    [[nodiscard]] m110::Status hash_source_step() noexcept;
    [[nodiscard]] m110::Status scenario_read_step() noexcept;
    [[nodiscard]] m110::Status path_check_step() noexcept;
    [[nodiscard]] m110::Status hash_secondary_step() noexcept;
    [[nodiscard]] m110::Status render_read_decode_step() noexcept;
    [[nodiscard]] m110::Status render_process_pack_step() noexcept;
    [[nodiscard]] m110::Status render_append_step() noexcept;
    [[nodiscard]] m110::Status verify_header_step() noexcept;
    [[nodiscard]] m110::Status verify_file_step() noexcept;
    [[nodiscard]] m110::Status manifest_create_step() noexcept;
    [[nodiscard]] m110::Status manifest_write_step() noexcept;
    [[nodiscard]] m110::Status close_work() noexcept;
    [[nodiscard]] m110::Status close_output() noexcept;
    [[nodiscard]] m110::Status close_source() noexcept;
    [[nodiscard]] m110::Status close_manifest() noexcept;
    [[nodiscard]] m110::Status unmount() noexcept;
    void detach_engine() noexcept;
    [[nodiscard]] static m110::Status read_source_callback(void* context, std::uint32_t offset, std::uint8_t* destination, std::uint32_t count) noexcept;

    prepare::Snapshot snapshot_{};
    signal_lab::Engine* engine_{};
    signal_lab::WattersonChannel* watterson_{};
    Rt1170SecondaryWav* secondary_{};
    signal_lab::Scenario* scenario_{};
    std::span<float> input_{};
    std::span<float> output_{};
    std::span<char> scenario_text_{};
    std::span<std::uint8_t> packed_{};
    std::span<std::uint8_t> stream_read_scratch_{};
    FATFS filesystem_{};
    FIL source_file_{};
    FIL work_file_{};
    FIL output_file_{};
    FIL manifest_file_{};
    artifact::Pcm24ArtifactWriter writer_{};
    Rt1170PreparedStream sink_{};
    artifact::Pcm24ArtifactSummary wav_summary_{};
    std::uint64_t source_file_bytes_{};
    std::uint64_t source_frames_{};
    std::uint64_t source_cursor_{};
    std::uint32_t render_block_input_frames_{};
    std::uint32_t render_block_output_frames_{};
    std::uint64_t scenario_file_bytes_{};
    std::uint64_t scenario_cursor_{};
    std::uint64_t secondary_file_bytes_{};
    std::uint64_t hash_cursor_{};
    std::uint64_t rendered_frames_{};
    std::uint64_t managed_produced_frames_{};
    std::uint64_t verified_file_bytes_{};
    std::uint64_t verified_pcm_bytes_{};
    std::uint64_t final_wav_bytes_{};
    std::uint32_t data_offset_{};
    std::uint32_t scenario_length_{};
    std::uint32_t manifest_length_{};
    std::uint32_t path_check_index_{};
    std::uint32_t manifest_cursor_{};
    std::uint32_t managed_request_frames_{};
    std::array<char, 19U> source_path_{};
    std::array<char, 19U> scenario_path_{};
    std::array<char, 19U> secondary_path_{};
    std::array<char, 19U> output_path_{};
    std::array<char, 19U> temporary_wav_path_{};
    std::array<char, 19U> final_manifest_path_{};
    std::array<char, 19U> temporary_manifest_path_{};
    std::array<char, 19U> companion_scenario_path_{};
    std::array<char, 65U> source_sha256_{};
    std::array<char, 65U> scenario_sha256_{};
    std::array<char, 65U> secondary_sha256_{};
    std::array<char, 65U> output_file_sha256_{};
    waveform_source::Sha256 source_digest_{};
    waveform_source::Sha256 scenario_digest_{};
    waveform_source::Sha256 secondary_digest_{};
    waveform_source::Sha256 verify_file_digest_{};
    waveform_source::Sha256 verify_pcm_digest_{};
    std::uint64_t output_frames_{};
    std::uint64_t engine_output_digest_{};
    Work work_{};
    prepare::Phase terminal_phase_{prepare::Phase::idle};
    m110::Status pending_failure_{};
    bool mounted_{};
    bool source_open_{};
    bool work_open_{};
    bool output_open_{};
    bool manifest_open_{};
    bool secondary_open_{};
    bool engine_configured_{};
    bool cancel_requested_{};
    bool manifest_published_{};
    bool has_secondary_{};
    bool managed_{};
};

} // namespace waveform_generator
