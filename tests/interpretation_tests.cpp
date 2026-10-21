#include "transcribe/interpretation.hpp"
#include "transcribe/musicxml.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

transcribe::CanonicalNoteEvent note_event(int midi, double onset, double offset,
                                          std::string id = {}) {
    transcribe::CanonicalNoteEvent event;
    event.id = id.empty() ? "event-" + std::to_string(midi) + "-" +
                                std::to_string(onset) : std::move(id);
    event.midi_pitch = midi;
    event.onset_seconds = onset;
    event.offset_seconds = offset;
    event.source_sample_rate = 44100;
    event.onset_sample = static_cast<std::uint64_t>(std::llround(onset * 44100));
    event.offset_sample = static_cast<std::uint64_t>(std::llround(offset * 44100));
    event.source_model = "fixture";
    event.detection_method = "deterministic-fixture";
    event.state = transcribe::EventLifecycle::Stabilized;
    event.calibrated_confidence = 0.8;
    return event;
}

std::vector<transcribe::CanonicalNoteEvent> beat_fixture(int numerator, int denominator,
                                                          double beat_seconds,
                                                          std::size_t count = 12) {
    (void)numerator;
    (void)denominator;
    std::vector<transcribe::CanonicalNoteEvent> result;
    for (std::size_t i = 0; i < count; ++i) {
        const double onset = static_cast<double>(i) * beat_seconds;
        result.push_back(note_event(60 + static_cast<int>(i % 7), onset,
                                    onset + beat_seconds * 0.8));
    }
    return result;
}

std::string render_musicxml(const transcribe::ScoreInterpretation& interpretation,
                            bool preserve_expressive_timing = false) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("transcriber-interpretation-" + std::to_string(suffix) + ".musicxml");
    transcribe::write_musicxml(path, interpretation, 0, {}, preserve_expressive_timing);
    std::ifstream input(path, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
    std::error_code error;
    std::filesystem::remove(path, error);
    return contents;
}

} // namespace

TEST_CASE("interpretation preserves raw timing and honors user constraints", "[interpretation]") {
    auto events = beat_fixture(6, 8, 0.3, 8);
    const auto original_onset = events[2].onset_seconds;
    const auto original_offset = events[2].offset_seconds;
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 100;
    options.meter = transcribe::MeterConstraint{6, 8, {3, 3}};
    options.key_fifths = -2;
    options.minor_key = true;
    options.pickup_quarters = 1.0;
    options.instrument = "clarinet";
    options.written_transposition_semitones = 2;

    const auto result = transcribe::interpret_note_events(events, options);
    REQUIRE_FALSE(result.hypotheses.empty());
    const auto& score = result.hypotheses.front();
    REQUIRE(score.tempo_map.front().beats_per_minute == 100);
    REQUIRE(score.tempo_map.front().user_constrained);
    REQUIRE(score.meter.numerator == 6);
    REQUIRE(score.meter.denominator == 8);
    REQUIRE(score.meter.user_constrained);
    REQUIRE(score.key.fifths == -2);
    REQUIRE(score.key.minor);
    REQUIRE(score.key.user_constrained);
    REQUIRE(score.pickup_quarters == 1.0);
    REQUIRE(score.instrument == "clarinet");
    REQUIRE(score.notes[2].written_midi_pitch == 64);
    REQUIRE(result.source_events[2]->onset_seconds == original_onset);
    REQUIRE(result.source_events[2]->offset_seconds == original_offset);
    REQUIRE(events[2].onset_seconds == original_onset);
    REQUIRE(events[2].offset_seconds == original_offset);
}

TEST_CASE("legacy detector confidence remains heuristic evidence, not calibration",
          "[interpretation][review]") {
    const std::vector<transcribe::Note> notes{
        {60, 0.0, 0.5, "C4", 0.83, 0.2}};
    const auto result = transcribe::interpret_notes(notes);

    REQUIRE(result.source_events.size() == 1);
    REQUIRE_FALSE(result.source_events.front()->calibrated_confidence);
    REQUIRE(result.source_events.front()->evidence.size() == 1);
    REQUIRE(result.source_events.front()->evidence.front().raw_strength ==
            Catch::Approx(0.83));
    REQUIRE(result.source_events.front()->evidence.front().strength_semantics ==
            transcribe::EvidenceSemantics::HeuristicScore);
}

