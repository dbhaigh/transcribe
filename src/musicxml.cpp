#include "transcribe/musicxml.hpp"
#include "transcribe/output.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
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
    int tempo_offset_ticks{};
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
    bool tuplet_start{};
    bool tuplet_stop{};
    int stem_direction{};
    std::vector<std::string> beams;
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
        const auto tuning = configured_tuning(tab_options);
        output << "<clef><sign>TAB</sign><line>"
               << std::min<std::size_t>(5, tuning.size()) << "</line></clef>"
                  "<staff-details><staff-lines>" << tuning.size() << "</staff-lines>";
        for (std::size_t string = 0; string < tuning.size(); ++string) {
            const auto pitch = spell_pitch(tuning[string]);
            output << "<staff-tuning line=\"" << tuning.size() - string << "\"><tuning-step>"
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
        output << "<transpose>";
        if (written_transposition % 12 == 0) {
            output << "<diatonic>" << -written_transposition / 12 * 7 << "</diatonic>";
        }
        output << "<chromatic>" << -written_transposition
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
               << " BPM</words></direction-type>";
        if (token.tempo_offset_ticks) {
            output << "<offset sound=\"yes\">" << token.tempo_offset_ticks << "</offset>";
        }
        output << "<sound tempo=\"" << *token.tempo_change << "\"/></direction>\n";
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
        const char* accidental = token.spelling_alter == 2 ? "double-sharp" :
                                 token.spelling_alter == -2 ? "flat-flat" :
                                 token.spelling_alter == 1 ? "sharp" :
                                 token.spelling_alter == -1 ? "flat" : "natural";
        output << "<accidental>" << accidental << "</accidental>";
    }
    if (token.tuplet_actual > 0) {
        output << "<time-modification><actual-notes>" << token.tuplet_actual
               << "</actual-notes><normal-notes>" << token.tuplet_normal
               << "</normal-notes></time-modification>";
    }
    if (token.stem_direction != 0 && pitch >= 0 && token.include_type &&
        std::string_view(token.type) != "whole") {
        output << "<stem>" << (token.stem_direction > 0 ? "up" : "down") << "</stem>";
    }
    for (std::size_t level = 0; level < token.beams.size(); ++level) {
        output << "<beam number=\"" << level + 1 << "\">"
               << token.beams[level] << "</beam>";
    }
    if (pitch >= 0 || token.tuplet_start || token.tuplet_stop) {
        const bool technical = tablature && tab_position != nullptr &&
                               tab_position->string_number > 0 &&
                               tab_position->fret >= 0;
        const auto articulation = token.explicit_articulation.value_or("");
        const bool explicit_staccato = articulation == "staccato";
        const bool explicit_accent = articulation == "accent";
        const bool explicit_tenuto = articulation == "tenuto";
        const bool unresolved = pitch >= 0 && tablature && !technical && !token.tie_in;
        if (token.tie_in || token.tie_out || token.staccato || token.accent ||
            explicit_staccato || explicit_accent || explicit_tenuto || technical || unresolved ||
            token.tuplet_start || token.tuplet_stop) {
            output << "<notations>";
            if (token.tie_in) {
                output << "<tied type=\"stop\"/>";
            }
            if (token.tie_out) {
                output << "<tied type=\"start\"/>";
            }
            if (token.tuplet_start) output << "<tuplet type=\"start\" number=\"1\"/>";
            if (token.tuplet_stop) output << "<tuplet type=\"stop\" number=\"1\"/>";
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
                       << "</fret>";
                output << "</technical>";
            }
            if (unresolved) {
                output << "<other-notation type=\"single\">"
                          "Unresolved fingering: no assigned string/fret</other-notation>";
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

SymbolicRhythm notation_for_quarters(double quarters, double tolerance) {
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
                if (error < best_error - 1e-9 ||
                    (std::abs(error - best_error) < 1e-9 &&
                     ((best.tuplet_actual != 0 && actual == 0) ||
                      (best.tuplet_actual == actual && dots < best.dots)))) {
                    best = {denominator, dots, actual, normal, error <= tolerance};
                    best_error = error;
                }
            }
        }
    }
    return best;
}

int beam_level(const ScoreToken& token) {
    if (!token.include_type || token.midi_pitch < 0) return 0;
    constexpr std::array<std::string_view, 6> types{
        "eighth", "16th", "32nd", "64th", "128th", "256th"};
    const auto found = std::find(types.begin(), types.end(), token.type);
    return found == types.end() ? 0 :
        static_cast<int>(std::distance(types.begin(), found)) + 1;
}

