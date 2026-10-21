#include "transcribe/score.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace transcribe {
namespace {

std::string stable_id(std::string_view prefix, std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    std::ostringstream output;
    output << prefix << std::hex << std::setw(16) << std::setfill('0') << hash;
    return output.str();
}

std::string origin_name(ScoreEditOrigin origin) {
    switch (origin) {
    case ScoreEditOrigin::Inference: return "inference";
    case ScoreEditOrigin::User: return "user";
    case ScoreEditOrigin::Migration: return "migration";
    case ScoreEditOrigin::Refinement: return "refinement";
    case ScoreEditOrigin::Undo: return "undo";
    case ScoreEditOrigin::Redo: return "redo";
    }
    throw std::invalid_argument("Unknown score edit origin.");
}

ScoreEditOrigin parse_origin(std::string_view value) {
    if (value == "inference") return ScoreEditOrigin::Inference;
    if (value == "user") return ScoreEditOrigin::User;
    if (value == "migration") return ScoreEditOrigin::Migration;
    if (value == "refinement") return ScoreEditOrigin::Refinement;
    if (value == "undo") return ScoreEditOrigin::Undo;
    if (value == "redo") return ScoreEditOrigin::Redo;
    throw std::invalid_argument("Score document contains an unknown edit origin.");
}

ScoreNote* find_note(ScoreDocument& document, const std::string& id) {
    const auto found = std::find_if(document.notes.begin(), document.notes.end(),
                                    [&](const auto& note) { return note.id == id; });
    return found == document.notes.end() ? nullptr : &*found;
}

const ScoreNote* find_note(const ScoreDocument& document, const std::string& id) {
    const auto found = std::find_if(document.notes.begin(), document.notes.end(),
                                    [&](const auto& note) { return note.id == id; });
    return found == document.notes.end() ? nullptr : &*found;
}

void ensure_identifier_available(const ScoreDocument& document,
                                 const std::string& id) {
    if (id.empty()) throw std::invalid_argument("Score element ID must not be empty.");
    if (find_note(document, id) ||
        std::any_of(document.rests.begin(), document.rests.end(),
                    [&](const auto& rest) { return rest.id == id; }) ||
        std::any_of(document.tab_notes.begin(), document.tab_notes.end(),
                    [&](const auto& tab) { return tab.id == id; })) {
        throw std::invalid_argument("Duplicate score element ID: " + id);
    }
}

ScoreTabNote* tab_note_for(ScoreDocument& document, const std::string& note_id) {
    const auto found = std::find_if(document.tab_notes.begin(), document.tab_notes.end(),
        [&](const auto& tab) { return tab.notation_note_id == note_id; });
    return found == document.tab_notes.end() ? nullptr : &*found;
}

ScoreTabNote make_tab_note(const ScoreNote& note) {
    return {stable_id("tab-", note.id), note.id, std::nullopt, std::nullopt,
            false, note.revision};
}

void mark_user_edit(ScoreNote& note, const ScoreTransaction& transaction,
                    std::uint64_t revision) {
    note.user_authored = true;
    note.revision = revision;
    note.provenance = {ScoreEditOrigin::User, transaction.actor,
                       transaction.reason, transaction.command_id};
}

void mark_user_edit(ScoreRest& rest, const ScoreTransaction& transaction,
                    std::uint64_t revision) {
    rest.user_authored = true;
    rest.revision = revision;
    rest.provenance = {ScoreEditOrigin::User, transaction.actor,
                       transaction.reason, transaction.command_id};
}

void validate_rhythm(const RhythmValue& rhythm) {
    if (rhythm.denominator < 0 || rhythm.dots < 0 || rhythm.dots > 2 ||
        rhythm.tuplet_actual < 0 || rhythm.tuplet_normal < 0 ||
        !std::isfinite(rhythm.duration_quarters) || rhythm.duration_quarters < 0.0 ||
        !std::isfinite(rhythm.fit_error_quarters) || rhythm.fit_error_quarters < 0.0 ||
        ((rhythm.tuplet_actual == 0) != (rhythm.tuplet_normal == 0))) {
        throw std::invalid_argument("Score contains an invalid rhythmic value.");
    }
}

void validate_evidence(const EventEvidence& evidence) {
    if (!std::isfinite(evidence.time_seconds) || evidence.time_seconds < 0.0 ||
        evidence.midi_pitch < -1 || evidence.midi_pitch > 127 ||
        !std::isfinite(evidence.raw_strength) || evidence.raw_strength < 0.0 ||
        !std::isfinite(evidence.raw_voicing) || evidence.raw_voicing < 0.0 ||
        evidence.raw_voicing > 1.0 ||
        (evidence.fractional_midi &&
         (!std::isfinite(*evidence.fractional_midi) ||
          *evidence.fractional_midi < 0.0 || *evidence.fractional_midi > 127.0)) ||
        (evidence.raw_model_value && !std::isfinite(*evidence.raw_model_value)) ||
        evidence.raw_midi_pitch < -1 || evidence.raw_midi_pitch > 127) {
        throw std::invalid_argument("Score contains invalid raw inference evidence.");
    }
}

void add_tab_note(ScoreDocument& document, const ScoreNote& note) {
    if (tab_note_for(document, note.id) == nullptr) {
        document.tab_notes.push_back(make_tab_note(note));
    }
}

void sort_score(ScoreDocument& document) {
    std::stable_sort(document.notes.begin(), document.notes.end(),
        [](const ScoreNote& left, const ScoreNote& right) {
            if (left.onset_quarters != right.onset_quarters) {
                return left.onset_quarters < right.onset_quarters;
            }
            if (left.sounding_midi_pitch != right.sounding_midi_pitch) {
                return left.sounding_midi_pitch < right.sounding_midi_pitch;
            }
            return left.id < right.id;
        });
    std::stable_sort(document.rests.begin(), document.rests.end(),
        [](const ScoreRest& left, const ScoreRest& right) {
            if (left.onset_quarters != right.onset_quarters) {
                return left.onset_quarters < right.onset_quarters;
            }
            return left.voice < right.voice;
        });
    std::stable_sort(document.tab_notes.begin(), document.tab_notes.end(),
        [&](const ScoreTabNote& left, const ScoreTabNote& right) {
            const auto* left_note = find_note(document, left.notation_note_id);
            const auto* right_note = find_note(document, right.notation_note_id);
            if (left_note == nullptr || right_note == nullptr) {
                return left.notation_note_id < right.notation_note_id;
            }
            if (left_note->onset_quarters != right_note->onset_quarters) {
                return left_note->onset_quarters < right_note->onset_quarters;
            }
            return left_note->sounding_midi_pitch < right_note->sounding_midi_pitch;
        });
}

void apply_command(ScoreDocument& document, const ScoreEditCommand& command,
                   const ScoreTransaction& transaction,
                   std::uint64_t next_revision) {
    std::visit([&](const auto& operation) {
        using Operation = std::decay_t<decltype(operation)>;
        if constexpr (std::is_same_v<Operation, InsertScoreNote>) {
            auto note = operation.note;
            ensure_identifier_available(document, note.id);
            if (!note.source_event_ids.empty()) {
                throw std::invalid_argument(
                    "A user-inserted note cannot claim an existing audio source event.");
            }
            mark_user_edit(note, transaction, next_revision);
            document.notes.push_back(std::move(note));
            add_tab_note(document, document.notes.back());
        } else if constexpr (std::is_same_v<Operation, DeleteScoreElement>) {
            const auto note = std::find_if(document.notes.begin(), document.notes.end(),
                [&](const auto& candidate) { return candidate.id == operation.element_id; });
            if (note != document.notes.end()) {
                document.notes.erase(note);
                std::erase_if(document.tab_notes, [&](const auto& tab) {
                    return tab.notation_note_id == operation.element_id;
                });
                return;
            }
            const auto rest = std::find_if(document.rests.begin(), document.rests.end(),
                [&](const auto& candidate) { return candidate.id == operation.element_id; });
            if (rest == document.rests.end()) {
                throw std::invalid_argument("Unknown score element ID: " + operation.element_id);
            }
            document.rests.erase(rest);
        } else if constexpr (std::is_same_v<Operation, ChangeScorePitch>) {
            auto* note = find_note(document, operation.note_id);
            if (note == nullptr) throw std::invalid_argument("Unknown score note ID: " + operation.note_id);
            if (operation.sounding_midi_pitch < 0 || operation.sounding_midi_pitch > 127) {
                throw std::invalid_argument("Sounding MIDI pitch must be between 0 and 127.");
            }
            const int written = operation.sounding_midi_pitch +
                document.instrument.written_transposition_semitones;
            if (written < 0 || written > 127) {
                throw std::invalid_argument("Instrument transposition moves the written pitch outside MIDI range.");
            }
            note->sounding_midi_pitch = operation.sounding_midi_pitch;
            note->written_midi_pitch = written;
            note->spelling_step.clear();
            note->spelling_alter = 0;
            note->spelling_octave = 0;
            note->accidental_required = false;
            note->revision = next_revision;
            note->user_authored = true;
            note->provenance = {ScoreEditOrigin::User, transaction.actor,
                                transaction.reason, transaction.command_id};
            if (auto* tab = tab_note_for(document, note->id)) {
                const bool valid = tab->string_number && tab->fret &&
                    *tab->string_number >= 1 &&
                    static_cast<std::size_t>(*tab->string_number) <= document.instrument.tuning.size() &&
                    *tab->fret >= 0 &&
                    document.instrument.tuning[static_cast<std::size_t>(*tab->string_number - 1)] +
                        document.instrument.capo_fret + *tab->fret == note->sounding_midi_pitch;
                if (!valid) {
                    tab->string_number.reset();
                    tab->fret.reset();
                    tab->manually_corrected = false;
                }
                tab->revision = next_revision;
            }
        } else if constexpr (std::is_same_v<Operation, ChangeScoreDuration>) {
            auto* note = find_note(document, operation.note_id);
            if (note == nullptr) throw std::invalid_argument("Unknown score note ID: " + operation.note_id);
            if (!std::isfinite(operation.duration_quarters) ||
                operation.duration_quarters <= 0.0 || operation.duration_quarters > 4096.0) {
                throw std::invalid_argument("Score note duration must be positive and at most 4096 quarter notes.");
            }
            note->duration_quarters = operation.duration_quarters;
            note->rhythm.duration_quarters = operation.duration_quarters;
            note->revision = next_revision;
            note->user_authored = true;
            note->provenance = {ScoreEditOrigin::User, transaction.actor,
                                transaction.reason, transaction.command_id};
        } else if constexpr (std::is_same_v<Operation, SplitScoreNote>) {
            auto* note = find_note(document, operation.note_id);
            if (note == nullptr) throw std::invalid_argument("Unknown score note ID: " + operation.note_id);
            if (!std::isfinite(operation.first_duration_quarters) ||
                operation.first_duration_quarters <= 0.0 ||
                operation.first_duration_quarters >= note->duration_quarters) {
                throw std::invalid_argument("A note split must be inside the note's duration.");
            }
            ensure_identifier_available(document, operation.second_note_id);
            const auto original_duration = note->duration_quarters;
            const auto tie_id = transaction.command_id.empty()
                ? stable_id("tie-", note->id + std::to_string(next_revision))
                : transaction.command_id + "-tie";
            ScoreNote second = *note;
            second.id = operation.second_note_id;
            second.onset_quarters += operation.first_duration_quarters;
            second.duration_quarters = original_duration - operation.first_duration_quarters;
            second.rhythm.duration_quarters = second.duration_quarters;
            note->duration_quarters = operation.first_duration_quarters;
            note->rhythm.duration_quarters = operation.first_duration_quarters;
            note->tie_group_id = tie_id;
            note->tie_start = true;
            note->tie_stop = false;
            second.tie_group_id = tie_id;
            second.tie_start = false;
            second.tie_stop = true;
            mark_user_edit(*note, transaction, next_revision);
            mark_user_edit(second, transaction, next_revision);
            document.notes.push_back(std::move(second));
            add_tab_note(document, document.notes.back());
        } else if constexpr (std::is_same_v<Operation, MergeScoreNotes>) {
            if (operation.first_note_id == operation.second_note_id) {
                throw std::invalid_argument("A score note cannot be merged with itself.");
            }
            const auto* first_found = find_note(document, operation.first_note_id);
            const auto* second_found = find_note(document, operation.second_note_id);
            if (first_found == nullptr || second_found == nullptr) {
                throw std::invalid_argument("Both notes in a merge must exist.");
            }
            const ScoreNote first = *first_found;
            const ScoreNote second = *second_found;
            const ScoreNote* left = &first;
            const ScoreNote* right = &second;
            if (right->onset_quarters < left->onset_quarters) std::swap(left, right);
            if (left->voice != right->voice ||
                left->sounding_midi_pitch != right->sounding_midi_pitch ||
                std::abs(left->onset_quarters + left->duration_quarters -
                         right->onset_quarters) > 1e-6) {
                throw std::invalid_argument(
                    "Only adjacent notes with the same pitch and voice can be merged.");
            }
            ensure_identifier_available(document, operation.merged_note_id);
            ScoreNote merged = *left;
            merged.id = operation.merged_note_id;
            merged.duration_quarters += right->duration_quarters;
            merged.rhythm.duration_quarters = merged.duration_quarters;
            merged.measured_onset_seconds = std::min(first.measured_onset_seconds,
                                                       second.measured_onset_seconds);
            merged.measured_offset_seconds = std::max(first.measured_offset_seconds,
                                                        second.measured_offset_seconds);
            for (const auto& source_id : right->source_event_ids) {
                if (std::find(merged.source_event_ids.begin(), merged.source_event_ids.end(),
                              source_id) == merged.source_event_ids.end()) {
                    merged.source_event_ids.push_back(source_id);
                }
            }
            merged.tie_group_id.reset();
            merged.tie_start = false;
            merged.tie_stop = false;
            mark_user_edit(merged, transaction, next_revision);
            document.notes.erase(std::remove_if(document.notes.begin(), document.notes.end(),
                [&](const auto& candidate) {
                    return candidate.id == first.id || candidate.id == second.id;
                }), document.notes.end());
            std::erase_if(document.tab_notes, [&](const auto& tab) {
                return tab.notation_note_id == first.id || tab.notation_note_id == second.id;
            });
            document.notes.push_back(std::move(merged));
            add_tab_note(document, document.notes.back());
        } else if constexpr (std::is_same_v<Operation, InsertScoreRest>) {
            auto rest = operation.rest;
            ensure_identifier_available(document, rest.id);
            mark_user_edit(rest, transaction, next_revision);
            document.rests.push_back(std::move(rest));
        } else if constexpr (std::is_same_v<Operation, ChangeScoreFingering>) {
            auto* note = find_note(document, operation.note_id);
            if (note == nullptr) throw std::invalid_argument("Unknown score note ID: " + operation.note_id);
            if (operation.string_number.has_value() != operation.fret.has_value()) {
                throw std::invalid_argument("A fingering must specify both string and fret, or neither.");
            }
            if (operation.string_number) {
                if (*operation.string_number < 1 ||
                    static_cast<std::size_t>(*operation.string_number) > document.instrument.tuning.size() ||
                    *operation.fret < 0 || *operation.fret > 36) {
                    throw std::invalid_argument("Score fingering is outside the configured instrument range.");
                }
                const auto open_pitch = document.instrument.tuning[
                    static_cast<std::size_t>(*operation.string_number - 1)];
                if (open_pitch + document.instrument.capo_fret + *operation.fret !=
                    note->sounding_midi_pitch) {
                    throw std::invalid_argument("String and fret do not preserve the note's sounding pitch.");
                }
            }
            auto* tab = tab_note_for(document, note->id);
            if (tab == nullptr) {
                document.tab_notes.push_back(make_tab_note(*note));
                tab = &document.tab_notes.back();
            }
            tab->string_number = operation.string_number;
            tab->fret = operation.fret;
            tab->manually_corrected = operation.string_number.has_value();
            tab->revision = next_revision;
            note->user_authored = true;
            note->revision = next_revision;
            note->provenance = {ScoreEditOrigin::User, transaction.actor,
                                transaction.reason, transaction.command_id};
        } else if constexpr (std::is_same_v<Operation, ReinterpretScoreRhythm>) {
            auto* note = find_note(document, operation.note_id);
            if (note == nullptr) throw std::invalid_argument("Unknown score note ID: " + operation.note_id);
            if (!std::isfinite(operation.onset_quarters) || operation.onset_quarters < 0.0 ||
                operation.onset_quarters > 4096.0 ||
                !std::isfinite(operation.duration_quarters) ||
                operation.duration_quarters <= 0.0 ||
                operation.duration_quarters > 4096.0) {
                throw std::invalid_argument("Interpreted note position or duration is invalid.");
            }
            validate_rhythm(operation.rhythm);
            note->onset_quarters = operation.onset_quarters;
            note->duration_quarters = operation.duration_quarters;
            note->rhythm = operation.rhythm;
            note->rhythm.duration_quarters = operation.duration_quarters;
            note->revision = next_revision;
            note->user_authored = true;
            note->provenance = {ScoreEditOrigin::User, transaction.actor,
                                transaction.reason, transaction.command_id};
        } else if constexpr (std::is_same_v<Operation, UpdateScoreContext>) {
            if (!std::isfinite(operation.pickup_quarters) ||
                operation.pickup_quarters < 0.0 || operation.pickup_quarters > 16.0) {
                throw std::invalid_argument("Pickup length must be between 0 and 16 quarter notes.");
            }
            document.metadata = operation.metadata;
            document.instrument = operation.instrument;
            document.pickup_quarters = operation.pickup_quarters;
            document.meter.numerator = operation.metadata.time_numerator;
            document.meter.denominator = operation.metadata.time_denominator;
            document.meter.user_constrained = true;
            document.key.fifths = operation.metadata.key_fifths;
            document.key.minor = operation.metadata.minor_key;
            document.key.user_constrained = true;
            if (operation.replace_tempo_map || document.tempo_map.empty()) {
                const auto end_seconds = document.tempo_map.empty()
                    ? 0.0 : document.tempo_map.back().end_seconds;
                document.tempo_map = {{0.0, end_seconds, 0.0,
                    operation.metadata.tempo_bpm, 1.0, true}};
                for (auto& beat : document.beats) {
                    beat.time_seconds =
                        score_position_seconds(document, beat.quarter_position);
                    beat.confidence = 1.0;
                }
            }
            for (auto& tab : document.tab_notes) {
                if (!tab.string_number || !tab.fret) continue;
                const auto* note = find_note(document, tab.notation_note_id);
                const bool valid = note != nullptr &&
                    *tab.string_number >= 1 &&
                    static_cast<std::size_t>(*tab.string_number) <= document.instrument.tuning.size() &&
                    document.instrument.tuning[static_cast<std::size_t>(*tab.string_number - 1)] +
                        document.instrument.capo_fret + *tab.fret ==
                            note->sounding_midi_pitch;
                if (!valid) {
                    tab.string_number.reset();
                    tab.fret.reset();
                    tab.manually_corrected = false;
                }
                tab.revision = next_revision;
            }
            for (auto& note : document.notes) {
                const int written = note.sounding_midi_pitch +
                    document.instrument.written_transposition_semitones;
                if (written < 0 || written > 127) {
                    throw std::invalid_argument(
                        "Instrument transposition moves a written pitch outside MIDI range.");
                }
                note.written_midi_pitch = written;
                note.spelling_step.clear();
                note.spelling_alter = 0;
                note.spelling_octave = 0;
                note.accidental_required = false;
                note.revision = next_revision;
                note.user_authored = true;
                note.provenance = {ScoreEditOrigin::User, transaction.actor,
                                   transaction.reason, transaction.command_id};
            }
        }
    }, command);
}

void json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20) {
                output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned int>(byte) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(byte);
            }
        }
    }
    output << '"';
}

