#include "transcribe/musicxml.hpp"
#include "transcribe/score.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

transcribe::ScoreDocument sample_score() {
    const std::vector<transcribe::Note> notes{
        {60, 0.1, 1.0, "C4", 0.8, 0.4},
        {64, 1.1, 0.5, "E4", 0.7, 0.3},
    };
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120.0;
    options.meter = transcribe::MeterConstraint{4, 4, {1, 1, 1, 1}};
    options.key_fifths = 0;
    options.minor_key = false;
    const auto interpretation = transcribe::interpret_notes(notes, options);
    const auto metadata =
        transcribe::metadata_for_hypothesis(interpretation.hypotheses.front());
    return transcribe::make_score_document(
        interpretation, 0, "fixture.wav", metadata);
}

std::string musicxml_note_id(const std::string& note_id, bool tablature = false) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = tablature ? "tab-score-note-" : "score-note-";
    for (const unsigned char byte : note_id) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

} // namespace

TEST_CASE("canonical score project round trips source evidence and notation relationships",
          "[score][serialization]") {
    auto original = sample_score();
    REQUIRE(original.schema_version == transcribe::score_schema_version);
    REQUIRE(original.notes.size() == 2);
    REQUIRE(original.tab_notes.size() == 2);
    REQUIRE(original.notes.front().source_event_ids.size() == 1);
    REQUIRE(original.notes.front().measured_onset_seconds == Catch::Approx(0.1));

    const auto json = transcribe::serialize_score_document(original);
    const auto restored = transcribe::deserialize_score_document(json);
    REQUIRE(restored.id == original.id);
    REQUIRE(restored.revision == original.revision);
    REQUIRE(restored.source_events.size() == original.source_events.size());
    REQUIRE(restored.notes.size() == original.notes.size());
    REQUIRE(restored.notes.front().id == original.notes.front().id);
    REQUIRE(restored.notes.front().source_event_ids ==
            original.notes.front().source_event_ids);
    REQUIRE(restored.notes.front().sounding_midi_pitch == 60);
    REQUIRE(restored.notes.front().measured_onset_seconds == Catch::Approx(0.1));
    REQUIRE(restored.tab_notes.front().notation_note_id ==
            restored.notes.front().id);
    REQUIRE(restored.instrument.tuning == original.instrument.tuning);
    REQUIRE(restored.tempo_map.size() == original.tempo_map.size());
    REQUIRE(transcribe::score_position_seconds(restored, 2.0) ==
            Catch::Approx(1.0));
    REQUIRE(transcribe::score_position_quarters(restored, 1.0) ==
            Catch::Approx(2.0));
}

TEST_CASE("legacy note confidence remains available without becoming calibrated",
          "[score][review]") {
    const auto document = sample_score();
    REQUIRE_FALSE(document.notes.front().calibrated_confidence);
    const auto notes = transcribe::score_notes(document);
    REQUIRE(notes.front().confidence == Catch::Approx(0.8));
}

TEST_CASE("review decisions are revisioned, serialized, and undoable",
          "[score][review][serialization]") {
    transcribe::ScoreEditor editor(sample_score());
    const auto note_id = editor.document().notes.front().id;

    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.actor = "review-ui";
    transaction.reason = "Accept an evidence review suggestion";
    transaction.command_id = "review-1";
    transaction.commands.emplace_back(transcribe::RecordReviewDecision{
        "suggestion-1", note_id,
        transcribe::ReviewDecisionOutcome::Accepted, "Applied manually"});
    editor.apply(transaction);

    REQUIRE(editor.document().review_decisions.size() == 1);
    REQUIRE(editor.document().review_decisions.front().revision ==
            editor.document().revision);
    const auto restored = transcribe::deserialize_score_document(
        transcribe::serialize_score_document(editor.document()));
    REQUIRE(restored.review_decisions.size() == 1);
    REQUIRE(restored.review_decisions.front().suggestion_id == "suggestion-1");
    REQUIRE(restored.review_decisions.front().outcome ==
            transcribe::ReviewDecisionOutcome::Accepted);

    editor.undo(editor.document().revision);
    REQUIRE(editor.document().review_decisions.empty());
    editor.redo(editor.document().revision);
    REQUIRE(editor.document().review_decisions.size() == 1);
}

