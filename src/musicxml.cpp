#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string_view>

namespace transcribe {
namespace {

std::string xml_escape(const std::string& text);

std::string score_note_xml_id(std::string_view note_id, bool tablature) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = tablature ? "tab-score-note-" : "score-note-";
    result.reserve(result.size() + note_id.size() * 2);
    for (const unsigned char byte : note_id) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

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

double score_time_seconds(const ScoreHypothesis& hypothesis,
                          double quarter_position) {
    if (hypothesis.tempo_map.empty()) return quarter_position * 0.5;
    const TempoSegment* selected = &hypothesis.tempo_map.front();
    for (const auto& segment : hypothesis.tempo_map) {
        if (segment.start_quarters <= quarter_position) selected = &segment;
        else break;
    }
    return selected->start_seconds +
        (quarter_position - selected->start_quarters) * 60.0 /
            selected->beats_per_minute;
}

struct RhythmValue {
    int ticks;
    const char* type;
    int dots;
    int tuplet_actual{};
    int tuplet_normal{};
};

constexpr std::array<RhythmValue, 15> rhythm_values{{
    {192, "whole", 0}, {144, "half", 1}, {96, "half", 0},
    {72, "quarter", 1}, {48, "quarter", 0}, {36, "eighth", 1},
    {32, "quarter", 0, 3, 2}, {24, "eighth", 0},
    {18, "16th", 1}, {16, "eighth", 0, 3, 2},
    {12, "16th", 0}, {8, "16th", 0, 3, 2},
    {6, "32nd", 0}, {3, "64th", 0}, {1, "256th", 0}}};

struct ScoreToken {
    int duration{};
    int midi_pitch{-1};
    int voice{};
    bool tie_in{};
    bool tie_out{};
    bool chord_member{};
    bool include_type{true};
    bool spelling_override{};
    bool accidental_required{};
    int spelling_alter{};
    int spelling_octave{};
    std::string spelling_step;
    std::optional<double> tempo_change;
    std::optional<int> tablature_pitch;
    std::optional<int> explicit_velocity;
    std::optional<std::string> explicit_articulation;
    std::string score_note_id;
    std::size_t fragment_index{};
    bool suppress_heuristics{};
    const Note* source{};
    const char* type{"64th"};
    int dots{};
    int tuplet_actual{};
    int tuplet_normal{};
    double expressive_offset_seconds{};
    bool staccato{};
    bool accent{};
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

int ticks_for_time(double seconds, double tempo_bpm, double swing_ratio = 1.0) {
    double raw = std::max(0.0, seconds) * tempo_bpm * 48.0 / 60.0;
    if (swing_ratio > 1.2) {
        const double beat = std::floor(raw / 48.0) * 48.0;
        const double swing_eighth = 48.0 * swing_ratio / (swing_ratio + 1.0);
        if (std::abs(raw - (beat + swing_eighth)) <= 3.0) {
            raw = beat + swing_eighth;
        }
    }
    const double sixteenth = std::round(raw / 12.0) * 12.0;
    const double triplet = std::round(raw / 8.0) * 8.0;
    double snapped = std::round(raw);
    const double sixteenth_distance = std::abs(sixteenth - raw);
    const double triplet_distance = std::abs(triplet - raw);
    if (sixteenth_distance <= 3.0 && sixteenth_distance <= triplet_distance) {
        snapped = sixteenth;
    } else if (triplet_distance <= 2.0) {
        snapped = triplet;
    } else if (sixteenth_distance <= 3.0) {
        snapped = sixteenth;
    }
    return static_cast<int>(std::clamp(snapped, 0.0, 1.0e8));
}

std::vector<RhythmValue> decompose_duration(int ticks) {
    std::vector<RhythmValue> chunks;
    while (ticks > 0) {
        const auto value = std::find_if(rhythm_values.begin(), rhythm_values.end(),
                                        [ticks](const RhythmValue& candidate) {
                                            return candidate.ticks <= ticks;
                                        });
        if (value == rhythm_values.end()) {
            throw std::logic_error("Could not represent a MusicXML duration.");
        }
        chunks.push_back(*value);
        ticks -= value->ticks;
    }
    return chunks;
}

void append_rest(Score& measures, std::size_t voice, int ticks_per_measure,
                 int& cursor, int target) {
    while (cursor < target) {
        const int space = ticks_per_measure - cursor % ticks_per_measure;
        const int segment = std::min(target - cursor, space);
        for (const auto& rhythm : decompose_duration(segment)) {
            ScoreToken token;
            token.duration = rhythm.ticks;
            token.midi_pitch = -1;
            token.voice = static_cast<int>(voice + 1);
            token.type = rhythm.type;
            token.dots = rhythm.dots;
            token.tuplet_actual = rhythm.tuplet_actual;
            token.tuplet_normal = rhythm.tuplet_normal;
            measures[static_cast<std::size_t>(cursor / ticks_per_measure)][voice]
                .push_back(token);
            cursor += rhythm.ticks;
        }
    }
}

void append_note(Score& measures, std::size_t voice_index, int ticks_per_measure,
                 int& cursor, int duration, const Note* source, double tempo_bpm) {
    std::vector<std::pair<std::size_t, RhythmValue>> fragments;
    const int start_tick = cursor;
    int remaining = duration;
    while (remaining > 0) {
        const int space = ticks_per_measure - cursor % ticks_per_measure;
        const int segment = std::min(remaining, space);
        for (const auto& rhythm : decompose_duration(segment)) {
            fragments.emplace_back(static_cast<std::size_t>(cursor / ticks_per_measure),
                                   rhythm);
            cursor += rhythm.ticks;
            remaining -= rhythm.ticks;
        }
    }
    for (std::size_t index = 0; index < fragments.size(); ++index) {
        const auto& [measure, rhythm] = fragments[index];
        ScoreToken token;
        token.duration = rhythm.ticks;
        token.midi_pitch = source->midi_pitch;
        token.voice = static_cast<int>(voice_index + 1);
        token.tie_in = index > 0;
        token.tie_out = index + 1 < fragments.size();
        token.source = source;
        token.type = rhythm.type;
        token.dots = rhythm.dots;
        token.tuplet_actual = rhythm.tuplet_actual;
        token.tuplet_normal = rhythm.tuplet_normal;
        token.expressive_offset_seconds =
            source->start_seconds - start_tick * 60.0 / (tempo_bpm * 48.0);
        token.staccato = index == 0 &&
                         source->duration_seconds * tempo_bpm / 60.0 < 0.5;
        token.accent = index == 0 && source->amplitude >= 0.3;
        measures[measure][voice_index].push_back(token);
    }
}

void write_measure_start(std::ostream& output, int measure_number, bool first,
                         const ScoreMetadata& metadata, bool tablature,
                         const GuitarTabOptions& tab_options, int divisions = 48,
                         bool implicit = false, bool include_tempo = true,
                         bool include_key = true, bool include_time = true,
                         int written_transposition = 0) {
    output << "    <measure number=\"" << measure_number << "\"";
    if (implicit) output << " implicit=\"yes\"";
    output << ">\n";
    if (!first) {
        return;
    }
    output << "      <attributes><divisions>" << divisions << "</divisions>";
    if (include_key) {
        output << "<key><fifths>" << metadata.key_fifths << "</fifths><mode>"
               << (metadata.minor_key ? "minor" : "major") << "</mode></key>";
    }
    if (include_time) {
        output << "<time><beats>" << metadata.time_numerator
               << "</beats><beat-type>" << metadata.time_denominator
               << "</beat-type></time>";
    }
    if (tablature) {
        output << "<clef><sign>TAB</sign><line>5</line></clef>"
                  "<staff-details><staff-lines>6</staff-lines>";
        for (std::size_t string = 0; string < tab_options.tuning.size(); ++string) {
            const auto pitch = spell_pitch(tab_options.tuning[string]);
            output << "<staff-tuning line=\"" << 6 - string << "\"><tuning-step>"
                   << pitch.step << "</tuning-step>";
            if (pitch.alter != 0) {
                output << "<tuning-alter>" << pitch.alter << "</tuning-alter>";
            }
            output << "<tuning-octave>" << pitch.octave
                   << "</tuning-octave></staff-tuning>";
        }
        output << "</staff-details>";
    } else {
        output << "<clef><sign>G</sign><line>2</line></clef>"
                  "<staff-details><staff-lines>5</staff-lines></staff-details>";
    }
    if (!tablature && written_transposition != 0) {
        output << "<transpose><chromatic>" << -written_transposition
               << "</chromatic></transpose>";
    }
    output << "</attributes>\n";
    if (include_tempo) {
        output << "      <direction><direction-type><words>Estimated tempo and meter";
    if (metadata.swing_inferred) {
        output << "; swing " << std::fixed << std::setprecision(1)
               << metadata.swing_ratio << ":1";
    }
        output << "</words></direction-type><sound tempo=\"" << metadata.tempo_bpm
               << "\"/></direction>\n";
    }
    if (tablature && tab_options.capo_fret > 0) {
        output << "      <direction><direction-type><words>Capo "
               << tab_options.capo_fret << "</words></direction-type></direction>\n";
    }
}

const char* dynamic_for_amplitude(double amplitude) {
    if (amplitude < 0.03) return "pp";
    if (amplitude < 0.06) return "p";
    if (amplitude < 0.12) return "mp";
    if (amplitude < 0.22) return "mf";
    if (amplitude < 0.38) return "f";
    return "ff";
}

const char* dynamic_for_velocity(int velocity) {
    if (velocity < 16) return "ppp";
    if (velocity < 32) return "pp";
    if (velocity < 48) return "p";
    if (velocity < 64) return "mp";
    if (velocity < 80) return "mf";
    if (velocity < 96) return "f";
    if (velocity < 112) return "ff";
    return "fff";
}

void write_score_token(std::ostream& output, const ScoreToken& token,
                       const GuitarTabPosition* tab_position, bool tablature,
                       bool preserve_expressive_timing) {
    if (token.tempo_change) {
        output << "      <direction><direction-type><words>Tempo "
               << std::fixed << std::setprecision(1) << *token.tempo_change
               << " BPM</words></direction-type><sound tempo=\""
               << *token.tempo_change << "\"/></direction>\n";
        return;
    }
    if (!tablature && token.explicit_velocity && !token.tie_in) {
        output << "      <direction><direction-type><dynamics><"
               << dynamic_for_velocity(*token.explicit_velocity)
               << "/></dynamics></direction-type><staff>1</staff></direction>\n";
    }
    if (!token.suppress_heuristics && !tablature && token.source != nullptr && !token.tie_in) {
        if (token.source->amplitude > 0.0) {
            output << "      <direction><direction-type><dynamics><"
                   << dynamic_for_amplitude(token.source->amplitude)
                   << "/></dynamics></direction-type><staff>1</staff></direction>\n";
        }
        if (preserve_expressive_timing &&
            std::abs(token.expressive_offset_seconds) >= 0.01) {
            const auto flags = output.flags();
            const auto precision = output.precision();
            output << "      <direction><direction-type><words>Expressive onset "
                   << (token.expressive_offset_seconds >= 0.0 ? "+" : "")
                   << std::fixed << std::setprecision(3) << token.expressive_offset_seconds
                   << "s</words></direction-type>"
                      "<staff>1</staff></direction>\n";
            output.flags(flags);
            output.precision(precision);
        }
    }
    if (token.suppress_heuristics && !tablature && preserve_expressive_timing &&
        !token.tie_in && std::abs(token.expressive_offset_seconds) >= 0.01) {
        const auto flags = output.flags();
        const auto precision = output.precision();
        output << "      <direction><direction-type><words>Expressive onset "
               << (token.expressive_offset_seconds >= 0.0 ? "+" : "")
               << std::fixed << std::setprecision(3) << token.expressive_offset_seconds
               << "s</words></direction-type><staff>1</staff></direction>\n";
        output.flags(flags);
        output.precision(precision);
    }
    output << "      <note";
    if (!token.score_note_id.empty()) {
        std::string xml_id = score_note_xml_id(token.score_note_id, tablature);
        if (token.fragment_index > 0) {
            xml_id += "-continuation-" + std::to_string(token.fragment_index);
        }
        output << " id=\"" << xml_id << '"';
    }
    output << '>';
    if (token.chord_member) output << "<chord/>";
    const int pitch = tablature && token.tablature_pitch
        ? *token.tablature_pitch : token.midi_pitch;
    if (pitch < 0) {
        output << "<rest/>";
    } else {
        const auto spelling = token.spelling_override && !tablature
            ? Spelling{token.spelling_step.c_str(), token.spelling_alter,
                       token.spelling_octave}
            : spell_pitch(pitch);
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
    if (token.include_type) output << "<type>" << token.type << "</type>";
    for (int dot = 0; dot < token.dots; ++dot) {
        output << "<dot/>";
    }
    if (!tablature && token.spelling_override && token.accidental_required) {
        const char* accidental = token.spelling_alter > 0 ? "sharp" :
                                 token.spelling_alter < 0 ? "flat" : "natural";
        output << "<accidental>" << accidental << "</accidental>";
    }
    if (token.tuplet_actual > 0) {
        output << "<time-modification><actual-notes>" << token.tuplet_actual
               << "</actual-notes><normal-notes>" << token.tuplet_normal
               << "</normal-notes></time-modification>";
    }
    if (pitch >= 0) {
        const bool technical = tablature && tab_position != nullptr &&
                               tab_position->string_number > 0 &&
                               tab_position->fret >= 0;
        const auto articulation = token.explicit_articulation.value_or("");
        const bool explicit_staccato = articulation == "staccato";
        const bool explicit_accent = articulation == "accent";
        const bool explicit_tenuto = articulation == "tenuto";
        if (token.tie_in || token.tie_out || token.staccato || token.accent ||
            explicit_staccato || explicit_accent || explicit_tenuto || technical) {
            output << "<notations>";
            if (token.tie_in) {
                output << "<tied type=\"stop\"/>";
            }
            if (token.tie_out) {
                output << "<tied type=\"start\"/>";
            }
            if (token.staccato || token.accent || explicit_staccato || explicit_accent ||
                explicit_tenuto) {
                output << "<articulations>";
                if (token.staccato || explicit_staccato) output << "<staccato/>";
                if (token.accent || explicit_accent) output << "<accent/>";
                if (explicit_tenuto) output << "<tenuto/>";
                output << "</articulations>";
            }
            if (technical) {
                output << "<technical><string>" << tab_position->string_number
                       << "</string><fret>" << tab_position->fret
                       << "</fret></technical>";
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

std::string xml_escape(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&apos;"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

const char* xml_type_for_denominator(int denominator) {
    switch (denominator) {
    case 1: return "whole";
    case 2: return "half";
    case 4: return "quarter";
    case 8: return "eighth";
    case 16: return "16th";
    case 32: return "32nd";
    case 64: return "64th";
    case 128: return "128th";
    default: return "";
    }
}

struct SymbolicRhythm {
    int denominator{};
    int dots{};
    int tuplet_actual{};
    int tuplet_normal{};
    bool symbolic{};
};

SymbolicRhythm notation_for_quarters(double quarters) {
    SymbolicRhythm best;
    double best_error = std::numeric_limits<double>::infinity();
    for (const int denominator : {1, 2, 4, 8, 16, 32, 64, 128}) {
        const double base = 4.0 / denominator;
        for (int dots = 0; dots <= 2; ++dots) {
            const double multiplier = dots == 0 ? 1.0 : dots == 1 ? 1.5 : 1.75;
            for (const auto [actual, normal] :
                 {std::pair{0, 0}, std::pair{3, 2}, std::pair{5, 4}, std::pair{7, 4}}) {
                const double duration = base * multiplier *
                    (actual == 0 ? 1.0 : static_cast<double>(normal) / actual);
                const double error = std::abs(duration - quarters);
                if (error < best_error) {
                    best = {denominator, dots, actual, normal, error < 0.008};
                    best_error = error;
                }
            }
        }
    }
    return best;
}

void write_interpretation_note(std::ostream& output, const ScoreToken& token,
                               const GuitarTabPosition* tab_position, bool tablature,
                               bool preserve_expressive_timing) {
    write_score_token(output, token, tab_position, tablature,
                      preserve_expressive_timing);
}

} // namespace

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes) {
    write_musicxml(path, notes, infer_score_metadata(notes));
}

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& requested_metadata) {
    write_musicxml(path, notes, requested_metadata, GuitarTabOptions{});
}

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes,
                    const ScoreMetadata& requested_metadata,
                    const GuitarTabOptions& tab_options,
                    bool preserve_expressive_timing) {
    const auto tab_positions = assign_guitar_tab_positions(notes, tab_options);
    std::unordered_map<const Note*, GuitarTabPosition> position_by_note;
    position_by_note.reserve(notes.size());
    for (const auto& position : tab_positions) {
        position_by_note.emplace(&notes[position.note_index], position);
    }
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
    if (!std::isfinite(metadata.swing_ratio) || metadata.swing_ratio < 1.0 ||
        metadata.swing_ratio > 3.0) {
        metadata.swing_ratio = 1.0;
        metadata.swing_inferred = false;
    }
    const int ticks_per_measure =
        metadata.time_numerator * 192 / metadata.time_denominator;
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
        const int start = ticks_for_time(
            note->start_seconds, metadata.tempo_bpm,
            metadata.swing_inferred ? metadata.swing_ratio : 1.0);
        const int duration = std::max(1, ticks_for_time(note->duration_seconds,
                                                        metadata.tempo_bpm, 1.0));
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
                        assigned.duration_tick, assigned.note, metadata.tempo_bpm);
        }
        append_rest(score, voice_index, ticks_per_measure, cursor,
                    static_cast<int>(measure_count) * ticks_per_measure);
    }

    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
              "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
              "<score-partwise version=\"4.0\"><work><work-title>Transcription</work-title>"
              "</work><part-list><score-part id=\"P1\"><part-name>Guitar</part-name>"
              "</score-part><score-part id=\"P2\"><part-name>Guitar Tablature</part-name>"
              "</score-part></part-list>\n";

