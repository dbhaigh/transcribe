#include "transcribe/fingering.hpp"

#include <algorithm>
#include <cmath>
#include <charconv>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace transcribe {
namespace {

struct CandidatePosition {
    int string_index{};
    int fret{};
    bool manually_corrected{};
};

struct Group {
    std::vector<std::size_t> note_indices;
};

struct PathNode {
    std::shared_ptr<const PathNode> parent;
    std::vector<GuitarTabPosition> group_positions;
};

struct State {
    std::array<double, maximum_guitar_strings> occupied_until{};
    double hand_position{};
    FingeringCostComponents cost;
    std::shared_ptr<const PathNode> path;
    std::array<int, maximum_guitar_strings> held_frets{};
    std::array<int, maximum_guitar_strings> last_frets{};
    std::array<bool, maximum_guitar_strings> previous_strings{};
    double previous_onset{-1.0};
    std::size_t unresolved{};
};

struct LocalAssignment {
    std::vector<CandidatePosition> positions;
    FingeringCostComponents cost;
};

double position_cost(const FingeringCostComponents& cost) {
    return cost.total();
}

bool overlaps(const Note& left, const Note& right) {
    return left.start_seconds < right.start_seconds + right.duration_seconds &&
           right.start_seconds < left.start_seconds + left.duration_seconds;
}

std::vector<Group> build_groups(const std::vector<Note>& notes,
                                double onset_tolerance) {
    std::vector<std::size_t> ordered(notes.size());
    for (std::size_t index = 0; index < ordered.size(); ++index) {
        ordered[index] = index;
    }
    std::stable_sort(ordered.begin(), ordered.end(),
                     [&notes](std::size_t left, std::size_t right) {
                         return notes[left].start_seconds < notes[right].start_seconds;
                     });

    std::vector<Group> groups;
    for (std::size_t first = 0; first < ordered.size();) {
        std::size_t end = first + 1;
        while (end < ordered.size() &&
               notes[ordered[end]].start_seconds -
                       notes[ordered[first]].start_seconds <=
                   onset_tolerance) {
            ++end;
        }
        groups.push_back({std::vector<std::size_t>(
            ordered.begin() + static_cast<std::ptrdiff_t>(first),
            ordered.begin() + static_cast<std::ptrdiff_t>(end))});
        first = end;
    }
    return groups;
}

void validate_options(const FingeringOptions& options) {
    if (options.maximum_fret < 0 || options.maximum_fret > 36) {
        throw std::invalid_argument("Maximum fret must be between 0 and 36.");
    }
    if (options.preferred_position < 0 ||
        options.preferred_position > options.maximum_fret) {
        throw std::invalid_argument(
            "Preferred position must be within the allowed fret range.");
    }
    if (!std::isfinite(options.position_shift_cost) ||
        options.position_shift_cost < 0.0) {
        throw std::invalid_argument(
            "Position shift cost must be finite and non-negative.");
    }
    for (const double cost : {options.fret_height_cost, options.preferred_position_cost,
             options.string_preference_cost, options.stretch_cost, options.repeated_string_cost,
             options.string_crossing_cost, options.awkward_transition_cost, options.unresolved_note_cost}) {
        if (!std::isfinite(cost) || cost < 0.0) {
            throw std::invalid_argument("Fingering costs must be finite and non-negative.");
        }
    }
    if (options.beam_width == 0 || options.beam_width > 4096 ||
        options.local_solution_limit == 0 || options.local_solution_limit > 4096 ||
        options.search_node_limit == 0 || options.search_node_limit > 10000000 ||
        options.maximum_fretting_fingers < 1 || options.maximum_fretting_fingers > 4) {
        throw std::invalid_argument("Fingering budgets require beam/local 1-4096, nodes 1-10000000 and fingers 1-4.");
    }
    if (!std::isfinite(options.simultaneous_onset_tolerance_seconds) ||
        options.simultaneous_onset_tolerance_seconds < 0.0 ||
        options.simultaneous_onset_tolerance_seconds > 0.5) {
        throw std::invalid_argument(
            "Simultaneous onset tolerance must be between 0 and 0.5 seconds.");
    }
    if (!std::isfinite(options.correction_match_tolerance_seconds) ||
        options.correction_match_tolerance_seconds < 0.0 ||
        options.correction_match_tolerance_seconds > 5.0) {
        throw std::invalid_argument(
            "Correction match tolerance must be between 0 and 5 seconds.");
    }
    if (!std::isfinite(options.ambiguity_cost_margin) ||
        options.ambiguity_cost_margin < 0.0) {
        throw std::invalid_argument(
            "Ambiguity cost margin must be finite and non-negative.");
    }
    if (options.maximum_chord_fret_span < 0 ||
        options.maximum_chord_fret_span > 36) {
        throw std::invalid_argument(
            "Maximum chord fret span must be between 0 and 36.");
    }
    if (options.maximum_alternatives > 4095) {
        throw std::invalid_argument(
            "At most 4095 alternative fingerings can be requested.");
    }
    if (options.capo_fret < 0 || options.capo_fret > 24) {
        throw std::invalid_argument("Capo fret must be between 0 and 24.");
    }
    const auto tuning = configured_tuning(options);
    if (tuning.empty() || tuning.size() > maximum_guitar_strings ||
        options.minimum_midi_pitch < 0 || options.maximum_midi_pitch > 127 ||
        options.minimum_midi_pitch > options.maximum_midi_pitch ||
        (options.physical_fret_count &&
         (*options.physical_fret_count < options.capo_fret ||
          *options.physical_fret_count > 36))) {
        throw std::invalid_argument("Guitar requires 1-12 strings, MIDI range 0-127 and physical fret count between capo and 36.");
    }
    for (std::size_t index = 0; index < tuning.size(); ++index) {
        if (tuning[index] < 0 || tuning[index] > 127 ||
            (options.string_tuning.empty() && index > 0 && tuning[index] >= tuning[index - 1])) {
            throw std::invalid_argument(
                "Guitar tuning requires valid MIDI pitches; legacy six-string tuning must descend.");
        }
    }
}

std::vector<CandidatePosition> candidates_for(int midi_pitch,
                                              const FingeringOptions& options) {
    std::vector<CandidatePosition> candidates;
    if (midi_pitch < options.minimum_midi_pitch || midi_pitch > options.maximum_midi_pitch) return candidates;
    const auto tuning = configured_tuning(options);
    for (std::size_t string = 0; string < tuning.size(); ++string) {
        const int fret = midi_pitch - tuning[string] - options.capo_fret;
        if (fret >= 0 && fret <= options.maximum_fret &&
            (!options.physical_fret_count || fret + options.capo_fret <= *options.physical_fret_count)) {
            candidates.push_back(
                {static_cast<int>(string), fret, false});
        }
    }
    return candidates;
}

FingeringCostComponents add_costs(FingeringCostComponents left,
                                  const FingeringCostComponents& right) {
    left.fret_height += right.fret_height;
    left.preferred_position += right.preferred_position;
    left.hand_movement += right.hand_movement;
    left.string_preference += right.string_preference;
    left.chord_stretch += right.chord_stretch;
    left.unassigned_notes += right.unassigned_notes;
    left.repeated_strings += right.repeated_strings;
    left.string_crossings += right.string_crossings;
    left.awkward_transitions += right.awkward_transitions;
    return left;
}

FingeringCostComponents position_cost(const CandidatePosition& position,
                                      const FingeringOptions& options) {
    FingeringCostComponents cost;
    cost.fret_height = options.fret_height_cost * position.fret;
    cost.preferred_position =
        position.fret > 0 ? options.preferred_position_cost *
            std::abs(position.fret - options.preferred_position) : 0.0;
    cost.string_preference = options.string_preference_cost * position.string_index;
    return cost;
}

// Equal-fret strings may share a barre unless an open/lower fret blocks it.
std::optional<std::array<int, maximum_guitar_strings>> fingers_for(const std::array<int, maximum_guitar_strings>& frets,
                                             int maximum_fingers) {
    std::array<int, maximum_guitar_strings> fingers{};
    std::vector<int> levels;
    for (int fret : frets) if (fret > 0) levels.push_back(fret);
    std::sort(levels.begin(), levels.end());
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    int finger = 0;
    for (int fret : levels) {
        bool barre = false;
        for (std::size_t string = 0; string < frets.size(); ++string) {
            if (frets[string] >= 0 && frets[string] < fret) barre = false;
            if (frets[string] == fret) {
                if (!barre) ++finger;
                if (finger > maximum_fingers) return std::nullopt;
                fingers[string] = finger;
                barre = true;
            }
        }
    }
    return fingers;
}

std::vector<LocalAssignment> local_assignments(
    const Group& group, const std::vector<Note>& notes, const State& state,
    const std::vector<std::optional<CandidatePosition>>& fixed_positions,
    const std::vector<std::vector<CandidatePosition>>& candidates,
    const FingeringOptions& options, bool& search_truncated, std::size_t& total_visited) {
    std::vector<LocalAssignment> solutions;
    if (group.note_indices.size() > 64) {
        search_truncated = true;
        LocalAssignment unresolved;
        unresolved.positions.assign(group.note_indices.size(), {-1, -1, false});
        unresolved.cost.unassigned_notes = options.unresolved_note_cost * group.note_indices.size();
        return {std::move(unresolved)};
    }
    std::vector<CandidatePosition> current(group.note_indices.size(),
                                            {-1, -1, false});
    std::array<bool, maximum_guitar_strings> occupied{};
    std::vector<std::size_t> search_order(group.note_indices.size());
    for (std::size_t index = 0; index < search_order.size(); ++index) {
        search_order[index] = index;
    }
    std::stable_sort(search_order.begin(), search_order.end(),
                     [&group, &fixed_positions, &candidates](
                         std::size_t left, std::size_t right) {
                         const auto left_note = group.note_indices[left];
                         const auto right_note = group.note_indices[right];
                         const auto left_count = fixed_positions[left_note]
                             ? std::size_t{1} : candidates[left_note].size();
                         const auto right_count = fixed_positions[right_note]
                             ? std::size_t{1} : candidates[right_note].size();
                         return left_count < right_count;
                     });
    std::size_t visited = 0;

    const auto keep_solution = [&](LocalAssignment solution) {
        const auto place = std::upper_bound(
            solutions.begin(), solutions.end(), solution,
            [](const LocalAssignment& left, const LocalAssignment& right) {
                const auto missing = [](const auto& value) {
                    return std::count_if(value.positions.begin(), value.positions.end(),
                        [](const auto& p) { return p.string_index < 0; });
                };
                if (missing(left) != missing(right)) return missing(left) < missing(right);
                return position_cost(left.cost) < position_cost(right.cost);
            });
        solutions.insert(place, std::move(solution));
        if (solutions.size() > options.local_solution_limit) {
            solutions.pop_back();
            search_truncated = true;
        }
    };

    const auto search = [&](auto&& self, std::size_t index,
                            FingeringCostComponents cost) -> void {
        if (visited >= options.search_node_limit) {
            search_truncated = true;
            return;
        }
        ++visited;
        ++total_visited;
        if (index == group.note_indices.size()) {
            int minimum_fret = options.maximum_fret;
            int maximum_fret = 0;
            bool has_fretted_note = false;
            std::array<int, maximum_guitar_strings> sounding_frets;
            sounding_frets.fill(-1);
            const double onset = notes[group.note_indices.front()].start_seconds;
            for (std::size_t string = 0; string < sounding_frets.size(); ++string) {
                if (state.occupied_until[string] > onset) sounding_frets[string] = state.held_frets[string];
            }
            for (const auto& position : current) {
                if (position.string_index < 0) {
                    continue;
                }
                sounding_frets[static_cast<std::size_t>(position.string_index)] = position.fret;
            }
            for (int fret : sounding_frets) {
                if (fret <= 0) continue;
                minimum_fret = std::min(minimum_fret, fret);
                maximum_fret = std::max(maximum_fret, fret);
                has_fretted_note = true;
            }
            const int span =
                has_fretted_note ? maximum_fret - minimum_fret : 0;
            if (span > options.maximum_chord_fret_span ||
                !fingers_for(sounding_frets, options.maximum_fretting_fingers)) {
                return;
            }
            cost.chord_stretch += options.stretch_cost * std::max(0, span - 4);
            keep_solution({current, cost});
            return;
        }

        const auto group_slot = search_order[index];
        const auto note_index = group.note_indices[group_slot];
        const auto& note = notes[note_index];
        std::vector<CandidatePosition> choices;
        if (fixed_positions[note_index]) {
            choices.push_back(*fixed_positions[note_index]);
        } else {
            choices = candidates[note_index];
        }
        std::stable_sort(choices.begin(), choices.end(),
                         [&options](const CandidatePosition& left,
                                    const CandidatePosition& right) {
                             return position_cost(position_cost(left, options)) <
                                    position_cost(position_cost(right, options));
                         });
        for (const auto& candidate : choices) {
            if (visited >= options.search_node_limit) {
                search_truncated = true;
                break;
            }
            const auto string = static_cast<std::size_t>(candidate.string_index);
            if (occupied[string] ||
                state.occupied_until[string] > note.start_seconds) {
                continue;
            }
            occupied[string] = true;
            current[group_slot] = candidate;
            self(self, index + 1,
                 add_costs(cost, position_cost(candidate, options)));
            occupied[string] = false;
        }

        if (!fixed_positions[note_index] && visited < options.search_node_limit) {
            current[group_slot] = {-1, -1, false};
            auto unassigned_cost = cost;
            unassigned_cost.unassigned_notes += options.unresolved_note_cost;
            self(self, index + 1, unassigned_cost);
        } else if (!fixed_positions[note_index] &&
                   visited >= options.search_node_limit) {
            search_truncated = true;
        }
    };

    search(search, 0, {});
    if (solutions.empty()) {
        const bool has_fixed_position = std::any_of(
            group.note_indices.begin(), group.note_indices.end(),
            [&fixed_positions](std::size_t note_index) {
                return fixed_positions[note_index].has_value();
            });
        if (has_fixed_position) {
            return {};
        }
        LocalAssignment all_unassigned;
        all_unassigned.positions.assign(group.note_indices.size(),
                                         {-1, -1, false});
        all_unassigned.cost.unassigned_notes =
            options.unresolved_note_cost * static_cast<double>(group.note_indices.size());
        solutions.push_back(std::move(all_unassigned));
    }
    return solutions;
}

std::vector<GuitarTabPosition> positions_for_path(
    const std::shared_ptr<const PathNode>& path, std::size_t note_count) {
    std::vector<GuitarTabPosition> positions(note_count);
    for (std::size_t index = 0; index < positions.size(); ++index) {
        positions[index].note_index = index;
        positions[index].string_number = 0;
        positions[index].fret = -1;
    }
    for (auto node = path; node; node = node->parent) {
        for (const auto& position : node->group_positions) {
            positions[position.note_index] = position;
        }
    }
    return positions;
}

} // namespace