void group_notation(Score& score, const MeterHypothesis& meter, int divisions,
                    const std::vector<int>& measure_lengths, int pickup_ticks) {
    std::vector<int> grouping = meter.grouping;
    int grouping_total = 0;
    for (const int group : grouping) {
        if (group < 1 || group > meter.numerator) {
            throw std::invalid_argument("MusicXML meter grouping contains an invalid beat group.");
        }
        grouping_total += group;
    }
    if (!grouping.empty() && grouping_total != meter.numerator) {
        throw std::invalid_argument("MusicXML meter grouping must fill the measure.");
    }
    if (grouping.empty()) {
        const int unit = meter.denominator == 8 && meter.numerator > 3 &&
            meter.numerator % 3 == 0 ? 3 : 1;
        grouping.assign(static_cast<std::size_t>(meter.numerator / unit), unit);
    }
    for (std::size_t measure = 0; measure < score.size(); ++measure) {
        std::vector<int> boundaries{0};
        int beat = 0;
        for (const int group : grouping) {
            beat += group * 4 * divisions / meter.denominator;
            boundaries.push_back(beat);
        }
        const int pickup_shift = measure == 0 && pickup_ticks > 0
            ? boundaries.back() - pickup_ticks : 0;
        for (auto& voice : score[measure]) {
            std::vector<ScoreToken*> events;
            std::vector<int> starts;
            int cursor = 0;
            for (auto& token : voice) {
                if (token.tempo_change) continue;
                if (score[measure].size() > 1) {
                    token.stem_direction = token.voice % 2 == 1 ? 1 : -1;
                }
                if (token.chord_member) continue;
                events.push_back(&token);
                starts.push_back(cursor);
                cursor += token.duration;
            }
            for (std::size_t first = 0; first < events.size();) {
                const auto& token = *events[first];
                if (token.tuplet_actual == 0) { ++first; continue; }
                const std::size_t end = first +
                    static_cast<std::size_t>(token.tuplet_actual);
                bool complete = end <= events.size();
                for (std::size_t i = first; complete && i < end; ++i) {
                    complete = events[i]->tuplet_actual == token.tuplet_actual &&
                        events[i]->tuplet_normal == token.tuplet_normal &&
                        events[i]->duration == token.duration;
                }
                const int span = token.duration * token.tuplet_actual;
                complete = complete && starts[first] + span <= measure_lengths[measure] &&
                    (starts[first] + pickup_shift) % span == 0;
                if (complete) {
                    events[first]->tuplet_start = true;
                    events[end - 1]->tuplet_stop = true;
                    first = end;
                } else {
                    std::size_t partial_end = first + 1;
                    while (partial_end < std::min(end, events.size()) &&
                           events[partial_end]->tuplet_actual == token.tuplet_actual &&
                           events[partial_end]->tuplet_normal == token.tuplet_normal &&
                           events[partial_end]->duration == token.duration) ++partial_end;
                    events[first]->tuplet_start = true;
                    events[partial_end - 1]->tuplet_stop = true;
                    first = partial_end;
                }
            }
            for (std::size_t first = 0; first < events.size();) {
                if (beam_level(*events[first]) == 0) { ++first; continue; }
                const auto group = std::upper_bound(boundaries.begin(), boundaries.end(),
                                                     starts[first] + pickup_shift);
                const int group_end = group == boundaries.end() ? measure_lengths[measure] :
                    *group - pickup_shift;
                std::size_t end = first + 1;
                while (end < events.size() && beam_level(*events[end]) > 0 &&
                       starts[end] < group_end) ++end;
                if (end - first > 1) {
                    for (std::size_t i = first; i < end; ++i) {
                        auto& token = *events[i];
                        const int levels = beam_level(token);
                        for (int level = 1; level <= levels; ++level) {
                            const bool left = i > first && beam_level(*events[i - 1]) >= level;
                            const bool right = i + 1 < end && beam_level(*events[i + 1]) >= level;
                            token.beams.emplace_back(left ? (right ? "continue" : "end") :
                                right ? "begin" : i == first ? "forward hook" : "backward hook");
                        }
                    }
                }
                first = end;
            }
        }
    }
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
    std::ostringstream output;
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
    write_output_atomic(path, output.str());
}

