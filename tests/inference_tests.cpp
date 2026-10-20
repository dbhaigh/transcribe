#include "transcribe/inference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>

namespace {
transcribe::AudioBuffer model_tone(std::uint32_t rate = 22050) {
    transcribe::AudioBuffer audio{rate, std::vector<float>(rate)};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(.4 * std::sin(2 * std::numbers::pi * 440 * i / rate));
    }
    return audio;
}

void same_notes(const std::vector<transcribe::Note>& a, const std::vector<transcribe::Note>& b) {
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a[i].midi_pitch == b[i].midi_pitch);
        REQUIRE(a[i].start_seconds == b[i].start_seconds);
        REQUIRE(a[i].duration_seconds == b[i].duration_seconds);
        REQUIRE(a[i].confidence == b[i].confidence);
    }
}
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
