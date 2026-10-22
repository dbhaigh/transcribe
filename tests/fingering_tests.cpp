#include "transcribe/fingering.hpp"
#include "transcribe/guitar_tab.hpp"
#include "transcribe/guitar_analysis.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <algorithm>
#include <array>
#include <vector>
#include <limits>
#include <cmath>
#include <numbers>

namespace {

transcribe::CanonicalNoteEvent contour_fixture(int kind) {
    transcribe::CanonicalNoteEvent event;
    event.id = "hand-checked-contour";
    event.onset_seconds = 0;
    event.offset_seconds = 1;
    event.midi_pitch = 64;
    event.calibrated_confidence = .91;
    for (int index = 0; index <= 100; ++index) {
        const double t = index / 100.0;
        double pitch = 64;
        if (kind == 1) pitch += .35 * std::sin(2 * std::numbers::pi * 6 * t);
        if (kind == 2) pitch += 2 * std::min(1.0, t / .6);
        if (kind == 3) pitch += 2 - 2 * std::min(1.0, t / .6);
        if (kind == 4) pitch += t < .5 ? 0 : 2;
        transcribe::EventEvidence frame;
        frame.time_seconds = t;
        frame.midi_pitch = static_cast<int>(std::lround(pitch));
        frame.raw_voicing = .99;
        frame.fractional_midi = pitch;
        event.evidence.push_back(frame);
    }
    return event;
}

} // namespace

TEST_CASE("extended guitar strings, reentrant tuning and physical fret limits stay bounded",
          "[fingering][guitar]") {
    transcribe::FingeringOptions options;
    options.string_tuning = {64, 59, 55, 50, 45, 40, 35};
    const std::vector<transcribe::Note> bass{{35, 0, 1, "B1", .9}};
    auto result = transcribe::assign_fingerings(bass, options);
    REQUIRE(result.fully_playable);
    REQUIRE(result.positions.front().string_number == 7);
    REQUIRE(result.positions.front().fret == 0);
    transcribe::validate_fingering_assignment(bass, result.positions, options);
    options.string_tuning = {67, 60, 64, 69}; // High-G reentrant four-string instrument.
    const std::vector<transcribe::Note> chord{{67, 0, 1, "G4", .9}, {60, 0, 1, "C4", .9},
                                            {64, 0, 1, "E4", .9}, {69, 0, 1, "A4", .9}};
    result = transcribe::assign_fingerings(chord, options);
    REQUIRE(result.fully_playable);
    REQUIRE(std::all_of(result.positions.begin(), result.positions.end(),
        [](const auto& position) { return position.fret == 0; }));
    options.string_tuning.assign(12, 64);
    const std::vector<transcribe::Note> unison{{64, 0, 1, "E4", .9}};
    result = transcribe::assign_fingerings(unison, options);
    REQUIRE(result.notes.front().playable_positions.size() == 12);
    REQUIRE(result.notes.front().performed_position_unknown);
    REQUIRE_FALSE(result.notes.front().calibrated_fingering_confidence);
    REQUIRE(result.notes.front().explanation.find("does not identify") != std::string::npos);
    options.string_tuning = {64};
    options.capo_fret = 2;
    options.physical_fret_count = 12;
    const std::vector<transcribe::Note> high{{77, 0, 1, "F5", .9}};
    REQUIRE_FALSE(transcribe::assign_fingerings(high, options).fully_playable);
    REQUIRE_THROWS_AS(transcribe::validate_fingering_assignment(high, {{0, 1, 11}}, options),
                      std::invalid_argument);
    options.physical_fret_count = 24;
    options.minimum_midi_pitch = 78;
    REQUIRE_FALSE(transcribe::assign_fingerings(high, options).fully_playable);
    options.position_overrides = {{0, 1, 11}};
    REQUIRE_THROWS_AS(transcribe::assign_fingerings(high, options), std::invalid_argument);
    options.string_tuning.assign(13, 64);
    REQUIRE_THROWS_AS(transcribe::assign_fingerings({}, options), std::invalid_argument);
    REQUIRE(transcribe::parse_string_tuning("64,59,55,50,45,40,35").size() == 7);
    for (const auto text : {"", "64,", ",64", "128", "64,x", "64,,59"}) {
        REQUIRE_THROWS_AS(transcribe::parse_string_tuning(text), std::invalid_argument);
    }
}

