#include "transcribe/guitar_tab.hpp"
#include "transcribe/score.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace transcribe {
namespace {

struct TabEvent {
    double start{};
    double duration{};
    std::array<std::string, 6> strings;
};

void validate_options(const GuitarTabOptions& options) {
    if (options.maximum_fret < 0 || options.maximum_fret > 36) {
        throw std::invalid_argument("Maximum fret must be between 0 and 36.");
    }
    if (options.preferred_position < 0 ||
        options.preferred_position > options.maximum_fret) {
        throw std::invalid_argument("Preferred position must be within the allowed fret range.");
    }
    if (!std::isfinite(options.position_shift_cost) || options.position_shift_cost < 0.0) {
        throw std::invalid_argument("Position shift cost must be finite and non-negative.");
    }
    if (options.capo_fret < 0 || options.capo_fret > 24) {
        throw std::invalid_argument("Capo fret must be between 0 and 24.");
    }
    if (options.alternative_fingerings > 5) {
        throw std::invalid_argument("At most five alternative fingerings can be requested.");
    }
    for (std::size_t i = 0; i < options.tuning.size(); ++i) {
        if (options.tuning[i] < 0 || options.tuning[i] > 127 ||
            (i > 0 && options.tuning[i] >= options.tuning[i - 1])) {
            throw std::invalid_argument(
                "Guitar tuning must contain six descending MIDI pitches from high to low.");
        }
    }
}

std::vector<std::vector<std::size_t>> note_groups(
    const std::vector<Note>& notes, const std::vector<std::size_t>& ordered_indices) {
    std::vector<std::vector<std::size_t>> groups;
    for (std::size_t first = 0; first < ordered_indices.size();) {
        std::size_t end = first + 1;
        while (end < ordered_indices.size() &&
               notes[ordered_indices[end]].start_seconds -
                       notes[ordered_indices[first]].start_seconds <=
                   0.03) {
            ++end;
        }
        groups.emplace_back(ordered_indices.begin() + static_cast<std::ptrdiff_t>(first),
                            ordered_indices.begin() + static_cast<std::ptrdiff_t>(end));
        first = end;
    }
    return groups;
}

} // namespace

FingeringResult assign_guitar_tab_fingerings(
    const std::vector<Note>& notes, const GuitarTabOptions& options) {
    validate_options(options);
    FingeringOptions fingering_options;
    fingering_options.tuning = options.tuning;
    fingering_options.capo_fret = options.capo_fret;
    fingering_options.maximum_fret = options.maximum_fret;
    fingering_options.preferred_position = options.preferred_position;
    fingering_options.position_shift_cost = options.position_shift_cost;
    fingering_options.maximum_alternatives = options.alternative_fingerings;
    fingering_options.position_overrides = options.position_overrides;
    fingering_options.manual_corrections = options.manual_corrections;
    return assign_fingerings(notes, fingering_options);
}

std::vector<GuitarTabPosition> assign_guitar_tab_positions(
    const std::vector<Note>& notes, const GuitarTabOptions& options) {
    return assign_guitar_tab_fingerings(notes, options).positions;
}

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes) {
    write_guitar_tab(path, notes, GuitarTabOptions{});
}

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes,
                      const GuitarTabOptions& requested_options) {
    const auto fingering =
        assign_guitar_tab_fingerings(notes, requested_options);
    write_guitar_tab(path, notes, requested_options, fingering);
}