TEST_CASE("uncertain empty and sparse inputs retain alternatives instead of asserting a meter",
          "[interpretation]") {
    const auto empty = transcribe::interpret_note_events({});
    REQUIRE_FALSE(empty.hypotheses.empty());
    REQUIRE(empty.hypotheses.front().tempo_ambiguous);
    REQUIRE(empty.hypotheses.front().meter_ambiguous);
    REQUIRE(empty.hypotheses.front().key_ambiguous);
    const auto empty_xml = render_musicxml(empty);
    REQUIRE(empty_xml.find("<sound tempo=") == std::string::npos);
    REQUIRE(empty_xml.find("<time>") == std::string::npos);
    REQUIRE(empty_xml.find("Uncertain interpretation") != std::string::npos);

    const std::vector<transcribe::CanonicalNoteEvent> sparse{
        note_event(60, 0.0, 0.4), note_event(64, 0.5, 0.9)};
    const auto result = transcribe::interpret_note_events(sparse);
    REQUIRE(result.hypotheses.size() > 1);
    REQUIRE(result.hypotheses.front().meter_ambiguous);
    REQUIRE(std::any_of(result.hypotheses.begin(), result.hypotheses.end(),
                        [](const auto& hypothesis) {
                            return hypothesis.meter.numerator == 3 &&
                                   hypothesis.meter.denominator == 4;
                        }));
    REQUIRE(std::any_of(result.hypotheses.begin(), result.hypotheses.end(),
                        [](const auto& hypothesis) {
                            return hypothesis.meter.numerator == 4 &&
                                   hypothesis.meter.denominator == 4;
                        }));
}

TEST_CASE("interpretation keeps chords and repeated notes while assigning overlapping voices",
          "[interpretation]") {
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.7), note_event(64, 0.0, 0.7),
        note_event(67, 0.3, 0.8), note_event(60, 0.9, 1.2)};
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    options.key_fifths = -1;
    const auto score = transcribe::interpret_note_events(events, options).hypotheses.front();
    REQUIRE(score.notes.size() == 4);
    REQUIRE(score.notes[0].chord_group.has_value());
    REQUIRE(score.notes[1].chord_group == score.notes[0].chord_group);
    REQUIRE_FALSE(score.notes[2].chord_group.has_value());
    REQUIRE(score.notes[0].voice != score.notes[2].voice);
    REQUIRE(score.notes[3].source_event_index != score.notes[0].source_event_index);
    REQUIRE(score.notes[0].written_midi_pitch == 60);
    REQUIRE_FALSE(score.notes[0].velocity.has_value());
    REQUIRE_FALSE(score.notes[0].articulation.has_value());
    REQUIRE_FALSE(score.rests.empty());
}

TEST_CASE("rhythm interpretation identifies dotted values, triplets, rests and bar ties",
          "[interpretation]") {
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120;
    options.meter = transcribe::MeterConstraint{3, 4, {}};
    options.key_fifths = 0;
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.75),
        note_event(62, 1.0, 1.1666666667),
        note_event(64, 1.1666666667, 1.3333333333),
        note_event(65, 1.3333333333, 1.5),
        note_event(67, 1.75, 4.25)};
    const auto score = transcribe::interpret_note_events(events, options).hypotheses.front();
    REQUIRE(score.notes.size() == 5);
    REQUIRE(score.notes[0].rhythm.symbolic);
    REQUIRE(score.notes[0].rhythm.denominator == 4);
    REQUIRE(score.notes[0].rhythm.dots == 1);
    REQUIRE(score.notes[1].rhythm.symbolic);
    REQUIRE(score.notes[1].rhythm.tuplet_actual == 3);
    REQUIRE(score.notes[1].rhythm.tuplet_normal == 2);
    REQUIRE_FALSE(score.rests.empty());
    REQUIRE(score.notes[4].onset_quarters + score.notes[4].duration_quarters > 6.0);
    REQUIRE(score.notes[4].rhythm.symbolic);
}