TEST_CASE("guitar contour analysis preserves pitch calibration but never invents technique certainty",
          "[guitar][technique]") {
    transcribe::GuitarTechniqueOptions options;
    options.enabled = true;
    options.isolated_pitch_contour = true;
    const std::array expected{transcribe::GuitarPitchGesture::None, transcribe::GuitarPitchGesture::Vibrato,
        transcribe::GuitarPitchGesture::SustainedRise, transcribe::GuitarPitchGesture::SustainedFall,
        transcribe::GuitarPitchGesture::None};
    for (int kind = 0; kind < 5; ++kind) {
        const auto result = transcribe::analyze_guitar_technique(
            contour_fixture(kind), transcribe::InstrumentPrior::Guitar, options);
        INFO(kind);
        REQUIRE(result.gesture == expected[static_cast<std::size_t>(kind)]);
        REQUIRE(result.calibrated_pitch_confidence == .91);
        REQUIRE_FALSE(result.calibrated_technique_confidence);
        REQUIRE(result.physical_technique_unresolved);
        if (kind == 1) REQUIRE(*result.modulation_hz == Catch::Approx(6).margin(.2));
        if (kind == 2 || kind == 3) {
            REQUIRE(result.possible_techniques.size() == 3);
            REQUIRE(result.explanation.find("not a unique bend or slide") != std::string::npos);
        }
    }
    auto event = contour_fixture(1);
    options.enabled = false;
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture ==
            transcribe::GuitarPitchGesture::None);
    options.enabled = true;
    for (const auto instrument : {transcribe::InstrumentPrior::Piano, transcribe::InstrumentPrior::Voice,
                                  transcribe::InstrumentPrior::None}) {
        REQUIRE(transcribe::analyze_guitar_technique(event, instrument, options).gesture ==
                transcribe::GuitarPitchGesture::None);
    }
    options.isolated_pitch_contour = false;
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture ==
            transcribe::GuitarPitchGesture::None);
    options.isolated_pitch_contour = true;
    event.evidence.erase(event.evidence.begin() + 30, event.evidence.begin() + 60);
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture ==
            transcribe::GuitarPitchGesture::None);
    event = contour_fixture(1);
    for (auto& frame : event.evidence) frame.raw_voicing = .1;
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture ==
            transcribe::GuitarPitchGesture::None);
    event = contour_fixture(1);
    event.evidence[1].time_seconds = 0;
    REQUIRE_THROWS_AS(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options),
                      std::invalid_argument);
    event = contour_fixture(1);
    options.maximum_evidence_frames = 100;
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture ==
            transcribe::GuitarPitchGesture::None);
    options.maximum_evidence_frames = 8192;
    event = contour_fixture(1);
    for (auto& frame : event.evidence) {
        *frame.fractional_midi += 2 * frame.time_seconds;
    }
    REQUIRE(transcribe::analyze_guitar_technique(event, transcribe::InstrumentPrior::Guitar, options).gesture !=
            transcribe::GuitarPitchGesture::Vibrato);
}