void write_guitar_tab(const std::filesystem::path& path,
                      const std::vector<Note>& notes,
                      const GuitarTabOptions& requested_options,
                      const FingeringResult& fingering) {
    validate_options(requested_options);
    if (fingering.positions.size() != notes.size() ||
        fingering.notes.size() != notes.size()) {
        throw std::invalid_argument(
            "The supplied fingering does not match the note sequence.");
    }
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto& position = fingering.positions[index];
        if (position.note_index != index || position.string_number < 0 ||
            position.string_number > 6 ||
            (position.string_number == 0 && position.fret != -1) ||
            (position.string_number > 0 &&
             (position.fret < 0 ||
              position.fret > requested_options.maximum_fret ||
              requested_options.tuning[position.string_number - 1] +
                      requested_options.capo_fret + position.fret !=
                  notes[index].midi_pitch))) {
            throw std::invalid_argument(
                "The supplied fingering contains an invalid note position.");
        }
    }
    for (const auto& alternative : fingering.alternatives) {
        if (alternative.positions.size() != notes.size()) {
            throw std::invalid_argument(
                "A supplied fingering alternative does not match the note sequence.");
        }
    }
    const auto& assigned_positions = fingering.positions;

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        throw std::runtime_error("Could not create tablature output: " + path.string());
    }

    std::vector<std::size_t> ordered_indices(notes.size());
    for (std::size_t i = 0; i < ordered_indices.size(); ++i) {
        ordered_indices[i] = i;
    }
    std::stable_sort(ordered_indices.begin(), ordered_indices.end(),
                     [&notes](std::size_t left, std::size_t right) {
                         return notes[left].start_seconds < notes[right].start_seconds;
                     });

    std::vector<TabEvent> events;
    for (const auto& group_indices : note_groups(notes, ordered_indices)) {
        TabEvent event;
        event.start = notes[group_indices.front()].start_seconds;
        for (std::size_t i = 0; i < group_indices.size(); ++i) {
            const auto& position = assigned_positions[group_indices[i]];
            event.duration =
                std::max(event.duration, notes[group_indices[i]].duration_seconds);
            if (position.string_number < 1) {
                continue;
            }
            const auto string = static_cast<std::size_t>(position.string_number - 1);
            event.strings[string] =
                std::to_string(position.fret);
        }
        events.push_back(std::move(event));
    }

    for (std::size_t event = 1; event < events.size(); ++event) {
        const double gap = events[event].start - events[event - 1].start;
        if (gap < 0.12) {
            for (std::size_t string = 0; string < requested_options.tuning.size(); ++string) {
                auto& previous = events[event - 1].strings[string];
                const auto& current = events[event].strings[string];
                if (previous.empty() || current.empty()) {
                    continue;
                }
                const int previous_fret = std::stoi(previous);
                const int current_fret = std::stoi(current);
                const int distance = std::abs(current_fret - previous_fret);
                if (distance == 0) {
                    continue;
                }
                if (distance <= 3) {
                    previous += current_fret > previous_fret ? "h" : "p";
                } else {
                    previous += current_fret > previous_fret ? "/" : "\\";
                }
            }
        }
    }

    std::array<std::ostringstream, 6> lines;
    for (std::size_t string = 0; string < lines.size(); ++string) {
        lines[string] << midi_note_name(requested_options.tuning[string]) << '|';
    }
    for (const auto& event : events) {
        int width = 4;
        for (const auto& value : event.strings) {
            width = std::max(width, static_cast<int>(value.size()) + 2);
        }
        if (requested_options.duration_proportional_ascii) {
            width = std::clamp(static_cast<int>(std::lround(event.duration * 8.0)),
                               width, 24);
        }
        for (std::size_t string = 0; string < lines.size(); ++string) {
            const auto& value = event.strings[string];
            if (value.empty()) {
                lines[string] << std::string(static_cast<std::size_t>(width), '-');
                continue;
            }
            const int padding = std::max(0, width - static_cast<int>(value.size()));
            const int left = padding / 2;
            lines[string] << std::string(static_cast<std::size_t>(left), '-') << value
                          << std::string(static_cast<std::size_t>(padding - left), '-');
        }
    }

    output << "Estimated guitar tablature (high-to-low tuning: ";
    for (std::size_t string = requested_options.tuning.size(); string > 0; --string) {
        output << midi_note_name(requested_options.tuning[string - 1])
               << (string == 1 ? "" : "-");
    }
    output << ")\n";
    if (requested_options.capo_fret > 0) {
        output << "Capo: fret " << requested_options.capo_fret << '\n';
    }
    output << "Fingering, legato marks, and rhythm are approximate.\n";
    if (fingering.search_truncated) {
        output << "Warning: the bounded fingering search reached its per-chord "
                  "visit limit; review the assignments.\n";
    }
    for (auto& line : lines) {
        output << line.str() << "|\n";
    }
    output << "\nNotes:\n";
    for (const auto index : ordered_indices) {
        const auto* note = &notes[index];
        const auto& assignment = fingering.notes[index];
        output << std::fixed << std::setprecision(2) << note->start_seconds << "s  "
               << note->name << "  " << note->duration_seconds << "s  confidence "
               << std::setprecision(2) << note->confidence << "  ";
        if (!assignment.playable) {
            output << "fingering unassigned";
        } else {
            output << "string " << assignment.position.string_number
                   << " fret " << assignment.position.fret;
            if (assignment.ambiguous) output << " (ambiguous)";
            if (assignment.position.manually_corrected) output << " (manual)";
        }
        if (assignment.correction_rejected) {
            output << " (saved correction rejected)";
        }
        output << '\n';
    }
    if (requested_options.alternative_fingerings > 0) {
        output << "\nAlternative fingerings:\n";
        for (std::size_t index = 0; index < fingering.alternatives.size(); ++index) {
            const auto& alternative = fingering.alternatives[index];
            const auto& cost = alternative.cost;
            output << std::fixed << std::setprecision(2)
                   << "Option " << index + 1 << " cost=" << cost.total()
                   << " (fret=" << cost.fret_height
                   << ", preferred=" << cost.preferred_position
                   << ", hand movement=" << cost.hand_movement
                   << ", string preference=" << cost.string_preference
                   << ", chord stretch=" << cost.chord_stretch
                   << ", unassigned=" << cost.unassigned_notes << "):";
            for (const auto note_index : ordered_indices) {
                const auto& position = alternative.positions[note_index];
                output << ' ' << notes[note_index].name << '=';
                if (position.string_number < 1) {
                    output << "unplayable";
                } else {
                    output << "string " << position.string_number << " fret "
                           << position.fret;
                }
            }
            output << '\n';
        }
    }
    if (!output) {
        throw std::runtime_error("Failed while writing tablature output.");
    }
}

