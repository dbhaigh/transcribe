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

TEST_CASE("realtime transcription stabilizes note events across input chunks") {
    constexpr std::uint32_t sample_rate = 44100;
    std::vector<float> samples(static_cast<std::size_t>(sample_rate * 0.7), 0.0f);
    for (std::size_t i = sample_rate / 10; i < sample_rate * 45 / 100; ++i) {
        samples[i] = static_cast<float>(
            0.45 * std::sin(2.0 * std::numbers::pi * 440.0 * i / sample_rate));
    }

    transcribe::RealtimeOptions options;
    options.latency_ms = 40.0;
    transcribe::RealtimeTranscriber analyzer(options);
    std::size_t offset = 0;
    constexpr std::array<std::size_t, 5> chunk_sizes{127, 512, 89, 1024, 301};
    std::size_t chunk_index = 0;
    while (offset < samples.size()) {
        const auto count =
            std::min(chunk_sizes[chunk_index++ % chunk_sizes.size()], samples.size() - offset);
        analyzer.push_samples(std::span<const float>(samples).subspan(offset, count));
        offset += count;
    }
    analyzer.finish();

    const auto notes = analyzer.live_notes();
    const auto events = analyzer.take_events();
    REQUIRE(notes.size() == 1);
    REQUIRE(notes.front().midi_pitch == 69);
    REQUIRE(notes.front().start_seconds > 0.05);
    REQUIRE(notes.front().start_seconds < 0.15);
    REQUIRE(notes.front().duration_seconds > 0.25);
    REQUIRE(events.size() == 2);
    REQUIRE(events[0].type == transcribe::NoteEventType::Onset);
    REQUIRE(events[1].type == transcribe::NoteEventType::Offset);
    REQUIRE(events[0].midi_pitch == 69);
    REQUIRE(events[1].midi_pitch == 69);
    REQUIRE_THROWS(analyzer.push_samples(std::span<const float>(samples).first(1)));
}

TEST_CASE("realtime onset analysis stays within a 50 ms input-time latency budget") {
    constexpr std::uint32_t sample_rate = 44100;
    constexpr std::size_t onset_frame = sample_rate / 10;
    std::vector<float> samples(sample_rate / 2, 0.0f);
    for (std::size_t i = onset_frame; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(
            0.45 * std::sin(2.0 * std::numbers::pi * 440.0 * i / sample_rate));
    }

    transcribe::RealtimeOptions options;
    options.latency_ms = 20.0;
    transcribe::RealtimeTranscriber analyzer(options);
    constexpr std::size_t chunk_size = sample_rate / 100;
    double onset_latency_ms = -1.0;
    for (std::size_t offset = 0; offset < samples.size(); offset += chunk_size) {
        const auto count = std::min(chunk_size, samples.size() - offset);
        analyzer.push_samples(std::span<const float>(samples).subspan(offset, count));
        for (const auto& event : analyzer.take_events()) {
            if (event.type == transcribe::NoteEventType::Onset) {
                onset_latency_ms =
                    (event.detected_at_seconds - event.time_seconds) * 1000.0;
            }
        }
    }
    analyzer.finish();

    INFO("Measured input-time onset analysis latency: " << onset_latency_ms << " ms");
    REQUIRE(onset_latency_ms >= 0.0);
    REQUIRE(onset_latency_ms <= 50.0);
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

TEST_CASE("fast preview keeps monophonic pitch detection usable") {
    constexpr std::uint32_t sample_rate = 22050;
    transcribe::AudioBuffer audio{sample_rate,
                                  std::vector<float>(sample_rate, 0.0f)};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(
            0.4 * std::sin(2.0 * std::numbers::pi * 440.0 * i / sample_rate));
    }

    transcribe::AnalysisOptions options;
    options.fast_preview = true;
    const auto notes = transcribe::transcribe_monophonic(audio, options);

    REQUIRE_FALSE(notes.empty());
    REQUIRE(notes.front().midi_pitch == 69);
}

TEST_CASE("spectral polyphonic analysis detects two simultaneous notes") {
    constexpr std::uint32_t sample_rate = 22050;
    transcribe::AudioBuffer audio{sample_rate,
                                  std::vector<float>(sample_rate, 0.0f)};
    constexpr std::array<int, 2> pitches{60, 67};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double time = static_cast<double>(i) / sample_rate;
        if (time >= 0.1 && time < 0.9) {
            for (const int pitch : pitches) {
                const double frequency =
                    440.0 * std::pow(2.0, (pitch - 69) / 12.0);
                audio.samples[i] += static_cast<float>(
                    0.3 * std::sin(2.0 * std::numbers::pi * frequency * time));
            }
        }
    }

    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    options.minimum_confidence = 0.2;
    const auto notes = transcribe::transcribe_monophonic(audio, options);
    for (const int pitch : pitches) {
        REQUIRE(std::any_of(notes.begin(), notes.end(), [pitch](const auto& note) {
            return note.midi_pitch == pitch;
        }));
    }
}

