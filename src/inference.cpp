#include "transcribe/inference.hpp"
#include "onnx_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace transcribe {
namespace {
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

InferenceError error(InferenceErrorCode code, std::string message) {
    return {code, std::move(message)};
}

ModelMetadata describe(const ModelDefinition& definition, bool preview) {
    ModelMetadata m;
    m.definition = definition;
    const auto contract = definition.contract;
    if (contract == ModelContract::Yin || contract == ModelContract::Spectral) {
        const bool spectral = contract == ModelContract::Spectral;
        m.analysis_sample_rate = preview && !spectral ? 8000 : 11025;
        m.minimum_midi = spectral ? 28 : 36;
        m.maximum_midi = spectral ? 96 : 83;
        m.polyphonic = spectral;
        m.maximum_polyphony = spectral ? 69 : 1;
        m.window_samples = spectral ? (preview ? 2048 : 4096) : (preview ? 1024 : 2048);
        m.hop_samples = preview ? 512 : 256;
        m.overlap_samples = m.window_samples - m.hop_samples;
        m.output_frame_step_samples = m.hop_samples;
        m.activation_semantics = spectral ? "selected normalized spectral salience" :
            "one-minus-CMND and heuristic voicing; not correctness probabilities";
    } else {
        const bool basic = contract == ModelContract::BasicPitchV1;
        m.analysis_sample_rate = basic ? 22050 : 16000;
        m.minimum_midi = 28;
        m.maximum_midi = 96;
        m.polyphonic = true;
        m.maximum_polyphony = 69;
        m.window_samples = basic ? 43844 : 0;
        m.hop_samples = basic ? 36164 : 0;
        m.overlap_samples = basic ? 7680 : 0;
        m.output_frame_step_samples = basic ? 256 : 160;
        m.activation_semantics = basic ? "unclamped note-head activations; selected/clamped strengths" :
            "explicit legacy 10ms MIDI28-96 contract; softmax mono/sigmoid poly logits or clamped activations";
    }
    return m;
}

class Adapter final : public ModelBackend {
public:
    explicit Adapter(ModelDefinition definition) : metadata_(describe(definition, false)) {}

    const ModelMetadata& metadata() const noexcept override { return metadata_; }
    ModelResources resources() const noexcept override { return resources_; }
    BackendDiagnostics diagnostics() const override { return diagnostics_; }

    ModelOperation load(const ModelLoadOptions& options) override {
        release();
        diagnostics_.requested_provider = options.provider;
        diagnostics_.device_id = options.device_id;
        const auto start = Clock::now();
        try {
            if (options.device_id < 0) throw std::invalid_argument("Device ID must be nonnegative");
            if (metadata_.definition.contract_version != model_contract_version) {
                throw std::invalid_argument("Unsupported model contract version; expected 1");
            }
            const bool neural = is_neural();
            if ((!neural && options.provider != OnnxExecutionProvider::Cpu) ||
                (neural && options.fast_preview)) {
                throw std::invalid_argument("Built-ins require CPU; ONNX does not support preview mode");
            }
            metadata_ = describe(metadata_.definition, options.fast_preview);
            if (neural) {
                session_ = detail::open_onnx_session(metadata_.definition, options);
                session_->describe(metadata_);
                resources_.model_file_bytes = std::filesystem::file_size(metadata_.definition.path);
                resources_.live_sessions = 1;
                diagnostics_.runtime_version = detail::onnx_runtime_version();
                diagnostics_.cpu_fallback_disabled = options.provider != OnnxExecutionProvider::Cpu;
            } else if (options.device_id != 0) {
                throw std::invalid_argument("Built-in CPU adapter accepts only device ID 0");
            }
            options_ = options;
            resources_.loaded = true;
            resources_.load_seconds = elapsed(start);
            diagnostics_.active_provider = options.provider;
            diagnostics_.message = neural ?
                "Session loaded; accelerator requests disable CPU execution-provider fallback" :
                "Built-in CPU tracker loaded; incremental inference is not supported by this adapter";
            return std::monostate{};
        } catch (const std::exception& exception) {
            const auto message = "Cannot load model '" + metadata_.definition.id + "' using " +
                inference_provider_name(options.provider) + ": " + exception.what();
            release();
            diagnostics_.requested_provider = options.provider;
            diagnostics_.device_id = options.device_id;
            diagnostics_.message = message;
            return error(InferenceErrorCode::ModelLoad, message);
        }
    }

    void release() noexcept override {
        session_.reset();
        resources_ = {};
        diagnostics_ = {};
    }