void json_number(std::ostream& output, double value) {
    if (!std::isfinite(value)) throw std::invalid_argument("Cannot serialize a nonfinite score value.");
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
}

void write_rhythm(std::ostream& output, const RhythmValue& rhythm) {
    output << "{\"denominator\":" << rhythm.denominator
           << ",\"dots\":" << rhythm.dots
           << ",\"tuplet_actual\":" << rhythm.tuplet_actual
           << ",\"tuplet_normal\":" << rhythm.tuplet_normal
           << ",\"duration_quarters\":";
    json_number(output, rhythm.duration_quarters);
    output << ",\"fit_error_quarters\":";
    json_number(output, rhythm.fit_error_quarters);
    output << ",\"symbolic\":" << (rhythm.symbolic ? "true" : "false") << '}';
}

void write_evidence(std::ostream& output, const EventEvidence& evidence) {
    output << "{\"time_seconds\":";
    json_number(output, evidence.time_seconds);
    output << ",\"midi_pitch\":" << evidence.midi_pitch << ",\"raw_strength\":";
    json_number(output, evidence.raw_strength);
    output << ",\"raw_voicing\":";
    json_number(output, evidence.raw_voicing);
    output << ",\"fractional_midi\":";
    if (evidence.fractional_midi) json_number(output, *evidence.fractional_midi);
    else output << "null";
    output << ",\"raw_model_value\":";
    if (evidence.raw_model_value) json_number(output, *evidence.raw_model_value);
    else output << "null";
    output << ",\"raw_midi_pitch\":" << evidence.raw_midi_pitch << '}';
}

void write_event(std::ostream& output, const CanonicalNoteEvent& event) {
    output << "{\"id\":";
    json_string(output, event.id);
    output << ",\"midi_pitch\":";
    json_number(output, event.midi_pitch);
    output << ",\"onset_seconds\":";
    json_number(output, event.onset_seconds);
    output << ",\"offset_seconds\":";
    json_number(output, event.offset_seconds);
    output << ",\"onset_sample\":" << event.onset_sample
           << ",\"offset_sample\":" << event.offset_sample
           << ",\"source_sample_rate\":" << event.source_sample_rate
           << ",\"audio_source_id\":";
    json_string(output, event.audio_source_id);
    output << ",\"source_model\":";
    json_string(output, event.source_model);
    output << ",\"detection_method\":";
    json_string(output, event.detection_method);
    output << ",\"evidence_kind\":";
    json_string(output, event.evidence_kind);
    output << ",\"state\":" << static_cast<int>(event.state) << ",\"calibrated_confidence\":";
    if (event.calibrated_confidence) json_number(output, *event.calibrated_confidence);
    else output << "null";
    output << ",\"intensity_rms\":";
    if (event.intensity_rms) json_number(output, *event.intensity_rms);
    else output << "null";
    output << ",\"velocity\":";
    if (event.velocity) output << *event.velocity;
    else output << "null";
    output << ",\"instrument\":";
    if (event.instrument) json_string(output, *event.instrument);
    else output << "null";
    output << ",\"voice\":";
    if (event.voice) json_string(output, *event.voice);
    else output << "null";
    output << ",\"articulation\":";
    if (event.articulation) json_string(output, *event.articulation);
    else output << "null";
    output << ",\"evidence\":[";
    for (std::size_t i = 0; i < event.evidence.size(); ++i) {
        if (i) output << ',';
        write_evidence(output, event.evidence[i]);
    }
    output << "]}";
}