TEST_CASE("polyphonic analysis reuses a cached spectrogram for repeated audio") {
    constexpr std::uint32_t sample_rate = 22050;
    constexpr std::size_t duration_seconds = 7;
    transcribe::AudioBuffer audio{
        sample_rate, std::vector<float>(sample_rate * duration_seconds, 0.0f)};
    constexpr double frequency = 523.2511306;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(
            0.35 * std::sin(2.0 * std::numbers::pi * frequency * i / sample_rate));
    }

    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    bool first_hit = true;
    options.timing_callback = [&first_hit](const auto& timings) {
        first_hit = timings.spectrogram_cache_hit;
    };
    const auto first_notes = transcribe::transcribe_monophonic(audio, options);
    bool second_hit = false;
    options.timing_callback = [&second_hit](const auto& timings) {
        second_hit = timings.spectrogram_cache_hit;
    };
    const auto second_notes = transcribe::transcribe_monophonic(audio, options);

    REQUIRE_FALSE(first_hit);
    REQUIRE(second_hit);
    REQUIRE(first_notes.size() == second_notes.size());
    for (std::size_t i = 0; i < first_notes.size(); ++i) {
        REQUIRE(first_notes[i].midi_pitch == second_notes[i].midi_pitch);
        REQUIRE(first_notes[i].start_seconds == second_notes[i].start_seconds);
        REQUIRE(first_notes[i].duration_seconds == second_notes[i].duration_seconds);
    }
    options.fast_preview = true;
    bool different_fft_hit = true;
    options.timing_callback = [&different_fft_hit](const auto& timings) {
        different_fft_hit = timings.spectrogram_cache_hit;
    };
    (void)transcribe::transcribe_monophonic(audio, options);
    REQUIRE_FALSE(different_fft_hit);
}

TEST_CASE("harmonic salience retains a weak fundamental in an octave-heavy tone") {
    constexpr std::uint32_t sample_rate = 22050;
    transcribe::AudioBuffer audio{sample_rate,
                                  std::vector<float>(sample_rate, 0.0f)};
    constexpr double fundamental = 110.0;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double time = static_cast<double>(i) / sample_rate;
        audio.samples[i] = static_cast<float>(
            0.2 * std::sin(2.0 * std::numbers::pi * fundamental * time) +
            0.8 * std::sin(2.0 * std::numbers::pi * fundamental * 2.0 * time) +
            0.3 * std::sin(2.0 * std::numbers::pi * fundamental * 3.0 * time));
    }

    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    options.minimum_confidence = 0.2;
    const auto notes = transcribe::transcribe_monophonic(audio, options);

    REQUIRE(std::any_of(notes.begin(), notes.end(), [](const auto& note) {
        return note.midi_pitch == 45;
    }));
}

TEST_CASE("instrument priors constrain spectral pitch range") {
    constexpr std::uint32_t sample_rate = 22050;
    transcribe::AudioBuffer audio{sample_rate,
                                  std::vector<float>(sample_rate, 0.0f)};
    constexpr double frequency = 65.406391325;
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        audio.samples[i] = static_cast<float>(
            0.35 * std::sin(2.0 * std::numbers::pi * frequency * i / sample_rate));
    }

    transcribe::AnalysisOptions options;
    options.polyphonic = true;
    options.instrument = transcribe::InstrumentPrior::Piano;
    const auto piano_notes = transcribe::transcribe_monophonic(audio, options);
    options.instrument = transcribe::InstrumentPrior::Guitar;
    const auto guitar_notes = transcribe::transcribe_monophonic(audio, options);

    REQUIRE(std::any_of(piano_notes.begin(), piano_notes.end(), [](const auto& note) {
        return note.midi_pitch == 36;
    }));
    REQUIRE(std::none_of(guitar_notes.begin(), guitar_notes.end(), [](const auto& note) {
        return note.midi_pitch == 36;
    }));
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
    std::size_t preview_samples = 0;
    options.audio_callback = [&preview_samples](
                                 const transcribe::AudioBlockView& block) {
        REQUIRE(block.sample_rate == 44100);
        REQUIRE(block.first_frame == preview_samples);
        preview_samples += block.samples.size();
    };
    std::vector<std::size_t> progressive_note_counts;
    options.notes_callback = [&progressive_note_counts](
                                 const std::vector<transcribe::Note>& notes) {
        progressive_note_counts.push_back(notes.size());
    };

    const auto result = transcribe::transcribe_wav_streaming(path, options);
    REQUIRE(std::abs(result.duration_seconds - 2.4) < 0.001);
    REQUIRE(result.notes.size() == 1);
    REQUIRE(result.notes.front().midi_pitch == 69);
    REQUIRE(result.notes.front().duration_seconds > 2.2);
    REQUIRE_FALSE(progress.empty());
    REQUIRE(progress.back() == 1.0);
    REQUIRE(std::is_sorted(progress.begin(), progress.end()));
    REQUIRE(progressive_note_counts.size() >= 2);
    REQUIRE(progressive_note_counts.back() == result.notes.size());
    REQUIRE(preview_samples == static_cast<std::size_t>(44100 * 2.4));
}