TEST_CASE("review decisions reject invalid and duplicate transactions",
          "[score][review][editing]") {
    transcribe::ScoreEditor editor(sample_score());
    const auto note_id = editor.document().notes.front().id;

    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.reason = "Record a review result";
    transaction.commands.emplace_back(transcribe::RecordReviewDecision{
        "suggestion-1", note_id,
        transcribe::ReviewDecisionOutcome::Rejected, "Dismissed"});
    transaction.commands.emplace_back(transcribe::RecordReviewDecision{
        "suggestion-1", note_id,
        transcribe::ReviewDecisionOutcome::Accepted, "Duplicate"});
    REQUIRE_THROWS_AS(editor.apply(transaction), std::invalid_argument);
    REQUIRE(editor.document().review_decisions.empty());

    transaction.commands.resize(1);
    std::get<transcribe::RecordReviewDecision>(transaction.commands.front())
        .note_id = "missing-note";
    REQUIRE_THROWS_AS(editor.apply(transaction), std::invalid_argument);
    REQUIRE(editor.document().review_decisions.empty());
}

TEST_CASE("score note IDs remain distinct from linked source event IDs",
          "[score][identity]") {
    auto document = sample_score();
    auto& note = document.notes.front();
    REQUIRE(note.source_event_ids.size() == 1);
    REQUIRE(note.id != note.source_event_ids.front());
    REQUIRE_NOTHROW(transcribe::validate_score_document(document));

    note.id = note.source_event_ids.front();
    REQUIRE_THROWS_AS(
        transcribe::validate_score_document(document), std::invalid_argument);
}

TEST_CASE("score transactions preserve measured evidence and support undo redo and conflicts",
          "[score][editing]") {
    auto document = sample_score();
    const auto original_note_id = document.notes.front().id;
    const auto original_onset = document.notes.front().measured_onset_seconds;
    const auto original_event_onset = document.source_events.front().onset_seconds;
    transcribe::ScoreEditor editor(std::move(document));

    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.actor = "test-user";
    transaction.reason = "edit note and tab";
    transaction.command_id = "edit-1";
    transaction.commands.emplace_back(
        transcribe::ChangeScorePitch{original_note_id, 64});
    transaction.commands.emplace_back(
        transcribe::ChangeScoreFingering{original_note_id, 1, 0});
    transaction.commands.emplace_back(
        transcribe::ReinterpretScoreRhythm{original_note_id, 0.25, 1.5,
            transcribe::RhythmValue{2, 0, 0, 0, 1.5, 0.0, true}});
    const auto applied = editor.apply(transaction);
    REQUIRE(applied.revision == 2);
    REQUIRE(editor.document().notes.front().sounding_midi_pitch == 64);
    REQUIRE(editor.document().notes.front().written_midi_pitch == 64);
    REQUIRE(editor.document().notes.front().onset_quarters == Catch::Approx(0.25));
    REQUIRE(editor.document().notes.front().duration_quarters == Catch::Approx(1.5));
    REQUIRE(editor.document().notes.front().measured_onset_seconds ==
            Catch::Approx(original_onset));
    REQUIRE(editor.document().source_events.front().onset_seconds ==
            Catch::Approx(original_event_onset));
    const auto assigned = std::find_if(
        editor.document().tab_notes.begin(), editor.document().tab_notes.end(),
        [&](const auto& tab) { return tab.notation_note_id == original_note_id; });
    REQUIRE(assigned != editor.document().tab_notes.end());
    REQUIRE(assigned->string_number == 1);
    REQUIRE(assigned->fret == 0);
    REQUIRE(assigned->manually_corrected);
    REQUIRE_THROWS_AS(editor.apply(transaction), std::logic_error);

    REQUIRE(editor.undo(editor.document().revision) == 3);
    REQUIRE(editor.document().notes.front().sounding_midi_pitch == 60);
    REQUIRE(editor.redo(editor.document().revision) == 4);
    REQUIRE(editor.document().notes.front().sounding_midi_pitch == 64);
    REQUIRE_THROWS_AS(editor.undo(2), std::logic_error);
}