void write_guitar_tab(const std::filesystem::path& path,
                      const ScoreDocument& score,
                      const GuitarTabOptions& requested_options) {
    validate_score_document(score);
    if (score.instrument.tuning.size() != requested_options.tuning.size()) {
        throw std::invalid_argument(
            "ASCII tablature export requires exactly six configured guitar strings.");
    }

    auto options = requested_options;
    std::copy(score.instrument.tuning.begin(), score.instrument.tuning.end(),
              options.tuning.begin());
    options.capo_fret = score.instrument.capo_fret;

    auto notes = score_notes(score);
    std::vector<const ScoreNote*> ordered_score_notes;
    ordered_score_notes.reserve(score.notes.size());
    for (const auto& note : score.notes) ordered_score_notes.push_back(&note);
    std::stable_sort(ordered_score_notes.begin(), ordered_score_notes.end(),
        [](const auto* left, const auto* right) {
            if (left->onset_quarters != right->onset_quarters) {
                return left->onset_quarters < right->onset_quarters;
            }
            if (left->sounding_midi_pitch != right->sounding_midi_pitch) {
                return left->sounding_midi_pitch < right->sounding_midi_pitch;
            }
            return left->id < right->id;
        });

    std::unordered_map<std::string, const ScoreTabNote*> tab_by_note;
    tab_by_note.reserve(score.tab_notes.size());
    for (const auto& tab : score.tab_notes) {
        tab_by_note.emplace(tab.notation_note_id, &tab);
    }

    FingeringResult fingering;
    fingering.positions.reserve(notes.size());
    fingering.notes.reserve(notes.size());
    fingering.fully_playable = true;
    for (std::size_t index = 0; index < ordered_score_notes.size(); ++index) {
        const auto* note = ordered_score_notes[index];
        const auto found = tab_by_note.find(note->id);
        GuitarTabPosition position{index, 0, -1, false};
        if (found != tab_by_note.end() && found->second->string_number &&
            found->second->fret) {
            position.string_number = *found->second->string_number;
            position.fret = *found->second->fret;
            position.manually_corrected = found->second->manually_corrected;
        }
        FingeringNoteResult note_result;
        note_result.position = position;
        note_result.playable = position.string_number > 0;
        fingering.fully_playable &= note_result.playable;
        fingering.positions.push_back(position);
        fingering.notes.push_back(std::move(note_result));
    }
    write_guitar_tab(path, notes, options, fingering);
}

} // namespace transcribe
