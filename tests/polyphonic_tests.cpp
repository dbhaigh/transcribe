#include "transcribe/polyphonic.hpp"
#include "transcribe/decoding.hpp"
#include "transcribe/inference.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <set>

namespace {
transcribe::AudioBuffer mixture(const std::vector<int>& pitches, bool distorted = false) {
    transcribe::AudioBuffer audio{22050, std::vector<float>(33075)};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / audio.sample_rate;
        if (t < .2 || t >= 1.2) continue;
        double sample = 0;
        for (const int pitch : pitches) {
            const double frequency = 440 * std::exp2((pitch - 69) / 12.0);
            for (int harmonic = 1; harmonic <= 8; ++harmonic) {
                sample += .12 / pitches.size() / std::pow(harmonic, 1.2) *
                    std::sin(2 * std::numbers::pi * frequency * harmonic * t);
            }
        }
        audio.samples[i] = static_cast<float>(distorted ? std::tanh(sample * 8) : sample);
    }
    return audio;
}
}

TEST_CASE("frame by pitch grids declare raw semantics and reject incompatible track geometry",
          "[polyphonic]") {
    transcribe::FrameEvidenceTrack a;
    a.hop_seconds = .01;
    a.method = "test";
    a.raw_midi_pitch = 60;
    a.raw_value_semantics = transcribe::EvidenceSemantics::RawActivation;
    a.frames = {{60, .1, .8, .8}, {60, .11, .8, .8}};
    a.raw_values = {.3, .5};
    a.onset_strengths = {.7, .2};
    a.contour_midi = {60.1, 60.2};
    auto b = a;
    b.raw_midi_pitch = 61;
    b.raw_values = {.9, .4};
    const auto grid = transcribe::pitch_activation_grid(std::vector{a, b});
    REQUIRE(grid.minimum_midi == 60);
    REQUIRE(grid.maximum_midi == 61);
    REQUIRE(grid.values == std::vector<double>{.3, .9, .5, .4});
    REQUIRE(grid.timestamps == std::vector<double>{.1, .11});
    REQUIRE(grid.semantics == transcribe::EvidenceSemantics::RawActivation);
    REQUIRE(grid.frame_rate_hz() == 100);
    REQUIRE(grid.objective == transcribe::PitchObjective::Unspecified);
    REQUIRE(grid.onsets.size() == grid.values.size());
    REQUIRE(grid.fractional_midi.size() == grid.values.size());
    b.frames[1].time_seconds = .12;
    REQUIRE_THROWS(transcribe::pitch_activation_grid(std::vector{a, b}));
    b = a;
    REQUIRE_THROWS(transcribe::pitch_activation_grid(std::vector{a, b}));
    b.raw_midi_pitch = 62;
    REQUIRE_THROWS(transcribe::pitch_activation_grid(std::vector{a, b}));
    b.raw_midi_pitch = 61;
    b.raw_values.pop_back();
    REQUIRE_THROWS(transcribe::pitch_activation_grid(std::vector{a, b}));
    auto invalid = grid;
    invalid.values[0] = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(transcribe::validate_pitch_activation_grid(invalid));
}

TEST_CASE("bass profile constrains spectral candidates without claiming source recognition",
          "[polyphonic][preprocessing]") {
    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    options.parallel = false;
    options.instrument = transcribe::InstrumentPrior::Bass;
    const auto notes = transcribe::transcribe_monophonic(mixture({40, 80}), options);
    REQUIRE(std::any_of(notes.begin(), notes.end(), [](const auto& note) { return note.midi_pitch == 40; }));
    REQUIRE(std::all_of(notes.begin(), notes.end(),
        [](const auto& note) { return note.midi_pitch >= 28 && note.midi_pitch <= 72; }));
}

TEST_CASE("competing spectral fundamentals retain dyads chords octaves and bass melody",
          "[polyphonic]") {
    for (const auto& pitches : std::vector<std::vector<int>>{
             {52}, {52, 59}, {52, 64}, {40, 69}, {60, 64, 67}, {40, 47, 52, 56, 59, 64},
             {69, 70}}) {
        INFO("polyphony " << pitches.size() << " first " << pitches.front());
        std::vector<transcribe::FrameEvidenceTrack> tracks;
        transcribe::AnalysisOptions options;
        options.polyphonic = true;
        options.parallel = false;
        options.spectral_decoder = transcribe::SpectralDecoder::HarmonicCompetition;
        options.evidence_callback = [&](const auto& track) { tracks.push_back(track); };
        const auto audio = mixture(pitches);
        (void)transcribe::transcribe_monophonic(audio, options);
        const auto grid = transcribe::pitch_activation_grid(tracks);
        REQUIRE(grid.minimum_midi == 28);
        REQUIRE(grid.maximum_midi == 96);
        std::set<int> selected;
        for (const auto& track : tracks) {
            const auto frame = std::find_if(track.frames.begin(), track.frames.end(),
                [](const auto& frame) { return frame.time_seconds >= .5; });
            if (frame != track.frames.end() && frame->midi_pitch >= 0) selected.insert(frame->midi_pitch);
        }
        REQUIRE(selected == std::set<int>(pitches.begin(), pitches.end()));
        const auto decoded = transcribe::decode_note_events(audio, tracks, {}, "fixture", "spectral");
        for (const int pitch : pitches) {
            REQUIRE(std::any_of(decoded.events.begin(), decoded.events.end(),
                [&](const auto& event) { return event.midi_pitch == pitch; }));
        }
    }
}