TEST_CASE("score context edits preserve an inferred tempo map unless tempo changes",
          "[score][editing][tempo]") {
    auto document = sample_score();
    document.tempo_map = {
        {0.0, 1.0, 0.0, 120.0, 0.8, false},
        {1.0, 3.0, 2.0, 90.0, 0.6, false},
    };
    const auto original_tempo_map = document.tempo_map;
    transcribe::ScoreEditor editor(std::move(document));

    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.reason = "change score context without changing tempo";
    transaction.command_id = "context-1";
    transaction.commands.emplace_back(transcribe::UpdateScoreContext{
        editor.document().metadata, editor.document().instrument, 0.0, false});
    editor.apply(transaction);

    REQUIRE(editor.document().tempo_map.size() == original_tempo_map.size());
    REQUIRE(editor.document().tempo_map[1].beats_per_minute == Catch::Approx(90.0));
    REQUIRE(editor.document().tempo_map[1].start_quarters == Catch::Approx(2.0));
}

TEST_CASE("score commands split, merge, insert, and delete canonical elements",
          "[score][editing]") {
    auto document = sample_score();
    auto first = document.notes.front();
    first.duration_quarters = 2.0;
    first.rhythm.duration_quarters = 2.0;
    document.notes.front() = first;
    document.total_quarters = std::max(document.total_quarters, 2.0);
    transcribe::ScoreEditor editor(std::move(document));
    const auto inferred_rest_count = editor.document().rests.size();

    transcribe::ScoreTransaction split;
    split.base_revision = editor.document().revision;
    split.reason = "split note";
    split.command_id = "split-1";
    split.commands.emplace_back(
        transcribe::SplitScoreNote{first.id, 1.0, "note-second-half"});
    editor.apply(split);
    REQUIRE(editor.document().notes.size() == 3);
    const auto first_piece = std::find_if(
        editor.document().notes.begin(), editor.document().notes.end(),
        [&](const auto& note) { return note.id == first.id; });
    const auto second_piece = std::find_if(
        editor.document().notes.begin(), editor.document().notes.end(),
        [](const auto& note) { return note.id == "note-second-half"; });
    REQUIRE(first_piece != editor.document().notes.end());
    REQUIRE(second_piece != editor.document().notes.end());
    REQUIRE(first_piece->tie_start);
    REQUIRE(second_piece->tie_stop);
    REQUIRE(first_piece->tie_group_id == second_piece->tie_group_id);

    transcribe::ScoreTransaction merge;
    merge.base_revision = editor.document().revision;
    merge.reason = "merge tied notes";
    merge.command_id = "merge-1";
    merge.commands.emplace_back(
        transcribe::MergeScoreNotes{first.id, "note-second-half", "note-merged"});
    editor.apply(merge);
    REQUIRE(editor.document().notes.size() == 2);
    const auto merged = std::find_if(
        editor.document().notes.begin(), editor.document().notes.end(),
        [](const auto& note) { return note.id == "note-merged"; });
    REQUIRE(merged != editor.document().notes.end());
    REQUIRE(merged->duration_quarters == Catch::Approx(2.0));
    REQUIRE(merged->source_event_ids.size() == 1);

    transcribe::ScoreTransaction add;
    add.base_revision = editor.document().revision;
    add.reason = "add a note and rest";
    add.command_id = "add-1";
    transcribe::ScoreNote inserted;
    inserted.id = "manual-note-1";
    inserted.sounding_midi_pitch = 67;
    inserted.written_midi_pitch = 67;
    inserted.onset_quarters = 3.0;
    inserted.duration_quarters = 0.5;
    inserted.voice = 1;
    inserted.rhythm.duration_quarters = 0.5;
    add.commands.emplace_back(transcribe::InsertScoreNote{inserted});
    transcribe::ScoreRest rest;
    rest.id = "manual-rest-1";
    rest.onset_quarters = 3.5;
    rest.duration_quarters = 0.5;
    rest.rhythm.duration_quarters = 0.5;
    add.commands.emplace_back(transcribe::InsertScoreRest{rest});
    editor.apply(add);
    REQUIRE(editor.document().notes.size() == 3);
    REQUIRE(editor.document().rests.size() == inferred_rest_count + 1);
    const auto inserted_note = std::find_if(
        editor.document().notes.begin(), editor.document().notes.end(),
        [](const auto& note) { return note.id == "manual-note-1"; });
    REQUIRE(inserted_note != editor.document().notes.end());
    REQUIRE(inserted_note->user_authored);

    transcribe::ScoreTransaction remove;
    remove.base_revision = editor.document().revision;
    remove.reason = "delete rest";
    remove.command_id = "delete-1";
    remove.commands.emplace_back(
        transcribe::DeleteScoreElement{"manual-rest-1"});
    editor.apply(remove);
    REQUIRE(editor.document().rests.size() == inferred_rest_count);
}

