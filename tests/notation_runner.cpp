#include "transcribe/musicxml.hpp"
#include "transcribe/score.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    try {
        if (argc != 2 && argc != 3) throw std::invalid_argument("Expected an output file prefix.");
        double bpm{}, strength{}, minimum_evidence{}, pickup{};
        int numerator{}, denominator{}, fifths{};
        std::string mode;
        if (!(std::cin >> bpm >> numerator >> denominator >> fifths >> mode >>
              strength >> minimum_evidence >> pickup)) {
            throw std::invalid_argument("Invalid passage header.");
        }
        transcribe::InterpretationOptions options;
        if (bpm != 0) options.tempo_bpm = bpm;
        options.meter = transcribe::MeterConstraint{numerator, denominator, {}};
        options.key_fifths = fifths;
        options.infer_local_keys = false;
        options.quantization_mode = transcribe::parse_quantization_mode(mode);
        options.quantization_strength = strength;
        options.minimum_rhythm_evidence = minimum_evidence;
        if (argc == 3 && std::string(argv[2]) == "guitar-written-octave") {
            options.instrument = "Guitar";
            options.written_transposition_semitones = 12;
        }
        if (pickup > 0) options.pickup_quarters = pickup;
        std::vector<transcribe::CanonicalNoteEvent> events;
        int midi{};
        while (std::cin >> midi) {
            double onset{}, offset{};
            if (!(std::cin >> onset >> offset)) throw std::invalid_argument("Invalid passage note.");
            transcribe::CanonicalNoteEvent event;
            event.id = "reference-" + std::to_string(events.size());
            event.midi_pitch = midi;
            event.onset_seconds = onset;
            event.offset_seconds = offset;
            event.source_sample_rate = 48000;
            event.state = transcribe::EventLifecycle::Stabilized;
            events.push_back(std::move(event));
        }
        if (!std::cin.eof()) throw std::invalid_argument("Invalid trailing passage input.");
        auto interpretation = transcribe::interpret_note_events(events, options);
        transcribe::GuitarTabOptions tab_options;
        if (argc == 3) {
            const std::string fixture(argv[2]);
            if (fixture == "mid-note-tempo") {
                interpretation.hypotheses.front().tempo_map = {
                    {0, .75, 0, 120, 1, true}, {.75, 2, 1.5, 90, 1, true}};
            } else if (fixture == "unspelled") {
                for (auto& note : interpretation.hypotheses.front().notes) {
                    note.spelling_step.clear();
                    note.accidental_required = false;
                }
            } else if (fixture == "double-accidentals") {
                auto& notes = interpretation.hypotheses.front().notes;
                if (notes.size() != 2) throw std::invalid_argument("Expected two double accidental notes.");
                notes[0].spelling_step = "F";
                notes[0].spelling_alter = 2;
                notes[0].spelling_octave = 4;
                notes[1].spelling_step = "B";
                notes[1].spelling_alter = -2;
                notes[1].spelling_octave = 4;
            } else if (fixture == "unequal-chord") {
                for (auto& note : interpretation.hypotheses.front().notes) {
                    note.voice = 1;
                    note.chord_group = 0;
                }
            } else if (fixture == "partial-tuplet") {
                auto& hypothesis = interpretation.hypotheses.front();
                hypothesis.rests.clear();
                for (std::size_t index = 0; index < hypothesis.notes.size(); ++index) {
                    auto& note = hypothesis.notes[index];
                    note.onset_quarters = static_cast<double>(index) / 3;
                    note.duration_quarters = 1.0 / 3;
                    note.onset_residual_seconds = 0;
                    note.offset_residual_seconds = 0;
                    note.rhythm = {8, 0, 3, 2, 1.0 / 3, 0, true};
                }
            } else if (fixture.starts_with("guitar-")) {
                if (fixture == "guitar-drop-d") tab_options.tuning[5] = 38;
                else if (fixture == "guitar-dadgad") tab_options.tuning = {62, 57, 55, 50, 45, 38};
                else if (fixture == "guitar-open-d") tab_options.tuning = {62, 57, 54, 50, 45, 38};
                else if (fixture == "guitar-open-g") tab_options.tuning = {62, 59, 55, 50, 43, 38};
                else if (fixture == "guitar-custom") tab_options.tuning = {67, 60, 55, 48, 43, 36};
                else if (fixture == "guitar-seven") tab_options.string_tuning = {64, 59, 55, 50, 45, 40, 35};
                else if (fixture == "guitar-eight") tab_options.string_tuning = {64, 59, 55, 50, 45, 40, 35, 30};
                else if (fixture == "guitar-four-reentrant") tab_options.string_tuning = {69, 64, 60, 67};
                else if (fixture == "guitar-twelve") tab_options.string_tuning = {64, 64, 59, 59, 55, 67, 50, 62, 45, 57, 40, 52};
                else if (fixture == "guitar-capo") tab_options.capo_fret = 2;
                else if (fixture == "guitar-high") tab_options.maximum_fret = 36;
                else if (fixture == "guitar-slide") {
                    tab_options.position_overrides = {{0, 1, 0, true}, {1, 1, 5, true}};
                    tab_options.suggest_legato_marks = true;
                } else if (fixture != "guitar-standard" && fixture != "guitar-edit-pitch" &&
                           fixture != "guitar-written-octave") {
                    throw std::invalid_argument("Unknown guitar export fixture.");
                }
            } else {
                throw std::invalid_argument("Unknown exporter fixture.");
            }
        }
        const auto& hypothesis = interpretation.hypotheses.front();
        auto document = transcribe::make_score_document(
            interpretation, 0, "annotated-reference", transcribe::metadata_for_hypothesis(hypothesis), tab_options);
        const bool guitar = argc == 3 && std::string(argv[2]).starts_with("guitar-");
        if (!guitar) {
            const auto fingering = transcribe::assign_guitar_tab_fingerings(
                transcribe::score_notes(document), tab_options);
            for (const auto& position : fingering.positions) {
                if (position.string_number < 1) continue;
                auto& tab = document.tab_notes[position.note_index];
                tab.string_number = position.string_number;
                tab.fret = position.fret;
            }
        }
        if (guitar) {
            const auto notes = transcribe::score_notes(document);
            const auto fingering = transcribe::assign_guitar_tab_fingerings(notes, tab_options);
            for (const auto& position : fingering.positions) {
                if (position.string_number < 1) continue;
                auto& tab = document.tab_notes[position.note_index];
                tab.string_number = position.string_number;
                tab.fret = position.fret;
                tab.manually_corrected = position.manually_corrected;
            }
            transcribe::write_guitar_tab(std::string(argv[1]) + ".raw.tab.txt", notes, tab_options, fingering);
            if (std::string(argv[2]) == "guitar-edit-pitch") {
                transcribe::ScoreEditor editor(document);
                transcribe::ScoreTransaction edit;
                edit.base_revision = document.revision;
                edit.commands = {transcribe::ChangeScorePitch{document.notes[0].id, 65}};
                editor.apply(edit);
                document = editor.document();
            }
            transcribe::write_guitar_tab(std::string(argv[1]) + ".tab.txt", document, tab_options);
        }
        const auto json = transcribe::serialize_score_document(document);
        const auto restored = transcribe::deserialize_score_document(json);
        const std::filesystem::path prefix(argv[1]);
        transcribe::write_musicxml(prefix.string() + ".musicxml", interpretation, 0, tab_options);
        transcribe::write_musicxml(prefix.string() + ".roundtrip.musicxml", restored);
        std::ofstream output(prefix.string() + ".json");
        output << json;
        if (!output) throw std::runtime_error("Could not write passage score JSON.");
    } catch (const std::exception& error) {
        std::cerr << "notation-runner: " << error.what() << '\n';
        return 1;
    }
}
