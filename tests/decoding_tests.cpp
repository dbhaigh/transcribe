#include "transcribe/decoding.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <fstream>
#include <limits>
#include <numbers>
#include <set>

namespace {
transcribe::AudioBuffer tone(std::uint32_t rate, double duration = 1) {
    transcribe::AudioBuffer audio{rate, std::vector<float>(
        static_cast<std::size_t>(std::llround(duration * rate)))};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(.4 * std::sin(
            2 * std::numbers::pi * 440 * i / rate));
    }
    return audio;
}

transcribe::FrameEvidenceTrack track(int pitch = 69) {
    transcribe::FrameEvidenceTrack result;
    result.hop_seconds = .01;
    result.method = "yin";
    result.evidence_kind = "raw-test-score";
    for (int i = 0; i < 20; ++i) {
        result.frames.push_back({pitch, .2 + i * .01, .9, .8});
    }
    return result;
}

auto decode(const transcribe::AudioBuffer& audio,
            const std::vector<transcribe::FrameEvidenceTrack>& tracks,
            const transcribe::TemporalDecodingOptions& options = {}) {
    return transcribe::decode_note_events(audio, tracks, options, "test-audio-sha256", "test-model");
}

void write_wav(const std::filesystem::path& path, const transcribe::AudioBuffer& audio) {
    std::ofstream output(path, std::ios::binary);
    REQUIRE(output.good());
    const auto word = [&](std::uint16_t n) {
        output.put(static_cast<char>(n & 255));
        output.put(static_cast<char>(n >> 8));
    };
    const auto dword = [&](std::uint32_t n) {
        word(static_cast<std::uint16_t>(n));
        word(static_cast<std::uint16_t>(n >> 16));
    };
    output.write("RIFF", 4); dword(36 + static_cast<std::uint32_t>(audio.samples.size()) * 2);
    output.write("WAVEfmt ", 8); dword(16); word(1); word(1);
    dword(audio.sample_rate); dword(audio.sample_rate * 2); word(2); word(16);
    output.write("data", 4); dword(static_cast<std::uint32_t>(audio.samples.size()) * 2);
    for (const float sample : audio.samples) {
        word(static_cast<std::uint16_t>(static_cast<std::int16_t>(sample * 32767)));
    }
    output.close();
    REQUIRE(output.good());
}
}

TEST_CASE("canonical events retain identity provenance and uncalibrated evidence", "[decoding]") {
    auto frames = track();
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    const auto audio = tone(44100);
    const auto first = decode(audio, {frames}, options);
    const auto second = decode(audio, {frames}, options);
    REQUIRE(first.events.size() == 1);
    const auto& e = first.events.front();
    REQUIRE(e.id == second.events.front().id);
    REQUIRE(e.onset_sample == 8820);
    REQUIRE(e.offset_sample == 17640);
    REQUIRE(e.source_sample_rate == 44100);
    REQUIRE(e.audio_source_id == "test-audio-sha256");
    REQUIRE(e.source_model == "test-model");
    REQUIRE(e.state == transcribe::EventLifecycle::Stabilized);
    REQUIRE_FALSE(e.calibrated_confidence);
    REQUIRE_FALSE(e.velocity);
    REQUIRE_FALSE(e.instrument);
    REQUIRE_FALSE(e.voice);
    REQUIRE_FALSE(e.articulation);
    REQUIRE(e.intensity_rms.value() > .1);
    REQUIRE(e.evidence.size() == frames.frames.size());
    REQUIRE(e.evidence.front().raw_strength == .9);
    REQUIRE_FALSE(e.evidence.front().fractional_midi);
    REQUIRE(first.raw_tracks.front().frames.size() == frames.frames.size());
}