void write_key(std::ostream& output, const KeyHypothesis& key) {
    output << "{\"fifths\":" << key.fifths << ",\"minor\":"
           << (key.minor ? "true" : "false") << ",\"confidence\":";
    json_number(output, key.confidence);
    output << ",\"user_constrained\":" << (key.user_constrained ? "true" : "false") << '}';
}

void write_provenance(std::ostream& output, const ScoreEditProvenance& provenance) {
    output << "{\"origin\":";
    json_string(output, origin_name(provenance.origin));
    output << ",\"actor\":";
    json_string(output, provenance.actor);
    output << ",\"reason\":";
    json_string(output, provenance.reason);
    output << ",\"command_id\":";
    json_string(output, provenance.command_id);
    output << '}';
}

class JsonValue {
public:
    using Object = std::map<std::string, JsonValue>;
    using Array = std::vector<JsonValue>;
    using Data = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;

    explicit JsonValue(Data data) : data_(std::move(data)) {}
    const Data& data() const { return data_; }

private:
    Data data_;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input) {
        if (input.size() > 64U * 1024U * 1024U) {
            throw std::length_error("Score project JSON exceeds the 64 MiB limit.");
        }
    }

    JsonValue parse() {
        skip_space();
        auto value = parse_value(0);
        skip_space();
        if (position_ != input_.size()) fail("Trailing data after JSON value.");
        return value;
    }

private:
    [[noreturn]] void fail(const char* message) const {
        throw std::invalid_argument(std::string("Invalid score JSON at byte ") +
                                    std::to_string(position_) + ": " + message);
    }

    void skip_space() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\n' ||
                input_[position_] == '\r' || input_[position_] == '\t')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue parse_value(std::size_t depth) {
        if (depth > 64) fail("Nesting limit exceeded.");
        skip_space();
        if (position_ >= input_.size()) fail("Unexpected end of input.");
        const char value = input_[position_];
        if (value == '{') return parse_object(depth + 1);
        if (value == '[') return parse_array(depth + 1);
        if (value == '"') return JsonValue(parse_string());
        if (value == 't' && input_.substr(position_, 4) == "true") {
            position_ += 4;
            return JsonValue(true);
        }
        if (value == 'f' && input_.substr(position_, 5) == "false") {
            position_ += 5;
            return JsonValue(false);
        }
        if (value == 'n' && input_.substr(position_, 4) == "null") {
            position_ += 4;
            return JsonValue(nullptr);
        }
        return JsonValue(parse_number());
    }

    JsonValue parse_object(std::size_t depth) {
        consume('{');
        JsonValue::Object values;
        skip_space();
        if (consume('}')) return JsonValue(std::move(values));
        for (;;) {
            skip_space();
            if (position_ >= input_.size() || input_[position_] != '"') {
                fail("Object keys must be strings.");
            }
            auto key = parse_string();
            skip_space();
            if (!consume(':')) fail("Expected ':' after object key.");
            auto value = parse_value(depth);
            if (!values.emplace(std::move(key), std::move(value)).second) {
                fail("Duplicate object key.");
            }
            skip_space();
            if (consume('}')) break;
            if (!consume(',')) fail("Expected ',' or '}' in object.");
            if (values.size() > 1'000'000) fail("Object member limit exceeded.");
        }
        return JsonValue(std::move(values));
    }

    JsonValue parse_array(std::size_t depth) {
        consume('[');
        JsonValue::Array values;
        skip_space();
        if (consume(']')) return JsonValue(std::move(values));
        for (;;) {
            values.push_back(parse_value(depth));
            if (values.size() > 1'000'000) fail("Array element limit exceeded.");
            skip_space();
            if (consume(']')) break;
            if (!consume(',')) fail("Expected ',' or ']' in array.");
        }
        return JsonValue(std::move(values));
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7f) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else if (codepoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
        }
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4 > input_.size()) fail("Incomplete Unicode escape.");
        std::uint32_t result = 0;
        for (int i = 0; i < 4; ++i) {
            const char value = input_[position_++];
            result <<= 4;
            if (value >= '0' && value <= '9') result |= static_cast<unsigned>(value - '0');
            else if (value >= 'a' && value <= 'f') result |= static_cast<unsigned>(value - 'a' + 10);
            else if (value >= 'A' && value <= 'F') result |= static_cast<unsigned>(value - 'A' + 10);
            else fail("Invalid hexadecimal Unicode escape.");
        }
        return result;
    }

    std::string parse_string() {
        if (!consume('"')) fail("Expected string.");
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char value = static_cast<unsigned char>(input_[position_++]);
            if (value == '"') return result;
            if (value < 0x20) fail("Unescaped control character in string.");
            if (value != '\\') {
                result.push_back(static_cast<char>(value));
                continue;
            }
            if (position_ >= input_.size()) fail("Incomplete string escape.");
            const char escape = input_[position_++];
            switch (escape) {
            case '"': result.push_back('"'); break;
            case '\\': result.push_back('\\'); break;
            case '/': result.push_back('/'); break;
            case 'b': result.push_back('\b'); break;
            case 'f': result.push_back('\f'); break;
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            case 'u': {
                auto codepoint = parse_hex4();
                if (codepoint >= 0xd800 && codepoint <= 0xdbff) {
                    if (position_ + 2 > input_.size() ||
                        input_[position_] != '\\' || input_[position_ + 1] != 'u') {
                        fail("Unpaired high surrogate.");
                    }
                    position_ += 2;
                    const auto low = parse_hex4();
                    if (low < 0xdc00 || low > 0xdfff) fail("Invalid low surrogate.");
                    codepoint = 0x10000 + ((codepoint - 0xd800) << 10) + (low - 0xdc00);
                } else if (codepoint >= 0xdc00 && codepoint <= 0xdfff) {
                    fail("Unpaired low surrogate.");
                }
                append_utf8(result, codepoint);
                break;
            }
            default: fail("Unknown string escape.");
            }
        }
        fail("Unterminated string.");
    }

    double parse_number() {
        const auto first = position_;
        if (consume('-')) {}
        if (consume('0')) {
            if (position_ < input_.size() && input_[position_] >= '0' &&
                input_[position_] <= '9') fail("Leading zero in number.");
        } else {
            if (position_ >= input_.size() || input_[position_] < '1' ||
                input_[position_] > '9') fail("Expected JSON value.");
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
        }
        if (consume('.')) {
            const auto fraction_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (fraction_start == position_) fail("Fraction requires a digit.");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            const auto exponent_start = position_;
            while (position_ < input_.size() && input_[position_] >= '0' &&
                   input_[position_] <= '9') ++position_;
            if (exponent_start == position_) fail("Exponent requires a digit.");
        }
        double result = 0;
        const auto text = input_.substr(first, position_ - first);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
            !std::isfinite(result)) fail("Invalid or nonfinite number.");
        return result;
    }

    std::string_view input_;
    std::size_t position_{};
};

const JsonValue::Object& object(const JsonValue& value, const char* context) {
    const auto* result = std::get_if<JsonValue::Object>(&value.data());
    if (result == nullptr) throw std::invalid_argument(std::string("Expected JSON object for ") + context + ".");
    return *result;
}

const JsonValue::Array& array(const JsonValue& value, const char* context) {
    const auto* result = std::get_if<JsonValue::Array>(&value.data());
    if (result == nullptr) throw std::invalid_argument(std::string("Expected JSON array for ") + context + ".");
    return *result;
}

const JsonValue& field(const JsonValue::Object& values, const char* name) {
    const auto found = values.find(name);
    if (found == values.end()) throw std::invalid_argument(std::string("Missing score JSON field: ") + name);
    return found->second;
}

const JsonValue* optional_field(const JsonValue::Object& values, const char* name) {
    const auto found = values.find(name);
    return found == values.end() ? nullptr : &found->second;
}

double number(const JsonValue& value, const char* name) {
    const auto* result = std::get_if<double>(&value.data());
    if (result == nullptr) throw std::invalid_argument(std::string("Expected number for ") + name + ".");
    return *result;
}

std::int64_t integer(const JsonValue& value, const char* name) {
    const double parsed = number(value, name);
    constexpr double minimum = -9223372036854775808.0;
    constexpr double maximum_exclusive = 9223372036854775808.0;
    if (std::trunc(parsed) != parsed || parsed < minimum ||
        parsed >= maximum_exclusive) {
        throw std::invalid_argument(std::string("Expected integer for ") + name + ".");
    }
    return static_cast<std::int64_t>(parsed);
}

int int_value(const JsonValue& value, const char* name) {
    const auto parsed = integer(value, name);
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(std::string("Integer out of range for ") + name + ".");
    }
    return static_cast<int>(parsed);
}

std::uint64_t uint64_value(const JsonValue& value, const char* name) {
    const auto parsed = integer(value, name);
    if (parsed < 0) {
        throw std::invalid_argument(std::string("Expected nonnegative integer for ") + name + ".");
    }
    return static_cast<std::uint64_t>(parsed);
}

std::uint32_t uint32_value(const JsonValue& value, const char* name) {
    const auto parsed = integer(value, name);
    if (parsed < 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(std::string("Unsigned integer out of range for ") + name + ".");
    }
    return static_cast<std::uint32_t>(parsed);
}

std::size_t size_value(const JsonValue& value, const char* name) {
    const auto parsed = integer(value, name);
    if (parsed < 0 ||
        static_cast<std::uint64_t>(parsed) >
            std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("Size out of range for ") + name + ".");
    }
    return static_cast<std::size_t>(parsed);
}

bool boolean(const JsonValue& value, const char* name) {
    const auto* result = std::get_if<bool>(&value.data());
    if (result == nullptr) throw std::invalid_argument(std::string("Expected boolean for ") + name + ".");
    return *result;
}

std::string string(const JsonValue& value, const char* name) {
    const auto* result = std::get_if<std::string>(&value.data());
    if (result == nullptr) throw std::invalid_argument(std::string("Expected string for ") + name + ".");
    return *result;
}

bool is_null(const JsonValue& value) {
    return std::holds_alternative<std::nullptr_t>(value.data());
}

std::optional<double> optional_number(const JsonValue::Object& values, const char* name) {
    const auto* value = optional_field(values, name);
    if (value == nullptr || is_null(*value)) return std::nullopt;
    return number(*value, name);
}

std::optional<int> optional_int(const JsonValue::Object& values, const char* name) {
    const auto* value = optional_field(values, name);
    if (value == nullptr || is_null(*value)) return std::nullopt;
    const auto parsed = integer(*value, name);
    if (parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        throw std::invalid_argument(std::string("Integer out of range for ") + name + ".");
    }
    return static_cast<int>(parsed);
}

std::optional<std::string> optional_string(const JsonValue::Object& values,
                                           const char* name) {
    const auto* value = optional_field(values, name);
    if (value == nullptr || is_null(*value)) return std::nullopt;
    return string(*value, name);
}

