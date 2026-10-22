// Registration/lifecycle example. The analysis delegates to YIN rather than adding a new DSP algorithm.
#include "transcribe/engine.hpp"

#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

namespace {
using namespace transcribe;

class ExampleBackend final : public ModelBackend {
public:
    explicit ExampleBackend(ModelDefinition definition) : definition_(std::move(definition)) {
        ModelRegistry builtins;
        auto result = builtins.create("builtin:yin");
        if (const auto* error = std::get_if<InferenceError>(&result))
            throw std::runtime_error(error->message);
        delegate_ = std::move(std::get<std::unique_ptr<ModelBackend>>(result));
        describe();
    }
    const ModelMetadata& metadata() const noexcept override { return metadata_; }
    ModelOperation load(const ModelLoadOptions& options) override {
        auto result = delegate_->load(options);
        describe();
        return result;
    }
    void release() noexcept override { delegate_->release(); }
    InferenceResult<InferenceOutput> infer(const AudioBuffer& audio, const InferenceRequest& request) override {
        return delegate_->infer(audio, request);
    }
    ModelOperation warm_up(const InferenceRequest& request) override { return delegate_->warm_up(request); }
    ModelResources resources() const noexcept override { return delegate_->resources(); }
    BackendDiagnostics diagnostics() const override { return delegate_->diagnostics(); }

private:
    void describe() {
        metadata_ = delegate_->metadata();
        metadata_.definition = definition_;
    }
    ModelDefinition definition_;
    ModelMetadata metadata_;
    std::unique_ptr<ModelBackend> delegate_;
};

void require_success(const ModelOperation& operation) {
    if (const auto* error = std::get_if<InferenceError>(&operation))
        throw std::runtime_error(error->message);
}
}

int main() {
    try {
        EngineRegistry engines;
        ModelDefinition definition;
        definition.id = "example:yin";
        definition.contract = ModelContract::Yin;
        definition.objective = PitchObjective::Monophonic;
        definition.provenance = "Reference adapter delegating Transcriber's existing YIN implementation";
        definition.license = "MIT";
        require_success(engines.add_model(definition, [](const auto& model) {
            return std::make_unique<ExampleBackend>(model);
        }));
        EngineRequirements requirements;
        requirements.id = definition.id;
        requirements.sample_rate = 8000;
        const auto selection = engines.select(requirements);
        if (!selection.selected_id) throw std::runtime_error("Example engine is unavailable");
        auto created = engines.create(*selection.selected_id);
        if (const auto* error = std::get_if<InferenceError>(&created))
            throw std::runtime_error(error->message);
        auto engine = std::move(std::get<std::unique_ptr<TranscriptionEngine>>(created));
        require_success(engine->initialize());
        EngineConfiguration configuration;
        configuration.audio_source_id = "generated-reference-tone";
        configuration.inference.retain_evidence = true;
        require_success(engine->configure(configuration));
        AudioBuffer audio{8000, std::vector<float>(8000)};
        for (std::size_t i = 0; i < audio.samples.size(); ++i)
            audio.samples[i] = static_cast<float>(.4 * std::sin(2 * std::numbers::pi * 220 * i / audio.sample_rate));
        auto result = engine->transcribe(audio);
        if (const auto* error = std::get_if<InferenceError>(&result))
            throw std::runtime_error(error->message);
        const auto& update = std::get<EngineUpdate>(result);
        if (update.events.empty() || !update.activations || update.provenance.engine_id != definition.id)
            throw std::runtime_error("Example engine returned an incomplete result");
        if (engine->incremental())
            throw std::runtime_error("The reference offline plugin must not silently become built-in live YIN");
        std::cout << update.provenance.engine_id << ": " << update.events.size() << " timestamped events\n";
        engine->shutdown();
    } catch (const std::exception& error) {
        std::cerr << "Engine example: " << error.what() << '\n';
        return 1;
    }
}