TEST_CASE("temporal hysteresis gaps duration and overlap have explicit semantics", "[decoding]") {
    const auto audio = tone(22050);
    auto f = track();
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    options.activation_on = .5;
    options.activation_off = .3;
    options.maximum_gap_seconds = .04;
    f.frames[0].confidence = .4;
    f.frames[5].confidence = .4;
    f.frames[8].midi_pitch = -1;
    auto result = decode(audio, {f}, options);
    REQUIRE(result.events.size() == 1);
    REQUIRE(std::abs(result.events.front().onset_seconds - .21) < .001);
    options.activation_off = .45;
    options.maximum_gap_seconds = 0;
    result = decode(audio, {f}, options);
    REQUIRE(result.events.size() == 1);
    REQUIRE_FALSE(result.rejected_events.empty());
    options.minimum_duration_seconds = .005;
    result = decode(audio, {f, track(64)}, options);
    REQUIRE(result.events.size() >= 3);
    REQUIRE(std::any_of(result.events.begin(), result.events.end(),
                        [](const auto& e) { return e.midi_pitch == 64; }));
    options.minimum_duration_seconds = 1;
    REQUIRE(decode(audio, {track()}, options).events.empty());
}

TEST_CASE("pitch stability retains vibrato evidence without hiding sustained pitch changes",
          "[decoding]") {
    auto f = track();
    for (std::size_t i = 0; i < f.frames.size(); ++i) {
        const double pitch = 69 + .65 * std::sin(i * .7);
        f.frames[i].fractional_midi = pitch;
        f.frames[i].midi_pitch = static_cast<int>(std::lround(pitch));
    }
    const auto stable = transcribe::stabilize_pitch_activity(f, {});
    REQUIRE(std::all_of(stable.begin(), stable.end(), [](int pitch) { return pitch == 69; }));
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    const auto result = decode(tone(22050), {f}, options);
    REQUIRE(result.events.size() == 1);
    REQUIRE(result.events.front().evidence[2].fractional_midi == f.frames[2].fractional_midi);
    REQUIRE(result.events.front().evidence[2].midi_pitch == f.frames[2].midi_pitch);
    for (std::size_t i = 10; i < f.frames.size(); ++i) {
        f.frames[i].midi_pitch = 70;
        f.frames[i].fractional_midi = 70;
    }
    REQUIRE(decode(tone(22050), {f}, options).events.size() == 2);
    options.pitch_stability_seconds = 0;
    const auto original = transcribe::stabilize_pitch_activity(f, options);
    REQUIRE(original[2] == f.frames[2].midi_pitch);
}

TEST_CASE("state hysteresis releases at last supported frame and can bridge a brief dropout",
          "[decoding]") {
    auto f = track();
    f.method = "model-test";
    f.raw_midi_pitch = 69;
    f.note_states.assign(f.frames.size(), .6);
    for (auto& frame : f.frames) frame.midi_pitch = -1;
    f.note_states[0] = .4;
    f.note_states[5] = .35;
    f.note_states[10] = .1;
    f.note_states.back() = .1;
    transcribe::TemporalDecodingOptions options;
    options.activation_on = .5;
    options.activation_off = .3;
    options.maximum_gap_seconds = .011;
    const auto segments = transcribe::detect_pitch_activity_regions(f, options);
    REQUIRE(segments.size() == 1);
    REQUIRE(std::abs(segments[0].onset_seconds - .21) < 1e-10);
    REQUIRE(std::abs(segments[0].offset_seconds - .39) < 1e-10);
    REQUIRE(segments[0].frame_indices.size() == 17);
    options.maximum_gap_seconds = 0;
    REQUIRE(transcribe::segment_pitch_activity(f, options).size() == 2);
}

TEST_CASE("short semitone notes remain distinct and evidence duration uses frame centers",
          "[decoding]") {
    auto f = track();
    for (std::size_t i = 7; i < 10; ++i) {
        f.frames[i].midi_pitch = 70;
        f.frames[i].fractional_midi = 70;
    }
    transcribe::TemporalDecodingOptions options;
    options.minimum_duration_seconds = .02;
    options.refine_with_audio = false;
    REQUIRE(decode(tone(22050), {f}, options).events.size() == 3);
    f.window_seconds = .1;
    const auto result = decode(tone(22050), {f, f}, options);
    REQUIRE(result.events.size() == 3);
    const auto& e = result.events.front();
    const auto diagnostic = transcribe::summarize_event_evidence(e, .01, .1);
    REQUIRE(std::abs(diagnostic.activity_duration_seconds - .07) <= 1.0 / 22050);
    REQUIRE(diagnostic.pitch_stability == 1);
    REQUIRE_FALSE(diagnostic.maximum_note_state);
    auto unsupported = e;
    unsupported.evidence.clear();
    const auto absent = transcribe::summarize_event_evidence(unsupported, .01, .1);
    REQUIRE(absent.activity_duration_seconds == 0);
    REQUIRE_FALSE(absent.pitch_stability);
    REQUIRE_FALSE(absent.onset_strength);
    REQUIRE_THROWS(transcribe::summarize_event_evidence(e, 0));
}

