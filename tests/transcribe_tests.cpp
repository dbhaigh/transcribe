#include "transcribe/audio.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/musicxml.hpp"
#include "transcribe/transcriber.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <string>
#include <vector>

namespace {

struct TemporaryDirectory {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("transcribe-tests-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));

    TemporaryDirectory() {
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void write_u16(std::ostream& output, std::uint16_t value) {
    output.put(static_cast<char>(value & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
}

void write_u32(std::ostream& output, std::uint32_t value) {
    write_u16(output, static_cast<std::uint16_t>(value & 0xffff));
    write_u16(output, static_cast<std::uint16_t>(value >> 16));
}

void create_pcm16_wav(const std::filesystem::path& path, double frequency,
                      double duration_seconds, std::uint32_t sample_rate = 44100) {
    const auto sample_count = static_cast<std::uint32_t>(duration_seconds * sample_rate);
    const auto data_bytes = sample_count * 2;
    std::ofstream output(path, std::ios::binary);
    output.write("RIFF", 4);
    write_u32(output, 36 + data_bytes);
    output.write("WAVEfmt ", 8);
    write_u32(output, 16);
    write_u16(output, 1);
    write_u16(output, 1);
    write_u32(output, sample_rate);
    write_u32(output, sample_rate * 2);
    write_u16(output, 2);
    write_u16(output, 16);
    output.write("data", 4);
    write_u32(output, data_bytes);
    for (std::uint32_t i = 0; i < sample_count; ++i) {
        const auto value = static_cast<std::int16_t>(
            std::lround(0.5 * 32767.0 *
                        std::sin(2.0 * std::numbers::pi * frequency * i / sample_rate)));
        write_u16(output, static_cast<std::uint16_t>(value));
    }
}

} // namespace

TEST_CASE("YIN estimates a synthetic concert A with confidence") {
    constexpr std::uint32_t sample_rate = 11025;
    std::vector<float> samples(2048);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(0.5 * std::sin(
            2.0 * std::numbers::pi * 440.0 * i / sample_rate));
    }

    const auto estimate = transcribe::yin_pitch_estimate(samples, sample_rate);
    REQUIRE(estimate.midi_pitch == 69);
    REQUIRE(estimate.confidence > 0.8);
    REQUIRE(estimate.voicing_probability > 0.8);
}

TEST_CASE("preprocessing gates quiet blocks without mutating the source") {
    transcribe::AudioBuffer audio{44100, std::vector<float>(44100, 0.0001f)};
    for (std::size_t i = 22050; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(0.4 * std::sin(
            2.0 * std::numbers::pi * 220.0 * i / audio.sample_rate));
    }
    const auto filtered = transcribe::preprocess_audio(audio);

    REQUIRE(filtered.samples.size() == audio.samples.size());
    REQUIRE(audio.samples.front() == 0.0001f);
    REQUIRE(std::all_of(filtered.samples.begin(), filtered.samples.begin() + 1024,
                        [](float sample) { return sample == 0.0f; }));
    REQUIRE(std::any_of(filtered.samples.begin() + 22050, filtered.samples.end(),
                        [](float sample) { return std::abs(sample) > 0.1f; }));
}

TEST_CASE("onsets split repeated pitches and low confidence frames are dropped") {
    std::vector<transcribe::PitchFrame> frames;
    for (int i = 0; i < 8; ++i) {
        frames.push_back({69, i * 0.02, 0.9, 0.95});
    }
    const std::array<double, 1> onsets{0.08};
    const auto notes = transcribe::notes_from_frames(frames, 0.16, onsets, 0.25);

    REQUIRE(notes.size() == 2);
    REQUIRE(std::abs(notes[0].start_seconds) < 0.001);
    REQUIRE(std::abs(notes[1].start_seconds - 0.08) < 0.001);
    REQUIRE(std::abs(notes[0].confidence - 0.9) < 0.001);

    for (auto& frame : frames) {
        frame.confidence = 0.1;
    }
    REQUIRE(transcribe::notes_from_frames(frames, 0.16, onsets, 0.25).empty());
}

TEST_CASE("monophonic analysis detects a WAV tone") {
    transcribe::AudioBuffer audio{44100, std::vector<float>(44100)};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(0.5 * std::sin(
            2.0 * std::numbers::pi * 440.0 * i / audio.sample_rate));
    }
    const auto notes = transcribe::transcribe_monophonic(audio);

    REQUIRE_FALSE(notes.empty());
    REQUIRE(notes.front().midi_pitch == 69);
    REQUIRE(notes.front().confidence > 0.25);
}

TEST_CASE("WAV reader decodes PCM in bounded chunks and rejects truncated data") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "tone.wav";
    create_pcm16_wav(path, 440.0, 0.2);
    const auto audio = transcribe::read_wav(path);
    REQUIRE(audio.sample_rate == 44100);
    REQUIRE(audio.samples.size() == 8820);

    std::size_t streamed_samples = 0;
    std::size_t block_count = 0;
    transcribe::read_wav_blocks(path, 1024,
        [&streamed_samples, &block_count](const transcribe::AudioBlockView& block) {
            REQUIRE(block.sample_rate == 44100);
            REQUIRE(block.samples.size() <= 1024);
            REQUIRE(block.first_frame == streamed_samples);
            REQUIRE(block.total_frames == 8820);
            streamed_samples += block.samples.size();
            ++block_count;
        });
    REQUIRE(streamed_samples == audio.samples.size());
    REQUIRE(block_count == 9);

    const auto truncated = temporary.path / "truncated.wav";
    {
        std::ofstream output(truncated, std::ios::binary);
        output.write("RIFF", 4);
        write_u32(output, 36);
        output.write("WAVEfmt ", 8);
        write_u32(output, 16);
        write_u16(output, 1);
        write_u16(output, 1);
        write_u32(output, 44100);
        write_u32(output, 88200);
        write_u16(output, 2);
        write_u16(output, 16);
        output.write("data", 4);
        write_u32(output, 4);
        write_u16(output, 100);
    }
    REQUIRE_THROWS(transcribe::read_wav(truncated));
}

TEST_CASE("WAV transcription uses bounded overlapping chunks") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "long-tone.wav";
    create_pcm16_wav(path, 440.0, 2.4);
    std::vector<double> progress;
    transcribe::StreamingOptions options;
    options.chunk_seconds = 1.0;
    options.overlap_seconds = 0.25;
    options.analysis.progress_callback = [&progress](double value) {
        progress.push_back(value);
    };