std::string render_musicxml(const ScoreInterpretation& interpretation,
                            std::size_t hypothesis_index,
                            const GuitarTabOptions& tab_options,
                            bool preserve_expressive_timing,
                            const std::vector<std::string>* score_note_ids,
                            const std::vector<GuitarTabPosition>* canonical_positions = nullptr) {
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
    const int divisions = std::any_of(hypothesis.notes.begin(), hypothesis.notes.end(),
        [](const auto& note) { return note.rhythm.tuplet_actual == 7; }) ||
        std::any_of(hypothesis.rests.begin(), hypothesis.rests.end(),
        [](const auto& rest) { return rest.rhythm.tuplet_actual == 7; }) ? 6720 : 960;
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
    const auto to_tick = [divisions](double quarters) {
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
    FingeringResult fingering;
    if (!canonical_positions) fingering = assign_guitar_tab_fingerings(tab_notes, tab_options);
    const auto tab_positions = canonical_positions ? *canonical_positions : fingering.positions;
    if (canonical_positions) {
        auto constraints = tab_options.search;
        constraints.tuning = tab_options.tuning;
        constraints.string_tuning = tab_options.string_tuning;
        constraints.capo_fret = tab_options.capo_fret;
        constraints.maximum_fret = 36;
        constraints.maximum_chord_fret_span = 36;
        constraints.simultaneous_onset_tolerance_seconds = 0;
        validate_fingering_assignment(tab_notes, tab_positions, constraints);
    }
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
            int fragment_end = std::min(end_tick, measure_ends[measure]);
            if (fragment_end <= cursor) {
                throw std::logic_error("MusicXML rest could not advance to the next measure.");
            }
            int duration = fragment_end - cursor;
            auto rhythm = notation_for_quarters(
                static_cast<double>(duration) / divisions, 0.5 / divisions + 1e-9);
            if (!rhythm.symbolic) {
                for (const int denominator : {1, 2, 4, 8, 16, 32, 64, 128}) {
                    const int candidate = 4 * divisions / denominator;
                    if (candidate <= duration && candidate > 0) {
                        duration = candidate;
                        fragment_end = cursor + duration;
                        rhythm = {denominator, 0, 0, 0, true};
                        break;
                    }
                }
            }
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
                static_cast<double>(duration) / divisions, 0.5 / divisions + 1e-9);
            ScoreToken token;
            token.duration = duration;
            token.midi_pitch = interpreted.written_midi_pitch;
            token.voice = voice;
            token.tie_in = !first_fragment ||
                (first_fragment && interpreted.tie_stop);
            token.tie_out = fragment_end < end_tick ||
                (fragment_end == end_tick && interpreted.tie_start);
            token.chord_member = chord_member;
            token.include_type = rhythm.symbolic && interpreted.rhythm.symbolic;
            if (token.include_type) {
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
            for (std::size_t item = first + 1; item < end; ++item) {
                if (to_tick(hypothesis.notes[voice_notes[item]].duration_quarters) !=
                    to_tick(hypothesis.notes[voice_notes[first]].duration_quarters)) {
                    throw std::invalid_argument(
                        "MusicXML chord members with different durations require independent voices.");
                }
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
    std::vector<int> measure_lengths;
    measure_lengths.reserve(measure_starts.size());
    for (std::size_t measure = 0; measure < measure_starts.size(); ++measure) {
        measure_lengths.push_back(measure_ends[measure] - measure_starts[measure]);
    }
    group_notation(score, hypothesis.meter, divisions, measure_lengths,
                   to_tick(hypothesis.pickup_quarters));

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
                if (elapsed >= target_tick ||
                    (!insertion->chord_member && elapsed + insertion->duration > target_tick)) break;
                if (!insertion->chord_member) elapsed += insertion->duration;
            }
            ScoreToken tempo_token;
            tempo_token.tempo_change = segment.beats_per_minute;
            tempo_token.tempo_offset_ticks = target_tick - elapsed;
            tempo_token.suppress_heuristics = true;
            tokens.insert(insertion, std::move(tempo_token));
        }
    }

    const auto metadata = metadata_for_hypothesis(hypothesis);
    const bool include_key = !hypothesis.key_ambiguous || hypothesis.key.user_constrained;
    const bool include_time = !hypothesis.meter_ambiguous || hypothesis.meter.user_constrained;
    int active_fifths = include_key ? hypothesis.key.fifths : 0;
    for (std::size_t measure = 0; measure < score.size(); ++measure) {
        for (const auto& region : hypothesis.local_tonal_context) {
            if (region.first_measure == measure && region.key.confidence >= 0.5) {
                active_fifths = region.key.fifths;
            }
        }
        std::vector<std::pair<int, ScoreToken*>> attacks;
        for (auto& voice : score[measure]) {
            int cursor = 0, chord_start = 0;
            for (auto& token : voice) {
                if (token.tempo_change) continue;
                if (!token.chord_member) chord_start = cursor;
                if (token.midi_pitch >= 0 && !token.spelling_override) {
                    const auto spelling = spell_written_pitch(token.midi_pitch, active_fifths);
                    token.spelling_override = true;
                    token.spelling_step = spelling.step;
                    token.spelling_alter = spelling.alter;
                    token.spelling_octave = spelling.octave;
                }
                if (token.midi_pitch >= 0 && token.spelling_override && !token.tie_in) {
                    attacks.emplace_back(token.chord_member ? chord_start : cursor, &token);
                } else if (token.tie_in) token.accidental_required = false;
                if (!token.chord_member) cursor += token.duration;
            }
        }
        std::stable_sort(attacks.begin(), attacks.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        std::map<std::pair<std::string, int>, int> accidental_state;
        const std::string order = active_fifths < 0 ? "BEADGCF" : "FCGDAEB";
        for (const auto& [tick, token] : attacks) {
            (void)tick;
            const auto key = std::pair{token->spelling_step, token->spelling_octave};
            const auto at = order.find(token->spelling_step);
            const int signature = at < static_cast<std::size_t>(std::abs(active_fifths)) ?
                (active_fifths < 0 ? -1 : 1) : 0;
            const auto [state, inserted] = accidental_state.emplace(key, signature);
            (void)inserted;
            token->accidental_required = state->second != token->spelling_alter;
            state->second = token->spelling_alter;
        }
    }
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
            if (tablature && measure == 0) {
                output << "      <direction><direction-type><words>Suggested string/fret realization; "
                          "performed fingering unknown.</words></direction-type></direction>\n";
                if (!fingering.explanation.empty()) {
                    output << "      <direction><direction-type><words>"
                           << xml_escape(fingering.explanation)
                           << "</words></direction-type></direction>\n";
                }
                if (fingering.search_truncated) {
                    output << "      <direction><direction-type><words>"
                              "Bounded fingering search: alternatives are not exhaustive."
                              "</words></direction-type></direction>\n";
                }
            }
            if (!tablature) {
                for (const auto& region : hypothesis.local_tonal_context) {
                    if (region.first_measure == measure &&
                        region.key.confidence >= 0.5) {
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
            if (measure + 1 == score.size()) {
                output << "      <barline location=\"right\"><bar-style>light-heavy</bar-style></barline>\n";
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
    write_output_atomic(path, xml);
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
    auto interpretation = to_score_interpretation(score);
    GuitarTabOptions options;
    options.string_tuning = score.instrument.tuning;
    options.capo_fret = score.instrument.capo_fret;
    options.search.physical_fret_count = score.instrument.physical_fret_count;
    std::vector<GuitarTabPosition> canonical_positions;
    for (std::size_t index = 0; index < score.notes.size(); ++index) {
        const auto found = std::find_if(score.tab_notes.begin(), score.tab_notes.end(),
            [&](const auto& tab) {
                return tab.notation_note_id == score.notes[index].id;
            });
        if (found != score.tab_notes.end() && found->string_number && found->fret) {
            options.position_overrides.push_back(
                {index, *found->string_number, *found->fret,
                 found->manually_corrected});
            canonical_positions.push_back({index, *found->string_number, *found->fret,
                                           found->manually_corrected});
        } else {
            canonical_positions.push_back({index, 0, -1, false});
        }
    }
    std::vector<std::string> score_note_ids;
    score_note_ids.reserve(score.notes.size());
    for (const auto& note : score.notes) {
        score_note_ids.push_back(note.id);
    }
    const auto xml = render_musicxml(interpretation, 0, options,
                                     preserve_expressive_timing, &score_note_ids, &canonical_positions);
    if (rendered_musicxml != nullptr) {
        *rendered_musicxml = xml;
    }
    write_output_atomic(path, xml);
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

    std::ostringstream output;
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
    write_output_atomic(path, output.str());
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

    std::ostringstream output;
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
    write_output_atomic(path, output.str());
}

} // namespace transcribe