TEST_CASE("new decoder parameters and aligned evidence fail explicitly on invalid input",
          "[decoding]") {
    auto f = track();
    f.onset_strengths = {.5};
    REQUIRE_THROWS(transcribe::segment_pitch_activity(f, {}));
    f.onset_strengths.assign(f.frames.size(), .5);
    f.note_states.assign(f.frames.size(), .8);
    f.note_states[3] = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS(transcribe::segment_pitch_activity(f, {}));
    f.note_states.clear();
    transcribe::TemporalDecodingOptions options;
    for (auto member : {&transcribe::TemporalDecodingOptions::pitch_stability_seconds,
                       &transcribe::TemporalDecodingOptions::pitch_tolerance_semitones,
                       &transcribe::TemporalDecodingOptions::onset_threshold,
                       &transcribe::TemporalDecodingOptions::minimum_onset_spacing_seconds,
                       &transcribe::TemporalDecodingOptions::onset_energy_rise,
                       &transcribe::TemporalDecodingOptions::onset_state_drop}) {
        options.*member = std::numeric_limits<double>::quiet_NaN();
        REQUIRE_THROWS(transcribe::segment_pitch_activity(f, options));
        options = {};
        options.*member = -1;
        REQUIRE_THROWS(transcribe::segment_pitch_activity(f, options));
        options = {};
    }
    REQUIRE(transcribe::segment_pitch_activity(f, {}).size() == 1);
}

TEST_CASE("model onset and note state distinguish repeated notes from vibrato onset ripples",
          "[decoding]") {
    auto f = track();
    f.method = "basic-pitch";
    f.raw_midi_pitch = 69;
    f.onset_strengths.assign(f.frames.size(), .1);
    f.note_states.assign(f.frames.size(), .9);
    f.onset_strengths[10] = .9;
    REQUIRE(transcribe::segment_pitch_activity(f, {}).size() == 1);
    f.note_states[8] = .6;
    f.note_states[9] = .7;
    const auto segments = transcribe::segment_pitch_activity(f, {});
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].offset_from_attack);
    REQUIRE(segments[1].onset_from_attack);
    REQUIRE(segments[0].offset_seconds == segments[1].onset_seconds);
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    const auto result = decode(tone(22050), {f}, options);
    REQUIRE(result.events.size() == 2);
    REQUIRE(result.events[1].evidence.front().onset_strength == .9);
    REQUIRE(result.events[1].evidence.front().note_state == .9);
    options.minimum_onset_spacing_seconds = 0;
    REQUIRE(transcribe::segment_pitch_activity(f, options).size() == 2);
    options.minimum_onset_spacing_seconds = .07;
    f.onset_strengths[11] = .9;
    REQUIRE(transcribe::segment_pitch_activity(f, {}).size() == 2);
    f.note_states[10] = .6;
    REQUIRE(transcribe::segment_pitch_activity(f, {}).size() == 1);
    f.note_states[10] = .9;
    options.use_model_onsets = false;
    REQUIRE(decode(tone(22050), {f}, options).events.size() == 1);
    f.onset_strengths.assign(f.frames.size(), .1);
    f.onset_strengths[16] = .9;
    f.note_states.assign(f.frames.size(), .9);
    f.note_states[14] = .6;
    f.note_states[15] = .7;
    REQUIRE(transcribe::segment_pitch_activity(f, {}).size() == 1);
    options.minimum_duration_seconds = .02;
    options.use_model_onsets = true;
    REQUIRE(transcribe::segment_pitch_activity(f, options).size() == 2);
}