TEST_CASE("distortion and overlapping release tails preserve supported guitar fundamentals",
          "[polyphonic]") {
    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    options.spectral_decoder = transcribe::SpectralDecoder::HarmonicCompetition;
    options.instrument = transcribe::InstrumentPrior::Guitar;
    auto audio = mixture({40, 47, 52}, true);
    const auto distorted = transcribe::transcribe_note_events(audio, options, {}, "distorted-fixture");
    for (const int pitch : {40, 47, 52}) {
        REQUIRE(std::any_of(distorted.events.begin(), distorted.events.end(),
            [&](const auto& e) { return e.midi_pitch == pitch; }));
    }
    audio = mixture({52});
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double t = static_cast<double>(i) / audio.sample_rate;
        audio.samples[i] *= static_cast<float>(std::exp(-2 * t));
        if (t >= .6 && t < 1.4) {
            audio.samples[i] += static_cast<float>(.08 * std::sin(
                2 * std::numbers::pi * 246.94165 * t));
        }
    }
    const auto tails = transcribe::transcribe_note_events(audio, options, {}, "release-tails");
    REQUIRE(std::any_of(tails.events.begin(), tails.events.end(),
        [](const auto& e) { return e.midi_pitch == 52; }));
    REQUIRE(std::any_of(tails.events.begin(), tails.events.end(),
        [](const auto& e) { return e.midi_pitch == 59; }));
}

TEST_CASE("harmonic dictionaries reject invalid spectrum data", "[polyphonic]") {
    REQUIRE_THROWS(transcribe::HarmonicCompetition(0, 4096, 28, 96));
    REQUIRE_THROWS(transcribe::HarmonicCompetition(11025, 4096, 96, 28));
    transcribe::HarmonicCompetition dictionary(11025, 4096, 28, 96);
    std::vector<double> spectrum(2049);
    REQUIRE(dictionary.activations(spectrum) == std::vector<double>(69));
    spectrum[1] = -1;
    REQUIRE_THROWS(dictionary.activations(spectrum));
    spectrum.resize(20);
    REQUIRE_THROWS(dictionary.activations(spectrum));
}

#ifdef TRANSCRIBE_ENABLE_ONNX
TEST_CASE("Basic Pitch aligned heads retain contour evidence across multiple windows",
          "[polyphonic][onnx]") {
    auto audio = mixture({52, 64});
    audio.samples.resize(22050 * 5);
    transcribe::ModelRegistry registry(TRANSCRIBE_BASIC_PITCH_MODEL_PATH);
    auto created = registry.create("onnx:basic-pitch-v1");
    auto model = std::move(std::get<std::unique_ptr<transcribe::ModelBackend>>(created));
    REQUIRE(std::holds_alternative<std::monostate>(model->load()));
    REQUIRE(model->metadata().objective == transcribe::PitchObjective::IndependentMultiPitch);
    REQUIRE(model->metadata().trimmed_edge_frames == 15);
    REQUIRE(model->metadata().outputs.size() == 3);
    transcribe::InferenceRequest request;
    request.onnx.polyphonic = true;
    request.onnx.aligned_model_heads = true;
    request.retain_evidence = true;
    const auto result = model->infer(audio, request);
    REQUIRE(std::holds_alternative<transcribe::InferenceOutput>(result));
    const auto& output = std::get<transcribe::InferenceOutput>(result);
    REQUIRE(output.activations);
    const auto& grid = *output.activations;
    REQUIRE(grid.objective == transcribe::PitchObjective::IndependentMultiPitch);
    REQUIRE(grid.timestamps.size() == (audio.samples.size() + 255) / 256);
    REQUIRE(grid.fractional_midi.size() == grid.values.size());
    REQUIRE(grid.onsets.size() == grid.values.size());
    REQUIRE(grid.timestamps.back() < 5);
    for (std::size_t i = 0; i < grid.timestamps.size(); ++i) {
        REQUIRE(std::abs(grid.timestamps[i] - i * 256.0 / 22050) < 1e-10);
    }
    const auto legacy = registry.create("onnx:legacy-pitch-v1");
    const auto& other = std::get<std::unique_ptr<transcribe::ModelBackend>>(legacy);
    REQUIRE_FALSE(other->metadata().polyphonic);
    REQUIRE(other->metadata().objective == transcribe::PitchObjective::Unspecified);
    auto conflicting = model->metadata().definition;
    conflicting.id = "conflicting-objective";
    conflicting.objective = transcribe::PitchObjective::Monophonic;
    REQUIRE(std::holds_alternative<transcribe::InferenceError>(registry.add_onnx(conflicting)));
}
#endif