TEST_CASE("synthetic audio YIN evidence supports guitar pitch gestures without physical technique claims",
          "[guitar][technique][audio]") {
    for (const int kind : {0, 1, 2}) {
        transcribe::AudioBuffer audio;
        audio.sample_rate = 22050;
        audio.samples.resize(22050 * 2);
        double phase = 0;
        for (std::size_t i = 0; i < audio.samples.size(); ++i) {
            const double time = static_cast<double>(i) / audio.sample_rate;
            const double offset = kind == 1 ? .35 * std::sin(2 * std::numbers::pi * 5 * time) :
                kind == 2 ? 2 * std::clamp((time - .2) / 1.2, 0.0, 1.0) : 0;
            phase += 2 * std::numbers::pi * 329.6275569 * std::exp2(offset / 12) / audio.sample_rate;
            audio.samples[i] = static_cast<float>(.3 * std::sin(phase) + .1 * std::sin(2 * phase));
        }
        transcribe::CanonicalNoteEvent event;
        event.onset_seconds = .1;
        event.offset_seconds = 1.9;
        transcribe::AnalysisOptions analysis;
        analysis.instrument = transcribe::InstrumentPrior::Guitar;
        analysis.evidence_callback = [&](const auto& track) {
            REQUIRE(track.method == "yin");
            for (const auto& pitch : track.frames) {
                transcribe::EventEvidence frame;
                frame.time_seconds = pitch.time_seconds + track.window_seconds / 2;
                frame.midi_pitch = pitch.midi_pitch;
                frame.fractional_midi = pitch.fractional_midi;
                frame.raw_voicing = pitch.voicing_probability;
                event.evidence.push_back(frame);
            }
        };
        transcribe::transcribe_monophonic(audio, analysis);
        transcribe::GuitarTechniqueOptions options;
        options.enabled = true;
        options.isolated_pitch_contour = true;
        const auto result = transcribe::analyze_guitar_technique(
            event, transcribe::InstrumentPrior::Guitar, options);
        INFO(result.explanation);
        INFO(kind);
        INFO(result.modulation_hz.value_or(0));
        INFO(result.excursion_semitones);
        REQUIRE(result.gesture == (kind == 0 ? transcribe::GuitarPitchGesture::None :
            kind == 1 ? transcribe::GuitarPitchGesture::Vibrato : transcribe::GuitarPitchGesture::SustainedRise));
        REQUIRE(result.physical_technique_unresolved);
    }
}

TEST_CASE("joint sequence ranking agrees with an independent exhaustive enumeration", "[fingering]") {
    const std::vector<transcribe::Note> notes{
        {64, 0, .2, "E4", .9}, {65, .3, .2, "F4", .9}, {64, .6, .2, "E4", .9}};
    transcribe::FingeringOptions options;
    options.maximum_fret = 9;
    options.beam_width = 512;
    options.local_solution_limit = 512;
    options.maximum_alternatives = 511;
    const auto result = transcribe::assign_fingerings(notes, options);
    std::vector<double> expected;
    for (int a = 0; a < 6; ++a) for (int b = 0; b < 6; ++b) for (int c = 0; c < 6; ++c) {
        const std::array<int, 3> strings{a, b, c};
        bool valid = true;
        double cost = 0, hand = 5;
        for (std::size_t i = 0; i < 3; ++i) {
            const int fret = notes[i].midi_pitch - options.tuning[strings[i]];
            if (fret < 0 || fret > 9) { valid = false; break; }
            cost += .12*fret + .1*strings[i];
            if (fret > 0) {
                cost += .75*std::abs(fret-5) + 1.4*std::abs(fret-hand);
                hand = fret;
            }
        }
        if (valid) expected.push_back(cost);
    }
    std::sort(expected.begin(), expected.end());
    REQUIRE(result.alternatives_complete);
    REQUIRE(result.alternatives.size()+1 == expected.size());
    REQUIRE(result.cost.total() == Catch::Approx(expected[0]));
    for (std::size_t i = 0; i < result.alternatives.size(); ++i) {
        REQUIRE(result.alternatives[i].cost.total() == Catch::Approx(expected[i+1]));
    }
    options.local_solution_limit = 1;
    REQUIRE(transcribe::assign_fingerings(notes, options).search_truncated);
}