TEST_CASE("audio attacks split repeated YIN notes but not amplitude-stable pitch motion",
          "[decoding]") {
    auto f = track();
    std::vector<transcribe::OnsetEvidence> onsets{{.3, .8, .7, .6, .5}};
    const auto repeated = transcribe::segment_pitch_activity(f, {}, onsets);
    REQUIRE(repeated.size() == 2);
    onsets[0].energy_rise = .01;
    REQUIRE(transcribe::segment_pitch_activity(f, {}, onsets).size() == 1);
    f.method = "spectral";
    onsets[0].energy_rise = .5;
    REQUIRE(transcribe::segment_pitch_activity(f, {}, onsets).size() == 1);
}

TEST_CASE("quiet valid notes are not discarded by another note's global peak",
          "[decoding]") {
    auto audio = tone(22050);
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double time = static_cast<double>(i) / audio.sample_rate;
        if (time < .15 || (time >= .4 && time < .6) || time >= .85) audio.samples[i] = 0;
        else if (time < .4) audio.samples[i] *= .015f;
    }
    auto f = track();
    f.frames.clear();
    for (int i = 0; i < 100; ++i) f.frames.push_back({69, i * .01, .9, .9});
    const auto result = decode(audio, {f});
    REQUIRE(result.events.size() == 2);
    REQUIRE(std::abs(result.events[0].onset_seconds - .15) < .006);
    REQUIRE(std::abs(result.events[0].offset_seconds - .4) < .006);
    REQUIRE(result.events[0].intensity_rms.value() < .005);
    REQUIRE(result.events[1].intensity_rms.value() > .1);
}

TEST_CASE("onset evidence preserves legacy timing and exposes independent flux and HFC",
          "[decoding]") {
    auto audio = tone(22050);
    std::fill(audio.samples.begin(), audio.samples.begin() + 5000, 0);
    const auto evidence = transcribe::detect_onset_evidence(audio);
    const auto legacy = transcribe::detect_onsets(audio);
    REQUIRE(evidence.size() == legacy.size());
    REQUIRE_FALSE(evidence.empty());
    for (std::size_t i = 0; i < evidence.size(); ++i) {
        REQUIRE(evidence[i].time_seconds == legacy[i]);
        REQUIRE(evidence[i].strength > 0);
        REQUIRE(evidence[i].spectral_flux >= 0);
        REQUIRE(evidence[i].high_frequency_content >= 0);
    }
    transcribe::OnsetDetectionOptions invalid;
    invalid.minimum_spacing_seconds = -1;
    REQUIRE_THROWS(transcribe::detect_onset_evidence(audio, invalid));
}

TEST_CASE("temporal audio refinement splits repeats rejects silence and retains short evidence",
          "[decoding]") {
    auto audio = tone(48000);
    std::fill(audio.samples.begin(), audio.samples.begin() + 4800, 0);
    std::fill(audio.samples.begin() + 14400, audio.samples.begin() + 19200, 0);
    std::fill(audio.samples.begin() + 33600, audio.samples.end(), 0);
    auto f = track();
    f.frames.clear();
    for (int i = 0; i < 80; ++i) f.frames.push_back({69, i * .01, .9, .9});
    const auto result = decode(audio, {f});
    REQUIRE(result.events.size() == 2);
    REQUIRE(result.events[0].id != result.events[1].id);
    REQUIRE(std::abs(result.events[0].onset_seconds - .1) < .006);
    REQUIRE(std::abs(result.events[0].offset_seconds - .3) < .006);
    REQUIRE(std::abs(result.events[1].onset_seconds - .4) < .006);
    REQUIRE(std::abs(result.events[1].offset_seconds - .7) < .006);
    std::fill(audio.samples.begin(), audio.samples.end(), 0);
    const auto silent = decode(audio, {f});
    REQUIRE(silent.events.empty());
    REQUIRE_FALSE(silent.rejected_events.empty());
    f.frames.resize(1);
    REQUIRE(decode(tone(48000), {f}).events.empty());
    REQUIRE(decode(tone(48000), {f}).raw_tracks.front().frames.size() == 1);
}

