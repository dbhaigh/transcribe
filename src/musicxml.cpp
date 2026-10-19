#include "transcribe/musicxml.hpp"

#include <algorithm>
#include <array>
#include <fstream>
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
    const int octave = midi / 12 - 1;
    Spelling result = spellings[midi % 12];
    result.octave = octave;
    return result;
}

struct RhythmValue {
    int ticks;
    const char* type;
    bool dotted;
};

constexpr std::array<RhythmValue, 8> rhythm_values{{
    {16, "whole", false}, {12, "half", true},
    {8, "half", false}, {6, "quarter", true}, {4, "quarter", false},
    {3, "eighth", true}, {2, "eighth", false}, {1, "16th", false}}};

std::vector<int> decompose_duration(int ticks) {
    std::vector<int> chunks;
    while (ticks > 0) {
        const auto value = std::find_if(rhythm_values.begin(), rhythm_values.end(),
                                        [ticks](const RhythmValue& candidate) {
                                            return candidate.ticks <= ticks;
                                        });
        chunks.push_back(value->ticks);
        ticks -= value->ticks;
    }
    return chunks;
}

void write_measure_start(std::ostream& output, int measure_number, bool first) {
    output << "    <measure number=\"" << measure_number << "\">\n";
    if (first) {
        output << "      <attributes><divisions>4</divisions><key><fifths>0</fifths></key>"
                  "<time><beats>4</beats><beat-type>4</beat-type></time>"
                  "<clef><sign>G</sign><line>2</line></clef>"
                  "<staff-details><staff-lines>5</staff-lines></staff-details></attributes>\n"
                  "      <direction><direction-type><words>Pitch detected; rhythm estimated</words>"
                  "</direction-type><sound tempo=\"120\"/></direction>\n";
    }
}

void write_rhythmic_note(std::ostream& output, int duration, int midi_pitch,
                         bool tied_from_previous, bool tied_to_next) {
    const auto value = std::find_if(rhythm_values.begin(), rhythm_values.end(),
                                    [duration](const RhythmValue& candidate) {
                                        return candidate.ticks == duration;
                                    });
    output << "      <note>";
    if (midi_pitch < 0) {
        output << "<rest/>";
    } else {
        const auto spelling = spell_pitch(midi_pitch);
        output << "<pitch><step>" << spelling.step << "</step>";
        if (spelling.alter != 0) {
            output << "<alter>" << spelling.alter << "</alter>";
        }
        output << "<octave>" << spelling.octave << "</octave></pitch>";
    }
    output << "<duration>" << duration << "</duration>";
    if (tied_from_previous) {
        output << "<tie type=\"stop\"/>";
    }
    if (tied_to_next) {
        output << "<tie type=\"start\"/>";
    }
    output << "<voice>1</voice><type>" << value->type << "</type>";
    if (value->dotted) {
        output << "<dot/>";
    }
    if (midi_pitch >= 0) {
        int string_number = 0;
        int fret = 0;
        static constexpr int open_strings[] = {40, 45, 50, 55, 59, 64};
        for (int index = 5; index >= 0; --index) {
            const int candidate_fret = midi_pitch - open_strings[index];
            if (candidate_fret >= 0 && candidate_fret <= 24) {
                string_number = 6 - index;
                fret = candidate_fret;
                break;
            }
        }
        if (tied_from_previous || tied_to_next || string_number != 0) {
            output << "<notations>";
            if (tied_from_previous) {
                output << "<tied type=\"stop\"/>";
            }
            if (tied_to_next) {
                output << "<tied type=\"start\"/>";
            }
            if (string_number != 0) {
                output << "<technical><string>" << string_number << "</string><fret>"
                       << fret << "</fret></technical>";
            }
            output << "</notations>";
        }
    }
    output << "</note>\n";
}

} // namespace

void write_musicxml(const std::filesystem::path& path, const std::vector<Note>& notes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create MusicXML output: " + path.string());
    }

    output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<!DOCTYPE score-partwise PUBLIC \"-//Recordare//DTD MusicXML 4.0 Partwise//EN\" "
              "\"http://www.musicxml.org/dtds/partwise.dtd\">\n"
              "<score-partwise version=\"4.0\">\n"
              "  <work><work-title>Transcription</work-title></work>\n"
              "  <part-list><score-part id=\"P1\"><part-name>Guitar</part-name>"
              "</score-part></part-list>\n"
              "  <part id=\"P1\">\n";

    constexpr int ticks_per_measure = 16;
    int cursor_ticks = 0;
    int measure = 1;
    write_measure_start(output, measure, true);
    for (const auto& note : notes) {
        const int requested_start = std::max(0, static_cast<int>(note.start_seconds * 8.0 + 0.5));
        const int start_ticks = std::max(cursor_ticks, requested_start);
        if (start_ticks > cursor_ticks) {
            int rest_remaining = start_ticks - cursor_ticks;
            while (rest_remaining > 0) {
                if (cursor_ticks > 0 && cursor_ticks % ticks_per_measure == 0) {
                    output << "    </measure>\n";
                    write_measure_start(output, ++measure, false);
                }
                const int in_measure = cursor_ticks % ticks_per_measure;
                const int segment = std::min(rest_remaining, ticks_per_measure - in_measure);
                for (const int chunk : decompose_duration(segment)) {
                    write_rhythmic_note(output, chunk, -1, false, false);
                    cursor_ticks += chunk;
                    rest_remaining -= chunk;
                }
            }
        }

        const int duration = std::max(1, static_cast<int>(note.duration_seconds * 8.0 + 0.5));
        int remaining = duration;
        int measure_space = ticks_per_measure - cursor_ticks % ticks_per_measure;
        std::vector<int> chunks;
        while (remaining > 0) {
            const int segment = std::min(remaining, measure_space);
            auto segment_chunks = decompose_duration(segment);
            chunks.insert(chunks.end(), segment_chunks.begin(), segment_chunks.end());
            remaining -= segment;
            measure_space = ticks_per_measure;
        }
        for (std::size_t index = 0; index < chunks.size(); ++index) {
            if (cursor_ticks > 0 && cursor_ticks % ticks_per_measure == 0) {
                output << "    </measure>\n";
                write_measure_start(output, ++measure, false);
            }
            write_rhythmic_note(output, chunks[index], note.midi_pitch, index > 0,
                                index + 1 < chunks.size());
            cursor_ticks += chunks[index];
        }
    }
    output << "    </measure>\n  </part>\n</score-partwise>\n";
    if (!output) {
        throw std::runtime_error("Failed while writing MusicXML output.");
    }
}

} // namespace transcribe