TEST_CASE("score migration accepts version zero and rejects unknown future schemas",
          "[score][serialization]") {
    const auto migrated = transcribe::deserialize_score_document(
        R"({"version":0,"notes":[{"midi":60,"start":0.5,"duration":0.25,"confidence":0.7}]})");
    REQUIRE(migrated.schema_version == transcribe::score_schema_version);
    REQUIRE(migrated.notes.size() == 1);
    REQUIRE(migrated.notes.front().provenance.origin ==
            transcribe::ScoreEditOrigin::Migration);
    REQUIRE(migrated.notes.front().measured_onset_seconds ==
            Catch::Approx(0.5));
    REQUIRE_FALSE(migrated.notes.front().calibrated_confidence);
    REQUIRE(migrated.notes.front().raw_evidence.size() == 1);
    REQUIRE(migrated.notes.front().raw_evidence.front().strength_semantics ==
            transcribe::EvidenceSemantics::HeuristicScore);
    REQUIRE_THROWS_AS(transcribe::deserialize_score_document(
        R"({"schema_version":999})"), std::invalid_argument);
    REQUIRE_THROWS_AS(transcribe::deserialize_score_document(
        R"({"schema_version":1,"schema_version":1})"), std::invalid_argument);
}

TEST_CASE("score fingering rejects assignments that change sounding pitch",
          "[score][editing]") {
    transcribe::ScoreEditor editor(sample_score());
    const auto note_id = editor.document().notes.front().id;
    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.reason = "invalid fret";
    transaction.commands.emplace_back(
        transcribe::ChangeScoreFingering{note_id, 1, 1});
    REQUIRE_THROWS_AS(editor.apply(transaction), std::invalid_argument);
    REQUIRE(editor.document().revision == 1);
    REQUIRE(editor.document().notes.front().sounding_midi_pitch == 60);
}

