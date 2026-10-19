#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace transcribe {
namespace {

struct Spelling {
    const char* step;
    int alter;
    int octave;
};

Spelling spell_pitch(int midi) {
    static constexpr Spelling spellings[] = {
        {"C", 0, 0}, {"C", 1, 0}, {"D", 0, 0}, {"D", 1, 0},
        {"E", 0, 0}, {"F", 0, 0}, {"F", 1, 0}, {"G", 0, 0},
        {"G", 1, 0}, {"A", 0, 0}, {"A", 1, 0}, {"B", 0, 0}};
    Spelling result = spellings[midi % 12];
    result.octave = midi / 12 - 1;
    return result;
}

struct RhythmValue {
    int ticks;
    const char* type;
    int dots;
};

constexpr std::array<RhythmValue, 12> rhythm_values{{
    {64, "whole", 0}, {48, "half", 1}, {32, "half", 0},
    {24, "quarter", 1}, {16, "quarter", 0}, {12, "eighth", 1},
    {8, "eighth", 0}, {6, "16th", 1}, {4, "16th", 0},
    {3, "32nd", 1}, {2, "32nd", 0}, {1, "64th", 0}}};

struct ScoreToken {
    int duration{};
    int midi_pitch{-1};
    int voice{};
    bool tie_in{};
    bool tie_out{};
};

struct AssignedNote {
    const Note* note{};
    int start_tick{};
    int duration_tick{};
};

using Score = std::vector<std::vector<std::vector<ScoreToken>>>;

struct Voice {
    int end_tick{};
    std::vector<AssignedNote> notes;
};

int ticks_for_time(double seconds, double tempo_bpm) {
    const double raw = std::max(0.0, seconds) * tempo_bpm * 16.0 / 60.0;
    const double sixteenth = std::round(raw / 4.0) * 4.0;
    const double snapped = std::abs(sixteenth - raw) <= 0.45 ? sixteenth : std::round(raw);
    return static_cast<int>(std::clamp(snapped, 0.0, 1.0e8));
}

std::vector<int> decompose_duration(int ticks) {
    std::vector<int> chunks;
    while (ticks > 0) {
        const auto value = std::find_if(rhythm_values.begin(), rhythm_values.end(),
                                        [ticks](const RhythmValue& candidate) {
                                            return candidate.ticks <= ticks;
                                        });
        if (value == rhythm_values.end()) {
            throw std::logic_error("Could not represent a MusicXML duration.");
        }
        chunks.push_back(value->ticks);
        ticks -= value->ticks;
    }
    return chunks;
}

void append_rest(Score& measures, std::size_t voice, int ticks_per_measure,
                 int& cursor, int target) {
    while (cursor < target) {
        const int space = ticks_per_measure - cursor % ticks_per_measure;
        const int segment = std::min(target - cursor, space);
        for (const int duration : decompose_duration(segment)) {
            measures[static_cast<std::size_t>(cursor / ticks_per_measure)][voice].push_back(
                {duration, -1, 0, false, false});
            cursor += duration;
        }
    }
}

void append_note(Score& measures, std::size_t voice_index, int ticks_per_measure,
                 int& cursor, int duration, int midi_pitch) {
    std::vector<std::pair<std::size_t, int>> fragments;
    int remaining = duration;
    while (remaining > 0) {
        const int space = ticks_per_measure - cursor % ticks_per_measure;
        const int segment = std::min(remaining, space);
        for (const int chunk : decompose_duration(segment)) {
            fragments.emplace_back(static_cast<std::size_t>(cursor / ticks_per_measure), chunk);
            cursor += chunk;
            remaining -= chunk;
        }
    }
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const auto [measure, ticks] = fragments[index];
        measures[measure][voice_index].push_back(
            {ticks, midi_pitch, static_cast<int>(voice_index + 1), index > 0,
             index + 1 < fragments.size()});
    }
}

void write_measure_start(std::ostream& output, int measure_number, bool first,
                         const ScoreMetadata& metadata) {
    output << "    <measure number=\"" << measure_number << "\">\n";
    if (!first) {
        return;
    }
    output << "      <attributes><divisions>16</divisions><key><fifths>"
           << metadata.key_fifths << "</fifths><mode>"
           << (metadata.minor_key ? "minor" : "major")
           << "</mode></key><time><beats>" << metadata.time_numerator
           << "</beats><beat-type>" << metadata.time_denominator
           << "</beat-type></time><clef><sign>G</sign><line>2</line></clef>"
              "<staff-details><staff-lines>5</staff-lines></staff-details></attributes>\n"
              "      <direction><direction-type><words>Estimated tempo and meter</words>"
              "</direction-type><sound tempo=\""
           << metadata.tempo_bpm << "\"/></direction>\n";
}