TEST_CASE("supplied tablature alternatives cannot invent pitches or reuse occupied strings", "[fingering]") {
    const std::vector<transcribe::Note> notes{
        {64, 0, .5, "E4", .9}, {69, .1, .2, "A4", .9}};
    transcribe::FingeringOptions options;
    const std::vector<transcribe::GuitarTabPosition> collision{{0, 1, 0}, {1, 1, 5}};
    REQUIRE_THROWS_AS(transcribe::validate_fingering_assignment(notes, collision, options), std::invalid_argument);
    const std::vector<transcribe::GuitarTabPosition> wrong_pitch{{0, 2, 4}, {1, 1, 5}};
    REQUIRE_THROWS_AS(transcribe::validate_fingering_assignment(notes, wrong_pitch, options), std::invalid_argument);
    const std::vector<transcribe::GuitarTabPosition> legal{{0, 2, 5}, {1, 1, 5}};
    REQUIRE_NOTHROW(transcribe::validate_fingering_assignment(notes, legal, options));
}

TEST_CASE("small exhaustive searches retain every playable string alternative", "[fingering]") {
    const std::vector<transcribe::Note> notes{{64, 0, .2, "E4", .9}};
    transcribe::FingeringOptions options;
    options.maximum_alternatives = 63;
    const auto result = transcribe::assign_fingerings(notes, options);
    REQUIRE(result.fully_playable);
    REQUIRE(result.notes[0].playable_positions.size() == 6);
    REQUIRE(result.alternatives.size() == 5);
    REQUIRE(result.alternatives_complete);
    REQUIRE_FALSE(result.search_truncated);
    REQUIRE(result.visited_search_nodes > 0);
    options.beam_width = 1;
    const auto bounded = transcribe::assign_fingerings(notes, options);
    REQUIRE(bounded.search_truncated);
    REQUIRE_FALSE(bounded.alternatives_complete);
    REQUIRE(bounded.notes[0].playable_positions.size() == 6);
    options.beam_width = 0;
    REQUIRE_THROWS_AS(transcribe::assign_fingerings(notes, options), std::invalid_argument);
}

TEST_CASE("held frets constrain later attacks rather than only simultaneous onset groups", "[fingering]") {
    transcribe::FingeringOptions options;
    options.position_overrides = {{0, 6, 1, true}, {1, 1, 12, true}};
    const std::vector<transcribe::Note> notes{
        {41, 0, 2, "F2", .9}, {76, .5, .5, "E5", .9}};
    const auto impossible = transcribe::assign_fingerings(notes, options);
    REQUIRE_FALSE(impossible.fully_playable);
    REQUIRE(impossible.explanation.find("Infeasible") != std::string::npos);
    REQUIRE(impossible.positions[0].string_number == 0);
    options.maximum_chord_fret_span = 12;
    const auto stretched = transcribe::assign_fingerings(notes, options);
    REQUIRE(stretched.fully_playable);
    REQUIRE(stretched.cost.chord_stretch > 0);
    REQUIRE_FALSE(stretched.positions[0].suggested_finger);
    REQUIRE_FALSE(stretched.positions[1].suggested_finger);
}

TEST_CASE("conservative left hand finger and barre constraints reject impossible shapes", "[fingering]") {
    const std::vector<transcribe::Note> notes{
        {65, 0, 1, "F4", .9}, {59, 0, 1, "B3", .9}, {56, 0, 1, "G#3", .9}};
    transcribe::FingeringOptions options;
    options.maximum_fretting_fingers = 1;
    options.position_overrides = {{0, 1, 1, true}, {1, 2, 0, true}, {2, 3, 1, true}};
    REQUIRE_FALSE(transcribe::assign_fingerings(notes, options).fully_playable);
    options.maximum_fretting_fingers = 2;
    const auto result = transcribe::assign_fingerings(notes, options);
    REQUIRE(result.fully_playable);
    REQUIRE(result.positions[0].suggested_finger == 1);
    REQUIRE(result.positions[1].suggested_finger == 0);
    REQUIRE(result.positions[2].suggested_finger == 2);
    const std::vector<transcribe::Note> barre{
        {65, 0, 1, "F4", .9}, {56, 0, 1, "G#3", .9}};
    options.maximum_fretting_fingers = 1;
    options.position_overrides = {{0, 1, 1, true}, {1, 3, 1, true}};
    const auto shared = transcribe::assign_fingerings(barre, options);
    REQUIRE(shared.fully_playable);
    REQUIRE(shared.positions[0].suggested_finger == shared.positions[1].suggested_finger);
    const std::vector<transcribe::Note> five_fingers{
        {65, 0, 1, "", .9}, {61, 0, 1, "", .9}, {58, 0, 1, "", .9},
        {54, 0, 1, "", .9}, {50, 0, 1, "", .9}};
    options.maximum_fretting_fingers = 4;
    options.position_overrides = {{0, 1, 1, true}, {1, 2, 2, true}, {2, 3, 3, true},
                                 {3, 4, 4, true}, {4, 5, 5, true}};
    REQUIRE_FALSE(transcribe::assign_fingerings(five_fingers, options).fully_playable);
}