std::span<const int> configured_tuning(const FingeringOptions& options) noexcept {
    return options.string_tuning.empty() ? std::span<const int>(options.tuning) :
        std::span<const int>(options.string_tuning);
}

std::vector<int> parse_string_tuning(std::string_view midi_pitches) {
    std::vector<int> tuning;
    while (!midi_pitches.empty()) {
        const auto comma = midi_pitches.find(',');
        const auto token = midi_pitches.substr(0, comma);
        int pitch{};
        const auto parsed = std::from_chars(token.data(), token.data() + token.size(), pitch);
        if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
            pitch < 0 || pitch > 127 || tuning.size() == maximum_guitar_strings) {
            throw std::invalid_argument("String tuning requires 1-12 comma-separated MIDI integers (0-127), string 1 first.");
        }
        tuning.push_back(pitch);
        if (comma == std::string_view::npos) break;
        midi_pitches.remove_prefix(comma + 1);
        if (midi_pitches.empty()) throw std::invalid_argument("String tuning cannot end with a comma.");
    }
    if (tuning.empty()) throw std::invalid_argument("String tuning cannot be empty.");
    return tuning;
}

double FingeringCostComponents::total() const {
    return fret_height + preferred_position + hand_movement +
           string_preference + chord_stretch + unassigned_notes +
           repeated_strings + string_crossings + awkward_transitions;
}