TEST_CASE("isolated YIN octave correction retains original evidence and sustained octave changes",
          "[decoding]") {
    auto f = track();
    f.frames[9].midi_pitch = 81;
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    options.maximum_gap_seconds = .04;
    options.correct_isolated_octaves = true;
    options.minimum_duration_seconds = .005;
    auto result = decode(tone(22050), {f}, options);
    REQUIRE(result.events.size() == 1);
    REQUIRE(result.events.front().evidence[9].midi_pitch == 81);
    options.correct_isolated_octaves = false;
    REQUIRE(decode(tone(22050), {f}, options).events.size() == 2);
    options.correct_isolated_octaves = true;
    for (std::size_t i = 10; i < f.frames.size(); ++i) f.frames[i].midi_pitch = 81;
    REQUIRE(decode(tone(22050), {f}, options).events.size() == 2);
}

TEST_CASE("temporal sample timestamps are explicit across rates and window origins", "[decoding]") {
    for (const auto rate : {8000U, 11025U, 22050U, 44100U, 48000U}) {
        auto f = track();
        f.window_seconds = .1;
        transcribe::TemporalDecodingOptions options;
        options.refine_with_audio = false;
        const auto result = decode(tone(rate), {f}, options);
        REQUIRE(result.events.size() == 1);
        REQUIRE(result.events.front().onset_sample == static_cast<std::uint64_t>(
            std::llround(.25 * rate)));
        REQUIRE(result.events.front().offset_sample == static_cast<std::uint64_t>(
            std::llround(.45 * rate)));
    }
}

TEST_CASE("every temporal threshold is validated and spectral evidence rejects diffuse noise",
          "[decoding]") {
    const auto audio = tone(22050);
    for (const double value : {-1., std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity()}) {
        for (int field = 0; field < 7; ++field) {
            transcribe::TemporalDecodingOptions o;
            double* fields[] = {&o.activation_on, &o.activation_off, &o.maximum_gap_seconds,
                &o.minimum_duration_seconds, &o.energy_floor_rms, &o.energy_relative_threshold,
                &o.spectral_peak_fraction};
            *fields[field] = value;
            REQUIRE_THROWS(decode(audio, {track()}, o));
        }
    }
    transcribe::TemporalDecodingOptions o;
    o.activation_off = .9;
    REQUIRE_THROWS(decode(audio, {track()}, o));
    o = {};
    auto f = track();
    f.spectral_peak_fractions.assign(f.frames.size(), .01);
    REQUIRE(decode(audio, {f}, o).events.empty());
    o.spectral_peak_fraction = .005;
    REQUIRE(decode(audio, {f}, o).events.size() == 1);
    o.energy_floor_rms = 1;
    REQUIRE(decode(audio, {track()}, o).events.empty());
    o = {};
    o.energy_relative_threshold = 1;
    REQUIRE(decode(audio, {track()}, o).events.size() <= 1);
    for (const int field : {0, 1, 2, 3}) {
        o = {};
        if (field == 0) o.activation_on = 1.1;
        if (field == 1) o.minimum_duration_seconds = 0;
        if (field == 2) o.energy_relative_threshold = 1.1;
        if (field == 3) o.spectral_peak_fraction = 1.1;
        REQUIRE_THROWS(decode(audio, {track()}, o));
    }
    f.hop_seconds = 0;
    REQUIRE_THROWS(decode(audio, {f}));
}

TEST_CASE("opt-in frame evidence does not change legacy notes", "[decoding]") {
    const auto audio = tone(22050);
    for (const bool polyphonic : {false, true}) {
        transcribe::AnalysisOptions options;
        options.polyphonic = polyphonic;
        const auto before = transcribe::transcribe_monophonic(audio, options);
        std::size_t frames = 0;
        options.evidence_callback = [&](const auto& evidence) { frames += evidence.frames.size(); };
        const auto after = transcribe::transcribe_monophonic(audio, options);
        REQUIRE(frames > 0);
        REQUIRE(before.size() == after.size());
        for (std::size_t i = 0; i < before.size(); ++i) {
            REQUIRE(before[i].midi_pitch == after[i].midi_pitch);
            REQUIRE(before[i].start_seconds == after[i].start_seconds);
            REQUIRE(before[i].duration_seconds == after[i].duration_seconds);
            REQUIRE(before[i].confidence == after[i].confidence);
        }
    }
}