TEST_CASE("open strings retain the hand position and high frets respect relative capo limits", "[fingering]") {
    transcribe::FingeringOptions options;
    options.position_overrides = {{0, 1, 0, true}, {1, 2, 5, true}};
    const std::vector<transcribe::Note> phrase{
        {64, 0, .2, "E4", .9}, {64, .5, .2, "E4", .9}};
    const auto open = transcribe::assign_fingerings(phrase, options);
    REQUIRE(open.cost.hand_movement == Catch::Approx(0));
    REQUIRE(open.positions[0].suggested_finger == 0);
    options = {};
    options.capo_fret = 2;
    options.maximum_fret = 36;
    const std::vector<transcribe::Note> high{{102, 0, .2, "F#7", .9}};
    REQUIRE(transcribe::assign_fingerings(high, options).positions[0].fret == 36);
    options.maximum_fret = 35;
    const auto outside = transcribe::assign_fingerings(high, options);
    REQUIRE_FALSE(outside.fully_playable);
    REQUIRE(outside.notes[0].explanation.find("range") != std::string::npos);
}

TEST_CASE("transition weights and search budgets propagate through the guitar tab wrapper", "[fingering]") {
    const std::vector<transcribe::Note> phrase{
        {64, 0, .2, "E4", .9}, {69, .3, .2, "A4", .9}};
    transcribe::GuitarTabOptions options;
    options.position_overrides = {{0, 1, 0, true}, {1, 1, 5, true}};
    options.search.repeated_string_cost = 3;
    options.search.awkward_transition_cost = 2;
    const auto result = transcribe::assign_guitar_tab_fingerings(phrase, options);
    REQUIRE(result.cost.repeated_strings == Catch::Approx(3));
    REQUIRE(result.cost.awkward_transitions == Catch::Approx(2*5/.3));
    options.search.search_node_limit = 1;
    const auto unresolved = transcribe::assign_guitar_tab_fingerings(phrase, options);
    REQUIRE_FALSE(unresolved.fully_playable);
    REQUIRE(unresolved.search_truncated);
    REQUIRE(unresolved.explanation.find("budget") != std::string::npos);
    options.search.repeated_string_cost = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS_AS(transcribe::assign_guitar_tab_fingerings(phrase, options), std::invalid_argument);
}

TEST_CASE("fingering candidates preserve sounding pitch across supported tunings") {
    const std::array<std::array<int, 6>, 5> tunings{{
        {64, 59, 55, 50, 45, 40},
        {64, 59, 55, 50, 45, 38},
        {62, 57, 55, 50, 45, 38},
        {62, 57, 54, 50, 45, 38},
        {62, 59, 55, 50, 43, 38},
    }};

    for (const auto& tuning : tunings) {
        transcribe::FingeringOptions options;
        options.tuning = tuning;
        options.capo_fret = 2;
        const std::vector<transcribe::Note> notes{
            {tuning[0] + 2 + 3, 0.0, 0.4, "test", 0.9}};
        const auto result = transcribe::assign_fingerings(notes, options);

        REQUIRE(result.positions.size() == 1);
        REQUIRE(result.positions[0].string_number > 0);
        REQUIRE(tuning[result.positions[0].string_number - 1] +
                    options.capo_fret + result.positions[0].fret ==
                notes[0].midi_pitch);
    }
}