RhythmValue read_rhythm(const JsonValue& value) {
    const auto& fields = object(value, "rhythm");
    RhythmValue result;
    result.denominator = int_value(field(fields, "denominator"), "denominator");
    result.dots = int_value(field(fields, "dots"), "dots");
    result.tuplet_actual = int_value(field(fields, "tuplet_actual"), "tuplet_actual");
    result.tuplet_normal = int_value(field(fields, "tuplet_normal"), "tuplet_normal");
    result.duration_quarters = number(field(fields, "duration_quarters"), "duration_quarters");
    result.fit_error_quarters = number(field(fields, "fit_error_quarters"), "fit_error_quarters");
    result.symbolic = boolean(field(fields, "symbolic"), "symbolic");
    validate_rhythm(result);
    return result;
}

EventEvidence read_evidence(const JsonValue& value) {
    const auto& fields = object(value, "event evidence");
    EventEvidence result;
    result.time_seconds = number(field(fields, "time_seconds"), "time_seconds");
    result.midi_pitch = int_value(field(fields, "midi_pitch"), "midi_pitch");
    result.raw_strength = number(field(fields, "raw_strength"), "raw_strength");
    result.raw_voicing = number(field(fields, "raw_voicing"), "raw_voicing");
    result.fractional_midi = optional_number(fields, "fractional_midi");
    result.raw_model_value = optional_number(fields, "raw_model_value");
    result.raw_midi_pitch = int_value(field(fields, "raw_midi_pitch"), "raw_midi_pitch");
    return result;
}

CanonicalNoteEvent read_event(const JsonValue& value) {
    const auto& fields = object(value, "source event");
    CanonicalNoteEvent result;
    result.id = string(field(fields, "id"), "id");
    result.midi_pitch = number(field(fields, "midi_pitch"), "midi_pitch");
    result.onset_seconds = number(field(fields, "onset_seconds"), "onset_seconds");
    result.offset_seconds = number(field(fields, "offset_seconds"), "offset_seconds");
    result.onset_sample = uint64_value(field(fields, "onset_sample"), "onset_sample");
    result.offset_sample = uint64_value(field(fields, "offset_sample"), "offset_sample");
    result.source_sample_rate = uint32_value(field(fields, "source_sample_rate"), "source_sample_rate");
    result.audio_source_id = string(field(fields, "audio_source_id"), "audio_source_id");
    result.source_model = string(field(fields, "source_model"), "source_model");
    result.detection_method = string(field(fields, "detection_method"), "detection_method");
    result.evidence_kind = string(field(fields, "evidence_kind"), "evidence_kind");
    const auto state = integer(field(fields, "state"), "state");
    if (state < 0 || state > static_cast<int>(EventLifecycle::Rejected)) {
        throw std::invalid_argument("Source event lifecycle value is invalid.");
    }
    result.state = static_cast<EventLifecycle>(state);
    result.calibrated_confidence = optional_number(fields, "calibrated_confidence");
    result.intensity_rms = optional_number(fields, "intensity_rms");
    result.velocity = optional_int(fields, "velocity");
    result.instrument = optional_string(fields, "instrument");
    result.voice = optional_string(fields, "voice");
    result.articulation = optional_string(fields, "articulation");
    for (const auto& item : array(field(fields, "evidence"), "event evidence")) {
        result.evidence.push_back(read_evidence(item));
    }
    return result;
}

KeyHypothesis read_key(const JsonValue& value) {
    const auto& fields = object(value, "key");
    KeyHypothesis result;
    result.fifths = int_value(field(fields, "fifths"), "fifths");
    result.minor = boolean(field(fields, "minor"), "minor");
    result.confidence = number(field(fields, "confidence"), "confidence");
    result.user_constrained = boolean(field(fields, "user_constrained"), "user_constrained");
    return result;
}

ScoreEditProvenance read_provenance(const JsonValue& value) {
    const auto& fields = object(value, "edit provenance");
    return {parse_origin(string(field(fields, "origin"), "origin")),
            string(field(fields, "actor"), "actor"),
            string(field(fields, "reason"), "reason"),
            string(field(fields, "command_id"), "command_id")};
}

ScoreNote read_note(const JsonValue& value) {
    const auto& fields = object(value, "score note");
    ScoreNote result;
    result.id = string(field(fields, "id"), "id");
    for (const auto& source_id : array(field(fields, "source_event_ids"), "source_event_ids")) {
        result.source_event_ids.push_back(string(source_id, "source_event_id"));
    }
    result.measured_onset_seconds = number(field(fields, "measured_onset_seconds"), "measured_onset_seconds");
    result.measured_offset_seconds = number(field(fields, "measured_offset_seconds"), "measured_offset_seconds");
    result.onset_quarters = number(field(fields, "onset_quarters"), "onset_quarters");
    result.duration_quarters = number(field(fields, "duration_quarters"), "duration_quarters");
    result.onset_residual_seconds = number(field(fields, "onset_residual_seconds"), "onset_residual_seconds");
    result.offset_residual_seconds = number(field(fields, "offset_residual_seconds"), "offset_residual_seconds");
    result.voice = int_value(field(fields, "voice"), "voice");
    result.chord_group_id = optional_string(fields, "chord_group_id");
    result.sounding_midi_pitch = int_value(field(fields, "sounding_midi_pitch"), "sounding_midi_pitch");
    result.written_midi_pitch = int_value(field(fields, "written_midi_pitch"), "written_midi_pitch");
    result.spelling_step = string(field(fields, "spelling_step"), "spelling_step");
    result.spelling_alter = int_value(field(fields, "spelling_alter"), "spelling_alter");
    result.spelling_octave = int_value(field(fields, "spelling_octave"), "spelling_octave");
    result.accidental_required = boolean(field(fields, "accidental_required"), "accidental_required");
    result.rhythm = read_rhythm(field(fields, "rhythm"));
    for (const auto& rhythm : array(field(fields, "rhythm_alternatives"), "rhythm_alternatives")) {
        result.rhythm_alternatives.push_back(read_rhythm(rhythm));
    }
    result.interpretation_evidence = number(field(fields, "interpretation_evidence"), "interpretation_evidence");
    result.calibrated_confidence = optional_number(fields, "calibrated_confidence");
    result.intensity_rms = optional_number(fields, "intensity_rms");
    for (const auto& evidence : array(field(fields, "raw_evidence"), "raw_evidence")) {
        result.raw_evidence.push_back(read_evidence(evidence));
    }
    result.velocity = optional_int(fields, "velocity");
    result.articulation = optional_string(fields, "articulation");
    result.tie_group_id = optional_string(fields, "tie_group_id");
    result.tie_start = boolean(field(fields, "tie_start"), "tie_start");
    result.tie_stop = boolean(field(fields, "tie_stop"), "tie_stop");
    result.user_authored = boolean(field(fields, "user_authored"), "user_authored");
    result.revision = uint64_value(field(fields, "revision"), "revision");
    result.provenance = read_provenance(field(fields, "provenance"));
    return result;
}

ScoreDocument read_current_document(const JsonValue::Object& fields) {
    ScoreDocument result;
    result.schema_version = uint32_value(field(fields, "schema_version"), "schema_version");
    result.id = string(field(fields, "id"), "id");
    result.source_id = string(field(fields, "source_id"), "source_id");
    result.source_sample_rate = uint32_value(field(fields, "source_sample_rate"), "source_sample_rate");
    result.time_reference = string(field(fields, "time_reference"), "time_reference");
    result.revision = uint64_value(field(fields, "revision"), "revision");
    const auto& metadata = object(field(fields, "metadata"), "metadata");
    result.metadata.tempo_bpm = number(field(metadata, "tempo_bpm"), "tempo_bpm");
    result.metadata.time_numerator = int_value(field(metadata, "time_numerator"), "time_numerator");
    result.metadata.time_denominator = int_value(field(metadata, "time_denominator"), "time_denominator");
    result.metadata.key_fifths = int_value(field(metadata, "key_fifths"), "key_fifths");
    result.metadata.minor_key = boolean(field(metadata, "minor_key"), "minor_key");
    result.metadata.tempo_inferred = boolean(field(metadata, "tempo_inferred"), "tempo_inferred");
    result.metadata.meter_inferred = boolean(field(metadata, "meter_inferred"), "meter_inferred");
    result.metadata.key_inferred = boolean(field(metadata, "key_inferred"), "key_inferred");
    result.metadata.tempo_confidence = number(field(metadata, "tempo_confidence"), "tempo_confidence");
    result.metadata.meter_confidence = number(field(metadata, "meter_confidence"), "meter_confidence");
    result.metadata.key_confidence = number(field(metadata, "key_confidence"), "key_confidence");
    result.metadata.swing_ratio = number(field(metadata, "swing_ratio"), "swing_ratio");
    result.metadata.swing_inferred = boolean(field(metadata, "swing_inferred"), "swing_inferred");
    result.metadata.swing_confidence = number(field(metadata, "swing_confidence"), "swing_confidence");

    const auto& instrument = object(field(fields, "instrument"), "instrument");
    result.instrument.name = string(field(instrument, "name"), "instrument name");
    result.instrument.written_transposition_semitones =
        int_value(field(instrument, "written_transposition_semitones"),
                  "written_transposition_semitones");
    result.instrument.tuning.clear();
    for (const auto& pitch : array(field(instrument, "tuning"), "tuning")) {
        result.instrument.tuning.push_back(int_value(pitch, "tuning pitch"));
    }
    result.instrument.capo_fret = int_value(field(instrument, "capo_fret"), "capo_fret");

    for (const auto& event : array(field(fields, "source_events"), "source_events")) {
        result.source_events.push_back(read_event(event));
    }
    for (const auto& item : array(field(fields, "tempo_map"), "tempo_map")) {
        const auto& values = object(item, "tempo segment");
        result.tempo_map.push_back({
            number(field(values, "start_seconds"), "start_seconds"),
            number(field(values, "end_seconds"), "end_seconds"),
            number(field(values, "start_quarters"), "start_quarters"),
            number(field(values, "beats_per_minute"), "beats_per_minute"),
            number(field(values, "confidence"), "confidence"),
            boolean(field(values, "user_constrained"), "user_constrained")});
    }
    for (const auto& item : array(field(fields, "beats"), "beats")) {
        const auto& values = object(item, "beat");
        result.beats.push_back({
            number(field(values, "time_seconds"), "time_seconds"),
            number(field(values, "quarter_position"), "quarter_position"),
            number(field(values, "confidence"), "confidence"),
            boolean(field(values, "downbeat"), "downbeat")});
    }
    const auto& meter = object(field(fields, "meter"), "meter");
    result.meter.numerator = int_value(field(meter, "numerator"), "numerator");
    result.meter.denominator = int_value(field(meter, "denominator"), "denominator");
    result.meter.confidence = number(field(meter, "confidence"), "confidence");
    result.meter.user_constrained = boolean(field(meter, "user_constrained"), "user_constrained");
    result.meter.grouping.clear();
    for (const auto& item : array(field(meter, "grouping"), "meter grouping")) {
        result.meter.grouping.push_back(int_value(item, "meter group"));
    }
    result.key = read_key(field(fields, "key"));
    for (const auto& item : array(field(fields, "alternative_keys"), "alternative_keys")) {
        result.alternative_keys.push_back(read_key(item));
    }
    for (const auto& item : array(field(fields, "local_tonal_context"), "local_tonal_context")) {
        const auto& values = object(item, "tonal region");
        result.local_tonal_context.push_back({
            size_value(field(values, "first_measure"), "first_measure"),
            size_value(field(values, "end_measure"), "end_measure"),
            read_key(field(values, "key"))});
    }
    result.pickup_quarters = number(field(fields, "pickup_quarters"), "pickup_quarters");
    result.total_quarters = number(field(fields, "total_quarters"), "total_quarters");
    result.swing_ratio = number(field(fields, "swing_ratio"), "swing_ratio");
    result.swing_confidence = number(field(fields, "swing_confidence"), "swing_confidence");
    result.swing_inferred = boolean(field(fields, "swing_inferred"), "swing_inferred");
    result.tempo_ambiguous = boolean(field(fields, "tempo_ambiguous"), "tempo_ambiguous");
    result.meter_ambiguous = boolean(field(fields, "meter_ambiguous"), "meter_ambiguous");
    result.key_ambiguous = boolean(field(fields, "key_ambiguous"), "key_ambiguous");
    for (const auto& item : array(field(fields, "uncertainties"), "uncertainties")) {
        result.uncertainties.push_back(string(item, "uncertainty"));
    }
    for (const auto& item : array(field(fields, "notes"), "notes")) {
        result.notes.push_back(read_note(item));
    }
    for (const auto& item : array(field(fields, "rests"), "rests")) {
        const auto& values = object(item, "score rest");
        ScoreRest rest;
        rest.id = string(field(values, "id"), "id");
        rest.voice = int_value(field(values, "voice"), "voice");
        rest.onset_quarters = number(field(values, "onset_quarters"), "onset_quarters");
        rest.duration_quarters = number(field(values, "duration_quarters"), "duration_quarters");
        rest.rhythm = read_rhythm(field(values, "rhythm"));
        rest.user_authored = boolean(field(values, "user_authored"), "user_authored");
        rest.revision = uint64_value(field(values, "revision"), "revision");
        rest.provenance = read_provenance(field(values, "provenance"));
        result.rests.push_back(std::move(rest));
    }
    for (const auto& item : array(field(fields, "tab_notes"), "tab_notes")) {
        const auto& values = object(item, "tab note");
        ScoreTabNote tab;
        tab.id = string(field(values, "id"), "id");
        tab.notation_note_id = string(field(values, "notation_note_id"), "notation_note_id");
        tab.string_number = optional_int(values, "string_number");
        tab.fret = optional_int(values, "fret");
        tab.manually_corrected = boolean(field(values, "manually_corrected"), "manually_corrected");
        tab.revision = uint64_value(field(values, "revision"), "revision");
        result.tab_notes.push_back(std::move(tab));
    }
    for (const auto& item : array(field(fields, "history"), "history")) {
        const auto& values = object(item, "revision record");
        result.history.push_back({
            uint64_value(field(values, "revision"), "revision"),
            parse_origin(string(field(values, "origin"), "origin")),
            string(field(values, "actor"), "actor"),
            string(field(values, "reason"), "reason"),
            string(field(values, "command_id"), "command_id")});
    }
    return result;
}