TEST_CASE("audio preview builds bounded waveform and log-frequency spectrogram data") {
    constexpr std::uint32_t sample_rate = 44100;
    std::vector<float> samples(sample_rate / 2);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = static_cast<float>(
            0.6 * std::sin(2.0 * std::numbers::pi * 440.0 * index / sample_rate));
    }

    const auto preview = transcribe::make_audio_preview(samples, sample_rate, 32, 24);
    REQUIRE(preview.duration_seconds == 0.5);
    REQUIRE(preview.columns > 0);
    REQUIRE(preview.columns <= 32);
    REQUIRE(preview.frequency_bands == 24);
    REQUIRE(preview.waveform.size() == preview.columns);
    REQUIRE(preview.spectrogram.size() == preview.columns * 24);
    REQUIRE(*std::max_element(preview.waveform.begin(), preview.waveform.end()) > 0.5f);
    REQUIRE(*std::max_element(preview.spectrogram.begin(),
                              preview.spectrogram.end()) > 0.5f);
    REQUIRE_THROWS(transcribe::make_audio_preview(samples, sample_rate, 0, 24));
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
    REQUIRE(xml.find("<part id=\"P2\">") != std::string::npos);
    REQUIRE(xml.find("<clef><sign>TAB</sign><line>5</line></clef>") !=
            std::string::npos);
    REQUIRE(xml.find("<technical><string>") != std::string::npos);
}