void validate_fingering_assignment(const std::vector<Note>& notes,
                                   const std::vector<GuitarTabPosition>& positions,
                                   const FingeringOptions& options) {
    validate_options(options);
    if (notes.size() != positions.size()) {
        throw std::invalid_argument("Fingering assignment does not match the note count.");
    }
    for (const auto& note : notes) {
        if (!std::isfinite(note.start_seconds) || note.start_seconds < 0 ||
            !std::isfinite(note.duration_seconds) || note.duration_seconds <= 0 ||
            !std::isfinite(note.start_seconds + note.duration_seconds) ||
            note.midi_pitch < 0 || note.midi_pitch > 127) {
            throw std::invalid_argument("Fingering contains invalid note pitch or timing.");
        }
    }
    std::array<double, maximum_guitar_strings> until{};
    std::array<int, maximum_guitar_strings> frets;
    frets.fill(-1);
    for (const auto& group : build_groups(notes, options.simultaneous_onset_tolerance_seconds)) {
        const double onset = notes[group.note_indices.front()].start_seconds;
        std::array<bool, maximum_guitar_strings> used{};
        for (std::size_t string = 0; string < maximum_guitar_strings; ++string) {
            if (until[string] <= onset + 1e-9) frets[string] = -1;
        }
        for (auto index : group.note_indices) {
            const auto& position = positions[index];
            const auto& note = notes[index];
            if (position.note_index != index || position.string_number < 0 ||
                static_cast<std::size_t>(position.string_number) > configured_tuning(options).size() ||
                (position.string_number == 0 && position.fret != -1)) {
                throw std::invalid_argument("Fingering contains invalid note timing or position.");
            }
            if (position.string_number == 0) continue;
            const auto string = static_cast<std::size_t>(position.string_number - 1);
            if (position.fret < 0 || position.fret > options.maximum_fret ||
                configured_tuning(options)[string] + options.capo_fret + position.fret != note.midi_pitch ||
                note.midi_pitch < options.minimum_midi_pitch || note.midi_pitch > options.maximum_midi_pitch ||
                (options.physical_fret_count && position.fret + options.capo_fret > *options.physical_fret_count)) {
                throw std::invalid_argument("Fingering string/fret does not preserve pitch or exceeds fret limits.");
            }
            if (used[string] || until[string] > note.start_seconds + 1e-9) {
                throw std::invalid_argument("Fingering assigns overlapping or simultaneous notes to one string.");
            }
            used[string] = true;
            until[string] = note.start_seconds + note.duration_seconds;
            frets[string] = position.fret;
        }
        int low = options.maximum_fret, high = 0;
        for (int fret : frets) if (fret > 0) {
            low = std::min(low, fret);
            high = std::max(high, fret);
        }
        if (high - low > options.maximum_chord_fret_span ||
            !fingers_for(frets, options.maximum_fretting_fingers)) {
            throw std::invalid_argument("Fingering exceeds simultaneous stretch or fretting-finger limits.");
        }
    }
}

