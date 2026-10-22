#include "transcribe/guitar_tab.hpp"
#include "transcribe/output.hpp"
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
    std::vector<std::string> strings;
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
    if (options.alternative_fingerings > 4095) {
        throw std::invalid_argument("At most 4095 alternative fingerings can be requested.");
    }
    const auto tuning = configured_tuning(options);
    if (tuning.empty() || tuning.size() > maximum_guitar_strings) {
        throw std::invalid_argument("Guitar tablature requires between 1 and 12 strings.");
    }
    for (std::size_t i = 0; i < tuning.size(); ++i) {
        if (tuning[i] < 0 || tuning[i] > 127 ||
            (options.string_tuning.empty() && i > 0 && tuning[i] >= tuning[i - 1])) {
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

std::span<const int> configured_tuning(const GuitarTabOptions& options) noexcept {
    return options.string_tuning.empty() ? std::span<const int>(options.tuning) :
        std::span<const int>(options.string_tuning);
}

FingeringResult assign_guitar_tab_fingerings(
    const std::vector<Note>& notes, const GuitarTabOptions& options) {
    validate_options(options);
    auto fingering_options = options.search;
    fingering_options.tuning = options.tuning;
    fingering_options.string_tuning = options.string_tuning;
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
            static_cast<std::size_t>(position.string_number) > configured_tuning(requested_options).size() ||
            (position.string_number == 0 && position.fret != -1) ||
            (position.string_number > 0 &&
             (position.fret < 0 ||
              position.fret > requested_options.maximum_fret ||
              configured_tuning(requested_options)[position.string_number - 1] +
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
    auto constraints = requested_options.search;
    constraints.tuning = requested_options.tuning;
    constraints.string_tuning = requested_options.string_tuning;
    constraints.capo_fret = requested_options.capo_fret;
    constraints.maximum_fret = requested_options.maximum_fret;
    constraints.preferred_position = requested_options.preferred_position;
    validate_fingering_assignment(notes, fingering.positions, constraints);
    for (const auto& alternative : fingering.alternatives) {
        validate_fingering_assignment(notes, alternative.positions, constraints);
    }
    const auto& assigned_positions = fingering.positions;

    std::ostringstream output;

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
        event.strings.resize(configured_tuning(requested_options).size());
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

    for (std::size_t event = 1; requested_options.suggest_legato_marks && event < events.size(); ++event) {
        const double gap = events[event].start - events[event - 1].start;
        if (gap < 0.12) {
            for (std::size_t string = 0; string < configured_tuning(requested_options).size(); ++string) {
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

    std::vector<std::ostringstream> lines(configured_tuning(requested_options).size());
    for (std::size_t string = 0; string < lines.size(); ++string) {
        lines[string] << midi_note_name(configured_tuning(requested_options)[string]) << '|';
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
    for (std::size_t string = 0; string < configured_tuning(requested_options).size(); ++string) {
        output << midi_note_name(configured_tuning(requested_options)[string])
               << (string + 1 == configured_tuning(requested_options).size() ? "" : "-");
    }
    output << ")\n";
    if (requested_options.capo_fret > 0) {
        output << "Capo: fret " << requested_options.capo_fret << '\n';
    }
    output << "Suggested fingering only; performed strings and fingers are unknown. Rhythm is approximate.\n";
    if (requested_options.suggest_legato_marks) {
        output << "Legato/slide marks are heuristic suggestions, not detected techniques.\n";
    }
    if (!fingering.explanation.empty()) output << fingering.explanation << '\n';
    if (fingering.search_truncated) {
        output << "Warning: the bounded fingering search pruned states or reached its "
                  "visit limit; alternatives are not exhaustive.\n";
    }
    for (auto& line : lines) {
        output << line.str() << "|\n";
    }
    output << "\nNotes:\n";
    for (const auto index : ordered_indices) {
        const auto* note = &notes[index];
        const auto& assignment = fingering.notes[index];
        output << std::fixed << std::setprecision(2) << note->start_seconds << "s  "
               << note->name << "  " << note->duration_seconds << "s  raw pitch support "
               << std::setprecision(2) << note->confidence << "  ";
        if (!assignment.playable) {
            output << "fingering unassigned";
            if (!assignment.explanation.empty()) output << ": " << assignment.explanation;
        } else {
            output << "string " << assignment.position.string_number
                   << " fret " << assignment.position.fret;
            if (assignment.ambiguous) output << " (ambiguous)";
            if (assignment.position.manually_corrected) output << " (manual)";
            if (assignment.position.suggested_finger) {
                output << " suggested finger " << *assignment.position.suggested_finger;
            }
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
                   << ", repeated strings=" << cost.repeated_strings
                   << ", string crossings=" << cost.string_crossings
                   << ", awkward transitions=" << cost.awkward_transitions
                   << ", unassigned=" << cost.unassigned_notes << "):";
            for (const auto note_index : ordered_indices) {
                const auto& position = alternative.positions[note_index];
                output << ' ' << notes[note_index].name << '=';
                if (position.string_number < 1) {
                    output << "unresolved";
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
    write_output_atomic(path, output.str());
}

void write_guitar_tab(const std::filesystem::path& path,
                      const ScoreDocument& score,
                      const GuitarTabOptions& requested_options) {
    validate_score_document(score);
    auto options = requested_options;
    options.string_tuning = score.instrument.tuning;
    options.capo_fret = score.instrument.capo_fret;
    options.search.physical_fret_count = score.instrument.physical_fret_count;

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