    const auto result = transcribe::transcribe_wav_streaming(path, options);
    REQUIRE(std::abs(result.duration_seconds - 2.4) < 0.001);
    REQUIRE(result.notes.size() == 1);
    REQUIRE(result.notes.front().midi_pitch == 69);
    REQUIRE(result.notes.front().duration_seconds > 2.2);
    REQUIRE_FALSE(progress.empty());
    REQUIRE(progress.back() == 1.0);
    REQUIRE(std::is_sorted(progress.begin(), progress.end()));
}

TEST_CASE("MusicXML preserves overlapping voices, rests, ties, and metadata") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "score.musicxml";
    const std::vector<transcribe::Note> notes{
        {60, 0.0, 3.0, "C4", 0.9},
        {67, 0.0, 0.5, "G4", 0.8},
        {62, 4.0, 0.5, "D4", 0.9}};
    transcribe::ScoreMetadata metadata;
    metadata.tempo_bpm = 120.0;
    metadata.time_numerator = 6;
    metadata.time_denominator = 8;
    metadata.key_fifths = -3;
    metadata.minor_key = true;
    transcribe::write_musicxml(path, notes, metadata);

    std::ifstream input(path, std::ios::binary);
    const std::string xml{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(xml.find("<beats>6</beats><beat-type>8</beat-type>") != std::string::npos);
    REQUIRE(xml.find("<fifths>-3</fifths><mode>minor</mode>") != std::string::npos);
    REQUIRE(xml.find("<backup>") != std::string::npos);
    REQUIRE(xml.find("<rest/>") != std::string::npos);
    REQUIRE(xml.find("<tie type=\"start\"/>") != std::string::npos);
    REQUIRE(xml.find("<voice>2</voice>") != std::string::npos);
}

TEST_CASE("MIDI writer emits a valid single-track header") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "notes.mid";
    transcribe::ScoreMetadata metadata;
    const std::vector<transcribe::Note> notes{
        {69, 0.0, 0.5, "A4", 0.9}, {60, 10000000.0, 10000000.0, "C4", 0.9}};
    transcribe::write_midi(path, notes, metadata);

    std::ifstream input(path, std::ios::binary);
    std::array<char, 4> header{};
    input.read(header.data(), static_cast<std::streamsize>(header.size()));
    REQUIRE(std::string(header.data(), header.size()) == "MThd");
}

