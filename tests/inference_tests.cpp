#include "transcribe/inference.hpp"
#include "transcribe/decoding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <chrono>
#include <algorithm>

namespace {
transcribe::AudioBuffer model_tone(std::uint32_t rate = 22050) {
    transcribe::AudioBuffer audio{rate, std::vector<float>(rate)};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(.4 * std::sin(2 * std::numbers::pi * 440 * i / rate));
    }
    return audio;
}
void same_notes(const std::vector<transcribe::Note> &a, const std::vector<transcribe::Note> &b);

namespace {
struct StreamFixture {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("transcribe-stream-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
         ".wav");
    StreamFixture(std::uint32_t rate, std::size_t frames, bool positions = true) {
        std::ofstream file(path, std::ios::binary);
        const auto integer = [&](std::uint32_t value, int bytes) {
            for (int i = 0; i < bytes; ++i)
                file.put(static_cast<char>((value >> (i * 8)) & 255));
        };
        file.write("RIFF", 4);
        integer(36 + frames * 4, 4);
        file.write("WAVEfmt ", 8);
        integer(16, 4);
        integer(3, 2);
        integer(1, 2);
        integer(rate, 4);
        integer(rate * 4, 4);
        integer(4, 2);
        integer(32, 2);
        file.write("data", 4);
        integer(frames * 4, 4);
        for (std::size_t i = 0; i < frames; ++i) {
            const float value =
                positions ? static_cast<float>(i) / 1000000
                          : static_cast<float>(.4 * std::sin(2 * std::numbers::pi * 440 * i / rate));
            const float encoded = positions ? value : std::round(value * 32768) / 32768;
            file.write(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
        }
        REQUIRE(file.good());
    }
    ~StreamFixture() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

class WindowBackend final : public transcribe::ModelBackend {
  public:
    transcribe::ModelMetadata info;
    std::vector<transcribe::Note> truth;
    std::size_t calls{};
    std::size_t largest{};
    bool fail{};
    WindowBackend() { info.definition.contract = transcribe::ModelContract::Spectral; }
    const transcribe::ModelMetadata &metadata() const noexcept override { return info; }
    transcribe::ModelOperation load(const transcribe::ModelLoadOptions &) override { return {}; }
    void release() noexcept override {}
    transcribe::ModelOperation warm_up(const transcribe::InferenceRequest &) override { return {}; }
    transcribe::ModelResources resources() const noexcept override {
        transcribe::ModelResources value;
        value.loaded = true;
        return value;
    }
    transcribe::BackendDiagnostics diagnostics() const override { return {}; }
    transcribe::InferenceResult<transcribe::InferenceOutput>
    infer(const transcribe::AudioBuffer &audio, const transcribe::InferenceRequest &request) override {
        ++calls;
        largest = std::max(largest, audio.samples.size() * sizeof(float));
        if (fail)
            return transcribe::InferenceError{transcribe::InferenceErrorCode::Inference,
                                              "Injected model failure"};
        if (request.progress) {
            request.progress(0);
            request.progress(.5);
            request.progress(1);
        }
        transcribe::InferenceOutput output;
        const double origin =
            std::llround(audio.samples.front() * 1000000.0) / static_cast<double>(audio.sample_rate);
        const double end = origin + static_cast<double>(audio.samples.size()) / audio.sample_rate;
        for (auto note : truth) {
            const double start = std::max(note.start_seconds, origin);
            const double stop = std::min(note.start_seconds + note.duration_seconds, end);
            if (stop <= start)
                continue;
            note.start_seconds = start - origin;
            note.duration_seconds = stop - start;
            output.notes.push_back(note);
        }
        if (request.analysis.evidence_callback) {
            transcribe::FrameEvidenceTrack track;
            track.frames.push_back({69, 0, 1, 1, 1});
            request.analysis.evidence_callback(track);
        }
        return output;
    }
};
} // namespace

TEST_CASE("bounded file inference stitches sustained repeated overlapping and edge notes",
          "[inference][stream]") {
    for (auto rate : {8000u, 11025u, 22050u}) {
        for (double length : {2.0, 2.25, 5.0, 5.125}) {
            const auto frames = static_cast<std::size_t>(rate * length);
            length = static_cast<double>(frames) / rate;
            StreamFixture fixture(rate, frames);
            WindowBackend backend;
            backend.truth = {{60, 0, length}, {64, .3, .2},  {67, 1.4, .7},
                             {69, 2, .2},     {69, 2.3, .2}, {72, length - .05, .05}};
            std::erase_if(backend.truth, [&](const auto &n) { return n.start_seconds >= length; });
            for (auto &n : backend.truth)
                n.duration_seconds = std::min(n.duration_seconds, length - n.start_seconds);
            std::sort(backend.truth.begin(), backend.truth.end(),
                      [](const auto &a, const auto &b) { return a.start_seconds < b.start_seconds; });
            transcribe::FileInferenceOptions options;
            options.chunk_seconds = 2;
            options.overlap_seconds = .5;
            transcribe::InferenceRequest request;
            double progress = 0;
            request.progress = [&](double value) {
                REQUIRE(value >= progress);
                progress = value;
            };
            std::vector<double> evidence_times;
            request.analysis.evidence_callback = [&](const auto &track) {
                evidence_times.push_back(track.frames.front().time_seconds);
            };
            auto result = transcribe::infer_wav_file(backend, fixture.path, request, options);
            REQUIRE(std::holds_alternative<transcribe::FileInferenceResult>(result));
            const auto &output = std::get<transcribe::FileInferenceResult>(result);
            REQUIRE(output.notes.size() == backend.truth.size());
            for (std::size_t i = 0; i < output.notes.size(); ++i) {
                REQUIRE(output.notes[i].midi_pitch == backend.truth[i].midi_pitch);
                REQUIRE(std::abs(output.notes[i].start_seconds - backend.truth[i].start_seconds) < 1e-6);
                REQUIRE(std::abs(output.notes[i].duration_seconds - backend.truth[i].duration_seconds) <
                        1e-6);
            }
            REQUIRE(progress == 1);
            REQUIRE(output.source_frames == frames);
            REQUIRE(output.peak_buffered_audio_bytes <= rate * 2 * sizeof(float));
            REQUIRE(backend.largest == output.peak_buffered_audio_bytes);
            REQUIRE(evidence_times.front() == 0);
            if (length > 2)
                REQUIRE(evidence_times[1] ==
                        (rate * 2 - std::llround(rate * .5)) / static_cast<double>(rate));
            std::size_t emitted = 0;
            options.retain_notes = false;
            options.notes_callback = [&](auto notes) { emitted += notes.size(); };
            auto repeated = transcribe::infer_wav_file(backend, fixture.path, {}, options);
            REQUIRE(std::get<transcribe::FileInferenceResult>(repeated).notes.empty());
            REQUIRE(emitted == backend.truth.size());
        }
    }
}

TEST_CASE("bounded file inference cancellation failures and budgets permit safe reuse",
          "[inference][stream]") {
    StreamFixture fixture(8000, 40000);
    WindowBackend backend;
    backend.truth = {{60, .1, 4}, {64, .2, 4}};
    transcribe::FileInferenceOptions options;
    options.chunk_seconds = 2;
    options.overlap_seconds = .5;
    std::stop_source stop;
    transcribe::InferenceRequest request;
    request.cancellation = stop.get_token();
    stop.request_stop();
    auto result = transcribe::infer_wav_file(backend, fixture.path, request, options);
    REQUIRE(std::get<transcribe::InferenceError>(result).code == transcribe::InferenceErrorCode::Cancelled);
    REQUIRE(backend.calls == 0);
    stop = std::stop_source{};
    request.cancellation = stop.get_token();
    request.progress = [&](double value) {
        if (value > .1)
            stop.request_stop();
    };
    result = transcribe::infer_wav_file(backend, fixture.path, request, options);
    REQUIRE(std::get<transcribe::InferenceError>(result).code == transcribe::InferenceErrorCode::Cancelled);
    options.maximum_audio_bytes = 1;
    result = transcribe::infer_wav_file(backend, fixture.path, {}, options);
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(result));
    options.maximum_audio_bytes = 1024 * 1024;
    options.maximum_pending_notes = 1;
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(
        transcribe::infer_wav_file(backend, fixture.path, {}, options)));
    options.maximum_pending_notes = 4096;
    options.maximum_retained_notes = 1;
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(
        transcribe::infer_wav_file(backend, fixture.path, {}, options)));
    options.maximum_retained_notes = 100;
    options.audio_callback = [](const auto &) { throw std::runtime_error("Injected callback failure"); };
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(
        transcribe::infer_wav_file(backend, fixture.path, {}, options)));
    options.audio_callback = {};
    backend.fail = true;
    REQUIRE(
        std::get<transcribe::InferenceError>(transcribe::infer_wav_file(backend, fixture.path, {}, options))
            .message == "Injected model failure");
    backend.fail = false;
    REQUIRE(std::holds_alternative<transcribe::FileInferenceResult>(
        transcribe::infer_wav_file(backend, fixture.path, {}, options)));
    backend.info.definition.contract = transcribe::ModelContract::LegacyPitchV1;
    REQUIRE(
        std::get<transcribe::InferenceError>(transcribe::infer_wav_file(backend, fixture.path, {}, options))
            .code == transcribe::InferenceErrorCode::Unsupported);
}