void write_score_token(std::ostream& output, const ScoreToken& token) {
    output << "      <note>";
    if (token.midi_pitch < 0) {
        output << "<rest/>";
    } else {
        const auto spelling = spell_pitch(token.midi_pitch);
        output << "<pitch><step>" << spelling.step << "</step>";
        if (spelling.alter != 0) {
            output << "<alter>" << spelling.alter << "</alter>";
        }
        output << "<octave>" << spelling.octave << "</octave></pitch>";
    }
    output << "<duration>" << token.duration << "</duration>";
    if (token.tie_in) {
        output << "<tie type=\"stop\"/>";
    }
    if (token.tie_out) {
        output << "<tie type=\"start\"/>";
    }
    output << "<voice>" << token.voice << "</voice>";
    const auto rhythm = std::find_if(rhythm_values.begin(), rhythm_values.end(),
                                     [&token](const RhythmValue& candidate) {
                                         return candidate.ticks == token.duration;
                                     });
    output << "<type>" << rhythm->type << "</type>";
    for (int dot = 0; dot < rhythm->dots; ++dot) {
        output << "<dot/>";
    }
    if (token.midi_pitch >= 0) {
        if (token.tie_in || token.tie_out) {
            output << "<notations>";
            if (token.tie_in) {
                output << "<tied type=\"stop\"/>";
            }
            if (token.tie_out) {
                output << "<tied type=\"start\"/>";
            }
            output << "</notations>";
        }
    }
    output << "</note>\n";
}

void write_be32(std::ostream& output, std::uint32_t value) {
    output.put(static_cast<char>((value >> 24) & 0xff));
    output.put(static_cast<char>((value >> 16) & 0xff));
    output.put(static_cast<char>((value >> 8) & 0xff));
    output.put(static_cast<char>(value & 0xff));
}

void append_vlq(std::vector<std::uint8_t>& output, std::uint32_t value) {
    if (value > 0x0fffffffU) {
        throw std::length_error("MIDI variable-length delta exceeds the four-byte limit.");
    }
    std::array<std::uint8_t, 4> buffer{};
    std::size_t count = 1;
    buffer[3] = static_cast<std::uint8_t>(value & 0x7f);
    while ((value >>= 7) != 0 && count < buffer.size()) {
        buffer[3 - count] = static_cast<std::uint8_t>((value & 0x7f) | 0x80);
        ++count;
    }
    output.insert(output.end(), buffer.end() - static_cast<std::ptrdiff_t>(count),
                  buffer.end());
}

} // namespace

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes) {
    write_musicxml(path, notes, infer_score_metadata(notes));
}

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& requested_metadata) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create MusicXML output: " + path.string());
    }
    ScoreMetadata metadata = requested_metadata;
    if (metadata.time_numerator <= 0 ||
        (metadata.time_denominator != 4 && metadata.time_denominator != 8)) {
        metadata.time_numerator = 4;
        metadata.time_denominator = 4;
    }
    metadata.tempo_bpm = std::clamp(metadata.tempo_bpm, 40.0, 240.0);
    metadata.key_fifths = std::clamp(metadata.key_fifths, -7, 7);
    const int ticks_per_measure =
        metadata.time_numerator * 64 / metadata.time_denominator;
    if (ticks_per_measure <= 0) {
        throw std::invalid_argument("Time signature has an invalid measure length.");
    }

    std::vector<const Note*> sorted_notes;
    sorted_notes.reserve(notes.size());
    for (const auto& note : notes) {
        sorted_notes.push_back(&note);
    }
    std::stable_sort(sorted_notes.begin(), sorted_notes.end(),
                     [](const Note* left, const Note* right) {
                         return left->start_seconds < right->start_seconds;
                     });

    std::vector<Voice> voices;
    for (const auto* note : sorted_notes) {
        const int start = ticks_for_time(note->start_seconds, metadata.tempo_bpm);
        const int duration = std::max(1, ticks_for_time(note->duration_seconds,
                                                        metadata.tempo_bpm));
        auto voice = std::find_if(voices.begin(), voices.end(),
                                  [start](const Voice& candidate) {
                                      return candidate.end_tick <= start;
                                  });
        if (voice == voices.end()) {
            voices.push_back({});
            voice = std::prev(voices.end());
        }
        voice->notes.push_back({note, start, duration});
        voice->end_tick = start + duration;
    }
    if (voices.empty()) {
        voices.emplace_back();
    }

    int total_ticks = ticks_per_measure;
    for (const auto& voice : voices) {
        total_ticks = std::max(total_ticks, voice.end_tick);
    }
    const std::size_t measure_count =
        static_cast<std::size_t>((total_ticks + ticks_per_measure - 1) /
                                 ticks_per_measure);
    std::vector<std::vector<std::vector<ScoreToken>>> score(
        measure_count, std::vector<std::vector<ScoreToken>>(voices.size()));

    for (std::size_t voice_index = 0; voice_index < voices.size(); ++voice_index) {
        int cursor = 0;
        for (const auto& assigned : voices[voice_index].notes) {
            const int start = std::max(cursor, assigned.start_tick);
            append_rest(score, voice_index, ticks_per_measure, cursor, start);
            append_note(score, voice_index, ticks_per_measure, cursor,
                        assigned.duration_tick, assigned.note->midi_pitch);
        }
        append_rest(score, voice_index, ticks_per_measure, cursor,
                    static_cast<int>(measure_count) * ticks_per_measure);
    }

    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
              "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
              "<score-partwise version=\"4.0\"><work><work-title>Transcription</work-title>"
              "</work><part-list><score-part id=\"P1\"><part-name>Guitar</part-name>"
              "</score-part></part-list><part id=\"P1\">\n";

    for (std::size_t measure = 0; measure < measure_count; ++measure) {
        write_measure_start(output, static_cast<int>(measure + 1), measure == 0, metadata);
        for (std::size_t voice = 0; voice < voices.size(); ++voice) {
            for (const auto& token : score[measure][voice]) {
                write_score_token(output, token);
            }
            if (voice + 1 < voices.size()) {
                output << "      <backup><duration>" << ticks_per_measure
                       << "</duration></backup>\n";
            }
        }
        output << "    </measure>\n";
    }
    output << "  </part>\n</score-partwise>\n";
    if (!output) {
        throw std::runtime_error("Failed while writing MusicXML output.");
    }
}