TEST_CASE("MusicXML represents triplets, expressive timing, dynamics, and articulations") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "tuplets.musicxml";
    const std::vector<transcribe::Note> notes{
        {60, 0.07, 0.333, "C4", 0.9, 0.42},
        {62, 0.403, 0.333, "D4", 0.9, 0.1},
        {64, 0.736, 0.333, "E4", 0.9, 0.1}};
    transcribe::ScoreMetadata metadata;
    metadata.tempo_bpm = 120.0;
    transcribe::write_musicxml(path, notes, metadata, transcribe::GuitarTabOptions{}, true);

    std::ifstream input(path, std::ios::binary);
    const std::string xml{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(xml.find("<divisions>48</divisions>") != std::string::npos);
    REQUIRE(xml.find("<time-modification><actual-notes>3</actual-notes>") !=
            std::string::npos);
    REQUIRE(xml.find("Expressive onset") != std::string::npos);
    REQUIRE(xml.find("<dynamics><ff/></dynamics>") != std::string::npos);
    REQUIRE(xml.find("<accent/>") != std::string::npos);
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

TEST_CASE("overlapping guitar notes do not reuse a held string") {
    const std::vector<transcribe::Note> notes{
        {69, 0.0, 0.8, "A4", 0.9}, {67, 0.2, 0.4, "G4", 0.9}};
    const auto positions = transcribe::assign_guitar_tab_positions(notes);

    REQUIRE(positions.size() == notes.size());
    REQUIRE(positions[0].string_number > 0);
    REQUIRE(positions[1].string_number > 0);
    REQUIRE(positions[0].string_number != positions[1].string_number);
}

TEST_CASE("guitar tablature supports alternate tuning capo and alternatives") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "drop-d-capo-tab.txt";
    const std::vector<transcribe::Note> notes{{62, 0.0, 0.5, "D4", 0.9}};
    transcribe::GuitarTabOptions options;
    options.tuning = {64, 59, 55, 50, 45, 38};
    options.capo_fret = 2;
    options.alternative_fingerings = 2;

    const auto positions = transcribe::assign_guitar_tab_positions(notes, options);
    REQUIRE(positions.size() == 1);
    REQUIRE(positions.front().string_number > 0);
    REQUIRE(positions.front().fret >= 0);
    transcribe::write_guitar_tab(path, notes, options);

    std::ifstream input(path, std::ios::binary);
    const std::string tab{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(tab.find("Capo: fret 2") != std::string::npos);
    REQUIRE(tab.find("Alternative fingerings:") != std::string::npos);
}

TEST_CASE("overlapping overridden notes cannot occupy the same guitar string") {
    const std::vector<transcribe::Note> notes{
        {64, 0.0, 0.5, "E4", 0.9}, {64, 0.2, 0.5, "E4", 0.9}};
    transcribe::GuitarTabOptions options;
    options.position_overrides = {{0, 2, 5}, {1, 2, 5}};

    REQUIRE_THROWS(transcribe::assign_guitar_tab_positions(notes, options));
}

TEST_CASE("guitar tab overrides preserve user-selected string and fret in exports") {
    TemporaryDirectory temporary;
    const auto path = temporary.path / "edited-tab.musicxml";
    const auto tab_path = temporary.path / "edited-tab.txt";
    const std::vector<transcribe::Note> notes{{64, 0.0, 0.5, "E4", 0.9}};
    transcribe::GuitarTabOptions options;
    options.position_overrides = {{0, 2, 5}};
    const auto positions = transcribe::assign_guitar_tab_positions(notes, options);
    REQUIRE(positions.size() == 1);
    REQUIRE(positions.front().string_number == 2);
    REQUIRE(positions.front().fret == 5);
    transcribe::write_musicxml(
        path, notes, transcribe::ScoreMetadata{}, options, false);

    std::ifstream input(path, std::ios::binary);
    const std::string xml{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(xml.find("<technical><string>2</string><fret>5</fret></technical>") !=
            std::string::npos);

    transcribe::write_guitar_tab(tab_path, notes, options);
    std::ifstream tab_input(tab_path, std::ios::binary);
    const std::string tab{std::istreambuf_iterator<char>(tab_input),
                          std::istreambuf_iterator<char>()};
    const auto b_string = tab.find("B3|");
    REQUIRE(b_string != std::string::npos);
    const auto line_end = tab.find('\n', b_string);
    REQUIRE(tab.substr(b_string, line_end - b_string).find('5') !=
            std::string::npos);
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

TEST_CASE("score metadata detects swung eighth-note timing") {
    const std::array<double, 7> onsets{0.0, 0.333, 0.5, 0.833, 1.0, 1.333, 1.5};
    std::vector<transcribe::Note> notes;
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        const int pitch = 60 + static_cast<int>(i % 5) * 2;
        notes.push_back({pitch, onsets[i], 0.12,
                         transcribe::midi_note_name(pitch), 0.9});
    }

    const auto metadata = transcribe::infer_score_metadata(notes);
    REQUIRE(metadata.tempo_inferred);
    REQUIRE(std::abs(metadata.tempo_bpm - 120.0) < 5.0);
    REQUIRE(metadata.swing_inferred);
    REQUIRE(metadata.swing_ratio > 1.7);
    REQUIRE(metadata.swing_ratio < 2.3);
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

#ifdef TRANSCRIBE_ENABLE_ONNX
TEST_CASE("Basic Pitch ONNX adapter detects a synthetic chord") {
    constexpr std::uint32_t sample_rate = 22050;
    constexpr double duration_seconds = 2.5;
    transcribe::AudioBuffer audio{
        sample_rate,
        std::vector<float>(static_cast<std::size_t>(sample_rate * duration_seconds), 0.0f)};
    constexpr std::array<int, 3> pitches{60, 64, 67};
    for (std::size_t i = 0; i < audio.samples.size(); ++i) {
        const double time = static_cast<double>(i) / sample_rate;
        const double envelope = std::clamp(
            std::min(time / 0.05, (duration_seconds - time) / 0.05), 0.0, 1.0);
        for (const int pitch : pitches) {
            const double frequency = 440.0 * std::pow(2.0, (pitch - 69) / 12.0);
            audio.samples[i] += static_cast<float>(
                envelope * 0.22 * std::sin(2.0 * std::numbers::pi * frequency * time));
        }
    }

    transcribe::OnnxOptions options;
    options.polyphonic = true;
    options.polyphonic_threshold = 0.25;
    options.minimum_confidence = 0.2;
    const auto model = std::filesystem::path(TRANSCRIBE_BASIC_PITCH_MODEL_PATH);
    const auto notes = transcribe::transcribe_with_onnx(audio, model, options);
    for (const int pitch : pitches) {
        REQUIRE(std::any_of(notes.begin(), notes.end(), [pitch](const auto& note) {
            return note.midi_pitch == pitch;
        }));
    }
}
#endif
