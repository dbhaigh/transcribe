#include "transcribe/fingering.hpp"
#include "transcribe/guitar_tab.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

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