ScoreDocument migrate_legacy_document(const JsonValue::Object& fields) {
    ScoreDocument result;
    result.id = "migrated-" + stable_id("", "transcriber-score-v0");
    result.source_id = "legacy-project";
    result.revision = 1;
    result.metadata.tempo_bpm = 120.0;
    result.metadata.time_numerator = 4;
    result.metadata.time_denominator = 4;
    result.metadata.tempo_inferred = false;
    result.metadata.meter_inferred = false;
    result.metadata.key_inferred = false;
    result.tempo_map.push_back({0.0, 0.0, 0.0, 120.0, 0.0, false});
    result.meter.numerator = 4;
    result.meter.denominator = 4;
    result.key = {};
    const auto* legacy_notes = optional_field(fields, "notes");
    if (legacy_notes == nullptr) {
        throw std::invalid_argument("Version 0 score project must contain a notes array.");
    }
    const auto& notes = array(*legacy_notes, "legacy notes");
    if (notes.size() > 100000) throw std::length_error("Legacy score has too many notes.");
    for (std::size_t index = 0; index < notes.size(); ++index) {
        const auto& values = object(notes[index], "legacy note");
        const auto midi = int_value(field(values, "midi"), "midi");
        const auto start = number(field(values, "start"), "start");
        const auto duration = number(field(values, "duration"), "duration");
        const auto confidence = optional_number(values, "confidence").value_or(0.0);
        const auto amplitude = optional_number(values, "amplitude").value_or(0.0);
        const auto source_event_id = "legacy-event-" + std::to_string(index + 1);
        CanonicalNoteEvent event;
        event.id = source_event_id;
        event.midi_pitch = midi;
        event.onset_seconds = start;
        event.offset_seconds = start + duration;
        event.audio_source_id = result.source_id;
        event.detection_method = "legacy-project-migration";
        event.calibrated_confidence = confidence;
        event.intensity_rms = amplitude;
        result.source_events.push_back(event);
        ScoreNote note;
        note.id = "legacy-note-" + std::to_string(index + 1);
        note.source_event_ids.push_back(source_event_id);
        note.measured_onset_seconds = start;
        note.measured_offset_seconds = start + duration;
        note.onset_quarters = start * 2.0;
        note.duration_quarters = duration * 2.0;
        note.sounding_midi_pitch = midi;
        note.written_midi_pitch = midi;
        note.voice = 1;
        note.interpretation_evidence = confidence;
        note.calibrated_confidence = confidence;
        note.intensity_rms = amplitude;
        note.user_authored = false;
        note.provenance = {ScoreEditOrigin::Migration, "migration",
                           "Migrated from version 0 flat-note project", {}};
        note.rhythm.duration_quarters = note.duration_quarters;
        result.notes.push_back(note);
        result.tab_notes.push_back(make_tab_note(note));
        result.total_quarters = std::max(result.total_quarters,
                                         note.onset_quarters + note.duration_quarters);
    }
    result.history.push_back({1, ScoreEditOrigin::Migration, "migration",
                              "Migrated version 0 flat-note project", {}});
    return result;
}

} // namespace

ScoreDocument make_score_document(const ScoreInterpretation& interpretation,
                                 std::size_t hypothesis_index,
                                 std::string source_id,
                                 const ScoreMetadata& metadata,
                                 const GuitarTabOptions& tab_options) {
    if (hypothesis_index >= interpretation.hypotheses.size()) {
        throw std::out_of_range("Score hypothesis index is out of range.");
    }
    const auto& hypothesis = interpretation.hypotheses[hypothesis_index];
    ScoreDocument document;
    document.source_id = std::move(source_id);
    std::string score_identity = document.source_id + ":" + hypothesis.id;
    for (const auto& event : interpretation.source_events) {
        if (!event) continue;
        score_identity += ":" + event->id + ":" +
            std::to_string(event->midi_pitch) + ":" +
            std::to_string(event->onset_seconds) + ":" +
            std::to_string(event->offset_seconds);
    }
    document.id = stable_id("score-", score_identity);
    document.revision = 1;
    document.metadata = metadata;
    document.instrument.name = hypothesis.instrument;
    document.instrument.written_transposition_semitones =
        hypothesis.written_transposition_semitones;
    document.instrument.tuning.assign(tab_options.tuning.begin(), tab_options.tuning.end());
    document.instrument.capo_fret = tab_options.capo_fret;
    document.tempo_map = hypothesis.tempo_map;
    document.beats = hypothesis.beats;
    document.meter = hypothesis.meter;
    document.key = hypothesis.key;
    document.alternative_keys = hypothesis.alternative_keys;
    document.local_tonal_context = hypothesis.local_tonal_context;
    document.pickup_quarters = hypothesis.pickup_quarters;
    document.total_quarters = hypothesis.total_quarters;
    document.swing_ratio = hypothesis.swing_ratio;
    document.swing_confidence = hypothesis.swing_confidence;
    document.swing_inferred = hypothesis.swing_inferred;
    document.tempo_ambiguous = hypothesis.tempo_ambiguous;
    document.meter_ambiguous = hypothesis.meter_ambiguous;
    document.key_ambiguous = hypothesis.key_ambiguous;
    document.uncertainties = hypothesis.uncertainties;
    document.source_events.reserve(interpretation.source_events.size());
    for (const auto& event : interpretation.source_events) {
        if (event) {
            document.source_events.push_back(*event);
            if (document.source_sample_rate == 0) {
                document.source_sample_rate = event->source_sample_rate;
            }
        }
    }
    document.notes.reserve(hypothesis.notes.size());
    for (std::size_t index = 0; index < hypothesis.notes.size(); ++index) {
        const auto& interpreted = hypothesis.notes[index];
        if (interpreted.source_event_index >= document.source_events.size()) {
            throw std::invalid_argument("Interpreted score note refers to a missing source event.");
        }
        const auto& source = document.source_events[interpreted.source_event_index];
        ScoreNote note;
        const auto identity_seed = document.id + ":" + source.id + ":" + std::to_string(index);
        note.id = stable_id("note-", identity_seed);
        note.source_event_ids.push_back(source.id);
        note.measured_onset_seconds = source.onset_seconds;
        note.measured_offset_seconds = source.offset_seconds;
        note.onset_quarters = interpreted.onset_quarters;
        note.duration_quarters = interpreted.duration_quarters;
        note.onset_residual_seconds = interpreted.onset_residual_seconds;
        note.offset_residual_seconds = interpreted.offset_residual_seconds;
        note.voice = interpreted.voice;
        if (interpreted.chord_group) {
            note.chord_group_id = document.id + "-chord-" +
                                  std::to_string(*interpreted.chord_group);
        }
        note.sounding_midi_pitch = static_cast<int>(std::lround(source.midi_pitch));
        note.written_midi_pitch = interpreted.written_midi_pitch;
        note.spelling_step = interpreted.spelling_step;
        note.spelling_alter = interpreted.spelling_alter;
        note.spelling_octave = interpreted.spelling_octave;
        note.accidental_required = interpreted.accidental_required;
        note.rhythm = interpreted.rhythm;
        note.rhythm_alternatives = interpreted.rhythm_alternatives;
        note.interpretation_evidence = interpreted.confidence;
        note.calibrated_confidence = source.calibrated_confidence;
        note.intensity_rms = source.intensity_rms;
        note.raw_evidence = source.evidence;
        note.velocity = interpreted.velocity ? interpreted.velocity : source.velocity;
        note.articulation = interpreted.articulation ? interpreted.articulation : source.articulation;
        note.tie_start = interpreted.tie_start;
        note.tie_stop = interpreted.tie_stop;
        note.revision = document.revision;
        note.provenance = {ScoreEditOrigin::Inference, source.source_model,
                           "Initial interpretation", {}};
        document.notes.push_back(std::move(note));
    }
    document.rests.reserve(hypothesis.rests.size());
    for (std::size_t index = 0; index < hypothesis.rests.size(); ++index) {
        const auto& interpreted = hypothesis.rests[index];
        ScoreRest rest;
        rest.id = stable_id("rest-", document.id + ":" + std::to_string(index));
        rest.voice = interpreted.voice;
        rest.onset_quarters = interpreted.onset_quarters;
        rest.duration_quarters = interpreted.duration_quarters;
        rest.rhythm = interpreted.rhythm;
        rest.revision = document.revision;
        rest.provenance = {ScoreEditOrigin::Inference, "interpretation",
                           "Initial interpretation", {}};
        document.total_quarters = std::max(
            document.total_quarters, rest.onset_quarters + rest.duration_quarters);
        document.rests.push_back(std::move(rest));
    }
    sort_score(document);
    for (const auto& note : document.notes) {
        document.tab_notes.push_back(make_tab_note(note));
    }
    sort_score(document);
    document.history.push_back({document.revision, ScoreEditOrigin::Inference,
                                "transcriber", "Initial score interpretation", {}});
    validate_score_document(document);
    return document;
}