TEST_CASE("enharmonic pitches and octave boundaries are assigned by MIDI pitch") {
    const std::vector<transcribe::Note> notes{
        {61, 0.0, 0.3, "C-sharp or D-flat", 0.9},
        {40, 0.5, 0.3, "E2", 0.9},
        {88, 1.0, 0.3, "E6", 0.9},
    };
    const auto result = transcribe::assign_fingerings(notes);

    REQUIRE(result.notes[0].playable);
    REQUIRE(result.notes[1].playable);
    REQUIRE(result.notes[2].playable);
    for (const auto& position : result.positions) {
        REQUIRE(position.string_number > 0);
        REQUIRE(transcribe::GuitarTabOptions{}.tuning[position.string_number - 1] +
                    position.fret ==
                notes[position.note_index].midi_pitch);
    }
}

TEST_CASE("chord assignments use distinct strings and report ranked costs") {
    const std::vector<transcribe::Note> chord{
        {64, 0.0, 0.5, "E4", 0.9},
        {67, 0.0, 0.5, "G4", 0.9},
        {71, 0.0, 0.5, "B4", 0.9},
    };
    transcribe::FingeringOptions options;
    options.maximum_alternatives = 3;
    const auto result = transcribe::assign_fingerings(chord, options);

    REQUIRE(result.fully_playable);
    REQUIRE(result.positions.size() == chord.size());
    REQUIRE(result.alternatives.size() == 3);
    for (std::size_t left = 0; left < result.positions.size(); ++left) {
        for (std::size_t right = left + 1; right < result.positions.size(); ++right) {
            REQUIRE(result.positions[left].string_number !=
                    result.positions[right].string_number);
        }
        const auto& position = result.positions[left];
        REQUIRE(options.tuning[position.string_number - 1] + position.fret ==
                chord[left].midi_pitch);
    }
    REQUIRE(result.cost.total() >= 0.0);
    REQUIRE(result.alternatives[0].cost.total() <=
            result.alternatives[1].cost.total());
    REQUIRE(result.notes[0].candidates.size() > 1);
}

TEST_CASE("sequence optimization penalizes unnecessary hand movement") {
    const std::vector<transcribe::Note> phrase{
        {64, 0.0, 0.2, "E4", 0.9},
        {65, 0.3, 0.2, "F4", 0.9},
        {66, 0.6, 0.2, "F-sharp4", 0.9},
        {67, 0.9, 0.2, "G4", 0.9},
    };
    transcribe::FingeringOptions options;
    options.maximum_alternatives = 4;
    const auto result = transcribe::assign_fingerings(phrase, options);

    REQUIRE(result.fully_playable);
    REQUIRE(result.cost.hand_movement < 8.0);
    REQUIRE(result.alternatives.size() == 4);
    for (std::size_t index = 1; index < result.alternatives.size(); ++index) {
        REQUIRE(result.alternatives[index - 1].cost.total() <=
                result.alternatives[index].cost.total());
    }
}

