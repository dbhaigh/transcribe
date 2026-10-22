#include "transcribe/engine.hpp"
#include "../src/asio_transport.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
using namespace transcribe;

class NoteHeadOnlyBackend final : public ModelBackend {
public:
    explicit NoteHeadOnlyBackend(const ModelDefinition& definition) {
        metadata_.definition = definition;
        metadata_.polyphonic = true;
        metadata_.objective = PitchObjective::IndependentMultiPitch;
        metadata_.minimum_midi = 28;
        metadata_.maximum_midi = 96;
        metadata_.outputs.push_back({"StatefulPartitionedCall:1", "float32", {1, 172, 88}});
    }
    const ModelMetadata& metadata() const noexcept override { return metadata_; }
    ModelOperation load(const ModelLoadOptions&) override { loaded_ = true; return std::monostate{}; }
    void release() noexcept override { loaded_ = false; }
    InferenceResult<InferenceOutput> infer(const AudioBuffer&, const InferenceRequest&) override {
        return InferenceError{InferenceErrorCode::Unsupported, "Metadata-only test backend"};
    }
    ModelOperation warm_up(const InferenceRequest&) override {
        return InferenceError{InferenceErrorCode::Unsupported, "Metadata-only test backend"};
    }
    ModelResources resources() const noexcept override {
        ModelResources result;
        result.loaded = loaded_;
        return result;
    }
    BackendDiagnostics diagnostics() const override { return {}; }
private:
    ModelMetadata metadata_;
    bool loaded_{};
};

AudioBuffer fixture(bool chord = false) {
    AudioBuffer audio;
    audio.sample_rate = 8000;
    audio.samples.resize(12000);
    for (std::size_t i = 800; i < 10400; ++i) {
        const double t = static_cast<double>(i) / audio.sample_rate;
        audio.samples[i] = static_cast<float>(.35 * std::sin(2 * std::numbers::pi * 220 * t) +
            (chord ? .2 * std::sin(2 * std::numbers::pi * 330 * t) : 0));
    }
    return audio;
}

std::unique_ptr<TranscriptionEngine> engine(EngineRegistry& registry, const std::string& id) {
    auto created = registry.create(id);
    REQUIRE(std::holds_alternative<std::unique_ptr<TranscriptionEngine>>(created));
    return std::move(std::get<std::unique_ptr<TranscriptionEngine>>(created));
}

void success(const ModelOperation& result) {
    if (const auto* error = std::get_if<InferenceError>(&result)) INFO(error->message);
    REQUIRE(std::holds_alternative<std::monostate>(result));
}

void same_notes(const std::vector<Note>& left, const std::vector<Note>& right) {
    REQUIRE(left.size() == right.size());
    for (std::size_t i = 0; i < left.size(); ++i) {
        REQUIRE(left[i].midi_pitch == right[i].midi_pitch);
        REQUIRE(left[i].start_seconds == right[i].start_seconds);
        REQUIRE(left[i].duration_seconds == right[i].duration_seconds);
        REQUIRE(left[i].confidence == right[i].confidence);
    }
}

void structure(const EngineUpdate& update, const std::string& id, std::uint32_t rate) {
    REQUIRE(update.sequence > 0);
    REQUIRE(update.provenance.engine_id == id);
    REQUIRE(update.provenance.model_contract == model_contract_version);
    REQUIRE(update.provenance.audio_source_id == "test-recording");
    REQUIRE(update.sample_rate == rate);
    REQUIRE(update.finished);
    REQUIRE(update.events.size() == update.notes.size());
    for (const auto& event : update.events) {
        REQUIRE(event.finalized);
        REQUIRE(event.sample_rate == rate);
        REQUIRE(event.offset_frame);
        REQUIRE(*event.offset_frame >= event.onset_frame);
        REQUIRE(event.offset_seconds().value() >= event.onset_seconds());
        REQUIRE(event.provenance.engine_id == id);
        REQUIRE_FALSE(event.calibrated_confidence);
        REQUIRE(event.heuristic_strength);
    }
    if (update.activations) validate_pitch_activation_grid(*update.activations);
}
}