    const auto write_part = [&](const char* part_id, bool tablature) {
        output << "  <part id=\"" << part_id << "\">\n";
        for (std::size_t measure = 0; measure < measure_count; ++measure) {
            write_measure_start(output, static_cast<int>(measure + 1), measure == 0,
                                metadata, tablature, tab_options);
            for (std::size_t voice = 0; voice < voices.size(); ++voice) {
                for (const auto& token : score[measure][voice]) {
                    const GuitarTabPosition* tab_position = nullptr;
                    if (tablature && token.source != nullptr) {
                        tab_position = &position_by_note.at(token.source);
                    }
                    write_score_token(output, token, tab_position, tablature,
                                      preserve_expressive_timing);
                }
                if (voice + 1 < voices.size()) {
                    output << "      <backup><duration>" << ticks_per_measure
                           << "</duration></backup>\n";
                }
            }
            output << "    </measure>\n";
        }
        output << "  </part>\n";
    };
    write_part("P1", false);
    write_part("P2", true);
    output << "</score-partwise>\n";
    if (!output) {
        throw std::runtime_error("Failed while writing MusicXML output.");
    }
}

std::string render_musicxml(const ScoreInterpretation& interpretation,
                            std::size_t hypothesis_index,
                            const GuitarTabOptions& tab_options,
                            bool preserve_expressive_timing,
                            const std::vector<std::string>* score_note_ids) {
    if (hypothesis_index >= interpretation.hypotheses.size()) {
        throw std::out_of_range("MusicXML hypothesis index is out of range.");
    }
    const auto& hypothesis = interpretation.hypotheses[hypothesis_index];
    if (score_note_ids != nullptr &&
        score_note_ids->size() != hypothesis.notes.size()) {
        throw std::invalid_argument(
            "MusicXML canonical note IDs do not match the selected score hypothesis.");
    }
    if (hypothesis.meter.numerator <= 0 || hypothesis.meter.numerator > 16 ||
        (hypothesis.meter.denominator != 4 && hypothesis.meter.denominator != 8)) {
        throw std::invalid_argument("MusicXML requires a valid quarter- or eighth-denominator meter.");
    }
    if (!std::isfinite(hypothesis.pickup_quarters) || hypothesis.pickup_quarters < 0.0 ||
        !std::isfinite(hypothesis.total_quarters) || hypothesis.total_quarters < 0.0) {
        throw std::invalid_argument("MusicXML interpretation has invalid score duration.");
    }
    if (hypothesis.tempo_map.empty()) {
        throw std::invalid_argument("MusicXML interpretation has no tempo map.");
    }
    constexpr int divisions = 960;
    const double maximum_score_position = static_cast<double>(
        (std::numeric_limits<int>::max() - 1) / divisions);
    double total_quarters = std::max(hypothesis.total_quarters,
                                     hypothesis.pickup_quarters);
    double previous_tempo_position = -1.0;
    for (const auto& segment : hypothesis.tempo_map) {
        if (!std::isfinite(segment.beats_per_minute) ||
            segment.beats_per_minute < 1.0 || segment.beats_per_minute > 1000.0 ||
            !std::isfinite(segment.start_quarters) || segment.start_quarters < 0.0 ||
            segment.start_quarters < previous_tempo_position ||
            segment.start_quarters > maximum_score_position) {
            throw std::invalid_argument("MusicXML interpretation has an invalid tempo segment.");
        }
        previous_tempo_position = segment.start_quarters;
        total_quarters = std::max(total_quarters, segment.start_quarters);
    }

    const double bar_quarters = static_cast<double>(hypothesis.meter.numerator) * 4.0 /
                                hypothesis.meter.denominator;
    int voice_count = 1;
    for (const auto& note : hypothesis.notes) {
        if (note.source_event_index >= interpretation.source_events.size() ||
            !interpretation.source_events[note.source_event_index]) {
            throw std::invalid_argument("MusicXML note refers to a missing source event.");
        }
        if (!std::isfinite(note.onset_quarters) || note.onset_quarters < 0.0 ||
            !std::isfinite(note.duration_quarters) || note.duration_quarters <= 0.0 ||
            note.onset_quarters + note.duration_quarters > maximum_score_position ||
            note.written_midi_pitch < 0 || note.written_midi_pitch > 127 ||
            note.voice < 1) {
            throw std::invalid_argument("MusicXML interpretation contains an invalid note.");
        }
        const auto& source = *interpretation.source_events[note.source_event_index];
        if (!std::isfinite(source.midi_pitch) || source.midi_pitch < 0.0 ||
            source.midi_pitch > 127.0 || !std::isfinite(source.onset_seconds) ||
            !std::isfinite(source.offset_seconds) ||
            source.onset_seconds < 0.0 || source.offset_seconds < source.onset_seconds ||
            (note.velocity && (*note.velocity < 0 || *note.velocity > 127)) ||
            (!note.spelling_step.empty() &&
             (note.spelling_step.size() != 1 ||
              std::string("ABCDEFG").find(note.spelling_step) == std::string::npos ||
              note.spelling_alter < -2 || note.spelling_alter > 2 ||
              note.spelling_octave < -1 || note.spelling_octave > 9))) {
            throw std::invalid_argument("MusicXML note has invalid source evidence or spelling.");
        }
        total_quarters = std::max(total_quarters,
                                  note.onset_quarters + note.duration_quarters);
        voice_count = std::max(voice_count, note.voice);
    }
    if (total_quarters > maximum_score_position) {
        throw std::invalid_argument("MusicXML score duration exceeds the supported range.");
    }
    if (hypothesis.pickup_quarters > 0.0 && hypothesis.pickup_quarters > bar_quarters) {
        throw std::invalid_argument("Pickup measure cannot be longer than the selected meter.");
    }
    if (total_quarters <= 0.0) {
        total_quarters = bar_quarters;
    }

    std::vector<int> measure_starts;
    std::vector<int> measure_ends;
    const auto to_tick = [](double quarters) {
        return static_cast<int>(std::llround(quarters * divisions));
    };
    int current_tick = 0;
    if (hypothesis.pickup_quarters > 0.0) {
        const int pickup_ticks = to_tick(hypothesis.pickup_quarters);
        if (pickup_ticks <= 0) {
            throw std::invalid_argument("Pickup measure is shorter than one MusicXML tick.");
        }
        measure_starts.push_back(0);
        measure_ends.push_back(pickup_ticks);
        current_tick = pickup_ticks;
    }
    const int bar_ticks = to_tick(bar_quarters);
    if (bar_ticks <= 0) {
        throw std::invalid_argument("MusicXML meter has a zero-length measure.");
    }
    const int total_ticks = to_tick(total_quarters);
    while (current_tick < total_ticks || measure_starts.empty()) {
        const int next_tick = static_cast<int>(std::min<std::int64_t>(
            total_ticks, static_cast<std::int64_t>(current_tick) + bar_ticks));
        measure_starts.push_back(current_tick);
        measure_ends.push_back(std::max(current_tick + 1, next_tick));
        current_tick = measure_ends.back();
    }

    const auto measure_for_tick = [&](int tick) {
        const auto found = std::upper_bound(measure_starts.begin(),
                                            measure_starts.end(), tick);
        if (found == measure_starts.begin()) return std::size_t{0};
        return std::min<std::size_t>(
            static_cast<std::size_t>(std::distance(measure_starts.begin(), found) - 1),
            measure_starts.size() - 1);
    };
    std::vector<Note> tab_notes(hypothesis.notes.size());
    for (std::size_t index = 0; index < hypothesis.notes.size(); ++index) {
        const auto& interpreted = hypothesis.notes[index];
        const auto& source = *interpretation.source_events[interpreted.source_event_index];
        auto& tab_note = tab_notes[index];
        tab_note.midi_pitch = std::clamp(
            interpreted.sounding_midi_pitch >= 0
                ? interpreted.sounding_midi_pitch
                : static_cast<int>(std::lround(source.midi_pitch)), 0, 127);
        tab_note.start_seconds =
            score_time_seconds(hypothesis, interpreted.onset_quarters);
        tab_note.duration_seconds =
            score_time_seconds(hypothesis, interpreted.onset_quarters +
                               interpreted.duration_quarters) -
            tab_note.start_seconds;
        tab_note.name = midi_note_name(tab_note.midi_pitch);
        tab_note.confidence = source.calibrated_confidence.value_or(0.0);
        tab_note.amplitude = source.intensity_rms.value_or(0.0);
    }
    const auto tab_positions = assign_guitar_tab_positions(tab_notes, tab_options);
    std::unordered_map<const Note*, GuitarTabPosition> position_by_note;
    position_by_note.reserve(tab_notes.size());
    for (const auto& position : tab_positions) {
        if (position.note_index >= tab_notes.size()) {
            throw std::runtime_error("Guitar tablature assignment returned an invalid note index.");
        }
        position_by_note.emplace(&tab_notes[position.note_index], position);
    }

    Score score(measure_starts.size(),
                std::vector<std::vector<ScoreToken>>(static_cast<std::size_t>(voice_count)));
    const auto append_rest_interval = [&](int start_tick, int end_tick, int voice) {
        int cursor = start_tick;
        while (cursor < end_tick) {
            const std::size_t measure = measure_for_tick(cursor);
            const int fragment_end = std::min(end_tick, measure_ends[measure]);
            if (fragment_end <= cursor) {
                throw std::logic_error("MusicXML rest could not advance to the next measure.");
            }
            const int duration = fragment_end - cursor;
            const auto rhythm = notation_for_quarters(
                static_cast<double>(duration) / divisions);
            ScoreToken token;
            token.duration = duration;
            token.midi_pitch = -1;
            token.voice = voice;
            token.suppress_heuristics = true;
            token.include_type = rhythm.symbolic;
            if (rhythm.symbolic) {
                token.type = xml_type_for_denominator(rhythm.denominator);
                token.dots = rhythm.dots;
                token.tuplet_actual = rhythm.tuplet_actual;
                token.tuplet_normal = rhythm.tuplet_normal;
            }
            score[measure][static_cast<std::size_t>(voice - 1)].push_back(token);
            cursor = fragment_end;
        }
    };
    const auto append_note_fragments = [&](std::size_t note_index, int start_tick,
                                           int end_tick, int voice,
                                           bool chord_member) {
        int cursor = start_tick;
        bool first_fragment = true;
        std::size_t fragment_index = 0;
        const auto& interpreted = hypothesis.notes[note_index];
        const auto& source = *interpretation.source_events[interpreted.source_event_index];
        while (cursor < end_tick) {
            const std::size_t measure = measure_for_tick(cursor);
            const int fragment_end = std::min(end_tick, measure_ends[measure]);
            if (fragment_end <= cursor) {
                throw std::logic_error("MusicXML note could not advance to the next measure.");
            }
            const int duration = fragment_end - cursor;
            const auto rhythm = notation_for_quarters(
                static_cast<double>(duration) / divisions);
            ScoreToken token;
            token.duration = duration;
            token.midi_pitch = interpreted.written_midi_pitch;
            token.voice = voice;
            token.tie_in = !first_fragment ||
                (first_fragment && interpreted.tie_stop);
            token.tie_out = fragment_end < end_tick ||
                (fragment_end == end_tick && interpreted.tie_start);
            token.chord_member = chord_member;
            token.include_type = rhythm.symbolic;
            if (rhythm.symbolic) {
                token.type = xml_type_for_denominator(rhythm.denominator);
                token.dots = rhythm.dots;
                token.tuplet_actual = rhythm.tuplet_actual;
                token.tuplet_normal = rhythm.tuplet_normal;
            }
            token.spelling_override = !interpreted.spelling_step.empty();
            token.spelling_step = interpreted.spelling_step;
            token.spelling_alter = interpreted.spelling_alter;
            token.spelling_octave = interpreted.spelling_octave;
            token.accidental_required = interpreted.accidental_required;
            if (score_note_ids != nullptr) {
                token.score_note_id = (*score_note_ids)[note_index];
            }
            token.fragment_index = fragment_index++;
            token.tablature_pitch = std::clamp(
                interpreted.sounding_midi_pitch >= 0
                    ? interpreted.sounding_midi_pitch
                    : static_cast<int>(std::lround(source.midi_pitch)), 0, 127);
            token.explicit_velocity = interpreted.velocity;
            token.explicit_articulation = interpreted.articulation;
            token.suppress_heuristics = true;
            token.expressive_offset_seconds = -interpreted.onset_residual_seconds;
            token.source = &tab_notes[note_index];
            score[measure][static_cast<std::size_t>(voice - 1)].push_back(std::move(token));
            cursor = fragment_end;
            first_fragment = false;
        }
    };

    std::vector<std::vector<std::size_t>> notes_by_voice(
        static_cast<std::size_t>(voice_count));
    std::unordered_map<std::size_t, int> chord_start_ticks;
    for (std::size_t index = 0; index < hypothesis.notes.size(); ++index) {
        const auto& note = hypothesis.notes[index];
        const int start_tick = to_tick(note.onset_quarters);
        if (note.chord_group) {
            const auto [found, inserted] = chord_start_ticks.emplace(
                *note.chord_group, start_tick);
            if (!inserted) found->second = std::min(found->second, start_tick);
        }
        notes_by_voice[static_cast<std::size_t>(note.voice - 1)].push_back(index);
    }
    const auto note_start_tick = [&](std::size_t note_index) {
        const auto& note = hypothesis.notes[note_index];
        if (note.chord_group) return chord_start_ticks.at(*note.chord_group);
        return to_tick(note.onset_quarters);
    };
    for (std::size_t voice_index = 0; voice_index < notes_by_voice.size(); ++voice_index) {
        auto& voice_notes = notes_by_voice[voice_index];
        std::stable_sort(voice_notes.begin(), voice_notes.end(), [&](std::size_t left,
                                                                     std::size_t right) {
            const int left_start = note_start_tick(left);
            const int right_start = note_start_tick(right);
            if (left_start != right_start) return left_start < right_start;
            return hypothesis.notes[left].written_midi_pitch <
                   hypothesis.notes[right].written_midi_pitch;
        });
        int cursor = 0;
        for (std::size_t first = 0; first < voice_notes.size();) {
            const int start_tick = note_start_tick(voice_notes[first]);
            std::size_t end = first + 1;
            while (end < voice_notes.size() &&
                   note_start_tick(voice_notes[end]) == start_tick) {
                ++end;
            }
            if (start_tick < cursor) {
                throw std::invalid_argument(
                    "MusicXML score hypothesis contains overlapping notes in one voice.");
            }
            append_rest_interval(cursor, start_tick, static_cast<int>(voice_index + 1));
            int group_end_tick = start_tick;
            for (std::size_t item = first; item < end; ++item) {
                const std::size_t note_index = voice_notes[item];
                const auto& note = hypothesis.notes[note_index];
                const int note_end = start_tick +
                    std::max(1, to_tick(note.duration_quarters));
                append_note_fragments(note_index, start_tick, note_end,
                                      static_cast<int>(voice_index + 1), item > first);
                group_end_tick = std::max(group_end_tick, note_end);
            }
            cursor = group_end_tick;
            first = end;
        }
        append_rest_interval(cursor, measure_ends.back(),
                             static_cast<int>(voice_index + 1));
    }

    const bool emit_tempo_map = !hypothesis.tempo_ambiguous ||
                                hypothesis.tempo_map.front().user_constrained;
    if (emit_tempo_map) {
        for (const auto& segment : hypothesis.tempo_map) {
            const int target_tick = to_tick(segment.start_quarters);
            const std::size_t measure = measure_for_tick(target_tick);
            auto& tokens = score[measure][0];
            int elapsed = measure_starts[measure];
            auto insertion = tokens.begin();
            for (; insertion != tokens.end(); ++insertion) {
                if (elapsed >= target_tick) break;
                if (!insertion->chord_member) elapsed += insertion->duration;
            }
            ScoreToken tempo_token;
            tempo_token.tempo_change = segment.beats_per_minute;
            tempo_token.suppress_heuristics = true;
            tokens.insert(insertion, std::move(tempo_token));
        }
    }

    const auto metadata = metadata_for_hypothesis(hypothesis);
    const bool include_key = !hypothesis.key_ambiguous || hypothesis.key.user_constrained;
    const bool include_time = !hypothesis.meter_ambiguous || hypothesis.meter.user_constrained;
    const std::string part_name = xml_escape(
        hypothesis.instrument.empty() ? "Transcription" : hypothesis.instrument);
    std::ostringstream output;
    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
              "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
              "<score-partwise version=\"4.0\"><work><work-title>Transcription</work-title>"
              "</work><part-list><score-part id=\"P1\"><part-name>"
           << part_name << "</part-name></score-part>"
              "<score-part id=\"P2\"><part-name>"
           << part_name << " Tablature</part-name></score-part></part-list>\n";

    const auto write_part = [&](const char* part_id, bool tablature) {
        output << "  <part id=\"" << part_id << "\">\n";
        for (std::size_t measure = 0; measure < score.size(); ++measure) {
            write_measure_start(output, static_cast<int>(measure + 1), measure == 0,
                                metadata, tablature, tab_options, divisions,
                                measure == 0 && hypothesis.pickup_quarters > 0.0,
                                false, include_key, include_time,
                                hypothesis.written_transposition_semitones);
            if (!tablature && measure > 0) {
                for (const auto& region : hypothesis.local_tonal_context) {
                    if (region.first_measure == measure &&
                        region.key.confidence >= 0.5 &&
                        (region.key.fifths != hypothesis.key.fifths ||
                         region.key.minor != hypothesis.key.minor)) {
                        output << "      <attributes><key><fifths>"
                               << std::clamp(region.key.fifths, -7, 7)
                               << "</fifths><mode>"
                               << (region.key.minor ? "minor" : "major")
                               << "</mode></key></attributes>\n";
                    }
                }
            }
            if (!tablature && measure == 0) {
                std::vector<std::string> uncertainties = hypothesis.uncertainties;
                if (hypothesis.tempo_ambiguous) uncertainties.emplace_back("tempo");
                if (hypothesis.meter_ambiguous) uncertainties.emplace_back("meter");
                if (hypothesis.key_ambiguous) uncertainties.emplace_back("key");
                if (!uncertainties.empty()) {
                    std::ostringstream description;
                    for (std::size_t index = 0; index < uncertainties.size(); ++index) {
                        if (index != 0) description << ", ";
                        description << uncertainties[index];
                    }
                    output << "      <direction><direction-type><words>"
                           << xml_escape("Uncertain interpretation: " + description.str())
                           << "</words></direction-type><staff>1</staff></direction>\n";
                }
            }
            for (std::size_t voice = 0; voice < score[measure].size(); ++voice) {
                for (const auto& token : score[measure][voice]) {
                    const GuitarTabPosition* tab_position = nullptr;
                    if (tablature && token.source != nullptr) {
                        const auto found = position_by_note.find(token.source);
                        if (found != position_by_note.end()) tab_position = &found->second;
                    }
                    write_interpretation_note(output, token, tab_position, tablature,
                                              preserve_expressive_timing);
                }
                if (voice + 1 < score[measure].size()) {
                    output << "      <backup><duration>"
                           << measure_ends[measure] - measure_starts[measure]
                           << "</duration></backup>\n";
                }
            }
            output << "    </measure>\n";
        }
        output << "  </part>\n";
    };
    write_part("P1", false);
    write_part("P2", true);
    output << "</score-partwise>\n";
    if (!output) {
        throw std::runtime_error("Failed while writing interpretation MusicXML output.");
    }
    return output.str();
}

void write_musicxml(const std::filesystem::path& path,
                    const ScoreInterpretation& interpretation,
                    std::size_t hypothesis_index,
                    const GuitarTabOptions& tab_options,
                    bool preserve_expressive_timing) {
    const auto xml = render_musicxml(interpretation, hypothesis_index, tab_options,
                                     preserve_expressive_timing, nullptr);
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create MusicXML output: " + path.string());
    }
    output << xml;
    if (!output) {
        throw std::runtime_error("Failed while writing MusicXML output: " + path.string());
    }
}

void write_musicxml(const std::filesystem::path& path,
                    const ScoreDocument& score,
                    bool preserve_expressive_timing) {
    write_musicxml(path, score, preserve_expressive_timing, nullptr);
}

void write_musicxml(const std::filesystem::path& path,
                    const ScoreDocument& score,
                    bool preserve_expressive_timing,
                    std::string* rendered_musicxml) {
    validate_score_document(score);
    if (score.instrument.tuning.size() != 6) {
        throw std::invalid_argument(
            "MusicXML tablature export requires exactly six configured guitar strings.");
    }
    auto interpretation = to_score_interpretation(score);
    GuitarTabOptions options;
    std::copy(score.instrument.tuning.begin(), score.instrument.tuning.end(),
              options.tuning.begin());
    options.capo_fret = score.instrument.capo_fret;
    for (std::size_t index = 0; index < score.notes.size(); ++index) {
        const auto found = std::find_if(score.tab_notes.begin(), score.tab_notes.end(),
            [&](const auto& tab) {
                return tab.notation_note_id == score.notes[index].id;
            });
        if (found != score.tab_notes.end() && found->string_number && found->fret) {
            options.position_overrides.push_back(
                {index, *found->string_number, *found->fret,
                 found->manually_corrected});
        }
    }
    std::vector<std::string> score_note_ids;
    score_note_ids.reserve(score.notes.size());
    for (const auto& note : score.notes) {
        score_note_ids.push_back(note.id);
    }
    const auto xml = render_musicxml(interpretation, 0, options,
                                     preserve_expressive_timing, &score_note_ids);
    if (rendered_musicxml != nullptr) {
        *rendered_musicxml = xml;
    }
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create MusicXML output: " + path.string());
    }
    output << xml;
    if (!output) {
        throw std::runtime_error("Failed while writing MusicXML output: " + path.string());
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

void write_midi(const std::filesystem::path& path, const ScoreDocument& score) {
    validate_score_document(score);
    constexpr std::uint32_t ticks_per_quarter = 480;
    constexpr std::uint32_t maximum_tick = 0x0fffffffU;
    struct Event {
        std::uint32_t tick{};
        int order{};
        std::vector<std::uint8_t> bytes;
    };
    std::vector<Event> events;
    const auto tempo_event = [&](double bpm, double quarters) {
        if (!std::isfinite(bpm) || bpm < 1.0 || bpm > 1000.0 ||
            !std::isfinite(quarters) || quarters < 0.0) {
            throw std::invalid_argument("Score MIDI tempo map contains an invalid segment.");
        }
        const auto tick = static_cast<std::uint32_t>(std::clamp(
            std::llround(quarters * ticks_per_quarter), 0LL,
            static_cast<long long>(maximum_tick)));
        const auto microseconds =
            static_cast<std::uint32_t>(std::lround(60000000.0 / bpm));
        events.push_back({tick, 0, {0xff, 0x51, 0x03,
            static_cast<std::uint8_t>((microseconds >> 16) & 0xff),
            static_cast<std::uint8_t>((microseconds >> 8) & 0xff),
            static_cast<std::uint8_t>(microseconds & 0xff)}});
    };
    if (score.tempo_map.empty()) {
        tempo_event(score.metadata.tempo_bpm, 0.0);
    } else {
        for (const auto& segment : score.tempo_map) {
            tempo_event(segment.beats_per_minute, segment.start_quarters);
        }
    }
    const int denominator_power = static_cast<int>(
        std::lround(std::log2(static_cast<double>(score.metadata.time_denominator))));
    events.push_back({0, 1, {0xff, 0x58, 0x04,
        static_cast<std::uint8_t>(std::clamp(score.metadata.time_numerator, 1, 255)),
        static_cast<std::uint8_t>(denominator_power), 24, 8}});
    events.push_back({0, 1, {0xff, 0x59, 0x02,
        static_cast<std::uint8_t>(static_cast<std::int8_t>(
            std::clamp(score.metadata.key_fifths, -7, 7))),
        static_cast<std::uint8_t>(score.metadata.minor_key ? 1 : 0)}});

    for (const auto& note : score.notes) {
        const auto start = static_cast<std::uint32_t>(std::clamp(
            std::llround(note.onset_quarters * ticks_per_quarter), 0LL,
            static_cast<long long>(maximum_tick - 1)));
        const auto end = static_cast<std::uint32_t>(std::clamp(
            std::llround((note.onset_quarters + note.duration_quarters) *
                         ticks_per_quarter),
            static_cast<long long>(start + 1),
            static_cast<long long>(maximum_tick)));
        const auto pitch = static_cast<std::uint8_t>(note.sounding_midi_pitch);
        const auto velocity = static_cast<std::uint8_t>(
            std::clamp(note.velocity.value_or(80), 1, 127));
        events.push_back({start, 3, {0x90, pitch, velocity}});
        events.push_back({end, 2, {0x80, pitch, 0}});
    }
    std::stable_sort(events.begin(), events.end(),
        [](const Event& left, const Event& right) {
            return left.tick != right.tick ? left.tick < right.tick
                                           : left.order < right.order;
        });

    std::vector<std::uint8_t> track;
    std::uint32_t previous_tick = 0;
    for (const auto& event : events) {
        append_vlq(track, event.tick - previous_tick);
        track.insert(track.end(), event.bytes.begin(), event.bytes.end());
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
    for (const auto byte : track) output.put(static_cast<char>(byte));
    if (!output) throw std::runtime_error("Failed while writing MIDI output.");
}

} // namespace transcribe