void write_midi(const std::filesystem::path& path, const std::vector<Note>& notes,
                const ScoreMetadata& metadata) {
    const double tempo = std::clamp(metadata.tempo_bpm, 40.0, 240.0);
    const auto microseconds_per_quarter =
        static_cast<std::uint32_t>(std::lround(60000000.0 / tempo));
    std::vector<std::uint8_t> track;
    append_vlq(track, 0);
    track.insert(track.end(), {0xff, 0x51, 0x03,
                               static_cast<std::uint8_t>((microseconds_per_quarter >> 16) & 0xff),
                               static_cast<std::uint8_t>((microseconds_per_quarter >> 8) & 0xff),
                               static_cast<std::uint8_t>(microseconds_per_quarter & 0xff)});
    append_vlq(track, 0);
    const int denominator_power = metadata.time_denominator == 8 ? 3 : 2;
    track.insert(track.end(), {0xff, 0x58, 0x04,
                               static_cast<std::uint8_t>(std::clamp(metadata.time_numerator, 1, 255)),
                               static_cast<std::uint8_t>(denominator_power), 24, 8});
    append_vlq(track, 0);
    track.insert(track.end(), {0xff, 0x59, 0x02,
                               static_cast<std::uint8_t>(static_cast<std::int8_t>(
                                   std::clamp(metadata.key_fifths, -7, 7))),
                               static_cast<std::uint8_t>(metadata.minor_key ? 1 : 0)});

    struct Event {
        std::uint32_t tick;
        std::uint8_t status;
        std::uint8_t pitch;
        std::uint8_t velocity;
    };
    std::vector<Event> events;
    events.reserve(notes.size() * 2);
    constexpr std::uint32_t maximum_tick = 0x0fffffffU;
    for (const auto& note : notes) {
        const auto start = static_cast<std::uint32_t>(std::clamp(
            std::llround(note.start_seconds * tempo * 8.0), 0LL,
            static_cast<long long>(maximum_tick - 1)));
        const auto duration = static_cast<std::uint32_t>(std::clamp(
            std::llround(note.duration_seconds * tempo * 8.0), 1LL,
            static_cast<long long>(maximum_tick)));
        const auto end = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            maximum_tick, static_cast<std::uint64_t>(start) + duration));
        const auto velocity = static_cast<std::uint8_t>(std::clamp(
            std::lround(48.0 + 79.0 * note.confidence), 1L, 127L));
        const auto pitch = static_cast<std::uint8_t>(std::clamp(note.midi_pitch, 0, 127));
        events.push_back({start, 0x90, pitch, velocity});
        events.push_back({end, 0x80, pitch, 0});
    }
    std::stable_sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        if (left.tick != right.tick) {
            return left.tick < right.tick;
        }
        return left.status < right.status;
    });
    std::uint32_t previous_tick = 0;
    for (const auto& event : events) {
        append_vlq(track, event.tick - previous_tick);
        track.insert(track.end(), {event.status, event.pitch, event.velocity});
        previous_tick = event.tick;
    }
    append_vlq(track, 0);
    track.insert(track.end(), {0xff, 0x2f, 0x00});

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create MIDI output: " + path.string());
    }
    output.write("MThd", 4);
    write_be32(output, 6);
    output.put(0);
    output.put(0);
    output.put(0);
    output.put(1);
    output.put(0x01);
    output.put(0xe0);
    output.write("MTrk", 4);
    write_be32(output, static_cast<std::uint32_t>(track.size()));
    for (const auto byte : track) {
        output.put(static_cast<char>(byte));
    }
    if (!output) {
        throw std::runtime_error("Failed while writing MIDI output.");
    }
}

} // namespace transcribe