TEST_CASE("canonical streaming reconciles holds repeats and overlap across chunk boundaries", "[decoding]") {
    const auto path = std::filesystem::temp_directory_path() /
        ("decode-boundary-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); }
    } cleanup{path};
    for (const auto rate : {11025U, 48000U}) {
        for (const double onset : {.7, .8, .875, .95, 1.05}) {
            for (const int scenario : {0, 1, 2}) {
                auto audio = tone(rate, 3);
                for (std::size_t i = 0; i < audio.samples.size(); ++i) {
                    const double time = static_cast<double>(i) / rate;
                    if (time < onset || time >= onset + 1 ||
                        (scenario == 1 && time >= onset + .3 && time < onset + .42)) {
                        audio.samples[i] = 0;
                    }
                    if (scenario == 2 && time >= onset + .2 && time < onset + .8) {
                        audio.samples[i] += static_cast<float>(.3 * std::sin(
                            2 * std::numbers::pi * 329.62755691287 * time));
                    }
                }
                write_wav(path, audio);
                transcribe::StreamingOptions options;
                options.chunk_seconds = 1;
                options.overlap_seconds = .25;
                options.analysis.polyphonic = scenario == 2;
                double progress = 0;
                options.analysis.progress_callback = [&](double value) {
                    REQUIRE(value >= progress);
                    REQUIRE(value <= 1);
                    progress = value;
                };
                const auto decoded = transcribe::transcribe_note_events_wav_streaming(
                    path, options, {}, "boundary");
                INFO("rate=" << rate << " onset=" << onset << " scenario=" << scenario);
                REQUIRE(progress == 1);
                REQUIRE(decoded.events.size() == (scenario == 0 ? 1 : 2));
                REQUIRE(decoded.events.front().midi_pitch == 69);
                REQUIRE(std::abs(decoded.events.front().onset_seconds - onset) < .05);
                REQUIRE_FALSE(decoded.raw_tracks.empty());
                std::set<std::string> ids;
                for (const auto& event : decoded.events) {
                    REQUIRE(ids.insert(event.id).second);
                    REQUIRE(std::is_sorted(event.evidence.begin(), event.evidence.end(),
                        [](const auto& a, const auto& b) { return a.time_seconds < b.time_seconds; }));
                    const double end = event.midi_pitch == 64 ? onset + .8 :
                        (scenario == 1 && event.onset_seconds < onset + .3 ? onset + .3 : onset + 1);
                    const double start = event.midi_pitch == 64 ? onset + .2 :
                        (scenario == 1 && event.onset_seconds > onset + .3 ? onset + .42 : onset);
                    REQUIRE(std::abs(event.onset_seconds - start) < .05);
                    REQUIRE(std::abs(event.offset_seconds - end) < .1);
                }
            }
        }
    }
}

TEST_CASE("canonical evidence validates provenance raw shapes and short event configuration", "[decoding]") {
    const auto audio = tone(22050);
    auto f = track();
    f.raw_values.assign(f.frames.size(), 2.5);
    f.raw_midi_pitch = 69;
    const auto result = decode(audio, {f});
    REQUIRE(result.events.front().evidence.front().raw_model_value == 2.5);
    REQUIRE(result.raw_tracks.front().raw_values == f.raw_values);
    REQUIRE(result.events.front().evidence.front().raw_midi_pitch == 69);
    f.raw_values.pop_back();
    REQUIRE_THROWS(decode(audio, {f}));
    f.raw_values.assign(f.frames.size(), std::numeric_limits<double>::quiet_NaN());
    REQUIRE_THROWS(decode(audio, {f}));
    f.raw_values.clear();
    f.raw_midi_pitch = 128;
    REQUIRE_THROWS(decode(audio, {f}));
    f = track();
    f.frames[2].time_seconds = .1;
    REQUIRE_THROWS(decode(audio, {f}));
    REQUIRE_THROWS(transcribe::decode_note_events(audio, {}, {}, "", "model"));
    REQUIRE_THROWS(transcribe::decode_note_events(audio, {}, {}, "audio", ""));
    auto invalid_audio = audio;
    invalid_audio.samples[0] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS(transcribe::transcribe_note_events(invalid_audio, {}, {}, "audio"));
    REQUIRE(decode({22050, {}}, {}).events.empty());
    f = track();
    f.frames.resize(4);
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    REQUIRE(decode(audio, {f}, options).events.empty());
    options.minimum_duration_seconds = .03;
    REQUIRE(decode(audio, {f}, options).events.size() == 1);
    REQUIRE(decode(audio, {track(), track()}, options).events.size() == 1);
}