TEST_CASE("engine capabilities distinguish incremental YIN from windowed and unknown model objectives", "[engine]") {
    EngineRegistry registry;
    REQUIRE(registry.discover().size() == 2);
    auto selection = registry.select();
    REQUIRE(selection.selected_id == "builtin:yin");
    for (const auto& candidate : selection.candidates) {
        REQUIRE(candidate.available);
        REQUIRE_FALSE(candidate.explanation.empty());
    }
    EngineRequirements request;
    request.polyphonic = true;
    selection = registry.select(request);
    REQUIRE(selection.selected_id == "builtin:spectral");
    REQUIRE_FALSE(selection.candidates.front().available);
    REQUIRE_FALSE(selection.candidates.front().reasons.empty());
    request.streaming = EngineStreaming::Incremental;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.polyphonic = false;
    REQUIRE(registry.select(request).selected_id == "builtin:yin");
    request.activations = true;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.streaming = EngineStreaming::None;
    request.sample_rate = 1000;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.sample_rate = 8000;
    request.minimum_midi = 20;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.minimum_midi.reset();
    request.provider = OnnxExecutionProvider::Cuda;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.provider = OnnxExecutionProvider::Cpu;
    request.model_onsets = true;
    REQUIRE_FALSE(registry.select(request).selected_id);
    request.model_onsets = false;
    request.id = "not-registered";
    REQUIRE_FALSE(registry.select(request).selected_id);
    REQUIRE_FALSE(registry.select(request).reasons.empty());

    EngineRegistry missing("does-not-exist.onnx");
    request.id = "onnx:basic-pitch-v1";
    selection = missing.select(request);
    REQUIRE_FALSE(selection.selected_id);
    REQUIRE(std::any_of(selection.candidates.begin(), selection.candidates.end(), [](const auto& c) {
        return c.metadata.id == "onnx:basic-pitch-v1" &&
            std::any_of(c.reasons.begin(), c.reasons.end(), [](const auto& reason) {
                return reason.find("Model file") != std::string::npos;
            });
    }));
    auto generic = engine(missing, "onnx:legacy-pitch-v1");
    REQUIRE_FALSE(generic->metadata().capabilities.polyphonic);
    REQUIRE_FALSE(generic->incremental());
    REQUIRE(std::get<InferenceError>(generic->initialize()).code == InferenceErrorCode::ModelLoad);
    REQUIRE(generic->diagnostics().state == EngineState::Failed);
    request.instrument = InstrumentPrior::Guitar;
    selection = missing.select(request);
    const auto neural = std::find_if(selection.candidates.begin(), selection.candidates.end(),
        [](const auto& candidate) { return candidate.metadata.id == "onnx:basic-pitch-v1"; });
    REQUIRE(neural != selection.candidates.end());
    REQUIRE(std::any_of(neural->reasons.begin(), neural->reasons.end(),
        [](const auto& reason) { return reason.find("instrument") != std::string::npos; }));
}

TEST_CASE("YIN and spectral engines retain exact adapter notes with a shared timestamped result structure", "[engine]") {
    const auto audio = fixture();
    for (const std::string id : {"builtin:yin", "builtin:spectral"}) {
        EngineRegistry engines;
        auto instance = engine(engines, id);
        REQUIRE(instance->diagnostics().state == EngineState::Created);
        success(instance->initialize());
        EngineConfiguration config;
        config.audio_source_id = "test-recording";
        config.inference.retain_evidence = true;
        config.inference.analysis.polyphonic = id == "builtin:spectral";
        success(instance->configure(config));
        auto result = instance->transcribe(audio);
        if (const auto* error = std::get_if<InferenceError>(&result)) INFO(error->message);
        REQUIRE(std::holds_alternative<EngineUpdate>(result));
        const auto& update = std::get<EngineUpdate>(result);
        REQUIRE_FALSE(update.notes.empty());
        REQUIRE_FALSE(update.evidence.empty());
        REQUIRE(update.activations);
        structure(update, id, audio.sample_rate);
        ModelRegistry models;
        auto created = models.create(id);
        auto model = std::move(std::get<std::unique_ptr<ModelBackend>>(created));
        success(model->load());
        auto baseline = model->infer(audio, config.inference);
        REQUIRE(std::holds_alternative<InferenceOutput>(baseline));
        same_notes(update.notes, std::get<InferenceOutput>(baseline).notes);
        auto repeated = instance->transcribe(audio);
        REQUIRE(std::holds_alternative<EngineUpdate>(repeated));
        REQUIRE(std::get<EngineUpdate>(repeated).sequence == update.sequence + 1);
        same_notes(update.notes, std::get<EngineUpdate>(repeated).notes);
        REQUIRE(instance->diagnostics().state == EngineState::Ready);
        instance->shutdown();
        REQUIRE(instance->diagnostics().state == EngineState::Shutdown);
    }
}

