#pragma once

#include "transcribe/transcriber.hpp"

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
};

struct InferenceRequest {
    AnalysisOptions analysis;
    OnnxOptions onnx;
    std::stop_token cancellation;
    std::function<void(double)> progress;
    bool retain_evidence{};
};

struct InferenceOutput {
    std::vector<Note> notes;
    std::vector<FrameEvidenceTrack> evidence;
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

// ONNX Runtime availability; the built-in CPU trackers do not require ONNX.
std::vector<ProviderAvailability> inference_providers();
std::string inference_provider_name(OnnxExecutionProvider provider);

} // namespace transcribe