double score_position_seconds(const ScoreDocument& document, double quarter_position) {
    if (!std::isfinite(quarter_position) || quarter_position < 0.0) {
        throw std::invalid_argument("Score position must be finite and nonnegative.");
    }
    if (document.tempo_map.empty()) {
        return quarter_position * 60.0 / std::clamp(document.metadata.tempo_bpm, 1.0, 1000.0);
    }
    const TempoSegment* selected = &document.tempo_map.front();
    for (const auto& segment : document.tempo_map) {
        if (segment.start_quarters <= quarter_position) selected = &segment;
        else break;
    }
    const double bpm = std::clamp(selected->beats_per_minute, 1.0, 1000.0);
    return selected->start_seconds +
           (quarter_position - selected->start_quarters) * 60.0 / bpm;
}

double score_position_quarters(const ScoreDocument& document, double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        throw std::invalid_argument("Score time must be finite and nonnegative.");
    }
    if (document.tempo_map.empty()) {
        return seconds * std::clamp(document.metadata.tempo_bpm, 1.0, 1000.0) / 60.0;
    }
    const TempoSegment* selected = &document.tempo_map.front();
    for (const auto& segment : document.tempo_map) {
        if (segment.start_seconds <= seconds) selected = &segment;
        else break;
    }
    const double bpm = std::clamp(selected->beats_per_minute, 1.0, 1000.0);
    return selected->start_quarters +
           (seconds - selected->start_seconds) * bpm / 60.0;
}

std::vector<Note> score_notes(const ScoreDocument& document) {
    std::vector<const ScoreNote*> ordered;
    ordered.reserve(document.notes.size());
    for (const auto& note : document.notes) ordered.push_back(&note);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        if (left->onset_quarters != right->onset_quarters) {
            return left->onset_quarters < right->onset_quarters;
        }
        if (left->sounding_midi_pitch != right->sounding_midi_pitch) {
            return left->sounding_midi_pitch < right->sounding_midi_pitch;
        }
        return left->id < right->id;
    });
    std::vector<Note> result;
    result.reserve(ordered.size());
    for (const auto* note : ordered) {
        const double start = score_position_seconds(document, note->onset_quarters);
        const double end = score_position_seconds(
            document, note->onset_quarters + note->duration_quarters);
        const double confidence = note->calibrated_confidence.value_or(0.0);
        result.push_back({note->sounding_midi_pitch, start, std::max(0.0, end - start),
                          midi_note_name(note->sounding_midi_pitch),
                          std::clamp(confidence, 0.0, 1.0),
                          std::clamp(note->intensity_rms.value_or(0.0), 0.0, 1.0)});
    }
    return result;
}

ScoreInterpretation to_score_interpretation(const ScoreDocument& document) {
    validate_score_document(document);
    ScoreInterpretation result;
    result.source_events.reserve(document.source_events.size() + document.notes.size());
    std::unordered_map<std::string, std::size_t> event_indices;
    for (const auto& event : document.source_events) {
        event_indices.emplace(event.id, result.source_events.size());
        result.source_events.push_back(std::make_shared<const CanonicalNoteEvent>(event));
    }
    for (const auto& note : document.notes) {
        if (!note.source_event_ids.empty()) continue;
        CanonicalNoteEvent event;
        event.id = stable_id("manual-event-", document.id + ":" + note.id);
        event.midi_pitch = note.sounding_midi_pitch;
        event.onset_seconds = score_position_seconds(document, note.onset_quarters);
        event.offset_seconds = score_position_seconds(
            document, note.onset_quarters + note.duration_quarters);
        event.source_sample_rate = document.source_sample_rate;
        event.audio_source_id = document.source_id;
        event.source_model = "score-editor";
        event.detection_method = "user-authored";
        event.state = EventLifecycle::ManuallyCorrected;
        event.velocity = note.velocity;
        event.intensity_rms = note.intensity_rms;
        event.articulation = note.articulation;
        event_indices.emplace(event.id, result.source_events.size());
        result.source_events.push_back(
            std::make_shared<const CanonicalNoteEvent>(std::move(event)));
    }

    ScoreHypothesis hypothesis;
    hypothesis.id = document.id + "-revision-" + std::to_string(document.revision);
    hypothesis.tempo_map = document.tempo_map;
    hypothesis.beats = document.beats;
    hypothesis.meter = document.meter;
    hypothesis.key = document.key;
    hypothesis.alternative_keys = document.alternative_keys;
    hypothesis.local_tonal_context = document.local_tonal_context;
    hypothesis.pickup_quarters = document.pickup_quarters;
    hypothesis.total_quarters = document.total_quarters;
    hypothesis.swing_ratio = document.swing_ratio;
    hypothesis.swing_confidence = document.swing_confidence;
    hypothesis.swing_inferred = document.swing_inferred;
    hypothesis.tempo_ambiguous = document.tempo_ambiguous;
    hypothesis.meter_ambiguous = document.meter_ambiguous;
    hypothesis.key_ambiguous = document.key_ambiguous;
    hypothesis.uncertainties = document.uncertainties;
    hypothesis.instrument = document.instrument.name;
    hypothesis.written_transposition_semitones =
        document.instrument.written_transposition_semitones;
    std::unordered_map<std::string, std::size_t> chord_groups;
    for (const auto& note : document.notes) {
        std::size_t source_index = 0;
        if (!note.source_event_ids.empty()) {
            const auto found = event_indices.find(note.source_event_ids.front());
            if (found == event_indices.end()) {
                throw std::invalid_argument("Score note refers to a missing source event.");
            }
            source_index = found->second;
        } else {
            const auto id = stable_id("manual-event-", document.id + ":" + note.id);
            source_index = event_indices.at(id);
        }
        std::optional<std::size_t> chord_group;
        if (note.chord_group_id) {
            const auto [found, inserted] = chord_groups.emplace(
                *note.chord_group_id, chord_groups.size() + 1);
            (void)inserted;
            chord_group = found->second;
        }
        hypothesis.notes.push_back({
            source_index, note.onset_quarters, note.duration_quarters,
            note.onset_residual_seconds, note.offset_residual_seconds,
            note.voice, chord_group, note.written_midi_pitch,
            note.spelling_step, note.spelling_alter, note.spelling_octave,
            note.accidental_required, note.rhythm, note.rhythm_alternatives,
            note.interpretation_evidence, note.velocity, note.articulation,
            note.tie_start, note.tie_stop, note.sounding_midi_pitch});
    }
    for (const auto& rest : document.rests) {
        hypothesis.rests.push_back({rest.voice, rest.onset_quarters,
                                    rest.duration_quarters, rest.rhythm});
    }
    result.hypotheses.push_back(std::move(hypothesis));
    return result;
}