TEST_CASE("swing evidence is distinct from tuplets and raw onset seconds are unchanged",
          "[interpretation]") {
    std::vector<transcribe::CanonicalNoteEvent> events;
    for (int pair = 0; pair < 6; ++pair) {
        const double beat = pair * 0.5;
        events.push_back(note_event(60 + pair % 5, beat, beat + 1.0 / 3.0));
        events.push_back(note_event(65 + pair % 5, beat + 1.0 / 3.0, beat + 0.5));
    }
    events.push_back(note_event(60, 3.0, 3.3333333333));
    transcribe::InterpretationOptions options;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    const auto result = transcribe::interpret_note_events(events, options);
    const auto& score = result.hypotheses.front();
    REQUIRE(score.swing_ratio > 1.7);
    REQUIRE(score.swing_confidence >= 0.5);
    REQUIRE(score.swing_inferred);
    const auto raw_second_onset = events[1].onset_seconds;
    REQUIRE(result.source_events[1]->onset_seconds == raw_second_onset);
    REQUIRE(score.notes[1].onset_residual_seconds != 0.0);
    REQUIRE(score.notes[1].rhythm.tuplet_actual == 0);
}

TEST_CASE("tempo changes are represented as separate tempo-map segments", "[interpretation]") {
    const std::vector<double> onsets{
        0.0, 0.5, 1.0, 1.5, 2.0, 2.6666667, 3.3333333, 4.0, 4.6666667, 5.3333333};
    std::vector<transcribe::CanonicalNoteEvent> events;
    for (std::size_t i = 0; i < onsets.size(); ++i) {
        events.push_back(note_event(60 + static_cast<int>(i % 7), onsets[i], onsets[i] + 0.3));
    }
    transcribe::InterpretationOptions options;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    const auto result = transcribe::interpret_note_events(events, options);
    REQUIRE(result.hypotheses.front().tempo_map.size() >= 2);
    REQUIRE(result.hypotheses.front().tempo_map[0].beats_per_minute ==
            Catch::Approx(120.0).margin(1.0));
    REQUIRE(result.hypotheses.front().tempo_map[1].beats_per_minute ==
            Catch::Approx(90.0).margin(1.0));
    const auto xml = render_musicxml(result);
    REQUIRE(xml.find("<sound tempo=\"120.0\"/>") != std::string::npos);
    REQUIRE(xml.find("<sound tempo=\"90.0\"/>") != std::string::npos);
}

TEST_CASE("syncopated onsets remain off the beat grid when outside snap tolerance",
          "[interpretation]") {
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.2), note_event(62, 0.1875, 0.3875),
        note_event(64, 0.75, 0.95)};
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120.0;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    options.onset_quantization_tolerance_seconds = 0.01;
    const auto result = transcribe::interpret_note_events(events, options);
    const auto offbeat = std::find_if(result.hypotheses.front().notes.begin(),
                                      result.hypotheses.front().notes.end(),
        [](const auto& note) { return note.source_event_index == 1; });
    REQUIRE(offbeat != result.hypotheses.front().notes.end());
    REQUIRE(offbeat->onset_quarters == Catch::Approx(0.375).margin(1e-9));
    REQUIRE(result.source_events[1]->onset_seconds == Catch::Approx(0.1875));
}

TEST_CASE("MusicXML reconciles chord members with small onset differences",
          "[interpretation][musicxml]") {
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.5), note_event(64, 0.02, 0.52)};
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120.0;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    options.chord_onset_tolerance_seconds = 0.04;
    options.onset_quantization_tolerance_seconds = 0.0;
    const auto interpretation = transcribe::interpret_note_events(events, options);
    REQUIRE(interpretation.hypotheses.front().notes[0].chord_group.has_value());
    REQUIRE(interpretation.hypotheses.front().notes[1].chord_group ==
            interpretation.hypotheses.front().notes[0].chord_group);
    const auto xml = render_musicxml(interpretation);
    REQUIRE(xml.find("<chord/>") != std::string::npos);
}

TEST_CASE("interpretation rejects invalid constraints and leaves rejected events out of score",
          "[interpretation]") {
    transcribe::InterpretationOptions invalid;
    invalid.meter = transcribe::MeterConstraint{5, 8, {2, 2}};
    const std::vector<transcribe::CanonicalNoteEvent> one{note_event(60, 0.0, 0.5)};
    REQUIRE_THROWS_AS(transcribe::interpret_note_events(one, invalid), std::invalid_argument);
    invalid = {};
    invalid.tempo_bpm = 0.0;
    REQUIRE_THROWS_AS(transcribe::interpret_note_events(one, invalid), std::invalid_argument);

    auto rejected = note_event(60, 0.0, 0.5);
    rejected.state = transcribe::EventLifecycle::Rejected;
    const auto result = transcribe::interpret_note_events(
        std::span<const transcribe::CanonicalNoteEvent>(&rejected, 1));
    REQUIRE(result.source_events.size() == 1);
    REQUIRE(result.hypotheses.front().notes.empty());
}