TEST_CASE("engine segmentation remains optional and preserves canonical evidence", "[engine]") {
    EngineRegistry registry;
    auto instance = engine(registry, "builtin:yin");
    success(instance->initialize());
    EngineConfiguration config;
    config.audio_source_id = "test-recording";
    config.segmentation = TemporalDecodingOptions{};
    success(instance->configure(config));
    auto result = instance->transcribe(fixture());
    REQUIRE(std::holds_alternative<EngineUpdate>(result));
    const auto& update = std::get<EngineUpdate>(result);
    structure(update, "builtin:yin", 8000);
    REQUIRE_FALSE(update.events.empty());
    REQUIRE_FALSE(update.events.front().evidence.empty());
}

TEST_CASE("engine lifecycle rejects unsupported configuration and supports explicit recovery", "[engine]") {
    EngineRegistry registry;
    auto instance = engine(registry, "builtin:yin");
    REQUIRE(std::get<InferenceError>(instance->transcribe(fixture())).code == InferenceErrorCode::NotLoaded);
    REQUIRE(std::holds_alternative<InferenceError>(instance->configure({})));
    success(instance->initialize());
    REQUIRE(std::holds_alternative<InferenceError>(instance->initialize()));
    EngineConfiguration config;
    config.version = 99;
    REQUIRE(std::get<InferenceError>(instance->configure(config)).code == InferenceErrorCode::InvalidArgument);
    config.version = engine_configuration_version;
    config.inference.analysis.polyphonic = true;
    REQUIRE(std::get<InferenceError>(instance->configure(config)).code == InferenceErrorCode::Unsupported);
    REQUIRE(instance->diagnostics().state == EngineState::Ready);
    config.inference.analysis.polyphonic = false;
    config.inference.analysis.minimum_confidence = std::nan("");
    REQUIRE(std::holds_alternative<InferenceError>(instance->configure(config)));
    config.inference.analysis.minimum_confidence = .25;
    config.segmentation = TemporalDecodingOptions{};
    config.segmentation->activation_on = .1;
    config.segmentation->activation_off = .9;
    REQUIRE(std::get<InferenceError>(instance->configure(config)).code == InferenceErrorCode::InvalidArgument);
    config.segmentation.reset();
    success(instance->configure(config));
    std::stop_source stop;
    stop.request_stop();
    REQUIRE(std::get<InferenceError>(instance->transcribe(fixture(), stop.get_token())).code == InferenceErrorCode::Cancelled);
    REQUIRE(instance->diagnostics().state == EngineState::Cancelled);
    instance->shutdown();
    instance->shutdown();
    success(instance->initialize());
    config.inference.progress = [&](double) { instance->cancel(); };
    success(instance->configure(config));
    REQUIRE(std::get<InferenceError>(instance->transcribe(fixture())).code == InferenceErrorCode::Cancelled);
    instance->shutdown();
    success(instance->initialize());
    AudioBuffer invalid{1000, {0}};
    REQUIRE(std::holds_alternative<InferenceError>(instance->transcribe(invalid)));
    REQUIRE(instance->diagnostics().state == EngineState::Failed);
    REQUIRE(instance->diagnostics().last_error);
    instance->shutdown();
    ModelLoadOptions unavailable;
    unavailable.provider = OnnxExecutionProvider::Cuda;
    REQUIRE(std::get<InferenceError>(instance->initialize(unavailable)).code == InferenceErrorCode::ModelLoad);
    REQUIRE(instance->diagnostics().backend.requested_provider == OnnxExecutionProvider::Cuda);
    REQUIRE_FALSE(instance->diagnostics().backend.active_provider);
}