TEST_CASE("owned inference result cache is opt-in bounded exact and configuration sensitive",
          "[inference][cache]") {
    transcribe::ModelRegistry registry;
    for (const auto &definition : registry.discover()) {
        auto backend =
            std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(registry.create(definition.id)));
        REQUIRE(std::holds_alternative<std::monostate>(backend->load({})));
        transcribe::InferenceRequest request;
        request.reuse_cached_result = true;
        auto audio = model_tone();
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(audio, request)));
        REQUIRE(backend->resources().cached_bytes == 0);
        transcribe::ModelLoadOptions load;
        load.maximum_result_cache_bytes = 1024 * 1024;
        REQUIRE(std::holds_alternative<std::monostate>(backend->load(load)));
        const auto first = std::get<transcribe::InferenceOutput>(backend->infer(audio, request));
        REQUIRE_FALSE(backend->resources().result_cache_hit);
        REQUIRE(backend->resources().cached_bytes <= load.maximum_result_cache_bytes);
        const auto second = std::get<transcribe::InferenceOutput>(backend->infer(audio, request));
        REQUIRE(backend->resources().result_cache_hit);
        same_notes(first.notes, second.notes);
        request.analysis.minimum_confidence = .35;
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(audio, request)));
        REQUIRE_FALSE(backend->resources().result_cache_hit);
        audio.samples[500] += .01f;
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(audio, request)));
        REQUIRE_FALSE(backend->resources().result_cache_hit);
        std::stop_source stop;
        stop.request_stop();
        request.cancellation = stop.get_token();
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(audio, request)).code ==
                transcribe::InferenceErrorCode::Cancelled);
        request.cancellation = {};
        request.retain_evidence = true;
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(audio, request)));
        REQUIRE_FALSE(backend->resources().result_cache_hit);
        request.retain_evidence = false;
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(audio, request)));
        REQUIRE(backend->resources().result_cache_hit);
        backend->release();
        REQUIRE(backend->resources().cached_bytes == 0);
    }
}