TEST_CASE("interpretation MusicXML preserves pickup, compound meter and uncertainty",
          "[interpretation][musicxml]") {
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.25), note_event(64, 0.5, 0.75),
        note_event(67, 1.0, 1.25), note_event(65, 1.5, 1.75)};
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120.0;
    options.meter = transcribe::MeterConstraint{6, 8, {3, 3}};
    options.key_fifths = -2;
    options.minor_key = true;
    options.pickup_quarters = 1.0;
    options.instrument = "Clarinet & flute";
    options.written_transposition_semitones = 2;
    const auto interpretation = transcribe::interpret_note_events(events, options);
    const auto xml = render_musicxml(interpretation);

    REQUIRE(xml.find("<measure number=\"1\" implicit=\"yes\">") != std::string::npos);
    REQUIRE(xml.find("<beats>6</beats><beat-type>8</beat-type>") != std::string::npos);
    REQUIRE(xml.find("<fifths>-2</fifths><mode>minor</mode>") != std::string::npos);
    REQUIRE(xml.find("<divisions>960</divisions>") != std::string::npos);
    REQUIRE(xml.find("Clarinet &amp; flute") != std::string::npos);
    REQUIRE(xml.find("<transpose><chromatic>-2</chromatic></transpose>") !=
            std::string::npos);
    REQUIRE(xml.find("<dynamics>") == std::string::npos);
}

TEST_CASE("interpretation MusicXML writes tuplets, chord ties, spelling and expressive offsets",
          "[interpretation][musicxml]") {
    std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 3.0), note_event(61, 0.0, 3.0),
        note_event(67, 0.5, 0.75),
        note_event(62, 4.03, 4.1966666667),
        note_event(64, 4.1666666667, 4.3333333333),
        note_event(65, 4.3333333333, 4.5)};
    events.front().velocity = 95;
    events.front().articulation = "staccato";
    transcribe::InterpretationOptions options;
    options.tempo_bpm = 120.0;
    options.meter = transcribe::MeterConstraint{4, 4, {}};
    options.key_fifths = -1;
    options.written_transposition_semitones = 2;
    const auto interpretation = transcribe::interpret_note_events(events, options);
    const auto xml = render_musicxml(interpretation, true);

    REQUIRE(xml.find("<chord/>") != std::string::npos);
    REQUIRE(xml.find("<tie type=\"start\"/>") != std::string::npos);
    REQUIRE(xml.find("<time-modification><actual-notes>3</actual-notes>") !=
            std::string::npos);
    REQUIRE(xml.find("<accidental>flat</accidental>") != std::string::npos);
    REQUIRE(xml.find("Expressive onset") != std::string::npos);
    const auto first_chord = xml.find("<chord/>");
    REQUIRE(first_chord != std::string::npos);
    REQUIRE(xml.find("<chord/>", first_chord + 1) != std::string::npos);
    REQUIRE(xml.find("<backup><duration>") != std::string::npos);
    REQUIRE(xml.find("<rest/>") != std::string::npos);
    REQUIRE(xml.find("<dynamics><f/></dynamics>") != std::string::npos);
    REQUIRE(xml.find("<staccato/>") != std::string::npos);
}

TEST_CASE("interpretation MusicXML rejects invalid hypothesis selections and note data",
          "[interpretation][musicxml]") {
    const std::vector<transcribe::CanonicalNoteEvent> events{
        note_event(60, 0.0, 0.5)};
    auto interpretation = transcribe::interpret_note_events(events);
    const auto path = std::filesystem::temp_directory_path() /
        "transcriber-invalid-interpretation.musicxml";
    REQUIRE_THROWS_AS(transcribe::write_musicxml(path, interpretation, 10),
                      std::out_of_range);
    interpretation.hypotheses.front().notes.front().duration_quarters = -1.0;
    REQUIRE_THROWS_AS(transcribe::write_musicxml(path, interpretation),
                      std::invalid_argument);
}