void validate_score_document(const ScoreDocument& document) {
    if (document.schema_version != score_schema_version) {
        throw std::invalid_argument("Unsupported score schema version.");
    }
    if (document.id.empty() || document.time_reference != "recording-relative-seconds" ||
        document.revision == 0 || document.history.empty() ||
        document.history.back().revision != document.revision) {
        throw std::invalid_argument("Score document identity or time reference is invalid.");
    }
    if (!std::isfinite(document.metadata.tempo_bpm) ||
        document.metadata.tempo_bpm < 1.0 || document.metadata.tempo_bpm > 1000.0 ||
        document.metadata.time_numerator <= 0 || document.metadata.time_numerator > 255 ||
        (document.metadata.time_denominator != 1 &&
         document.metadata.time_denominator != 2 &&
         document.metadata.time_denominator != 4 &&
         document.metadata.time_denominator != 8 &&
         document.metadata.time_denominator != 16) ||
        document.metadata.key_fifths < -7 || document.metadata.key_fifths > 7 ||
        !std::isfinite(document.metadata.tempo_confidence) ||
        document.metadata.tempo_confidence < 0.0 ||
        document.metadata.tempo_confidence > 1.0 ||
        !std::isfinite(document.metadata.meter_confidence) ||
        document.metadata.meter_confidence < 0.0 ||
        document.metadata.meter_confidence > 1.0 ||
        !std::isfinite(document.metadata.key_confidence) ||
        document.metadata.key_confidence < 0.0 ||
        document.metadata.key_confidence > 1.0 ||
        !std::isfinite(document.metadata.swing_ratio) ||
        document.metadata.swing_ratio < 0.5 ||
        document.metadata.swing_ratio > 3.0 ||
        !std::isfinite(document.metadata.swing_confidence) ||
        document.metadata.swing_confidence < 0.0 ||
        document.metadata.swing_confidence > 1.0 ||
        document.instrument.written_transposition_semitones < -48 ||
        document.instrument.written_transposition_semitones > 48 ||
        document.instrument.capo_fret < 0 || document.instrument.capo_fret > 24 ||
        document.instrument.tuning.empty() || document.instrument.tuning.size() > 12) {
        throw std::invalid_argument("Score metadata or instrument settings are invalid.");
    }
    for (const int pitch : document.instrument.tuning) {
        if (pitch < 0 || pitch > 127) {
            throw std::invalid_argument("Instrument tuning pitches must be valid MIDI values.");
        }
    }
    if (!std::isfinite(document.pickup_quarters) || document.pickup_quarters < 0.0 ||
        !std::isfinite(document.total_quarters) ||
        document.total_quarters < document.pickup_quarters ||
        !std::isfinite(document.swing_ratio) || document.swing_ratio < 0.5 ||
        document.swing_ratio > 3.0) {
        throw std::invalid_argument("Score timing metadata is invalid.");
    }
    if (document.meter.numerator < 1 || document.meter.numerator > 255 ||
        (document.meter.denominator != 1 && document.meter.denominator != 2 &&
         document.meter.denominator != 4 && document.meter.denominator != 8 &&
         document.meter.denominator != 16) ||
        document.meter.confidence < 0.0 || document.meter.confidence > 1.0 ||
        !std::isfinite(document.meter.confidence) ||
        document.key.fifths < -7 || document.key.fifths > 7 ||
        document.key.confidence < 0.0 || document.key.confidence > 1.0 ||
        !std::isfinite(document.key.confidence)) {
        throw std::invalid_argument("Score key or meter interpretation is invalid.");
    }
    for (const auto group : document.meter.grouping) {
        if (group <= 0) throw std::invalid_argument("Meter group sizes must be positive.");
    }
    if (document.source_events.size() > 1000000 ||
        document.notes.size() > 100000 ||
        document.rests.size() > 100000 ||
        document.tab_notes.size() > 100000) {
        throw std::length_error("Score document exceeds a supported element limit.");
    }
    std::unordered_set<std::string> source_ids;
    for (const auto& event : document.source_events) {
        if (event.id.empty() || !source_ids.insert(event.id).second ||
            !std::isfinite(event.midi_pitch) || event.midi_pitch < 0.0 ||
            event.midi_pitch > 127.0 || !std::isfinite(event.onset_seconds) ||
            !std::isfinite(event.offset_seconds) || event.onset_seconds < 0.0 ||
            event.offset_seconds < event.onset_seconds ||
            static_cast<int>(event.state) < 0 ||
            static_cast<int>(event.state) > static_cast<int>(EventLifecycle::Rejected) ||
            (event.calibrated_confidence &&
             (!std::isfinite(*event.calibrated_confidence) ||
              *event.calibrated_confidence < 0.0 || *event.calibrated_confidence > 1.0)) ||
            (event.intensity_rms &&
             (!std::isfinite(*event.intensity_rms) || *event.intensity_rms < 0.0)) ||
            (event.velocity && (*event.velocity < 0 || *event.velocity > 127))) {
            throw std::invalid_argument("Score source event is invalid or has a duplicate ID.");
        }
        for (const auto& evidence : event.evidence) validate_evidence(evidence);
    }
    std::unordered_set<std::string> ids;
    for (const auto& event : document.source_events) ids.insert(event.id);
    double latest_score_position = document.pickup_quarters;
    std::unordered_map<std::string, std::pair<std::size_t, std::size_t>> tie_groups;
    for (const auto& note : document.notes) {
        if (note.id.empty() || !ids.insert(note.id).second ||
            note.sounding_midi_pitch < 0 || note.sounding_midi_pitch > 127 ||
            note.written_midi_pitch < 0 || note.written_midi_pitch > 127 ||
            note.voice < 1 || !std::isfinite(note.measured_onset_seconds) ||
            !std::isfinite(note.measured_offset_seconds) ||
            note.measured_onset_seconds < 0.0 ||
            note.measured_offset_seconds < note.measured_onset_seconds ||
            !std::isfinite(note.onset_quarters) || note.onset_quarters < 0.0 ||
            !std::isfinite(note.duration_quarters) || note.duration_quarters <= 0.0 ||
            !std::isfinite(note.onset_residual_seconds) ||
            !std::isfinite(note.offset_residual_seconds) ||
            !std::isfinite(note.interpretation_evidence) ||
            note.interpretation_evidence < 0.0 ||
            note.interpretation_evidence > 1.0 ||
            (note.calibrated_confidence &&
             (!std::isfinite(*note.calibrated_confidence) ||
              *note.calibrated_confidence < 0.0 || *note.calibrated_confidence > 1.0)) ||
            (note.intensity_rms &&
             (!std::isfinite(*note.intensity_rms) || *note.intensity_rms < 0.0)) ||
            (note.spelling_step != "" &&
             (note.spelling_step.size() != 1 ||
              std::string("ABCDEFG").find(note.spelling_step) == std::string::npos ||
              note.spelling_alter < -2 || note.spelling_alter > 2 ||
              note.spelling_octave < -1 || note.spelling_octave > 9)) ||
            (note.velocity && (*note.velocity < 0 || *note.velocity > 127))) {
            throw std::invalid_argument("Score note is invalid or has a duplicate ID.");
        }
        if ((note.tie_start || note.tie_stop) && !note.tie_group_id) {
            throw std::invalid_argument("A tied note must have a tie-group ID.");
        }
        if (note.tie_group_id) {
            auto& tie = tie_groups[*note.tie_group_id];
            tie.first += note.tie_start ? 1U : 0U;
            tie.second += note.tie_stop ? 1U : 0U;
        }
        latest_score_position = std::max(
            latest_score_position, note.onset_quarters + note.duration_quarters);
        for (const auto& evidence : note.raw_evidence) validate_evidence(evidence);
        validate_rhythm(note.rhythm);
        for (const auto& rhythm : note.rhythm_alternatives) validate_rhythm(rhythm);
        for (const auto& source_id : note.source_event_ids) {
            if (!source_ids.contains(source_id)) {
                throw std::invalid_argument("Score note links to a missing source event.");
            }
        }
    }
    for (const auto& rest : document.rests) {
        if (rest.id.empty() || !ids.insert(rest.id).second || rest.voice < 1 ||
            !std::isfinite(rest.onset_quarters) || rest.onset_quarters < 0.0 ||
            !std::isfinite(rest.duration_quarters) || rest.duration_quarters <= 0.0) {
            throw std::invalid_argument("Score rest is invalid or has a duplicate ID.");
        }
        validate_rhythm(rest.rhythm);
        latest_score_position = std::max(
            latest_score_position, rest.onset_quarters + rest.duration_quarters);
    }
    if (document.total_quarters + 1e-9 < latest_score_position) {
        throw std::invalid_argument("Score total duration precedes a note or rest.");
    }
    for (const auto& [tie_id, counts] : tie_groups) {
        if (tie_id.empty() || counts.first == 0 || counts.second == 0 ||
            counts.first + counts.second < 2) {
            throw std::invalid_argument("Score tie group has no matching start and stop.");
        }
    }
    std::unordered_set<std::string> tab_note_ids;
    for (const auto& tab : document.tab_notes) {
        const auto* note = find_note(document, tab.notation_note_id);
        if (tab.id.empty() || !ids.insert(tab.id).second ||
            !tab_note_ids.insert(tab.notation_note_id).second || note == nullptr ||
            tab.string_number.has_value() != tab.fret.has_value()) {
            throw std::invalid_argument("Score tablature relationship is invalid or duplicated.");
        }
        if (tab.string_number) {
            if (*tab.string_number < 1 ||
                static_cast<std::size_t>(*tab.string_number) > document.instrument.tuning.size() ||
                *tab.fret < 0 || *tab.fret > 36 ||
                document.instrument.tuning[static_cast<std::size_t>(*tab.string_number - 1)] +
                    document.instrument.capo_fret + *tab.fret != note->sounding_midi_pitch) {
                throw std::invalid_argument("Score tablature assignment does not match sounding pitch.");
            }
        }
    }
    double previous_start_seconds = -1.0;
    double previous_start_quarters = -1.0;
    for (const auto& segment : document.tempo_map) {
        if (!std::isfinite(segment.start_seconds) || !std::isfinite(segment.end_seconds) ||
            !std::isfinite(segment.start_quarters) ||
            !std::isfinite(segment.beats_per_minute) ||
            segment.start_seconds < 0.0 || segment.start_quarters < 0.0 ||
            segment.end_seconds < segment.start_seconds ||
            segment.start_seconds < previous_start_seconds ||
            segment.start_quarters < previous_start_quarters ||
            segment.beats_per_minute <= 0.0 ||
            !std::isfinite(segment.confidence) ||
            segment.confidence < 0.0 || segment.confidence > 1.0) {
            throw std::invalid_argument("Score tempo map contains an invalid segment.");
        }
        previous_start_seconds = segment.start_seconds;
        previous_start_quarters = segment.start_quarters;
    }
    for (const auto& key : document.alternative_keys) {
        if (key.fifths < -7 || key.fifths > 7 ||
            !std::isfinite(key.confidence) || key.confidence < 0.0 || key.confidence > 1.0) {
            throw std::invalid_argument("Score key hypothesis is invalid.");
        }
    }
    for (const auto& region : document.local_tonal_context) {
        if (region.first_measure > region.end_measure ||
            region.key.fifths < -7 || region.key.fifths > 7 ||
            !std::isfinite(region.key.confidence) ||
            region.key.confidence < 0.0 || region.key.confidence > 1.0) {
            throw std::invalid_argument("Score tonal region is invalid.");
        }
    }
    for (const auto& beat : document.beats) {
        if (!std::isfinite(beat.time_seconds) || beat.time_seconds < 0.0 ||
            !std::isfinite(beat.quarter_position) || beat.quarter_position < 0.0 ||
            !std::isfinite(beat.confidence) || beat.confidence < 0.0 ||
            beat.confidence > 1.0) {
            throw std::invalid_argument("Score beat position is invalid.");
        }
    }
    std::uint64_t previous_revision = 0;
    for (const auto& record : document.history) {
        if (record.revision < previous_revision ||
            record.revision > document.revision) {
            throw std::invalid_argument("Score revision history is not monotonic.");
        }
        previous_revision = record.revision;
    }
}

ScoreEditor::ScoreEditor(ScoreDocument document) : document_(std::move(document)) {
    validate_score_document(document_);
}

const ScoreDocument& ScoreEditor::document() const noexcept {
    return document_;
}

ScoreTransactionResult ScoreEditor::apply(const ScoreTransaction& transaction) {
    if (transaction.base_revision != document_.revision) {
        throw std::logic_error("Score transaction conflicts with the current revision.");
    }
    if (transaction.commands.empty()) {
        throw std::invalid_argument("Score transaction must contain at least one command.");
    }
    ScoreDocument candidate = document_;
    const std::uint64_t next_revision = document_.revision + 1;
    for (const auto& command : transaction.commands) {
        apply_command(candidate, command, transaction, next_revision);
    }
    candidate.revision = next_revision;
    for (const auto& note : candidate.notes) {
        candidate.total_quarters = std::max(
            candidate.total_quarters, note.onset_quarters + note.duration_quarters);
    }
    for (const auto& rest : candidate.rests) {
        candidate.total_quarters = std::max(
            candidate.total_quarters, rest.onset_quarters + rest.duration_quarters);
    }
    candidate.total_quarters = std::max(candidate.total_quarters,
                                        candidate.pickup_quarters);
    candidate.history.push_back({next_revision, ScoreEditOrigin::User,
                                 transaction.actor, transaction.reason,
                                 transaction.command_id});
    sort_score(candidate);
    validate_score_document(candidate);
    undo_stack_.push_back({document_, candidate});
    if (undo_stack_.size() > 100) undo_stack_.erase(undo_stack_.begin());
    redo_stack_.clear();
    document_ = std::move(candidate);
    return {document_.revision, transaction.commands.size()};
}

std::uint64_t ScoreEditor::undo(std::uint64_t expected_revision) {
    if (expected_revision != document_.revision) {
        throw std::logic_error("Score undo conflicts with the current revision.");
    }
    if (undo_stack_.empty()) throw std::logic_error("No score edit is available to undo.");
    auto snapshot = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    const auto next_revision = document_.revision + 1;
    auto history = document_.history;
    document_ = std::move(snapshot.before);
    document_.revision = next_revision;
    document_.history = std::move(history);
    document_.history.push_back({next_revision, ScoreEditOrigin::Undo, "user",
                                 "Undo score transaction", {}});
    for (auto& note : document_.notes) {
        if (note.revision > next_revision) note.revision = next_revision;
    }
    sort_score(document_);
    validate_score_document(document_);
    redo_stack_.push_back(std::move(snapshot));
    return document_.revision;
}

std::uint64_t ScoreEditor::redo(std::uint64_t expected_revision) {
    if (expected_revision != document_.revision) {
        throw std::logic_error("Score redo conflicts with the current revision.");
    }
    if (redo_stack_.empty()) throw std::logic_error("No score edit is available to redo.");
    auto snapshot = std::move(redo_stack_.back());
    redo_stack_.pop_back();
    const auto next_revision = document_.revision + 1;
    auto history = document_.history;
    document_ = std::move(snapshot.after);
    document_.revision = next_revision;
    document_.history = std::move(history);
    document_.history.push_back({next_revision, ScoreEditOrigin::Redo, "user",
                                 "Redo score transaction", {}});
    for (auto& note : document_.notes) {
        if (note.revision > next_revision) note.revision = next_revision;
    }
    sort_score(document_);
    validate_score_document(document_);
    undo_stack_.push_back(std::move(snapshot));
    return document_.revision;
}