TEST_CASE("unplayable pitches and oversized chords remain explicitly unknown") {
    SECTION("pitch outside the instrument range") {
        const std::vector<transcribe::Note> notes{
            {100, 0.0, 0.5, "out of range", 0.9}};
        const auto result = transcribe::assign_fingerings(notes);
        REQUIRE_FALSE(result.fully_playable);
        REQUIRE_FALSE(result.notes[0].playable);
        REQUIRE(result.positions[0].string_number == 0);
        REQUIRE(result.positions[0].fret == -1);
    }

    SECTION("more simultaneous tones than strings") {
        const std::vector<transcribe::Note> chord{
            {40, 0.0, 0.5, "E2", 0.9}, {42, 0.0, 0.5, "F#2", 0.9},
            {44, 0.0, 0.5, "G#2", 0.9}, {46, 0.0, 0.5, "A#2", 0.9},
            {48, 0.0, 0.5, "C3", 0.9}, {50, 0.0, 0.5, "D3", 0.9},
            {52, 0.0, 0.5, "E3", 0.9},
        };
        transcribe::FingeringOptions options;
        const auto result = transcribe::assign_fingerings(chord, options);
        REQUIRE_FALSE(result.fully_playable);
        REQUIRE(std::count_if(result.positions.begin(), result.positions.end(),
                              [](const auto& position) {
                                  return position.string_number == 0;
                              }) >= 1);
        for (std::size_t left = 0; left < result.positions.size(); ++left) {
            if (result.positions[left].string_number == 0) continue;
            REQUIRE(options.tuning[result.positions[left].string_number - 1] +
                        result.positions[left].fret ==
                    chord[left].midi_pitch);
            for (std::size_t right = left + 1;
                 right < result.positions.size(); ++right) {
                if (result.positions[right].string_number > 0) {
                    REQUIRE(result.positions[left].string_number !=
                            result.positions[right].string_number);
                }
            }
        }
    }
}

TEST_CASE("a bounded chord search exposes its truncation status") {
    std::vector<transcribe::Note> chord;
    for (int pitch = 60; pitch < 70; ++pitch) {
        chord.push_back({pitch, 0.0, 0.5, "chord tone", 0.9});
    }
    const auto result = transcribe::assign_fingerings(chord);
    REQUIRE(result.search_truncated);
}

TEST_CASE("saved corrections survive onset changes and report stale positions") {
    const std::vector<transcribe::Note> notes{
        {64, 1.1, 0.4, "E4", 0.9}};
    transcribe::FingeringOptions options;
    options.manual_corrections = {{64, 1.0, 0.4, 2, 5}};
    const auto corrected = transcribe::assign_fingerings(notes, options);

    REQUIRE(corrected.positions[0].manually_corrected);
    REQUIRE(corrected.positions[0].string_number == 2);
    REQUIRE(corrected.positions[0].fret == 5);
    REQUIRE(corrected.rejected_corrections.empty());

    options.tuning = {65, 60, 56, 51, 46, 41};
    const auto stale = transcribe::assign_fingerings(notes, options);
    REQUIRE_FALSE(stale.positions[0].manually_corrected);
    REQUIRE(stale.notes[0].correction_rejected);
    REQUIRE(stale.rejected_corrections == std::vector<std::size_t>{0});
}

TEST_CASE("conflicting saved corrections are rejected instead of silently reused") {
    const std::vector<transcribe::Note> notes{
        {64, 0.0, 0.5, "E4", 0.9},
        {64, 0.1, 0.5, "E4", 0.9},
    };
    transcribe::FingeringOptions options;
    options.manual_corrections = {
        {64, 0.0, 0.5, 2, 5}, {64, 0.1, 0.5, 2, 5}};
    const auto result = transcribe::assign_fingerings(notes, options);

    REQUIRE(result.rejected_corrections == std::vector<std::size_t>{0, 1});
    REQUIRE(result.notes[0].correction_rejected);
    REQUIRE(result.notes[1].correction_rejected);
    REQUIRE(result.positions[0].string_number !=
            result.positions[1].string_number);
}

TEST_CASE("manual tab corrections cannot change the note pitch") {
    const std::vector<transcribe::Note> notes{
        {64, 0.0, 0.5, "E4", 0.9}};
    transcribe::GuitarTabOptions options;
    options.position_overrides = {{0, 2, 5, true}};

    const auto result = transcribe::assign_guitar_tab_fingerings(notes, options);
    REQUIRE(result.positions[0].manually_corrected);
    REQUIRE(options.tuning[result.positions[0].string_number - 1] +
                result.positions[0].fret ==
            notes[0].midi_pitch);

    options.position_overrides[0].fret = 4;
    REQUIRE_THROWS(transcribe::assign_guitar_tab_positions(notes, options));
}