TEST_CASE("engine incremental updates use explicit recording origins and preserve live snapshots", "[engine]") {
    const auto audio = fixture();
    for (const std::string id : {"builtin:yin", "builtin:spectral"}) {
        EngineRegistry registry;
        auto instance = engine(registry, id);
        success(instance->initialize());
        EngineConfiguration config;
        config.audio_source_id = "test-recording";
        config.inference.analysis.polyphonic = id == "builtin:spectral";
        success(instance->configure(config));
        auto* incremental = instance->incremental();
        REQUIRE(incremental);
        live::LiveOptions options;
        options.format = {audio.sample_rate, 1};
        options.threaded = false;
        options.event_queue_capacity = 2;
        options.max_block_frames = 512;
        constexpr std::uint64_t origin = 12345;
        success(incremental->begin(options, origin));
        REQUIRE(instance->diagnostics().state == EngineState::Streaming);
        for (std::size_t offset = 0; offset < audio.samples.size(); offset += 512) {
            const auto count = std::min<std::size_t>(512, audio.samples.size() - offset);
            REQUIRE(incremental->push({options.format, origin + offset,
                std::span(audio.samples).subspan(offset, count), {}, false}) == live::PushStatus::Accepted);
            success(incremental->pump());
        }
        REQUIRE(incremental->push({options.format, origin, std::span(audio.samples).first(64), {}, false}) ==
                live::PushStatus::InvalidTimestamp);
        auto result = incremental->finish(origin + audio.samples.size());
        REQUIRE(std::holds_alternative<EngineUpdate>(result));
        const auto& update = std::get<EngineUpdate>(result);
        REQUIRE(update.finished);
        REQUIRE(update.live_changes);
        REQUIRE_FALSE(update.events.empty());
        REQUIRE(update.events.size() == update.live_changes->resync_snapshot.size());
        for (const auto& event : update.events) {
            REQUIRE(event.onset_seconds() < 1.5);
            REQUIRE(event.offset_frame);
            REQUIRE(*event.offset_frame <= audio.samples.size());
            REQUIRE(event.provenance.audio_source_id == "test-recording");
        }
        REQUIRE(instance->diagnostics().state == EngineState::Finished);
        REQUIRE(std::holds_alternative<InferenceError>(instance->transcribe(audio)));
        instance->shutdown();
        success(instance->initialize());
        success(incremental->begin(options));
        instance->cancel();
        REQUIRE(incremental->push({options.format, 0, std::span(audio.samples).first(64), {}, false}) ==
                live::PushStatus::Closed);
        REQUIRE(std::get<InferenceError>(incremental->poll()).code == InferenceErrorCode::Cancelled);
        REQUIRE(instance->diagnostics().state == EngineState::Cancelled);
    }
}

TEST_CASE("streaming capabilities fail explicitly rather than dropping requested evidence", "[engine]") {
    EngineRegistry registry;
    auto instance = engine(registry, "builtin:yin");
    success(instance->initialize());
    EngineConfiguration config;
    config.inference.retain_evidence = true;
    success(instance->configure(config));
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    REQUIRE(std::get<InferenceError>(instance->incremental()->begin(options)).code == InferenceErrorCode::Unsupported);
    config.inference.retain_evidence = false;
    config.segmentation = TemporalDecodingOptions{};
    success(instance->configure(config));
    REQUIRE(std::get<InferenceError>(instance->incremental()->begin(options)).code == InferenceErrorCode::Unsupported);
    EngineNoteEvent invalid;
    REQUIRE_THROWS(invalid.onset_seconds());
    REQUIRE_THROWS(invalid.offset_seconds());
    invalid.sample_rate = 8000;
    invalid.onset_frame = 800;
    REQUIRE(invalid.onset_seconds() == Catch::Approx(.1));
    REQUIRE_FALSE(invalid.offset_seconds());
}