    InferenceResult<InferenceOutput> infer(
        const AudioBuffer& audio, const InferenceRequest& request) override {
        if (!resources_.loaded) return fail(InferenceErrorCode::NotLoaded, "Load a model before inference");
        if (request.cancellation.stop_requested()) {
            return fail(InferenceErrorCode::Cancelled, "Inference cancelled before execution");
        }
        const auto start = Clock::now();
        try {
            validate(audio, request);
            InferenceOutput output;
            double progress = 0;
            const auto report_progress = [&](double value) {
                if (request.cancellation.stop_requested()) throw std::runtime_error("Inference cancelled");
                progress = std::max(progress, std::clamp(value, 0.0, 1.0));
                if (request.progress) request.progress(progress);
            };
            report_progress(0);
            auto analysis = request.analysis;
            analysis.polyphonic = metadata_.definition.contract == ModelContract::Spectral;
            analysis.fast_preview = options_.fast_preview;
            analysis.cancellation = request.cancellation;
            const auto analysis_progress = analysis.progress_callback;
            analysis.progress_callback = [&](double value) {
                report_progress(value);
                if (analysis_progress) analysis_progress(value);
            };
            const auto analysis_evidence = analysis.evidence_callback;
            auto onnx = request.onnx;
            onnx.execution_provider = options_.provider;
            onnx.device_id = options_.device_id;
            onnx.cancellation = request.cancellation;
            const auto onnx_progress = onnx.progress_callback;
            onnx.progress_callback = [&](double value) {
                report_progress(value);
                if (onnx_progress) onnx_progress(value);
            };
            const auto onnx_evidence = onnx.evidence_callback;
            const auto observe = [&](const FrameEvidenceTrack& track) {
                if (request.retain_evidence) output.evidence.push_back(track);
                if (is_neural()) {
                    if (onnx_evidence) onnx_evidence(track);
                } else if (analysis_evidence) analysis_evidence(track);
            };
            if (request.retain_evidence || analysis_evidence || onnx_evidence) {
                analysis.evidence_callback = observe;
                onnx.evidence_callback = observe;
            }
            output.notes = session_ ? session_->infer(audio, onnx) :
                transcribe_monophonic(audio, analysis);
            for (const auto& note : output.notes) {
                if (note.midi_pitch < metadata_.minimum_midi || note.midi_pitch > metadata_.maximum_midi ||
                    !std::isfinite(note.start_seconds) || note.start_seconds < 0 ||
                    !std::isfinite(note.duration_seconds) || note.duration_seconds <= 0 ||
                    !std::isfinite(note.confidence)) {
                    throw std::runtime_error("Backend returned a note outside its pitch/time/evidence contract");
                }
            }
            report_progress(1);
            resources_.last_input_bytes = audio.samples.size() * sizeof(float);
            resources_.last_evidence_frames = 0;
            for (const auto& track : output.evidence) resources_.last_evidence_frames += track.frames.size();
            ++resources_.completed_inferences;
            resources_.last_inference_seconds = elapsed(start);
            diagnostics_.message = "Inference completed";
            return output;
        } catch (const std::invalid_argument& exception) {
            return fail(InferenceErrorCode::InvalidArgument, exception.what());
        } catch (const std::exception& exception) {
            return fail(request.cancellation.stop_requested() ? InferenceErrorCode::Cancelled :
                InferenceErrorCode::Inference, "Model '" + metadata_.definition.id + "': " + exception.what());
        }
    }

    ModelOperation warm_up(const InferenceRequest& request) override {
        const auto rate = metadata_.analysis_sample_rate;
        AudioBuffer silence{rate, std::vector<float>(std::max<std::size_t>(
            metadata_.window_samples, static_cast<std::size_t>(rate) * 2), 0)};
        const auto result = infer(silence, request);
        if (const auto* failure = std::get_if<InferenceError>(&result)) return *failure;
        return std::monostate{};
    }

private:
    bool is_neural() const {
        return metadata_.definition.contract == ModelContract::BasicPitchV1 ||
            metadata_.definition.contract == ModelContract::LegacyPitchV1;
    }
    InferenceError fail(InferenceErrorCode code, const std::string& message) {
        diagnostics_.message = message;
        return error(code, message);
    }
    void validate(const AudioBuffer& audio, const InferenceRequest& request) const {
        if (audio.sample_rate < metadata_.minimum_input_sample_rate ||
            (is_neural() && audio.samples.empty())) {
            throw std::invalid_argument("Expected mono audio sampled at >=2000 Hz; ONNX requires nonempty input");
        }
        if (std::any_of(audio.samples.begin(), audio.samples.end(),
                       [](float sample) { return !std::isfinite(sample); })) {
            throw std::invalid_argument("Audio contains nonfinite samples");
        }
        const auto& a = request.analysis;
        const auto& o = request.onnx;
        if (metadata_.definition.contract == ModelContract::Yin && a.polyphonic) {
            throw std::invalid_argument("The selected YIN adapter is monophonic; select builtin:spectral for polyphony");
        }
        if (is_neural() && (o.execution_provider != options_.provider || o.device_id != options_.device_id)) {
            throw std::invalid_argument("Inference provider/device must match the explicitly loaded session");
        }
        const auto valid_ratio = [](double v) { return std::isfinite(v) && v >= 0 && v <= 1; };
        if (!valid_ratio(a.minimum_confidence) || !valid_ratio(a.spectral_threshold) ||
            a.spectral_threshold == 0 || a.maximum_pitches_per_frame == 0 ||
            !valid_ratio(o.minimum_confidence) || !valid_ratio(o.polyphonic_threshold) ||
            o.maximum_pitches_per_frame == 0 || o.maximum_pitches_per_frame > 69 ||
            (metadata_.definition.contract == ModelContract::Spectral &&
             a.maximum_pitches_per_frame > 69)) {
            throw std::invalid_argument("Invalid confidence/activation threshold or polyphony (expected 1-69)");
        }
    }
    ModelMetadata metadata_;
    ModelLoadOptions options_;
    ModelResources resources_;
    BackendDiagnostics diagnostics_;
    std::unique_ptr<detail::OnnxSession> session_;
};
}