TEST_CASE("bounded tracker inference retains a sustained tone across windows and reuses safely",
          "[inference][stream]") {
    StreamFixture fixture(11025, 55125, false);
    transcribe::ModelRegistry registry;
    for (const auto &definition : registry.discover()) {
        auto backend =
            std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(registry.create(definition.id)));
        REQUIRE(std::holds_alternative<std::monostate>(backend->load({})));
        transcribe::FileInferenceOptions options;
        options.chunk_seconds = 2;
        options.overlap_seconds = .5;
        const auto first = transcribe::infer_wav_file(*backend, fixture.path, {}, options);
        REQUIRE(std::holds_alternative<transcribe::FileInferenceResult>(first));
        const auto &notes = std::get<transcribe::FileInferenceResult>(first).notes;
        REQUIRE(std::count_if(notes.begin(), notes.end(), [](const auto &n) { return n.midi_pitch == 69; }) ==
                1);
        const auto tone =
            std::find_if(notes.begin(), notes.end(), [](const auto &n) { return n.midi_pitch == 69; });
        REQUIRE(tone->duration_seconds > 4.5);
        const auto second = transcribe::infer_wav_file(*backend, fixture.path, {}, options);
        same_notes(notes, std::get<transcribe::FileInferenceResult>(second).notes);
    }
}