TEST_CASE("progressive WAV decoding uses one persistent engine with block-invariant final notes", "[engine][stream]") {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "samples" / "single_a4.wav";
    const auto info = inspect_wav(path);
    const auto audio = read_wav(path);
    REQUIRE(info.sample_rate == audio.sample_rate);
    REQUIRE(info.frames == audio.samples.size());
    for (const std::string id : {"builtin:yin", "builtin:spectral"}) {
        std::vector<EngineNoteEvent> reference;
        for (const std::size_t block : {37U, 512U, 4096U}) {
            EngineRegistry registry;
            auto instance = engine(registry, id);
            success(instance->initialize());
            EngineConfiguration config;
            config.inference.analysis.polyphonic = id == "builtin:spectral";
            config.segmentation = TemporalDecodingOptions{};
            success(instance->configure(config));
            ProgressiveFileOptions options;
            const auto offline_result = instance->transcribe(audio);
            REQUIRE(std::holds_alternative<EngineUpdate>(offline_result));
            const auto& offline = std::get<EngineUpdate>(offline_result);
            if (id == "builtin:yin") {
                config.segmentation.reset();
                success(instance->configure(config));
            }
            options.live.threaded = false;
            options.live.max_recording_seconds = 0;
            options.live.input_queue_blocks = 1;
            options.block_frames = block;
            std::size_t updates = 0;
            std::uint64_t sequence = 0;
            options.update = [&](const EngineUpdate& update) {
                ++updates;
                REQUIRE(update.sequence > sequence);
                sequence = update.sequence;
            };
            auto result = transcribe_wav_progressive(*instance, path, options);
            if (const auto* error = std::get_if<InferenceError>(&result)) INFO(error->message);
            REQUIRE(std::holds_alternative<EngineUpdate>(result));
            const auto& update = std::get<EngineUpdate>(result);
            REQUIRE(update.finished);
            REQUIRE_FALSE(update.events.empty());
            REQUIRE(updates > 1);
            REQUIRE(update.events.size() == offline.events.size());
            for (std::size_t i = 0; i < update.events.size(); ++i) {
                REQUIRE(update.events[i].midi_pitch == offline.events[i].midi_pitch);
                REQUIRE(update.events[i].onset_seconds() ==
                    Catch::Approx(offline.events[i].onset_seconds()).margin(.1));
                REQUIRE(*update.events[i].offset_seconds() ==
                    Catch::Approx(*offline.events[i].offset_seconds()).margin(.15));
            }
            REQUIRE(instance->diagnostics().live_metrics->recording_frames == 0);
            REQUIRE(instance->diagnostics().live_metrics->skipped_frames == 0);
            REQUIRE(instance->diagnostics().live_metrics->input_queue_depth == 0);
            for (const auto& note : update.events) REQUIRE(note.finalized);
            if (reference.empty()) reference = update.events;
            else {
                REQUIRE(update.events.size() == reference.size());
                for (std::size_t i = 0; i < reference.size(); ++i) {
                    REQUIRE(update.events[i].midi_pitch == reference[i].midi_pitch);
                    REQUIRE(update.events[i].onset_frame == reference[i].onset_frame);
                    REQUIRE(update.events[i].offset_frame == reference[i].offset_frame);
                }
            }
        }
    }
}