std::string serialize_score_document(const ScoreDocument& document) {
    validate_score_document(document);
    std::ostringstream output;
    output << "{\"schema_version\":" << document.schema_version << ",\"id\":";
    json_string(output, document.id);
    output << ",\"source_id\":";
    json_string(output, document.source_id);
    output << ",\"source_sample_rate\":" << document.source_sample_rate
           << ",\"time_reference\":";
    json_string(output, document.time_reference);
    output << ",\"revision\":" << document.revision << ",\"metadata\":{\"tempo_bpm\":";
    json_number(output, document.metadata.tempo_bpm);
    output << ",\"time_numerator\":" << document.metadata.time_numerator
           << ",\"time_denominator\":" << document.metadata.time_denominator
           << ",\"key_fifths\":" << document.metadata.key_fifths
           << ",\"minor_key\":" << (document.metadata.minor_key ? "true" : "false")
           << ",\"tempo_inferred\":" << (document.metadata.tempo_inferred ? "true" : "false")
           << ",\"meter_inferred\":" << (document.metadata.meter_inferred ? "true" : "false")
           << ",\"key_inferred\":" << (document.metadata.key_inferred ? "true" : "false")
           << ",\"tempo_confidence\":";
    json_number(output, document.metadata.tempo_confidence);
    output << ",\"meter_confidence\":";
    json_number(output, document.metadata.meter_confidence);
    output << ",\"key_confidence\":";
    json_number(output, document.metadata.key_confidence);
    output << ",\"swing_ratio\":";
    json_number(output, document.metadata.swing_ratio);
    output << ",\"swing_inferred\":" << (document.metadata.swing_inferred ? "true" : "false")
           << ",\"swing_confidence\":";
    json_number(output, document.metadata.swing_confidence);
    output << "},\"instrument\":{\"name\":";
    json_string(output, document.instrument.name);
    output << ",\"written_transposition_semitones\":"
           << document.instrument.written_transposition_semitones << ",\"tuning\":[";
    for (std::size_t i = 0; i < document.instrument.tuning.size(); ++i) {
        if (i) output << ',';
        output << document.instrument.tuning[i];
    }
    output << "],\"capo_fret\":" << document.instrument.capo_fret << "},\"source_events\":[";
    for (std::size_t i = 0; i < document.source_events.size(); ++i) {
        if (i) output << ',';
        write_event(output, document.source_events[i]);
    }
    output << "],\"tempo_map\":[";
    for (std::size_t i = 0; i < document.tempo_map.size(); ++i) {
        if (i) output << ',';
        const auto& segment = document.tempo_map[i];
        output << "{\"start_seconds\":";
        json_number(output, segment.start_seconds);
        output << ",\"end_seconds\":";
        json_number(output, segment.end_seconds);
        output << ",\"start_quarters\":";
        json_number(output, segment.start_quarters);
        output << ",\"beats_per_minute\":";
        json_number(output, segment.beats_per_minute);
        output << ",\"confidence\":";
        json_number(output, segment.confidence);
        output << ",\"user_constrained\":" << (segment.user_constrained ? "true" : "false") << '}';
    }
    output << "],\"beats\":[";
    for (std::size_t i = 0; i < document.beats.size(); ++i) {
        if (i) output << ',';
        const auto& beat = document.beats[i];
        output << "{\"time_seconds\":";
        json_number(output, beat.time_seconds);
        output << ",\"quarter_position\":";
        json_number(output, beat.quarter_position);
        output << ",\"confidence\":";
        json_number(output, beat.confidence);
        output << ",\"downbeat\":" << (beat.downbeat ? "true" : "false") << '}';
    }
    output << "],\"meter\":{\"numerator\":" << document.meter.numerator
           << ",\"denominator\":" << document.meter.denominator
           << ",\"grouping\":[";
    for (std::size_t i = 0; i < document.meter.grouping.size(); ++i) {
        if (i) output << ',';
        output << document.meter.grouping[i];
    }
    output << "],\"confidence\":";
    json_number(output, document.meter.confidence);
    output << ",\"user_constrained\":" << (document.meter.user_constrained ? "true" : "false")
           << "},\"key\":";
    write_key(output, document.key);
    output << ",\"alternative_keys\":[";
    for (std::size_t i = 0; i < document.alternative_keys.size(); ++i) {
        if (i) output << ',';
        write_key(output, document.alternative_keys[i]);
    }
    output << "],\"local_tonal_context\":[";
    for (std::size_t i = 0; i < document.local_tonal_context.size(); ++i) {
        if (i) output << ',';
        const auto& region = document.local_tonal_context[i];
        output << "{\"first_measure\":" << region.first_measure
               << ",\"end_measure\":" << region.end_measure << ",\"key\":";
        write_key(output, region.key);
        output << '}';
    }
    output << "],\"pickup_quarters\":";
    json_number(output, document.pickup_quarters);
    output << ",\"total_quarters\":";
    json_number(output, document.total_quarters);
    output << ",\"swing_ratio\":";
    json_number(output, document.swing_ratio);
    output << ",\"swing_confidence\":";
    json_number(output, document.swing_confidence);
    output << ",\"swing_inferred\":" << (document.swing_inferred ? "true" : "false")
           << ",\"tempo_ambiguous\":" << (document.tempo_ambiguous ? "true" : "false")
           << ",\"meter_ambiguous\":" << (document.meter_ambiguous ? "true" : "false")
           << ",\"key_ambiguous\":" << (document.key_ambiguous ? "true" : "false")
           << ",\"uncertainties\":[";
    for (std::size_t i = 0; i < document.uncertainties.size(); ++i) {
        if (i) output << ',';
        json_string(output, document.uncertainties[i]);
    }
    output << "],\"notes\":[";
    for (std::size_t i = 0; i < document.notes.size(); ++i) {
        if (i) output << ',';
        const auto& note = document.notes[i];
        output << "{\"id\":";
        json_string(output, note.id);
        output << ",\"source_event_ids\":[";
        for (std::size_t j = 0; j < note.source_event_ids.size(); ++j) {
            if (j) output << ',';
            json_string(output, note.source_event_ids[j]);
        }
        output << "],\"measured_onset_seconds\":";
        json_number(output, note.measured_onset_seconds);
        output << ",\"measured_offset_seconds\":";
        json_number(output, note.measured_offset_seconds);
        output << ",\"onset_quarters\":";
        json_number(output, note.onset_quarters);
        output << ",\"duration_quarters\":";
        json_number(output, note.duration_quarters);
        output << ",\"onset_residual_seconds\":";
        json_number(output, note.onset_residual_seconds);
        output << ",\"offset_residual_seconds\":";
        json_number(output, note.offset_residual_seconds);
        output << ",\"voice\":" << note.voice << ",\"chord_group_id\":";
        if (note.chord_group_id) json_string(output, *note.chord_group_id);
        else output << "null";
        output << ",\"sounding_midi_pitch\":" << note.sounding_midi_pitch
               << ",\"written_midi_pitch\":" << note.written_midi_pitch
               << ",\"spelling_step\":";
        json_string(output, note.spelling_step);
        output << ",\"spelling_alter\":" << note.spelling_alter
               << ",\"spelling_octave\":" << note.spelling_octave
               << ",\"accidental_required\":" << (note.accidental_required ? "true" : "false")
               << ",\"rhythm\":";
        write_rhythm(output, note.rhythm);
        output << ",\"rhythm_alternatives\":[";
        for (std::size_t j = 0; j < note.rhythm_alternatives.size(); ++j) {
            if (j) output << ',';
            write_rhythm(output, note.rhythm_alternatives[j]);
        }
        output << "],\"interpretation_evidence\":";
        json_number(output, note.interpretation_evidence);
        output << ",\"calibrated_confidence\":";
        if (note.calibrated_confidence) json_number(output, *note.calibrated_confidence);
        else output << "null";
        output << ",\"intensity_rms\":";
        if (note.intensity_rms) json_number(output, *note.intensity_rms);
        else output << "null";
        output << ",\"raw_evidence\":[";
        for (std::size_t j = 0; j < note.raw_evidence.size(); ++j) {
            if (j) output << ',';
            write_evidence(output, note.raw_evidence[j]);
        }
        output << "],\"velocity\":";
        if (note.velocity) output << *note.velocity;
        else output << "null";
        output << ",\"articulation\":";
        if (note.articulation) json_string(output, *note.articulation);
        else output << "null";
        output << ",\"tie_group_id\":";
        if (note.tie_group_id) json_string(output, *note.tie_group_id);
        else output << "null";
        output << ",\"tie_start\":" << (note.tie_start ? "true" : "false")
               << ",\"tie_stop\":" << (note.tie_stop ? "true" : "false")
               << ",\"user_authored\":" << (note.user_authored ? "true" : "false")
               << ",\"revision\":" << note.revision << ",\"provenance\":";
        write_provenance(output, note.provenance);
        output << '}';
    }
    output << "],\"rests\":[";
    for (std::size_t i = 0; i < document.rests.size(); ++i) {
        if (i) output << ',';
        const auto& rest = document.rests[i];
        output << "{\"id\":";
        json_string(output, rest.id);
        output << ",\"voice\":" << rest.voice << ",\"onset_quarters\":";
        json_number(output, rest.onset_quarters);
        output << ",\"duration_quarters\":";
        json_number(output, rest.duration_quarters);
        output << ",\"rhythm\":";
        write_rhythm(output, rest.rhythm);
        output << ",\"user_authored\":" << (rest.user_authored ? "true" : "false")
               << ",\"revision\":" << rest.revision << ",\"provenance\":";
        write_provenance(output, rest.provenance);
        output << '}';
    }
    output << "],\"tab_notes\":[";
    for (std::size_t i = 0; i < document.tab_notes.size(); ++i) {
        if (i) output << ',';
        const auto& tab = document.tab_notes[i];
        output << "{\"id\":";
        json_string(output, tab.id);
        output << ",\"notation_note_id\":";
        json_string(output, tab.notation_note_id);
        output << ",\"string_number\":";
        if (tab.string_number) output << *tab.string_number;
        else output << "null";
        output << ",\"fret\":";
        if (tab.fret) output << *tab.fret;
        else output << "null";
        output << ",\"manually_corrected\":"
               << (tab.manually_corrected ? "true" : "false")
               << ",\"revision\":" << tab.revision << '}';
    }
    output << "],\"history\":[";
    for (std::size_t i = 0; i < document.history.size(); ++i) {
        if (i) output << ',';
        const auto& record = document.history[i];
        output << "{\"revision\":" << record.revision << ",\"origin\":";
        json_string(output, origin_name(record.origin));
        output << ",\"actor\":";
        json_string(output, record.actor);
        output << ",\"reason\":";
        json_string(output, record.reason);
        output << ",\"command_id\":";
        json_string(output, record.command_id);
        output << '}';
    }
    output << "]}";
    if (!output) throw std::runtime_error("Failed to serialize score document.");
    return output.str();
}

ScoreDocument deserialize_score_document(std::string_view json) {
    const auto parsed = JsonParser(json).parse();
    const auto& fields = object(parsed, "score project");
    const auto* schema_version = optional_field(fields, "schema_version");
    ScoreDocument result;
    if (schema_version == nullptr) {
        const auto* legacy_version = optional_field(fields, "version");
        if (legacy_version == nullptr || integer(*legacy_version, "version") != 0) {
            throw std::invalid_argument("Score project has no supported schema version.");
        }
        result = migrate_legacy_document(fields);
    } else {
        const auto version = integer(*schema_version, "schema_version");
        if (version != score_schema_version) {
            throw std::invalid_argument("Unsupported score project schema version: " +
                                        std::to_string(version));
        }
        result = read_current_document(fields);
    }
    validate_score_document(result);
    return result;
}

} // namespace transcribe