TEST_CASE("bounded Basic Pitch windows keep one session and deterministic final padding",
          "[inference][stream][onnx]") {
#ifdef TRANSCRIBE_BASIC_PITCH_MODEL_PATH
    StreamFixture fixture(8000, 41000, false);
    transcribe::ModelRegistry registry(TRANSCRIBE_BASIC_PITCH_MODEL_PATH);
    auto backend = std::move(
        std::get<std::unique_ptr<transcribe::ModelBackend>>(registry.create("onnx:basic-pitch-v1")));
    REQUIRE(std::holds_alternative<std::monostate>(backend->load({})));
    transcribe::FileInferenceOptions options;
    options.chunk_seconds = 2;
    options.overlap_seconds = .5;
    auto first = transcribe::infer_wav_file(*backend, fixture.path, {}, options);
    REQUIRE(std::holds_alternative<transcribe::FileInferenceResult>(first));
    const auto &output = std::get<transcribe::FileInferenceResult>(first);
    REQUIRE_FALSE(output.notes.empty());
    REQUIRE(backend->resources().live_sessions == 1);
    REQUIRE(backend->resources().completed_inferences == output.processed_windows);
    auto second = transcribe::infer_wav_file(*backend, fixture.path, {}, options);
    REQUIRE(std::holds_alternative<transcribe::FileInferenceResult>(second));
    same_notes(output.notes, std::get<transcribe::FileInferenceResult>(second).notes);
    backend->release();
    REQUIRE(backend->resources().live_sessions == 0);
#else
    SKIP("ONNX is disabled");
#endif
}
void same_notes(const std::vector<transcribe::Note> &a, const std::vector<transcribe::Note> &b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].midi_pitch == b[i].midi_pitch);
        REQUIRE(a[i].start_seconds == b[i].start_seconds);
        REQUIRE(a[i].duration_seconds == b[i].duration_seconds);
        REQUIRE(a[i].confidence == b[i].confidence);
    }
}
}

TEST_CASE("truncated WAV is rejected before advertising its declared allocation", "[inference][stream]") {
        StreamFixture fixture(8000, 40000);
        {
            std::fstream file(fixture.path, std::ios::in | std::ios::out | std::ios::binary);
            file.seekp(40);
            file.write("\xfc\xff\xff\xff", 4);
            REQUIRE(file.good());
        }
        std::size_t blocks = 0;
        REQUIRE_THROWS(transcribe::read_wav_blocks(fixture.path, 4096, [&](const auto&) { ++blocks; }));
        REQUIRE(blocks == 0);
        REQUIRE_THROWS(transcribe::read_wav(fixture.path));
}

TEST_CASE("model registry versions discovery and custom factories are explicit", "[inference]") {
    transcribe::ModelRegistry registry;
    REQUIRE(registry.discover().size() == 2);
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(registry.create("missing")));
    auto definition = registry.discover().front();
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(registry.add(definition, {})));
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(
        registry.add(definition, [](const auto&) { return std::unique_ptr<transcribe::ModelBackend>{}; })));
    definition.id = "test:null";
    REQUIRE(std::holds_alternative<std::monostate>(
        registry.add(definition, [](const auto&) { return std::unique_ptr<transcribe::ModelBackend>{}; })));
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(registry.create(definition.id)));
    definition.id = "test:unsupported";
    definition.contract_version = 2;
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(
        registry.add(definition, [](const auto&) { return std::unique_ptr<transcribe::ModelBackend>{}; })));
    definition = {1, "bad-onnx", transcribe::ModelContract::LegacyPitchV1, "missing", "", ""};
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(registry.add_onnx(definition)));
}

