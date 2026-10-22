#include "transcribe/engine.hpp"
#include "transcribe/score.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <thread>

namespace transcribe {
namespace {

InferenceError failure(InferenceErrorCode code, std::string message) {
    return {code, std::move(message)};
}

EngineMetadata describe_engine(const ModelMetadata& model) {
    EngineMetadata result;
    result.id = model.definition.id;
    result.model = model;
    auto& c = result.capabilities;
    const auto contract = model.definition.contract;
    const bool builtin = contract == ModelContract::Yin || contract == ModelContract::Spectral;
    c.instruments = builtin ?
        std::vector<InstrumentPrior>{InstrumentPrior::None, InstrumentPrior::Guitar,
                                    InstrumentPrior::Piano, InstrumentPrior::Voice,
                                    InstrumentPrior::Bass} :
        std::vector<InstrumentPrior>{InstrumentPrior::None};
    c.instrument_scope = builtin ?
        "General pitch tracker with optional instrument priors, not instrument recognition" :
        "Model-specific training scope; no instrument-specific guarantee declared";
    c.polyphonic = model.polyphonic;
    // Legacy models can use the explicit single-pitch decoder without claiming a multi-pitch objective.
    c.monophonic = true;
    c.minimum_midi = model.minimum_midi;
    c.maximum_midi = model.maximum_midi;
    c.minimum_sample_rate = model.minimum_input_sample_rate;
    c.streaming = contract == ModelContract::Yin && model.definition.id == "builtin:yin" ? EngineStreaming::Incremental :
        (contract == ModelContract::Spectral || contract == ModelContract::BasicPitchV1 ?
         EngineStreaming::OverlappingWindows : EngineStreaming::None);
    c.activations = true;
    c.onsets = contract == ModelContract::BasicPitchV1 ?
        EngineBoundaryEvidence::ModelHead : EngineBoundaryEvidence::Heuristic;
    if (contract == ModelContract::BasicPitchV1 && !model.outputs.empty() &&
        std::none_of(model.outputs.begin(), model.outputs.end(),
            [](const auto& head) { return head.name == "StatefulPartitionedCall:2"; }))
        c.onsets = EngineBoundaryEvidence::Heuristic;
    c.offsets = EngineBoundaryEvidence::Heuristic;
    c.providers = builtin ? std::vector{OnnxExecutionProvider::Cpu} :
        std::vector{OnnxExecutionProvider::Cpu, OnnxExecutionProvider::Cuda,
                    OnnxExecutionProvider::DirectML, OnnxExecutionProvider::Rocm};
    return result;
}

std::vector<std::string> mismatches(const EngineMetadata& metadata, const EngineRequirements& r) {
    const auto& c = metadata.capabilities;
    std::vector<std::string> reasons;
    if (r.id && *r.id != metadata.id) reasons.push_back("Not the requested engine ID");
    if (std::find(c.instruments.begin(), c.instruments.end(), r.instrument) == c.instruments.end())
        reasons.push_back("Requested instrument scope is not declared");
    if (r.polyphonic ? !c.polyphonic : !c.monophonic)
        reasons.push_back("Requested monophonic/polyphonic objective is not declared");
    if ((r.minimum_midi && *r.minimum_midi < c.minimum_midi) ||
        (r.maximum_midi && *r.maximum_midi > c.maximum_midi))
        reasons.push_back("Requested pitch range exceeds the engine contract");
    if (r.sample_rate && (*r.sample_rate < c.minimum_sample_rate ||
        (!c.sample_rates.empty() &&
         std::find(c.sample_rates.begin(), c.sample_rates.end(), *r.sample_rate) == c.sample_rates.end())))
        reasons.push_back("Requested input sample rate is unsupported");
    if (r.streaming != EngineStreaming::None && r.streaming != c.streaming)
        reasons.push_back("Requested streaming mode is unsupported; windowed inference is not stateful streaming");
    if (r.activations && !c.activations) reasons.push_back("Activation output is unavailable");
    if (r.activations && r.streaming != EngineStreaming::None && !c.streaming_activations)
        reasons.push_back("Streaming activation output is unavailable; use offline retained evidence");
    if (r.model_onsets && c.onsets != EngineBoundaryEvidence::ModelHead)
        reasons.push_back("Model onset head is unavailable");
    if (std::find(c.providers.begin(), c.providers.end(), r.provider) == c.providers.end())
        reasons.push_back("Requested hardware provider is not supported by this engine");
    return reasons;
}

PitchActivationGrid selected_pitch_grid(const FrameEvidenceTrack& track, const ModelMetadata& metadata) {
    PitchActivationGrid grid;
    grid.minimum_midi = metadata.minimum_midi;
    grid.maximum_midi = metadata.maximum_midi;
    grid.hop_seconds = track.hop_seconds;
    grid.window_seconds = track.window_seconds;
    grid.semantics = track.selected_strength_semantics;
    grid.objective = PitchObjective::Monophonic;
    grid.method = track.method + ":selected-pitch-projection-not-independent-activations";
    const auto pitches = static_cast<std::size_t>(grid.maximum_midi - grid.minimum_midi + 1);
    grid.values.resize(track.frames.size() * pitches);
    for (std::size_t i = 0; i < track.frames.size(); ++i) {
        const auto& frame = track.frames[i];
        grid.timestamps.push_back(frame.time_seconds);
        if (frame.midi_pitch >= grid.minimum_midi && frame.midi_pitch <= grid.maximum_midi)
            grid.values[i * pitches + static_cast<std::size_t>(frame.midi_pitch - grid.minimum_midi)] = frame.confidence;
    }
    validate_pitch_activation_grid(grid);
    return grid;
}

class ModelEngine final : public TranscriptionEngine, public IncrementalEngine {
public:
    ModelEngine(EngineMetadata metadata, ModelRegistry::Factory factory)
        : metadata_(std::move(metadata)), factory_(std::move(factory)) {}
    ~ModelEngine() override { shutdown(); }

