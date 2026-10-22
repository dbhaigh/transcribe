#pragma once

#include "transcribe/transcriber.hpp"
#include "transcribe/polyphonic.hpp"

#include <memory>
#include <stop_token>
#include <variant>

namespace transcribe {

inline constexpr std::uint32_t model_contract_version = 1;

enum class ModelContract { Yin, Spectral, BasicPitchV1, LegacyPitchV1 };
enum class InferenceErrorCode {
    InvalidArgument, Unsupported, NotLoaded, Cancelled, ModelLoad, Inference
};

struct InferenceError {
    InferenceErrorCode code{};
    std::string message;
};

template<class T>
using InferenceResult = std::variant<T, InferenceError>;
using ModelOperation = InferenceResult<std::monostate>;

struct TensorContract {
    std::string name;
    std::string element_type;
    std::vector<std::int64_t> dimensions;
};

struct ModelDefinition {
    std::uint32_t contract_version{model_contract_version};
    std::string id;
    ModelContract contract{ModelContract::Yin};
    std::filesystem::path path;
    std::string provenance;
    std::string license;
    PitchObjective objective{PitchObjective::Unspecified};
};

enum class ModelHeadRole { PitchActivity, Onset, Contour };

struct ModelOutputHead {
    std::string tensor_name;
    ModelHeadRole role{};
    std::uint32_t sample_rate{};
    std::size_t frame_step_samples{};
    double origin_seconds{};
    int minimum_midi{};
    std::size_t bins{};
    std::size_t bins_per_semitone{1};
    bool required{};
    bool available{};
};

struct ModelMetadata {
    ModelDefinition definition;
    std::uint32_t minimum_input_sample_rate{2000};
    std::uint32_t analysis_sample_rate{};
    int minimum_midi{};
    int maximum_midi{};
    bool polyphonic{};
    std::size_t maximum_polyphony{};
    std::size_t window_samples{};
    std::size_t hop_samples{};
    std::size_t overlap_samples{};
    std::size_t output_frame_step_samples{};
    std::size_t batch_size{1};
    bool offline{true};
    bool incremental{};
    std::string activation_semantics;
    std::vector<TensorContract> inputs;
    std::vector<TensorContract> outputs;
    PitchObjective objective{PitchObjective::Unspecified};
    std::size_t trimmed_edge_frames{};
    double output_origin_seconds{};
    std::string temporal_alignment;
    std::vector<std::string> output_head_semantics;
    std::vector<ModelOutputHead> heads{};
};

struct ProviderAvailability {
    OnnxExecutionProvider provider{};
    bool compiled{};
    bool runtime_available{};
    std::string diagnostic;
};

struct ModelLoadOptions {
    OnnxExecutionProvider provider{OnnxExecutionProvider::Cpu};
    int device_id{};
    bool fast_preview{};
    std::size_t maximum_result_cache_bytes{};
    // Zero preserves the existing unrestricted offline-input contract; bounded WAV is separate.
    std::size_t maximum_input_bytes{};
};

struct InferenceRequest {
    AnalysisOptions analysis;
    OnnxOptions onnx;
    std::stop_token cancellation;
    std::function<void(double)> progress;
    bool retain_evidence{};
    bool reuse_cached_result{};
};

struct InferenceOutput {
    std::vector<Note> notes;
    std::vector<FrameEvidenceTrack> evidence;
    std::optional<PitchActivationGrid> activations;
};

struct ModelResources {
    bool loaded{};
    std::size_t live_sessions{};
    std::uintmax_t model_file_bytes{};
    std::size_t last_input_bytes{};
    std::size_t last_evidence_frames{};
    std::size_t completed_inferences{};
    double load_seconds{};
    double last_inference_seconds{};
    std::optional<std::size_t> resident_bytes;
    std::size_t cached_bytes{};
    bool result_cache_hit{};
};

struct BackendDiagnostics {
    OnnxExecutionProvider requested_provider{OnnxExecutionProvider::Cpu};
    std::optional<OnnxExecutionProvider> active_provider;
    int device_id{};
    std::string runtime_version;
    bool cpu_fallback_disabled{};
    std::string message;
};

// Instances are single-owner/non-reentrant. No inference runs on a capture callback.
class ModelBackend {
public:
    virtual ~ModelBackend() = default;
    virtual const ModelMetadata& metadata() const noexcept = 0;
    virtual ModelOperation load(const ModelLoadOptions& options = {}) = 0;
    virtual void release() noexcept = 0;
    virtual InferenceResult<InferenceOutput> infer(
        const AudioBuffer& audio, const InferenceRequest& request = {}) = 0;
    virtual ModelOperation warm_up(const InferenceRequest& request = {}) = 0;
    virtual InferenceResult<InferenceOutput> infer_incremental(
        const AudioBuffer& block, bool final_block, const InferenceRequest& request = {});
    virtual ModelResources resources() const noexcept = 0;
    virtual BackendDiagnostics diagnostics() const = 0;
};

class ModelRegistry {
public:
    using Factory = std::function<std::unique_ptr<ModelBackend>(const ModelDefinition&)>;
    explicit ModelRegistry(const std::filesystem::path& onnx_path = {});
    ModelOperation add(ModelDefinition definition, Factory factory);
    ModelOperation add_onnx(ModelDefinition definition);
    std::vector<ModelDefinition> discover() const;
    InferenceResult<std::unique_ptr<ModelBackend>> create(const std::string& id) const;

private:
    struct Entry { ModelDefinition definition; Factory factory; };
    std::vector<Entry> entries_;
};

struct FileInferenceOptions {
    double chunk_seconds{8};
    double overlap_seconds{1};
    std::size_t maximum_audio_bytes{8 * 1024 * 1024};
    std::size_t maximum_pending_notes{4096};
    std::size_t maximum_retained_notes{1000000};
    bool retain_notes{true};
    std::function<void(std::span<const Note>)> notes_callback;
    std::function<void(const AudioBlockView&)> audio_callback;
};

struct FileInferenceResult {
    std::vector<Note> notes;
    std::uint32_t sample_rate{};
    std::size_t source_frames{};
    std::size_t peak_buffered_audio_bytes{};
    std::size_t processed_windows{};
    double first_window_seconds{};
};

// Bounded overlapping offline windows, not stateful neural streaming or full-buffer equivalence.
InferenceResult<FileInferenceResult> infer_wav_file(
    ModelBackend& backend, const std::filesystem::path& path,
    const InferenceRequest& request = {}, const FileInferenceOptions& options = {});

// ONNX Runtime availability; the built-in CPU trackers do not require ONNX.
std::vector<ProviderAvailability> inference_providers();
std::string inference_provider_name(OnnxExecutionProvider provider);

} // namespace transcribe