TEST_CASE("built-in model adapters preserve notes own lifetimes and report cancellation", "[inference]") {
    transcribe::ModelRegistry registry;
    const auto audio = model_tone();
    for (const auto& definition : registry.discover()) {
        auto created = registry.create(definition.id);
        REQUIRE(std::holds_alternative<std::unique_ptr<transcribe::ModelBackend>>(created));
        auto backend = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(audio)).code ==
                transcribe::InferenceErrorCode::NotLoaded);
        REQUIRE(std::holds_alternative<std::monostate>(backend->load()));
        REQUIRE(backend->metadata().definition.contract_version == 1);
        REQUIRE(backend->metadata().minimum_midi ==
            (definition.contract == transcribe::ModelContract::Yin ? 36 : 28));
        REQUIRE(backend->metadata().maximum_midi ==
            (definition.contract == transcribe::ModelContract::Yin ? 83 : 96));
        REQUIRE_FALSE(backend->metadata().incremental);
        REQUIRE(backend->resources().loaded);
        REQUIRE_FALSE(backend->resources().resident_bytes);
        transcribe::InferenceRequest request;
        request.analysis.polyphonic = definition.contract == transcribe::ModelContract::Spectral;
        request.retain_evidence = true;
        double progress = 0;
        request.progress = [&](double value) {
            REQUIRE(value >= progress);
            REQUIRE(value <= 1);
            progress = value;
        };
        auto result = backend->infer(audio, request);
        REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(result));
        const auto& output = std::get<transcribe::InferenceOutput>(result);
        same_notes(output.notes, transcribe::transcribe_monophonic(audio, request.analysis));
        REQUIRE_FALSE(output.evidence.empty());
        REQUIRE(progress == 1);
        REQUIRE(backend->resources().last_input_bytes == audio.samples.size() * sizeof(float));
        REQUIRE(backend->resources().last_evidence_frames > 0);
        REQUIRE(backend->resources().completed_inferences == 1);
        REQUIRE(std::get<transcribe::InferenceError>(
            backend->infer_incremental(audio, true)).code == transcribe::InferenceErrorCode::Unsupported);
        request.progress = {};
        REQUIRE(std::holds_alternative<std::monostate>(backend->warm_up(request)));
        std::stop_source stop;
        stop.request_stop();
        request.cancellation = stop.get_token();
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(audio, request)).code ==
                transcribe::InferenceErrorCode::Cancelled);
        stop = {};
        request.cancellation = stop.get_token();
        request.progress = [&](double) { stop.request_stop(); };
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(audio, request)).code ==
                transcribe::InferenceErrorCode::Cancelled);
        request = {};
        auto invalid = audio;
        invalid.sample_rate = 1999;
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(invalid)).code ==
                transcribe::InferenceErrorCode::InvalidArgument);
        invalid = audio;
        invalid.samples[0] = std::numeric_limits<float>::infinity();
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(invalid)).code ==
                transcribe::InferenceErrorCode::InvalidArgument);
        request.analysis.minimum_confidence = std::numeric_limits<double>::quiet_NaN();
        REQUIRE(std::get<transcribe::InferenceError>(backend->infer(audio, request)).code ==
                transcribe::InferenceErrorCode::InvalidArgument);
        backend->release();
        backend->release();
        REQUIRE_FALSE(backend->resources().loaded);
        REQUIRE(backend->resources().live_sessions == 0);
        REQUIRE_FALSE(backend->diagnostics().active_provider);
        transcribe::ModelLoadOptions preview;
        preview.fast_preview = true;
        REQUIRE(std::holds_alternative<std::monostate>(backend->load(preview)));
        REQUIRE(backend->metadata().window_samples ==
            (definition.contract == transcribe::ModelContract::Spectral ? 2048 : 1024));
        preview.provider = transcribe::OnnxExecutionProvider::Cuda;
        const auto failed = backend->load(preview);
        REQUIRE(std::holds_alternative<transcribe::InferenceError>(failed));
        REQUIRE_FALSE(backend->resources().loaded);
        REQUIRE(backend->diagnostics().requested_provider == transcribe::OnnxExecutionProvider::Cuda);
        REQUIRE_FALSE(backend->diagnostics().active_provider);
    }
}