    const EngineMetadata& metadata() const noexcept override { return metadata_; }
    IncrementalEngine* incremental() noexcept override {
        return metadata_.capabilities.streaming == EngineStreaming::None ? nullptr : this;
    }
    ModelOperation initialize(const ModelLoadOptions& options) override {
        if (state_ != EngineState::Created && state_ != EngineState::Shutdown)
            return fail(InferenceErrorCode::InvalidArgument, "Engine initialize requires Created or Shutdown state");
        initialization_diagnostics_ = {};
        initialization_diagnostics_.requested_provider = options.provider;
        initialization_diagnostics_.device_id = options.device_id;
        initialization_resources_ = {};
        try {
            auto backend = factory_(metadata_.model.definition);
            if (!backend) return fail(InferenceErrorCode::ModelLoad, "Engine model factory returned null", true);
            const auto loaded = backend->load(options);
            initialization_diagnostics_ = backend->diagnostics();
            initialization_resources_ = backend->resources();
            if (const auto* error = std::get_if<InferenceError>(&loaded))
                return fail(error->code, error->message, true);
            if (backend->metadata().definition.id != metadata_.id ||
                backend->metadata().definition.contract_version != model_contract_version)
                return fail(InferenceErrorCode::ModelLoad, "Model factory metadata does not match its registered identity/version", true);
            backend_ = std::move(backend);
            metadata_ = describe_engine(backend_->metadata());
            initialization_diagnostics_ = backend_->diagnostics();
            initialization_resources_ = backend_->resources();
            load_ = options;
            configuration_ = {};
            stop_ = std::stop_source{};
            sequence_ = 0;
            last_error_.reset();
            state_ = EngineState::Ready;
            return std::monostate{};
        } catch (const std::exception& error) {
            return fail(InferenceErrorCode::ModelLoad, error.what(), true);
        }
    }
    ModelOperation configure(const EngineConfiguration& configuration) override {
        if (state_ != EngineState::Ready)
            return fail(InferenceErrorCode::InvalidArgument, "Configure requires a Ready engine");
        if (configuration.version != engine_configuration_version || configuration.audio_source_id.empty())
            return fail(InferenceErrorCode::InvalidArgument, "Engine configuration requires version 1 and an audio source ID");
        EngineRequirements requirements;
        requirements.instrument = configuration.inference.analysis.instrument;
        requirements.polyphonic = configuration.inference.analysis.polyphonic || configuration.inference.onnx.polyphonic;
        requirements.activations = configuration.inference.retain_evidence || configuration.segmentation.has_value();
        requirements.provider = load_.provider;
        auto effective = describe_engine(backend_->metadata());
        if (effective.model.definition.contract == ModelContract::LegacyPitchV1 &&
            effective.model.objective == PitchObjective::Unspecified &&
            configuration.inference.onnx.objective != PitchObjective::Unspecified) {
            effective.model.objective = configuration.inference.onnx.objective;
            effective.model.polyphonic = effective.model.objective == PitchObjective::IndependentMultiPitch;
            effective.capabilities.polyphonic = effective.model.polyphonic;
            effective.capabilities.monophonic = true;
        }
        const auto reasons = mismatches(effective, requirements);
        if (!reasons.empty()) return fail(InferenceErrorCode::Unsupported, reasons.front());
        const auto ratio = [](double value) { return std::isfinite(value) && value >= 0 && value <= 1; };
        const auto& a = configuration.inference.analysis;
        if (configuration.inference.onnx.note_decoder != NeuralNoteDecoder::Legacy &&
            configuration.segmentation) {
            return fail(InferenceErrorCode::InvalidArgument,
                "Select engine segmentation or neural event decoding, not both");
        }
        const auto& o = configuration.inference.onnx;
        if (!ratio(a.minimum_confidence) || !ratio(a.spectral_threshold) || a.spectral_threshold == 0 ||
            !ratio(o.minimum_confidence) || !ratio(o.polyphonic_threshold) ||
            a.maximum_pitches_per_frame == 0 || a.maximum_pitches_per_frame > 69 ||
            o.maximum_pitches_per_frame == 0 || o.maximum_pitches_per_frame > 69)
            return fail(InferenceErrorCode::InvalidArgument, "Invalid activation/confidence threshold or polyphony limit (1-69)");
        if (configuration.segmentation) {
            try { validate_temporal_decoding_options(*configuration.segmentation); }
            catch (const std::invalid_argument& error) {
                return fail(InferenceErrorCode::InvalidArgument, error.what());
            }
        }
        configuration_ = configuration;
        metadata_ = std::move(effective);
        last_error_.reset();
        return std::monostate{};
    }
    InferenceResult<EngineUpdate> transcribe(const AudioBuffer& audio, std::stop_token cancellation) override {
        if (state_ != EngineState::Ready)
            return fail(InferenceErrorCode::NotLoaded, "Transcribe requires a Ready engine");
        std::stop_callback external(cancellation, [this] { cancel(); });
        std::stop_callback configured(configuration_.inference.cancellation, [this] { cancel(); });
        if (stop_.stop_requested()) return cancelled();
        state_ = EngineState::Processing;
        try {
            auto request = configuration_.inference;
            request.cancellation = stop_.get_token();
            request.onnx.execution_provider = load_.provider;
            request.onnx.device_id = load_.device_id;
            request.retain_evidence = request.retain_evidence || configuration_.segmentation.has_value();
            auto inferred = backend_->infer(audio, request);
            if (const auto* error = std::get_if<InferenceError>(&inferred))
                return fail(error->code, error->message, true);
            if (stop_.stop_requested()) return cancelled();
            auto output = std::move(std::get<InferenceOutput>(inferred));
            EngineUpdate update;
            update.provenance = provenance();
            update.sample_rate = audio.sample_rate;
            update.notes = std::move(output.notes);
            update.evidence = std::move(output.evidence);
            update.activations = std::move(output.activations);
            if (!update.activations && !update.evidence.empty()) {
                update.activations = update.evidence.size() == 1 && update.evidence.front().raw_midi_pitch < 0 ?
                    selected_pitch_grid(update.evidence.front(), metadata_.model) :
                    pitch_activation_grid(update.evidence, metadata_.model.objective);
            }
            if (configuration_.segmentation) {
                auto decoded = decode_note_events(audio, update.evidence, *configuration_.segmentation,
                    configuration_.audio_source_id, metadata_.id);
                update.notes.clear();
                for (const auto& event : decoded.rejected_events)
                    update.rejected_events.push_back(from_canonical(event));
                for (const auto& event : decoded.events) {
                    auto converted = from_canonical(event);
                    update.notes.push_back({static_cast<int>(std::lround(event.midi_pitch)),
                        event.onset_seconds, event.offset_seconds - event.onset_seconds,
                        midi_note_name(static_cast<int>(std::lround(event.midi_pitch))),
                        converted.heuristic_strength.value_or(0), event.intensity_rms.value_or(0)});
                    update.events.push_back(std::move(converted));
                }
            } else {
                for (std::size_t i = 0; i < update.notes.size(); ++i) {
                    const auto& note = update.notes[i];
                    EngineNoteEvent event;
                    event.id = metadata_.id + ":note-" + std::to_string(i + 1);
                    event.midi_pitch = note.midi_pitch;
                    event.sample_rate = audio.sample_rate;
                    event.onset_frame = recording_sample_position(note.start_seconds, audio.sample_rate);
                    event.offset_frame = recording_sample_position(note.start_seconds + note.duration_seconds, audio.sample_rate);
                    event.provenance = update.provenance;
                    event.heuristic_strength = note.confidence;
                    event.finalized = true;
                    update.events.push_back(std::move(event));
                }
            }
            if (stop_.stop_requested()) return cancelled();
            update.sequence = ++sequence_;
            update.finished = true;
            state_ = EngineState::Ready;
            last_error_.reset();
            return update;
        } catch (const std::exception& error) {
            return fail(stop_.stop_requested() ? InferenceErrorCode::Cancelled :
                InferenceErrorCode::Inference, error.what(), true);
        }
    }
    ModelOperation begin(live::LiveOptions options, std::uint64_t first_frame) override {
        if (state_ != EngineState::Ready)
            return fail(InferenceErrorCode::NotLoaded, "Begin stream requires a Ready engine");
        if (!incremental()) return fail(InferenceErrorCode::Unsupported, "Engine has no incremental interface");
        if (configuration_.inference.retain_evidence ||
            configuration_.inference.analysis.evidence_callback || configuration_.inference.onnx.evidence_callback)
            return fail(InferenceErrorCode::Unsupported, "Live engine publishes note revisions, not activation tracks");
        if (metadata_.capabilities.streaming == EngineStreaming::Incremental && configuration_.segmentation)
            return fail(InferenceErrorCode::Unsupported, "Incremental YIN uses LiveOptions::yin stabilization, not offline segmentation");
        if (metadata_.capabilities.streaming == EngineStreaming::Incremental &&
            (configuration_.inference.analysis.instrument != InstrumentPrior::None || load_.fast_preview))
            return fail(InferenceErrorCode::Unsupported, "Incremental YIN does not implement offline instrument priors or fast-preview settings");
        if (options.format.sample_rate < metadata_.capabilities.minimum_sample_rate)
            return fail(InferenceErrorCode::Unsupported, "Engine streaming requires a supported sample rate");
        if (stop_.stop_requested()) return cancelled();
        try {
            options.audio_source_id = configuration_.audio_source_id;
            if (options.stream_id.empty())
                options.stream_id = configuration_.audio_source_id + "-" + std::to_string(++stream_generation_);
            options.model_load = load_;
            options.backend = metadata_.capabilities.streaming == EngineStreaming::Incremental ?
                live::LiveBackendKind::IncrementalYin : live::LiveBackendKind::OverlappingWindow;
            options.window.request = configuration_.inference;
            options.window.request.onnx.execution_provider = load_.provider;
            options.window.request.onnx.device_id = load_.device_id;
            options.window.temporal_decoding = configuration_.segmentation.has_value();
            if (configuration_.segmentation) options.window.decoding = *configuration_.segmentation;
            std::unique_ptr<ModelBackend> model;
            if (options.backend == live::LiveBackendKind::OverlappingWindow) model = std::move(backend_);
            stream_ = std::make_unique<live::LiveTranscriptionSession>(options, std::move(model));
            const auto started = stream_->start();
            if (const auto* error = std::get_if<InferenceError>(&started))
                return fail(error->code, error->message, true);
            stream_->begin_input(first_frame);
            stream_cancel_ = std::make_unique<std::stop_callback<std::function<void()>>>(
                stop_.get_token(), [this] { stream_->request_cancel(); });
            configured_stream_cancel_ = std::make_unique<std::stop_callback<std::function<void()>>>(
                configuration_.inference.cancellation, [this] { cancel(); });
            stream_rate_ = options.format.sample_rate;
            stream_threaded_ = options.threaded;
            if (stop_.stop_requested()) return cancelled();
            state_ = EngineState::Streaming;
            last_error_.reset();
            return std::monostate{};
        } catch (const std::exception& error) {
            return fail(InferenceErrorCode::InvalidArgument, error.what(), true);
        }
    }
    live::PushStatus push(const live::AudioFrameBlock& block) noexcept override {
        if (stop_.stop_requested()) return live::PushStatus::Closed;
        return stream_ ? stream_->push(block) : live::PushStatus::Closed;
    }
    void report_input_lost() noexcept override {
        if (stream_) stream_->report_input_lost();
    }
    ModelOperation pump() override {
        if (!stream_ || state_ != EngineState::Streaming)
            return fail(InferenceErrorCode::NotLoaded, "Pump requires an active stream");
        if (stream_threaded_)
            return fail(InferenceErrorCode::Unsupported, "Manual pumping is unavailable for a threaded stream");
        if (stop_.stop_requested()) return cancelled();
        try {
            stream_->pump();
            if (stop_.stop_requested()) return cancelled();
            if (stream_->state() == live::LiveState::AnalysisFailed)
                return fail(InferenceErrorCode::Inference, stream_->last_error(), true);
            return std::monostate{};
        } catch (const std::exception& error) {
            return fail(InferenceErrorCode::Inference, error.what(), true);
        }
    }
    InferenceResult<EngineUpdate> poll() override {
        if (!stream_) return fail(InferenceErrorCode::NotLoaded, "No stream has been started");
        if (stop_.stop_requested()) { stream_->cancel(); return cancelled(); }
        if (stream_->state() == live::LiveState::AnalysisFailed)
            return fail(InferenceErrorCode::Inference, stream_->last_error(), true);
        try {
            EngineUpdate update;
            update.sequence = ++sequence_;
            update.provenance = provenance();
            update.sample_rate = stream_rate_;
            update.live_changes = stream_->poll_events_with_snapshot();
            for (const auto& note : update.live_changes->resync_snapshot) {
                EngineNoteEvent event;
                event.id = note.id;
                event.midi_pitch = note.midi_pitch;
                event.sample_rate = note.sample_rate;
                event.onset_frame = note.onset_frame;
                event.offset_frame = note.offset_frame;
                event.state = note.state;
                event.revision = note.revision;
                event.finalized = note.finalized;
                event.onset_truncated = note.onset_truncated;
                event.heuristic_strength = note.raw_strength;
                event.provenance = update.provenance;
                event.provenance.method = note.detection_method;
                update.events.push_back(std::move(event));
            }
            update.finished = stream_->state() == live::LiveState::Finished;
            if (update.finished) state_ = EngineState::Finished;
            last_error_.reset();
            return update;
        } catch (const std::exception& error) {
            return fail(InferenceErrorCode::Inference, error.what(), true);
        }
    }
    InferenceResult<EngineUpdate> finish(std::uint64_t end_frame) override {
        if (state_ != EngineState::Streaming || !stream_)
            return fail(InferenceErrorCode::NotLoaded, "Finish requires an active stream");
        if (stop_.stop_requested()) { stream_->cancel(); return cancelled(); }
        try {
            stream_->end_input(end_frame);
            stream_->finish();
            // A finished stream stays terminal until shutdown/reinitialize; no implicit reload.
            return poll();
        } catch (const std::exception& error) {
            return fail(InferenceErrorCode::Inference, error.what(), true);
        }
    }
    void cancel() noexcept override { stop_.request_stop(); }
    EngineDiagnostics diagnostics() const override {
        EngineDiagnostics result;
        result.state = stop_.stop_requested() && state_ != EngineState::Shutdown ?
            EngineState::Cancelled : state_;
        result.last_error = last_error_;
        result.backend = initialization_diagnostics_;
        result.resources = initialization_resources_;
        if (backend_) { result.backend = backend_->diagnostics(); result.resources = backend_->resources(); }
        else if (stream_) {
            result.backend.message += "; live session owns backend; resource counters are initialization-only";
        }
        if (stream_) result.live_metrics = stream_->metrics();
        if (stream_ && stream_->state() == live::LiveState::AnalysisFailed) {
            result.state = EngineState::Failed;
            result.last_error = failure(InferenceErrorCode::Inference, stream_->last_error());
        }
        return result;
    }
    void shutdown() noexcept override {
        stop_.request_stop();
        configured_stream_cancel_.reset();
        stream_cancel_.reset();
        if (stream_) { stream_->cancel(); stream_.reset(); }
        if (backend_) { backend_->release(); backend_.reset(); }
        initialization_diagnostics_ = {};
        initialization_resources_ = {};
        state_ = EngineState::Shutdown;
    }

private:
    InferenceError fail(InferenceErrorCode code, std::string message, bool terminal = false) {
        last_error_ = failure(code, "Engine '" + metadata_.id + "': " + std::move(message));
        if (terminal) state_ = code == InferenceErrorCode::Cancelled ? EngineState::Cancelled : EngineState::Failed;
        return *last_error_;
    }
    InferenceError cancelled() { return fail(InferenceErrorCode::Cancelled, "Operation cancelled", true); }
    EngineProvenance provenance() const {
        const auto& d = metadata_.model.definition;
        return {metadata_.id, d.id, d.contract_version, d.provenance, d.license,
                metadata_.model.activation_semantics, configuration_.audio_source_id};
    }
    EngineNoteEvent from_canonical(const CanonicalNoteEvent& note) const {
        EngineNoteEvent event;
        event.id = note.id;
        event.midi_pitch = note.midi_pitch;
        event.sample_rate = note.source_sample_rate;
        event.onset_frame = note.onset_sample;
        event.offset_frame = note.offset_sample;
        event.state = note.state;
        event.finalized = note.state != EventLifecycle::Rejected;
        event.calibrated_confidence = note.calibrated_confidence;
        event.provenance = provenance();
        event.provenance.method = note.detection_method;
        event.evidence = note.evidence;
        for (const auto& evidence : note.evidence)
            event.heuristic_strength = std::max(event.heuristic_strength.value_or(0), evidence.raw_strength);
        return event;
    }
    EngineMetadata metadata_;
    ModelRegistry::Factory factory_;
    std::unique_ptr<ModelBackend> backend_;
    std::unique_ptr<live::LiveTranscriptionSession> stream_;
    std::uint64_t stream_generation_{};
    std::unique_ptr<std::stop_callback<std::function<void()>>> stream_cancel_;
    std::unique_ptr<std::stop_callback<std::function<void()>>> configured_stream_cancel_;
    EngineConfiguration configuration_;
    ModelLoadOptions load_;
    BackendDiagnostics initialization_diagnostics_;
    ModelResources initialization_resources_;
    std::stop_source stop_;
    EngineState state_{EngineState::Created};
    std::optional<InferenceError> last_error_;
    std::uint64_t sequence_{};
    std::uint32_t stream_rate_{};
    bool stream_threaded_{};
};

} // namespace

double EngineNoteEvent::onset_seconds() const {
    if (!sample_rate) throw std::invalid_argument("Engine event has no source sample rate");
    return static_cast<double>(onset_frame) / sample_rate;
}
std::optional<double> EngineNoteEvent::offset_seconds() const {
    if (!sample_rate) throw std::invalid_argument("Engine event has no source sample rate");
    if (!offset_frame) return std::nullopt;
    return static_cast<double>(*offset_frame) / sample_rate;
}

EngineRegistry::EngineRegistry(const std::filesystem::path& path) {
    ModelRegistry models(path);
    for (const auto& definition : models.discover()) {
        const auto registered = add_model(definition,
            [models](const ModelDefinition& d) -> std::unique_ptr<ModelBackend> {
                auto created = models.create(d.id);
                if (const auto* error = std::get_if<InferenceError>(&created))
                    throw std::runtime_error(error->message);
                return std::move(std::get<std::unique_ptr<ModelBackend>>(created));
            });
        if (const auto* error = std::get_if<InferenceError>(&registered))
            throw std::logic_error(error->message);
    }
}

ModelOperation EngineRegistry::add(EngineMetadata metadata, Factory factory) {
    const auto& c = metadata.capabilities;
    if (metadata.contract_version != engine_contract_version || metadata.id.empty() || !factory ||
        metadata.model.definition.contract_version != model_contract_version ||
        c.minimum_midi < 0 || c.maximum_midi > 127 || c.minimum_midi > c.maximum_midi ||
        c.minimum_sample_rate == 0 || (!c.monophonic && !c.polyphonic && metadata.model.objective != PitchObjective::Unspecified))
        return failure(InferenceErrorCode::InvalidArgument, "Engine registration requires versioned metadata, valid capabilities, ID and factory");
    if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) { return e.metadata.id == metadata.id; }))
        return failure(InferenceErrorCode::InvalidArgument, "Duplicate engine ID: " + metadata.id);
    entries_.push_back({std::move(metadata), std::move(factory)});
    return std::monostate{};
}
ModelOperation EngineRegistry::add_model(ModelDefinition definition, ModelRegistry::Factory factory) {
    if (!factory || definition.contract_version != model_contract_version || definition.id.empty())
        return failure(InferenceErrorCode::InvalidArgument, "Engine model registration requires version 1, ID and factory");
    try {
        auto model = factory(definition);
        if (!model) return failure(InferenceErrorCode::ModelLoad, "Engine model factory returned null");
        if (model->metadata().definition.id != definition.id ||
            model->metadata().definition.contract_version != definition.contract_version)
            return failure(InferenceErrorCode::InvalidArgument, "Engine model factory metadata identity/version mismatch");
        auto metadata = describe_engine(model->metadata());
        Factory engine_factory = [metadata, factory = std::move(factory)] {
            return std::make_unique<ModelEngine>(metadata, factory);
        };
        return add(std::move(metadata), std::move(engine_factory));
    } catch (const std::exception& error) {
        return failure(InferenceErrorCode::ModelLoad, error.what());
    }
}
std::vector<EngineMetadata> EngineRegistry::discover() const {
    std::vector<EngineMetadata> result;
    for (const auto& entry : entries_) result.push_back(entry.metadata);
    return result;
}
EngineSelection EngineRegistry::select(const EngineRequirements& r) const {
    EngineSelection result;
    const bool invalid = (r.minimum_midi && (*r.minimum_midi < 0 || *r.minimum_midi > 127)) ||
        (r.maximum_midi && (*r.maximum_midi < 0 || *r.maximum_midi > 127)) ||
        (r.minimum_midi && r.maximum_midi && *r.minimum_midi > *r.maximum_midi);
    for (const auto& entry : entries_) {
        EngineAvailability candidate{entry.metadata, false, mismatches(entry.metadata, r), {}};
        if (invalid) candidate.reasons.push_back("Invalid requested pitch range");
        const auto& d = entry.metadata.model.definition;
        if (d.contract == ModelContract::BasicPitchV1 || d.contract == ModelContract::LegacyPitchV1) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(d.path, ec) || ec)
                candidate.reasons.push_back("Model file is missing or not a regular file");
            const auto providers = inference_providers();
            const auto provider = std::find_if(providers.begin(), providers.end(),
                [&](const auto& p) { return p.provider == r.provider; });
            if (provider == providers.end() || !provider->compiled || !provider->runtime_available)
                candidate.reasons.push_back(provider == providers.end() ? "Unknown provider" : provider->diagnostic);
        }
        candidate.available = candidate.reasons.empty();
        candidate.explanation = candidate.available ?
            "Declared capabilities match; initialization must still validate model tensors and runtime resources" :
            "Unavailable for this request: " + candidate.reasons.front();
        if (candidate.available && !result.selected_id) result.selected_id = entry.metadata.id;
        result.candidates.push_back(std::move(candidate));
    }
    if (r.id && std::none_of(entries_.begin(), entries_.end(),
        [&](const auto& entry) { return entry.metadata.id == *r.id; }))
        result.reasons.push_back("Requested engine is not registered: " + *r.id +
            ". Supply a model path or register its factory.");
    else if (!result.selected_id)
        result.reasons.push_back("No registered engine satisfies the requested capabilities; inspect candidate reasons.");
    return result;
}
InferenceResult<std::unique_ptr<TranscriptionEngine>> EngineRegistry::create(const std::string& id) const {
    for (const auto& entry : entries_) {
        if (entry.metadata.id != id) continue;
        try {
            auto engine = entry.factory();
            if (!engine) return failure(InferenceErrorCode::ModelLoad, "Engine factory returned null: " + id);
            if (engine->metadata().id != entry.metadata.id ||
                engine->metadata().contract_version != entry.metadata.contract_version)
                return failure(InferenceErrorCode::ModelLoad, "Engine factory metadata identity/version mismatch: " + id);
            return engine;
        } catch (const std::exception& error) {
            return failure(InferenceErrorCode::ModelLoad, "Engine factory '" + id + "': " + error.what());
        }
    }
    return failure(InferenceErrorCode::InvalidArgument, "Unknown model ID / engine ID: " + id);
}

