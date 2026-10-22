#pragma once

#include "transcribe/live.hpp"

namespace transcribe {

inline constexpr std::uint32_t engine_contract_version = 1;
inline constexpr std::uint32_t engine_configuration_version = 1;

enum class EngineState { Created, Ready, Processing, Streaming, Finished, Cancelled, Failed, Shutdown };
enum class EngineStreaming { None, Incremental, OverlappingWindows };
enum class EngineBoundaryEvidence { None, Heuristic, ModelHead };

struct EngineCapabilities {
    std::vector<InstrumentPrior> instruments;
    std::string instrument_scope;
    bool monophonic{true};
    bool polyphonic{};
    int minimum_midi{};
    int maximum_midi{};
    std::uint32_t minimum_sample_rate{2000};
    // Empty means any rate >= minimum_sample_rate, with adapter-specific resampling.
    std::vector<std::uint32_t> sample_rates;
    EngineStreaming streaming{EngineStreaming::None};
    bool activations{};
    bool streaming_activations{};
    EngineBoundaryEvidence onsets{EngineBoundaryEvidence::None};
    EngineBoundaryEvidence offsets{EngineBoundaryEvidence::None};
    std::vector<OnnxExecutionProvider> providers;
};

struct EngineMetadata {
    std::uint32_t contract_version{engine_contract_version};
    std::string id;
    ModelMetadata model;
    EngineCapabilities capabilities;
};

struct EngineConfiguration {
    std::uint32_t version{engine_configuration_version};
    std::string audio_source_id{"audio"};
    InferenceRequest inference;
    // Unset preserves the existing adapter's segmentation and legacy Note results.
    std::optional<TemporalDecodingOptions> segmentation;
};

struct EngineProvenance {
    std::string engine_id;
    std::string model_id;
    std::uint32_t model_contract{};
    std::string source;
    std::string license;
    std::string method;
    std::string audio_source_id;
};

struct EngineNoteEvent {
    std::string id;
    double midi_pitch{};
    std::uint32_t sample_rate{};
    std::uint64_t onset_frame{};
    std::optional<std::uint64_t> offset_frame;
    EventLifecycle state{EventLifecycle::Stabilized};
    std::uint32_t revision{};
    bool onset_truncated{};
    std::optional<double> heuristic_strength;
    std::optional<double> calibrated_confidence;
    EngineProvenance provenance;
    std::vector<EventEvidence> evidence;
    bool finalized{};

    double onset_seconds() const;
    std::optional<double> offset_seconds() const;
};

struct EngineUpdate {
    std::uint64_t sequence{};
    EngineProvenance provenance;
    std::uint32_t sample_rate{};
    // events is a complete current snapshot, never an implicit append-only delta.
    std::vector<EngineNoteEvent> events;
    std::vector<EngineNoteEvent> rejected_events;
    // Legacy offline projection only; open live notes remain exclusively in events.
    std::vector<Note> notes;
    std::vector<FrameEvidenceTrack> evidence;
    std::optional<PitchActivationGrid> activations;
    // Includes retractions, gap markers, revisions and overflow-resync semantics.
    std::optional<live::EventBatch> live_changes;
    bool finished{};
};

struct EngineDiagnostics {
    EngineState state{EngineState::Created};
    std::optional<InferenceError> last_error;
    BackendDiagnostics backend;
    ModelResources resources;
    std::optional<live::LiveMetrics> live_metrics;
};

class IncrementalEngine : public live::AudioInputSink {
public:
    virtual ~IncrementalEngine() = default;
    // Owner-thread calls. first_frame declares the recording origin explicitly.
    virtual ModelOperation begin(live::LiveOptions options, std::uint64_t first_frame = 0) = 0;
    // Same single-producer bounded callback contract as LiveTranscriptionSession.
    virtual live::PushStatus push(const live::AudioFrameBlock& block) noexcept override = 0;
    virtual void report_input_lost() noexcept override = 0;
    virtual ModelOperation pump() = 0;
    virtual InferenceResult<EngineUpdate> poll() = 0;
    // Quiesce the producer first; end_frame includes known dropped final frames.
    virtual InferenceResult<EngineUpdate> finish(std::uint64_t end_frame) = 0;
};

class TranscriptionEngine;

struct ProgressiveFileOptions {
    live::LiveOptions live;
    std::size_t block_frames{1024};
    std::stop_token cancellation;
    // Owned updates delivered synchronously on the caller/worker, never an audio callback.
    std::function<void(const EngineUpdate&)> update;
};

// Lossless bounded decoding into one persistent engine/session; no whole-file input buffer.
// Retention and snapshot limits remain the explicit LiveOptions limits.
InferenceResult<EngineUpdate> transcribe_wav_progressive(
    TranscriptionEngine& engine, const std::filesystem::path& path,
    const ProgressiveFileOptions& options = {});

// Single owner/non-reentrant except cancel(), and push() during an active stream.
// Decode/acquisition, score interpretation, export and UI remain outside this interface.
class TranscriptionEngine {
public:
    virtual ~TranscriptionEngine() = default;
    virtual const EngineMetadata& metadata() const noexcept = 0;
    virtual ModelOperation initialize(const ModelLoadOptions& options = {}) = 0;
    virtual ModelOperation configure(const EngineConfiguration& configuration) = 0;
    virtual InferenceResult<EngineUpdate> transcribe(
        const AudioBuffer& audio, std::stop_token cancellation = {}) = 0;
    virtual IncrementalEngine* incremental() noexcept { return nullptr; }
    virtual void cancel() noexcept = 0;
    virtual EngineDiagnostics diagnostics() const = 0;
    // Quiesce producers first. Releases and joins all owned resources.
    virtual void shutdown() noexcept = 0;
};

struct EngineRequirements {
    std::optional<std::string> id;
    InstrumentPrior instrument{InstrumentPrior::None};
    bool polyphonic{};
    std::optional<int> minimum_midi;
    std::optional<int> maximum_midi;
    std::optional<std::uint32_t> sample_rate;
    EngineStreaming streaming{EngineStreaming::None};
    bool activations{};
    bool model_onsets{};
    OnnxExecutionProvider provider{OnnxExecutionProvider::Cpu};
};

struct EngineAvailability {
    EngineMetadata metadata;
    bool available{};
    // Availability is a preflight, not proof of tensor compatibility or successful load.
    std::vector<std::string> reasons;
    std::string explanation;
};

struct EngineSelection {
    std::optional<std::string> selected_id;
    std::vector<EngineAvailability> candidates;
    std::vector<std::string> reasons;
};

class EngineRegistry {
public:
    using Factory = std::function<std::unique_ptr<TranscriptionEngine>()>;
    explicit EngineRegistry(const std::filesystem::path& onnx_path = {});
    ModelOperation add(EngineMetadata metadata, Factory factory);
    ModelOperation add_model(ModelDefinition definition, ModelRegistry::Factory factory);
    std::vector<EngineMetadata> discover() const;
    EngineSelection select(const EngineRequirements& requirements = {}) const;
    InferenceResult<std::unique_ptr<TranscriptionEngine>> create(const std::string& id) const;

private:
    struct Entry { EngineMetadata metadata; Factory factory; };
    std::vector<Entry> entries_;
};

} // namespace transcribe