InferenceResult<InferenceOutput> ModelBackend::infer_incremental(
    const AudioBuffer&, bool, const InferenceRequest&) {
    return InferenceError{InferenceErrorCode::Unsupported,
        "This adapter has no incremental contract; offline windows are not streaming inference"};
}

ModelRegistry::ModelRegistry(const std::filesystem::path& onnx_path) {
    const auto factory = [](const ModelDefinition& d) { return std::make_unique<Adapter>(d); };
    (void)add({1, "builtin:yin", ModelContract::Yin, {}, "Transcriber YIN implementation", "No repository license declared"}, factory);
    (void)add({1, "builtin:spectral", ModelContract::Spectral, {}, "Transcriber spectral implementation", "No repository license declared"}, factory);
    if (!onnx_path.empty()) {
        (void)add_onnx({1, "onnx:basic-pitch-v1", ModelContract::BasicPitchV1, onnx_path,
            "Caller-supplied ONNX file; Basic Pitch v1 tensor contract", "Verify license in model distribution"});
        (void)add_onnx({1, "onnx:legacy-pitch-v1", ModelContract::LegacyPitchV1, onnx_path,
            "Caller-supplied ONNX file; legacy pitch v1 contract", "Verify license in model distribution"});
    }
}

ModelOperation ModelRegistry::add(ModelDefinition definition, Factory factory) {
    if (definition.contract_version != model_contract_version || definition.id.empty() || !factory) {
        return error(InferenceErrorCode::InvalidArgument, "Registration requires contract version 1, an ID and factory");
    }
    if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& e) {
        return e.definition.id == definition.id;
    })) return error(InferenceErrorCode::InvalidArgument, "Duplicate model ID: " + definition.id);
    entries_.push_back({std::move(definition), std::move(factory)});
    return std::monostate{};
}

ModelOperation ModelRegistry::add_onnx(ModelDefinition definition) {
    if ((definition.contract != ModelContract::BasicPitchV1 &&
         definition.contract != ModelContract::LegacyPitchV1) ||
        definition.path.empty() || definition.provenance.empty() || definition.license.empty()) {
        return error(InferenceErrorCode::InvalidArgument,
            "ONNX registration requires an explicit supported contract, path, provenance and license");
    }
    return add(std::move(definition), [](const ModelDefinition& d) { return std::make_unique<Adapter>(d); });
}

std::vector<ModelDefinition> ModelRegistry::discover() const {
    std::vector<ModelDefinition> definitions;
    for (const auto& entry : entries_) definitions.push_back(entry.definition);
    return definitions;
}

InferenceResult<std::unique_ptr<ModelBackend>> ModelRegistry::create(const std::string& id) const {
    for (const auto& entry : entries_) {
        if (entry.definition.id != id) continue;
        try {
            auto backend = entry.factory(entry.definition);
            if (!backend) return error(InferenceErrorCode::ModelLoad, "Model factory returned null: " + id);
            return backend;
        } catch (const std::exception& exception) {
            return error(InferenceErrorCode::ModelLoad, "Model factory '" + id + "': " + exception.what());
        }
    }
    return error(InferenceErrorCode::InvalidArgument, "Unknown model ID: " + id);
}

std::string inference_provider_name(OnnxExecutionProvider provider) {
    switch (provider) {
    case OnnxExecutionProvider::Cpu: return "cpu";
    case OnnxExecutionProvider::Cuda: return "cuda";
    case OnnxExecutionProvider::DirectML: return "directml";
    case OnnxExecutionProvider::Rocm: return "rocm";
    }
    return "unknown";
}

std::vector<ProviderAvailability> inference_providers() { return detail::onnx_providers(); }
}