TEST_CASE("MusicXML and MIDI export the same accepted score revision",
          "[score][export]") {
    transcribe::ScoreEditor editor(sample_score());
    const auto note_id = editor.document().notes.front().id;
    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.reason = "change accepted pitch";
    transaction.command_id = "pitch-1";
    transaction.commands.emplace_back(
        transcribe::ChangeScorePitch{note_id, 62});
    editor.apply(transaction);

    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto base = std::filesystem::temp_directory_path() /
        ("transcriber-score-" + std::to_string(suffix));
    const auto xml_path = base.string() + ".musicxml";
    const auto midi_path = base.string() + ".mid";
    std::string rendered_musicxml;
    transcribe::write_musicxml(xml_path, editor.document(), false,
                               &rendered_musicxml);
    transcribe::write_midi(midi_path, editor.document());

    std::ifstream xml_input(xml_path, std::ios::binary);
    const std::string xml{std::istreambuf_iterator<char>(xml_input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(xml.find("<step>D</step>") != std::string::npos);
    REQUIRE(xml.find("<octave>4</octave>") != std::string::npos);
    REQUIRE(xml == rendered_musicxml);
    REQUIRE(xml.find("id=\"" + musicxml_note_id(note_id) + "\"") !=
            std::string::npos);
    const auto tab_part = xml.find("<part id=\"P2\">");
    REQUIRE(tab_part != std::string::npos);
    REQUIRE(xml.substr(tab_part).find("id=\"" +
            musicxml_note_id(note_id, true) + "\"") != std::string::npos);
    REQUIRE(xml.substr(tab_part).find("<step>D</step>") != std::string::npos);

    std::ifstream midi_input(midi_path, std::ios::binary);
    const std::string midi{std::istreambuf_iterator<char>(midi_input),
                           std::istreambuf_iterator<char>()};
    bool midi_contains_edited_pitch = false;
    for (std::size_t index = 0; index + 1 < midi.size(); ++index) {
        if (static_cast<unsigned char>(midi[index]) == 0x90 &&
            static_cast<unsigned char>(midi[index + 1]) == 62) {
            midi_contains_edited_pitch = true;
            break;
        }
    }
    REQUIRE(midi_contains_edited_pitch);
    std::error_code error;
    std::filesystem::remove(xml_path, error);
    std::filesystem::remove(midi_path, error);
}

TEST_CASE("MusicXML score IDs survive bar-crossing tie fragments",
          "[score][export]") {
    auto score = sample_score();
    auto& note = score.notes.front();
    note.onset_quarters = 3.5;
    note.duration_quarters = 2.0;
    note.rhythm.denominator = 0;
    note.rhythm.dots = 0;
    note.rhythm.tuplet_actual = 0;
    note.rhythm.tuplet_normal = 0;
    note.rhythm.duration_quarters = 2.0;
    note.rhythm.fit_error_quarters = 0.0;
    note.rhythm.symbolic = false;
    score.total_quarters = 5.5;

    const auto xml_path = std::filesystem::temp_directory_path() /
        ("transcriber-score-fragment-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch().count()) +
         ".musicxml");
    std::string xml;
    transcribe::write_musicxml(xml_path, score, false, &xml);
    REQUIRE(xml.find("id=\"" + musicxml_note_id(note.id) + "\"") !=
            std::string::npos);
    REQUIRE(xml.find("id=\"" + musicxml_note_id(note.id) +
                     "-continuation-1\"") != std::string::npos);
    const auto tab_part = xml.find("<part id=\"P2\">");
    REQUIRE(tab_part != std::string::npos);
    REQUIRE(xml.substr(tab_part).find("id=\"" +
            musicxml_note_id(note.id, true) + "\"") != std::string::npos);
    REQUIRE(xml.substr(tab_part).find("id=\"" +
            musicxml_note_id(note.id, true) +
                                     "-continuation-1\"") !=
            std::string::npos);
    std::error_code error;
    std::filesystem::remove(xml_path, error);
}

TEST_CASE("ASCII tablature export uses assignments from the canonical score revision",
          "[score][export][tablature]") {
    auto document = sample_score();
    const auto note_id = document.notes.front().id;
    transcribe::ScoreEditor editor(std::move(document));

    transcribe::ScoreTransaction transaction;
    transaction.base_revision = editor.document().revision;
    transaction.reason = "set a canonical fingering";
    transaction.command_id = "tab-export-1";
    transaction.commands.emplace_back(
        transcribe::ChangeScorePitch{note_id, 64});
    transaction.commands.emplace_back(
        transcribe::ChangeScoreFingering{note_id, 1, 0});
    editor.apply(transaction);

    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("transcriber-score-" + std::to_string(suffix) + ".tab");
    transcribe::write_guitar_tab(path, editor.document());

    std::ifstream input(path, std::ios::binary);
    const std::string tab{std::istreambuf_iterator<char>(input),
                          std::istreambuf_iterator<char>()};
    REQUIRE(tab.find("E4|-0") != std::string::npos);
    REQUIRE(tab.find("string 1 fret 0 (manual)") != std::string::npos);
    std::error_code error;
    std::filesystem::remove(path, error);
}