InferenceResult<EngineUpdate> transcribe_wav_progressive(
    TranscriptionEngine& engine, const std::filesystem::path& path,
    const ProgressiveFileOptions& options) {
    auto* input = engine.incremental();
    if (!input) return failure(InferenceErrorCode::Unsupported, "Engine has no incremental input contract");
    if (!options.block_frames || options.block_frames > 1048576)
        return failure(InferenceErrorCode::InvalidArgument, "Progressive WAV block size must be 1..1048576 frames");
    if (options.cancellation.stop_requested())
        return failure(InferenceErrorCode::Cancelled, "Progressive WAV cancelled before input");
    bool began = false;
    try {
        auto live = options.live;
        const auto info = inspect_wav(path);
        live.format = {info.sample_rate, 1, live::ChannelLayout::Mono};
        live.lag_policy = live::LagPolicy::Lossless;
        live.max_block_frames = std::min(options.block_frames, live.max_block_frames);
        const auto started = input->begin(live);
        if (const auto* error = std::get_if<InferenceError>(&started)) return *error;
        began = true;
        std::stop_callback stop(options.cancellation, [&] { engine.cancel(); });
        std::uint64_t sequence = 0;
        const auto poll = [&] {
            if (!live.threaded) {
                const auto pumped = input->pump();
                if (const auto* error = std::get_if<InferenceError>(&pumped))
                    throw std::runtime_error(error->message);
            }
            auto result = input->poll();
            if (const auto* error = std::get_if<InferenceError>(&result))
                throw std::runtime_error(error->message);
            if (options.update) options.update(std::get<EngineUpdate>(result));
        };
        read_wav_blocks(path, live.max_block_frames, [&](const AudioBlockView& block) {
            if (options.cancellation.stop_requested()) throw std::runtime_error("Progressive WAV cancelled");
            if (block.sample_rate != info.sample_rate || block.total_frames != info.frames)
                throw std::runtime_error("WAV metadata changed after progressive input began");
            const live::AudioFrameBlock frames{live.format, block.first_frame, block.samples, {}, false,
                sequence, live.stream_epoch};
            auto status = input->push(frames);
            while (status == live::PushStatus::QueueFull) {
                poll();
                if (options.cancellation.stop_requested()) throw std::runtime_error("Progressive WAV cancelled");
                if (live.threaded) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                status = input->push(frames);
            }
            if (status != live::PushStatus::Accepted)
                throw std::runtime_error("Progressive WAV input rejected; inspect engine diagnostics");
            ++sequence;
            poll();
        });
        auto final = input->finish(info.frames);
        if (options.update && std::holds_alternative<EngineUpdate>(final))
            options.update(std::get<EngineUpdate>(final));
        return final;
    } catch (const std::exception& error) {
        if (began) engine.cancel();
        return failure(options.cancellation.stop_requested() ? InferenceErrorCode::Cancelled :
            InferenceErrorCode::Inference, error.what());
    }
}

} // namespace transcribe