TEST_CASE("guitar tablature assigns simultaneous notes to different strings") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "tab.txt";
    const std::vector<transcribe::Note> notes{
        {57, 0.0, 0.5, "A3", 0.9}, {64, 0.0, 0.5, "E4", 0.9},
        {59, 0.5, 0.25, "B3", 0.9}};
    transcribe::GuitarTabOptions options;
    options.maximum_fret = 12;
    options.preferred_position = 3;
    options.duration_proportional_ascii = true;
    transcribe::write_guitar_tab(path, notes, options);

    std::ifstream input(path, std::ios::binary);
    const std::string tab{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(tab.find("Estimated guitar tablature") != std::string::npos);
    REQUIRE(tab.find("A3") != std::string::npos);
    REQUIRE(tab.find("E4") != std::string::npos);
    REQUIRE(tab.find("confidence") != std::string::npos);
}

TEST_CASE("audio ring buffer safely wraps and reports available samples") {
    transcribe::AudioRingBuffer ring(4);
    const std::array<float, 3> first{1.0f, 2.0f, 3.0f};
    REQUIRE(ring.write(first) == 3);
    std::array<float, 2> read{};
    REQUIRE(ring.read(read) == 2);
    REQUIRE(read[0] == 1.0f);
    REQUIRE(read[1] == 2.0f);
    const std::array<float, 3> second{4.0f, 5.0f, 6.0f};
    REQUIRE(ring.write(second) == 3);
    REQUIRE(ring.readable() == 4);
    std::array<float, 4> all{};
    REQUIRE(ring.read(all) == 4);
    REQUIRE((all == std::array<float, 4>{3.0f, 4.0f, 5.0f, 6.0f}));
}

TEST_CASE("score metadata inference returns supported meter key and tempo") {
    std::vector<transcribe::Note> notes;
    for (int i = 0; i < 12; ++i) {
        const int pitch = 60 + std::array<int, 7>{0, 2, 4, 5, 7, 9, 11}[i % 7];
        notes.push_back({pitch, i * 0.5, 0.25, transcribe::midi_note_name(pitch), 0.9});
    }
    const auto metadata = transcribe::infer_score_metadata(notes);
    REQUIRE(metadata.tempo_inferred);
    REQUIRE(metadata.key_inferred);
    REQUIRE(metadata.tempo_bpm >= 60.0);
    REQUIRE(metadata.tempo_bpm <= 180.0);
    REQUIRE((metadata.time_numerator == 3 || metadata.time_numerator == 4 ||
             metadata.time_numerator == 6));
    REQUIRE((metadata.key_fifths >= -7 && metadata.key_fifths <= 7));
}

TEST_CASE("score metadata recognizes repeated compound eighth-note grouping") {
    const std::array<int, 8> pitches{60, 62, 64, 65, 67, 69, 71, 72};
    const std::array<double, 9> onsets{0.0, 0.25, 0.5, 1.0, 1.25,
                                       1.5, 2.0, 2.25, 2.5};
    std::vector<transcribe::Note> notes;
    for (std::size_t i = 0; i < pitches.size(); ++i) {
        notes.push_back({pitches[i], onsets[i], 0.2,
                         transcribe::midi_note_name(pitches[i]), 0.9});
    }

    const auto metadata = transcribe::infer_score_metadata(notes);
    REQUIRE(metadata.time_numerator == 6);
    REQUIRE(metadata.time_denominator == 8);
    REQUIRE(metadata.meter_inferred);
    REQUIRE(metadata.meter_confidence > 0.5);
}

TEST_CASE("score metadata distinguishes regular 3/4 and 4/4 phrases") {
    const auto make_phrase = [](int count) {
        std::vector<transcribe::Note> notes;
        for (int i = 0; i < count; ++i) {
            const int pitch = 60 + (i % 7) * 2;
            notes.push_back({pitch, i * 0.5, 0.45,
                             transcribe::midi_note_name(pitch), 0.9});
        }
        return notes;
    };
    const auto three_four = transcribe::infer_score_metadata(make_phrase(6));
    const auto four_four = transcribe::infer_score_metadata(make_phrase(8));

    REQUIRE(three_four.meter_inferred);
    REQUIRE(three_four.time_numerator == 3);
    REQUIRE(four_four.meter_inferred);
    REQUIRE(four_four.time_numerator == 4);
    REQUIRE(four_four.time_denominator == 4);
}

TEST_CASE("single-note material is reported as insufficient for metadata inference") {
    const std::vector<transcribe::Note> notes{{69, 0.0, 1.0, "A4", 0.95}};
    const auto metadata = transcribe::infer_score_metadata(notes);

    REQUIRE_FALSE(metadata.tempo_inferred);
    REQUIRE_FALSE(metadata.meter_inferred);
    REQUIRE_FALSE(metadata.key_inferred);
    REQUIRE(metadata.tempo_confidence == 0.0);
    REQUIRE(metadata.meter_confidence == 0.0);
    REQUIRE(metadata.key_confidence == 0.0);
}