TEST_CASE("incremental engine is an ASIO-compatible stereo sink and restart IDs remain distinct", "[engine][asio]") {
    EngineRegistry registry;
    auto instance = engine(registry, "builtin:yin");
    const auto audio = fixture();
    std::vector<float> stereo;
    for (const auto value : audio.samples) { stereo.push_back(value); stereo.push_back(value); }
    std::string previous_id;
    for (int take = 0; take < 2; ++take) {
        success(instance->initialize());
        live::LiveOptions options;
        options.format = {audio.sample_rate, 2, live::ChannelLayout::Stereo};
        options.stream_epoch = 20 + take;
        options.threaded = false;
        success(instance->incremental()->begin(options));
        detail::LiveCaptureTransport transport;
        transport.session = instance->incremental();
        transport.format = options.format;
        transport.stream_epoch = options.stream_epoch;
        transport.capacity = audio.samples.size();
        for (std::size_t offset = 0; offset < audio.samples.size(); offset += 128) {
            const auto count = std::min<std::size_t>(128, audio.samples.size() - offset);
            REQUIRE(transport.receive(stereo.data() + offset * 2, count, false, {}) ==
                (offset + count == audio.samples.size() ? 1 : 0));
            success(instance->incremental()->pump());
        }
        auto result = instance->incremental()->finish(audio.samples.size());
        REQUIRE(std::holds_alternative<EngineUpdate>(result));
        const auto& update = std::get<EngineUpdate>(result);
        REQUIRE_FALSE(update.events.empty());
        REQUIRE(update.events.front().finalized);
        REQUIRE(update.events.front().id != previous_id);
        previous_id = update.events.front().id;
        instance->shutdown();
    }
}

TEST_CASE("progressive WAV cancellation and callback failure leave no successful partial stream", "[engine][stream]") {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "samples" / "single_a4.wav";
    for (bool callback_failure : {false, true}) {
        EngineRegistry registry;
        auto instance = engine(registry, "builtin:yin");
        success(instance->initialize());
        ProgressiveFileOptions options;
        options.live.threaded = false;
        std::stop_source stop;
        options.cancellation = stop.get_token();
        options.update = [&](const EngineUpdate&) {
            if (callback_failure) throw std::runtime_error("scripted consumer failure");
            stop.request_stop();
        };
        auto result = transcribe_wav_progressive(*instance, path, options);
        REQUIRE(std::holds_alternative<InferenceError>(result));
        REQUIRE(std::get<InferenceError>(result).code ==
            (callback_failure ? InferenceErrorCode::Inference : InferenceErrorCode::Cancelled));
        REQUIRE(instance->diagnostics().state == EngineState::Cancelled);
        instance->shutdown();
    }
}

TEST_CASE("streaming inference callbacks can request cancellation without joining their worker", "[engine]") {
    EngineRegistry registry;
    auto instance = engine(registry, "builtin:spectral");
    success(instance->initialize());
    EngineConfiguration config;
    config.inference.progress = [&](double) { instance->cancel(); };
    success(instance->configure(config));
    live::LiveOptions options;
    options.format = {8000, 1};
    options.threaded = false;
    options.max_recording_seconds = 0;
    options.max_block_frames = 16000;
    options.input_queue_blocks = 16;
    options.lag_policy = live::LagPolicy::Lossless;
    success(instance->incremental()->begin(options));
    auto audio = fixture();
    audio.samples.resize(24000);
    REQUIRE(instance->incremental()->push({options.format, 0, audio.samples, {}, false}) ==
            live::PushStatus::Accepted);
    REQUIRE(std::get<InferenceError>(instance->incremental()->pump()).code == InferenceErrorCode::Cancelled);
    instance->shutdown();
    REQUIRE(instance->diagnostics().state == EngineState::Shutdown);
}