TEST_CASE("octave correction respects observed attacks", "[decoding]") {
    auto audio = tone(11025);
    std::fill(audio.samples.begin(), audio.samples.begin() + 3000, 0);
    const auto onsets = transcribe::detect_onsets(audio);
    REQUIRE_FALSE(onsets.empty());
    const double onset = onsets.front() + 512.0 / audio.sample_rate;
    auto f = track();
    f.frames = {{69, onset - .01, .9, .9}, {81, onset, .9, .9},
                {69, onset + .01, .9, .9}};
    transcribe::TemporalDecodingOptions options;
    options.refine_with_audio = false;
    options.maximum_gap_seconds = .04;
    options.minimum_duration_seconds = .005;
    options.correct_isolated_octaves = true;
    const auto result = decode(audio, {f}, options);
    REQUIRE(result.events.size() + result.rejected_events.size() == 2);
    const auto is_octave = [](const auto& event) { return event.midi_pitch == 81; };
    REQUIRE((std::any_of(result.events.begin(), result.events.end(), is_octave) ||
             std::any_of(result.rejected_events.begin(), result.rejected_events.end(), is_octave)));
}

#ifdef TRANSCRIBE_ENABLE_ONNX
TEST_CASE("Basic Pitch canonical adapter retains raw note heads without changing legacy output", "[decoding][onnx]") {
    const auto audio = tone(22050);
    transcribe::OnnxOptions options;
    options.polyphonic = true;
    const auto before = transcribe::transcribe_with_onnx(audio, TRANSCRIBE_BASIC_PITCH_MODEL_PATH, options);
    std::size_t observed = 0;
    options.evidence_callback = [&](const auto& track) {
        ++observed;
        REQUIRE(track.raw_values.size() == track.frames.size());
        REQUIRE(track.onset_strengths.size() == track.frames.size());
        REQUIRE(track.note_states == track.raw_values);
        REQUIRE(track.raw_midi_pitch >= 28);
        REQUIRE(track.raw_midi_pitch <= 96);
    };
    const auto after = transcribe::transcribe_with_onnx(audio, TRANSCRIBE_BASIC_PITCH_MODEL_PATH, options);
    REQUIRE(observed == 69);
    REQUIRE(before.size() == after.size());
    for (std::size_t i = 0; i < before.size(); ++i) {
        REQUIRE(before[i].midi_pitch == after[i].midi_pitch);
        REQUIRE(before[i].start_seconds == after[i].start_seconds);
        REQUIRE(before[i].duration_seconds == after[i].duration_seconds);
        REQUIRE(before[i].confidence == after[i].confidence);
    }
    const auto canonical = transcribe::transcribe_note_events_with_onnx(
        audio, TRANSCRIBE_BASIC_PITCH_MODEL_PATH, options, {}, "audio", "model-sha256");
    REQUIRE_FALSE(canonical.events.empty());
    REQUIRE(canonical.raw_tracks.size() == 69);
    REQUIRE(canonical.events.front().source_model == "model-sha256");
    REQUIRE(canonical.events.front().evidence.front().raw_model_value);
    REQUIRE(canonical.events.front().evidence.front().onset_strength);
    REQUIRE(canonical.events.front().evidence.front().note_state);
    REQUIRE_FALSE(canonical.events.front().calibrated_confidence);
}
#endif