TEST_CASE("ONNX model discovery and unavailable providers never claim a fallback", "[inference]") {
    transcribe::ModelRegistry registry;
    transcribe::ModelDefinition definition{1, "test:onnx", transcribe::ModelContract::BasicPitchV1,
#ifdef TRANSCRIBE_BASIC_PITCH_MODEL_PATH
        TRANSCRIBE_BASIC_PITCH_MODEL_PATH,
#else
        "missing.onnx",
#endif
        "Spotify Basic Pitch", "Apache-2.0"};
    REQUIRE(std::holds_alternative<std::monostate>(registry.add_onnx(definition)));
    auto created = registry.create(definition.id);
    auto backend = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
    const auto providers = transcribe::inference_providers();
    REQUIRE(providers.size() == 4);
    for (const auto& provider : providers) {
        if (provider.compiled && provider.runtime_available) continue;
        transcribe::ModelLoadOptions options;
        options.provider = provider.provider;
        const auto result = backend->load(options);
        REQUIRE(std::holds_alternative<transcribe::InferenceError>(result));
        REQUIRE_FALSE(backend->resources().loaded);
        REQUIRE_FALSE(backend->diagnostics().active_provider);
        REQUIRE_FALSE(std::get<transcribe::InferenceError>(result).message.empty());
    }
#ifdef TRANSCRIBE_ENABLE_ONNX
    REQUIRE(std::holds_alternative<std::monostate>(backend->load()));
    REQUIRE(backend->resources().live_sessions == 1);
    REQUIRE(backend->metadata().inputs.size() == 1);
    REQUIRE(backend->metadata().inputs.front().dimensions == std::vector<std::int64_t>{-1,43844,1});
    REQUIRE(backend->metadata().outputs.size() == 3);
    REQUIRE(backend->metadata().batch_size == 1);
    REQUIRE(backend->metadata().overlap_samples == 7680);
    REQUIRE(backend->metadata().output_frame_step_samples == 256);
    REQUIRE(backend->metadata().hop_samples == 36164);
    REQUIRE(backend->metadata().heads.size() == 3);
    for (const auto& head : backend->metadata().heads) {
        REQUIRE(head.available);
        REQUIRE(head.frame_step_samples == 256);
        REQUIRE(head.sample_rate == 22050);
    }
    transcribe::InferenceRequest request;
    request.onnx.polyphonic = true;
    request.onnx.independent_probabilities = true;
    request.retain_evidence = true;
    auto inferred = backend->infer(model_tone(), request);
    REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(inferred));
    same_notes(std::get<transcribe::InferenceOutput>(inferred).notes,
               transcribe::transcribe_with_onnx(model_tone(), definition.path, request.onnx));
    REQUIRE(backend->resources().live_sessions == 1);
    REQUIRE(std::holds_alternative<std::monostate>(backend->warm_up(request)));
    auto invalid_request = request;
    invalid_request.onnx.polyphonic_threshold = 1.1;
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(model_tone(), invalid_request)).code ==
            transcribe::InferenceErrorCode::InvalidArgument);
    invalid_request = request;
    invalid_request.onnx.execution_provider = transcribe::OnnxExecutionProvider::Cuda;
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(model_tone(), invalid_request)).code ==
            transcribe::InferenceErrorCode::InvalidArgument);
    std::stop_source stop;
    request.cancellation = stop.get_token();
    request.progress = [&](double value) { if (value > .5) stop.request_stop(); };
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(model_tone(), request)).code ==
            transcribe::InferenceErrorCode::Cancelled);
    request.cancellation = {};
    request.progress = {};
    REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(model_tone(), request)));
    request.onnx.note_decoder = transcribe::NeuralNoteDecoder::MultiHeadEvents;
    inferred = backend->infer(model_tone(), request);
    REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(inferred));
    const auto& event_output = std::get<transcribe::InferenceOutput>(inferred);
    REQUIRE(event_output.activations);
    REQUIRE_FALSE(event_output.activations->onsets.empty());
    REQUIRE_FALSE(event_output.evidence.front().contour_midi.empty());
    transcribe::TemporalDecodingOptions decoding;
    const auto canonical = transcribe::decode_note_events(
        model_tone(), event_output.evidence, decoding, "reference", "basic-pitch-v1");
    REQUIRE(event_output.notes.size() == canonical.events.size());
    for (std::size_t i = 0; i < event_output.notes.size(); ++i) {
        REQUIRE(event_output.notes[i].start_seconds == canonical.events[i].onset_seconds);
        REQUIRE(event_output.notes[i].duration_seconds ==
            canonical.events[i].offset_seconds - canonical.events[i].onset_seconds);
    }
    same_notes(event_output.notes, std::get<transcribe::InferenceOutput>(
        backend->infer(model_tone(), request)).notes);
    REQUIRE(backend->resources().live_sessions == 1);
    request.onnx.polyphonic = false;
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(model_tone(), request)).code ==
        transcribe::InferenceErrorCode::InvalidArgument);
    backend->release();
    REQUIRE(backend->resources().live_sessions == 0);
    definition.id = "test:wrong-contract";
    definition.contract = transcribe::ModelContract::LegacyPitchV1;
    REQUIRE(std::holds_alternative<std::monostate>(registry.add_onnx(definition)));
    created = registry.create(definition.id);
    backend = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(backend->load()));
#endif
}

TEST_CASE("adapter input limits reject work before inference and preserve initialized sessions", "[inference]") {
    transcribe::ModelRegistry registry;
    auto created = registry.create("builtin:yin");
    auto backend = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
    transcribe::ModelLoadOptions options;
    options.maximum_input_bytes = 4096;
    REQUIRE(std::holds_alternative<std::monostate>(backend->load(options)));
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(model_tone())).code ==
        transcribe::InferenceErrorCode::InvalidArgument);
    REQUIRE(backend->resources().completed_inferences == 0);
    REQUIRE(backend->resources().loaded);
    transcribe::AudioBuffer short_audio{22050, std::vector<float>(1024)};
    REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(backend->infer(short_audio)));
    transcribe::InferenceRequest request;
    request.onnx.note_decoder = transcribe::NeuralNoteDecoder::MultiHeadEvents;
    REQUIRE(std::get<transcribe::InferenceError>(backend->infer(short_audio, request)).code ==
        transcribe::InferenceErrorCode::InvalidArgument);
}