FingeringResult assign_fingerings(const std::vector<Note>& notes,
                                  const FingeringOptions& options) {
    validate_options(options);
    for (const auto& note : notes) {
        if (note.midi_pitch < 0 || note.midi_pitch > 127 ||
            !std::isfinite(note.start_seconds) || note.start_seconds < 0.0 ||
            !std::isfinite(note.duration_seconds) ||
            note.duration_seconds <= 0.0 ||
            !std::isfinite(note.start_seconds + note.duration_seconds)) {
            throw std::invalid_argument(
                "Fingering notes require a valid MIDI pitch, onset, and duration.");
        }
    }

    FingeringResult result;
    result.positions.resize(notes.size());
    result.notes.resize(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        result.positions[index] = {index, 0, -1, false};
        result.notes[index].position = result.positions[index];
    }
    if (notes.empty()) {
        result.fully_playable = true;
        result.alternatives_complete = true;
        return result;
    }

    std::vector<std::optional<CandidatePosition>> fixed_positions(notes.size());
    std::vector<std::vector<CandidatePosition>> candidates(notes.size());
    for (std::size_t index = 0; index < notes.size(); ++index) {
        candidates[index] = candidates_for(notes[index].midi_pitch, options);
        for (const auto& candidate : candidates[index]) {
            result.notes[index].playable_positions.push_back(
                {index, candidate.string_index + 1, candidate.fret, false});
        }
    }
    std::vector<std::optional<std::size_t>> correction_for_note(notes.size());
    std::vector<bool> rejected_correction(options.manual_corrections.size(), false);

    for (const auto& position : options.position_overrides) {
        if (position.note_index >= notes.size() ||
            fixed_positions[position.note_index] ||
            position.string_number < 1 ||
            static_cast<std::size_t>(position.string_number) > configured_tuning(options).size() ||
            position.fret < 0 || position.fret > options.maximum_fret) {
            throw std::invalid_argument("A guitar tab position override is invalid.");
        }
        const auto string = static_cast<std::size_t>(position.string_number - 1);
        if (notes[position.note_index].midi_pitch !=
            configured_tuning(options)[string] + options.capo_fret + position.fret ||
            candidates[position.note_index].end() == std::find_if(
                candidates[position.note_index].begin(), candidates[position.note_index].end(),
                [&](const auto& candidate) { return candidate.string_index == static_cast<int>(string) && candidate.fret == position.fret; })) {
            throw std::invalid_argument(
                "A guitar tab position override does not match its note pitch.");
        }
        fixed_positions[position.note_index] =
            CandidatePosition{static_cast<int>(string), position.fret, true};
    }

    std::vector<bool> correction_note_rejected(notes.size(), false);
    for (std::size_t correction_index = 0;
         correction_index < options.manual_corrections.size();
         ++correction_index) {
        const auto& correction = options.manual_corrections[correction_index];
        if (correction.midi_pitch < 0 || correction.midi_pitch > 127 ||
            !std::isfinite(correction.onset_seconds) ||
            correction.onset_seconds < 0.0 ||
            !std::isfinite(correction.duration_seconds) ||
            correction.duration_seconds <= 0.0 ||
            correction.string_number < 1 ||
            static_cast<std::size_t>(correction.string_number) > configured_tuning(options).size() ||
            correction.fret < 0 || correction.fret > options.maximum_fret) {
            throw std::invalid_argument("A saved fingering correction is invalid.");
        }

        std::optional<std::size_t> best_note;
        double best_distance = options.correction_match_tolerance_seconds;
        bool tied = false;
        for (std::size_t note_index = 0; note_index < notes.size(); ++note_index) {
            if (notes[note_index].midi_pitch != correction.midi_pitch ||
                correction_for_note[note_index]) {
                continue;
            }
            const double distance =
                std::abs(notes[note_index].start_seconds -
                         correction.onset_seconds);
            if (distance > best_distance) {
                continue;
            }
            if (best_note && std::abs(distance - best_distance) <= 1e-9) {
                tied = true;
                continue;
            }
            best_distance = distance;
            best_note = note_index;
            tied = false;
        }
        if (!best_note || tied) {
            rejected_correction[correction_index] = true;
            continue;
        }
        const auto string =
            static_cast<std::size_t>(correction.string_number - 1);
        if (notes[*best_note].midi_pitch !=
            configured_tuning(options)[string] + options.capo_fret + correction.fret ||
            candidates[*best_note].end() == std::find_if(candidates[*best_note].begin(),
                candidates[*best_note].end(), [&](const auto& candidate) {
                    return candidate.string_index == static_cast<int>(string) && candidate.fret == correction.fret;
                })) {
            rejected_correction[correction_index] = true;
            correction_note_rejected[*best_note] = true;
            continue;
        }
        if (fixed_positions[*best_note]) {
            rejected_correction[correction_index] = true;
            correction_note_rejected[*best_note] = true;
            continue;
        }
        fixed_positions[*best_note] =
            CandidatePosition{static_cast<int>(string), correction.fret, true};
        correction_for_note[*best_note] = correction_index;
    }

    for (std::size_t left = 0; left < notes.size(); ++left) {
        if (!fixed_positions[left]) {
            continue;
        }
        for (std::size_t right = left + 1; right < notes.size(); ++right) {
            if (!fixed_positions[right] ||
                fixed_positions[left]->string_index !=
                    fixed_positions[right]->string_index ||
                !overlaps(notes[left], notes[right])) {
                continue;
            }
            const auto left_correction = correction_for_note[left];
            const auto right_correction = correction_for_note[right];
            if (!left_correction && !right_correction) {
                throw std::invalid_argument(
                    "Overlapping notes cannot use the same overridden guitar string.");
            }
            if (left_correction) {
                rejected_correction[*left_correction] = true;
                correction_note_rejected[left] = true;
                correction_for_note[left].reset();
                fixed_positions[left].reset();
            }
            if (right_correction) {
                rejected_correction[*right_correction] = true;
                correction_note_rejected[right] = true;
                correction_for_note[right].reset();
                fixed_positions[right].reset();
            }
        }
    }
    for (std::size_t note_index = 0; note_index < notes.size(); ++note_index) {
        if (!correction_for_note[note_index]) {
            continue;
        }
        const int string = fixed_positions[note_index]->string_index;
        for (std::size_t other_index = 0; other_index < notes.size();
             ++other_index) {
            if (other_index == note_index ||
                !overlaps(notes[note_index], notes[other_index]) ||
                fixed_positions[other_index] ||
                candidates[other_index].size() != 1 ||
                candidates[other_index].front().string_index != string) {
                continue;
            }
            const auto correction_index = *correction_for_note[note_index];
            rejected_correction[correction_index] = true;
            correction_note_rejected[note_index] = true;
            correction_for_note[note_index].reset();
            fixed_positions[note_index].reset();
            break;
        }
    }
    for (std::size_t index = 0; index < rejected_correction.size(); ++index) {
        if (rejected_correction[index]) {
            result.rejected_corrections.push_back(index);
        }
    }

    const auto groups =
        build_groups(notes, options.simultaneous_onset_tolerance_seconds);

    State initial;
    initial.hand_position = options.preferred_position;
    std::vector<State> beam{std::move(initial)};
    const auto ranked_before = [](const State& left, const State& right) {
        if (left.unresolved != right.unresolved) return left.unresolved < right.unresolved;
        return left.cost.total() < right.cost.total();
    };
    for (const auto& group : groups) {
        std::vector<State> next;
        next.reserve(options.beam_width + 1);
        bool group_search_truncated = false;
        for (const auto& previous : beam) {
            const auto local = local_assignments(
                group, notes, previous, fixed_positions, candidates, options,
                group_search_truncated, result.visited_search_nodes);
            for (const auto& assignment : local) {
                State state = previous;
                std::vector<GuitarTabPosition> group_positions;
                group_positions.reserve(group.note_indices.size());
                double fret_sum = 0.0;
                std::size_t positioned_count = 0;
                const double onset = notes[group.note_indices.front()].start_seconds;
                state.previous_strings.fill(false);
                for (std::size_t index = 0;
                     index < group.note_indices.size(); ++index) {
                    const auto note_index = group.note_indices[index];
                    const auto& position = assignment.positions[index];
                    const bool positioned = position.string_index >= 0;
                    const int string_number =
                        positioned ? position.string_index + 1 : 0;
                    group_positions.push_back(
                        {note_index, string_number,
                         positioned ? position.fret : -1,
                         positioned && position.manually_corrected});
                    if (positioned) {
                        const auto string = static_cast<std::size_t>(position.string_index);
                        state.held_frets[string] = position.fret;
                        state.last_frets[string] = position.fret;
                        state.previous_strings[string] = true;
                        if (previous.previous_strings[string]) {
                            state.cost.repeated_strings += options.repeated_string_cost;
                            const double gap = std::max(0.05, onset - previous.previous_onset);
                            state.cost.awkward_transitions += options.awkward_transition_cost *
                                std::abs(position.fret - previous.last_frets[string]) / gap;
                        }
                        int crossing = static_cast<int>(maximum_guitar_strings);
                        for (std::size_t prior = 0; prior < maximum_guitar_strings; ++prior) {
                            if (previous.previous_strings[prior]) crossing = std::min(crossing,
                                std::abs(static_cast<int>(string) - static_cast<int>(prior)));
                        }
                        if (crossing < static_cast<int>(maximum_guitar_strings)) state.cost.string_crossings +=
                            options.string_crossing_cost * std::max(0, crossing - 1);
                        state.occupied_until[static_cast<std::size_t>(
                            position.string_index)] =
                            std::max(state.occupied_until[static_cast<std::size_t>(
                                         position.string_index)],
                                     notes[note_index].start_seconds +
                                         notes[note_index].duration_seconds);
                        if (position.fret > 0) {
                            fret_sum += position.fret;
                            ++positioned_count;
                        }
                    } else ++state.unresolved;
                }
                auto incremental_cost = assignment.cost;
                if (positioned_count > 0) {
                    const double group_position =
                        fret_sum / static_cast<double>(positioned_count);
                    incremental_cost.hand_movement =
                        options.position_shift_cost *
                        std::abs(group_position - previous.hand_position);
                    state.hand_position = group_position;
                }
                state.cost = add_costs(state.cost, incremental_cost);
                state.previous_onset = onset;
                auto node = std::make_shared<PathNode>();
                node->parent = previous.path;
                node->group_positions = std::move(group_positions);
                state.path = std::move(node);
                const auto place = std::upper_bound(next.begin(), next.end(), state, ranked_before);
                next.insert(place, std::move(state));
                if (next.size() > options.beam_width) {
                    next.pop_back();
                    group_search_truncated = true;
                }
            }
        }
        result.search_truncated =
            result.search_truncated || group_search_truncated;
        if (next.empty()) {
            result.search_truncated |= group_search_truncated;
            result.explanation = group_search_truncated ?
                "Unresolved: search budget exhausted before satisfying fixed positions." :
                "Infeasible: fixed positions violate simultaneous stretch or finger constraints.";
            for (auto& note : result.notes) note.explanation = result.explanation;
            return result;
        }
        beam = std::move(next);
    }

    const auto ranked_states = beam;
    std::vector<std::vector<GuitarTabPosition>> ranked_positions;
    ranked_positions.reserve(ranked_states.size());
    for (const auto& state : ranked_states) {
        ranked_positions.push_back(
            positions_for_path(state.path, notes.size()));
    }
    result.positions = ranked_positions.front();
    result.cost = ranked_states.front().cost;
    result.fully_playable = true;
    for (std::size_t index = 0; index < notes.size(); ++index) {
        auto& note_result = result.notes[index];
        note_result.position = result.positions[index];
        note_result.playable = result.positions[index].string_number > 0;
        note_result.correction_rejected = correction_note_rejected[index];
        result.fully_playable = result.fully_playable && note_result.playable;
        if (!note_result.playable) note_result.explanation = candidates[index].empty() ?
            "Pitch is outside the configured tuning, capo and fret range." :
            "Unresolved: no retained sequence assigns this note under string occupancy, stretch and finger limits.";
    }
    result.explanation = result.fully_playable ?
        "Suggested realization only; audio does not identify the performed string or finger." :
        (result.search_truncated ?
            "Unresolved within the configured search budget; infeasibility is not proven." :
            "Infeasible under the configured string, fret, overlap, stretch and finger constraints.");

    for (std::size_t rank = 1;
         rank < ranked_states.size() &&
         result.alternatives.size() < options.maximum_alternatives;
         ++rank) {
        if (ranked_states[rank].unresolved != ranked_states.front().unresolved) continue;
        result.alternatives.push_back(
            {ranked_positions[rank], ranked_states[rank].cost});
    }
    result.alternatives_complete = !result.search_truncated &&
        result.alternatives.size() + 1 == static_cast<std::size_t>(std::count_if(
            ranked_states.begin(), ranked_states.end(), [&](const auto& state) {
                return state.unresolved == ranked_states.front().unresolved;
            }));

    // Finger numbers are conservative suggestions, independent of musical pitch.
    const auto suggest_fingers = [&](std::vector<GuitarTabPosition>& positions) {
        std::array<double, maximum_guitar_strings> until{};
        std::array<int, maximum_guitar_strings> frets;
        std::array<std::size_t, maximum_guitar_strings> held_notes{};
        frets.fill(-1);
        for (const auto& group : groups) {
            const double onset = notes[group.note_indices.front()].start_seconds;
            bool held_fretted = false;
            for (std::size_t string = 0; string < maximum_guitar_strings; ++string) {
                if (until[string] <= onset) frets[string] = -1;
                else if (frets[string] > 0) {
                    held_fretted = true;
                    positions[held_notes[string]].suggested_finger.reset();
                }
            }
            for (auto index : group.note_indices) {
                const auto& position = positions[index];
                if (position.string_number < 1) continue;
                const auto string = static_cast<std::size_t>(position.string_number - 1);
                frets[string] = position.fret;
                held_notes[string] = index;
                until[string] = notes[index].start_seconds + notes[index].duration_seconds;
            }
            const auto fingers = fingers_for(frets, options.maximum_fretting_fingers);
            if (!fingers) continue;
            for (auto index : group.note_indices) {
                auto& position = positions[index];
                if (position.string_number > 0 && (!held_fretted || position.fret == 0)) {
                    position.suggested_finger =
                        (*fingers)[static_cast<std::size_t>(position.string_number - 1)];
                }
            }
        }
    };
    suggest_fingers(result.positions);
    for (auto& alternative : result.alternatives) suggest_fingers(alternative.positions);
    for (std::size_t index = 0; index < notes.size(); ++index) {
        result.notes[index].position = result.positions[index];
    }

    for (std::size_t note_index = 0; note_index < notes.size(); ++note_index) {
        std::map<std::pair<int, int>, FingeringCostComponents> ranked_candidates;
        for (std::size_t rank = 0; rank < ranked_states.size(); ++rank) {
            const auto& position = ranked_positions[rank][note_index];
            if (position.string_number < 1) {
                continue;
            }
            const auto key = std::pair{position.string_number, position.fret};
            const auto [entry, inserted] =
                ranked_candidates.emplace(key, ranked_states[rank].cost);
            if (!inserted) {
                if (ranked_states[rank].cost.total() <
                    entry->second.total()) {
                    entry->second = ranked_states[rank].cost;
                }
            }
        }
        auto& candidates_for_note = result.notes[note_index].candidates;
        for (const auto& [position, cost] : ranked_candidates) {
            candidates_for_note.push_back(
                {position.first, position.second, cost.total(), cost});
        }
        std::stable_sort(
            candidates_for_note.begin(), candidates_for_note.end(),
            [](const FingeringCandidate& left, const FingeringCandidate& right) {
                if (left.sequence_cost != right.sequence_cost) {
                    return left.sequence_cost < right.sequence_cost;
                }
                return std::tie(left.string_number, left.fret) <
                       std::tie(right.string_number, right.fret);
            });
        if (candidates_for_note.size() > configured_tuning(options).size()) {
            candidates_for_note.resize(configured_tuning(options).size());
        }
        result.notes[note_index].ambiguous =
            candidates_for_note.size() > 1 &&
            candidates_for_note[1].sequence_cost -
                    candidates_for_note[0].sequence_cost <=
                options.ambiguity_cost_margin;
        if (result.notes[note_index].playable_positions.size() > 1 &&
            result.notes[note_index].explanation.empty()) {
            result.notes[note_index].explanation =
                "Several string/fret positions produce this pitch. Ergonomic ranking does not identify the performed position.";
        }
    }
    return result;
}

} // namespace transcribe