TEST_CASE("engine registration rejects incompatible metadata and reports factory failures", "[engine]") {
    EngineRegistry registry;
    auto metadata = registry.discover().front();
    REQUIRE(std::holds_alternative<InferenceError>(registry.add(metadata, {})));
    metadata.contract_version = 99;
    REQUIRE(std::holds_alternative<InferenceError>(registry.add(metadata, [] { return nullptr; })));
    metadata.contract_version = engine_contract_version;
    REQUIRE(std::holds_alternative<InferenceError>(registry.add(metadata, [] { return nullptr; })));
    metadata.id = "external:null";
    success(registry.add(metadata, [] { return nullptr; }));
    REQUIRE(std::holds_alternative<InferenceError>(registry.create(metadata.id)));
    metadata.id = "external:throws";
    success(registry.add(metadata, []() -> std::unique_ptr<TranscriptionEngine> {
        throw std::runtime_error("factory failed");
    }));
    REQUIRE(std::get<InferenceError>(registry.create(metadata.id)).message.find("factory failed") != std::string::npos);
    REQUIRE(std::holds_alternative<InferenceError>(registry.add_model(
        {99, "external:invalid", ModelContract::Yin, {}, "", ""}, {})));
    ModelDefinition notes_only;
    notes_only.id = "external:note-head-only";
    notes_only.contract = ModelContract::BasicPitchV1;
    success(registry.add_model(notes_only, [](const auto& definition) {
        return std::make_unique<NoteHeadOnlyBackend>(definition);
    }));
    auto instance = engine(registry, notes_only.id);
    success(instance->initialize());
    REQUIRE(instance->metadata().capabilities.onsets == EngineBoundaryEvidence::Heuristic);
}

#ifdef TRANSCRIBE_ENABLE_ONNX
TEST_CASE("Basic Pitch engine preserves model outputs and uses the same result structure", "[engine][onnx]") {
    const AudioBuffer audio = fixture(true);
    EngineRegistry registry(TRANSCRIBE_BASIC_PITCH_MODEL_PATH);
    auto instance = engine(registry, "onnx:basic-pitch-v1");
    success(instance->initialize());
    EngineConfiguration config;
    config.audio_source_id = "test-recording";
    config.inference.onnx.polyphonic = true;
    config.inference.onnx.independent_probabilities = true;
    config.inference.onnx.aligned_model_heads = true;
    config.inference.retain_evidence = true;
    success(instance->configure(config));
    auto result = instance->transcribe(audio);
    REQUIRE(std::holds_alternative<EngineUpdate>(result));
    const auto& update = std::get<EngineUpdate>(result);
    structure(update, "onnx:basic-pitch-v1", audio.sample_rate);
    REQUIRE(update.activations);
    REQUIRE_FALSE(update.activations->onsets.empty());
    ModelRegistry models(TRANSCRIBE_BASIC_PITCH_MODEL_PATH);
    auto created = models.create("onnx:basic-pitch-v1");
    auto baseline = std::move(std::get<std::unique_ptr<ModelBackend>>(created));
    success(baseline->load());
    auto output = baseline->infer(audio, config.inference);
    REQUIRE(std::holds_alternative<InferenceOutput>(output));
    same_notes(update.notes, std::get<InferenceOutput>(output).notes);
    REQUIRE(instance->metadata().capabilities.streaming == EngineStreaming::OverlappingWindows);
    REQUIRE(instance->diagnostics().resources.live_sessions == 1);
    config.inference.retain_evidence = false;
    success(instance->configure(config));
    live::LiveOptions stream;
    stream.format = {audio.sample_rate, 1};
    stream.threaded = false;
    stream.max_recording_seconds = 0;
    stream.lag_policy = live::LagPolicy::Lossless;
    success(instance->incremental()->begin(stream));
    for (std::size_t offset = 0; offset < audio.samples.size(); offset += 1024) {
        const auto count = std::min<std::size_t>(1024, audio.samples.size() - offset);
        REQUIRE(instance->incremental()->push({stream.format, offset,
            std::span(audio.samples).subspan(offset, count), {}, false}) == live::PushStatus::Accepted);
        success(instance->incremental()->pump());
    }
    const auto completed = instance->incremental()->finish(audio.samples.size());
    REQUIRE(std::holds_alternative<EngineUpdate>(completed));
    const auto& final = std::get<EngineUpdate>(completed);
    REQUIRE(final.finished);
    REQUIRE(final.sample_rate == audio.sample_rate);
    REQUIRE(final.provenance.engine_id == "onnx:basic-pitch-v1");
    REQUIRE(final.live_changes);
    REQUIRE_FALSE(final.events.empty());
    for (const auto& event : final.events) {
        REQUIRE(event.sample_rate == audio.sample_rate);
        REQUIRE(event.offset_frame);
        REQUIRE(*event.offset_frame <= audio.samples.size());
    }
}
#endif
