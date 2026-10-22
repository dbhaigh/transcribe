#include "transcribe/project.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <limits>

namespace {

transcribe::ProjectDocument sample_project() {
    transcribe::ProjectDocument project;
    project.id = "project-fixture";
    project.title = "Fixture project";
    project.score = transcribe::make_empty_score_document("score-fixture");
    project.audio.policy = transcribe::ProjectAudioPolicy::ExternalReference;
    project.audio.reference_path = "audio/source.wav";
    project.audio.relative_to_project = true;
    project.audio.sample_rate = 48000;
    project.audio.channels = 1;
    project.audio.frame_count = 48000 * 3;
    project.analysis.backend_identifier = "builtin:yin";
    project.analysis.model_identifier = "builtin:yin";
    project.analysis.instrument_prior = "guitar";
    project.analysis.live_backend = "spectral";
    project.analysis.tempo_automatic = false;
    project.analysis.meter_automatic = false;
    project.analysis.key_automatic = false;
    project.analysis.preserve_expressive_timing = true;
    project.exports.push_back({
        "musicxml", "fixture.musicxml", project.score.revision, true, {}});
    return project;
}

std::filesystem::path temporary_base(std::string_view label) {
    const auto suffix = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
        ("transcriber-" + std::string(label) + "-" + std::to_string(suffix));
}

} // namespace

TEST_CASE("project version two preserves edited musical and tablature state",
          "[project][serialization]") {
    auto project = sample_project();
    transcribe::InterpretationOptions interpretation_options;
    interpretation_options.tempo_bpm = 100.;
    interpretation_options.meter = transcribe::MeterConstraint{3, 4, {1, 1, 1}};
    interpretation_options.key_fifths = 2;
    interpretation_options.instrument = "guitar";
    transcribe::GuitarTabOptions tab_options;
    tab_options.string_tuning = {64, 59, 55, 50, 45, 38};
    tab_options.capo_fret = 2;
    const std::vector<transcribe::Note> notes{
        {64, .125, .75, "E4", .8, .3}, {59, 1.25, .5, "B3", .7, .2}};
    const auto interpretation = transcribe::interpret_notes(notes, interpretation_options);
    project.score = transcribe::make_score_document(interpretation, 0, "source.wav",
        transcribe::metadata_for_hypothesis(interpretation.hypotheses.front()), tab_options);
    project.score.source_sample_rate = 48000;
    const auto id = project.score.notes.front().id;
    transcribe::ScoreEditor editor(project.score);
    transcribe::ScoreTransaction transaction;
    transaction.base_revision = project.score.revision;
    transaction.reason = "Hand-corrected phrase";
    transaction.commands = {
        transcribe::ChangeScorePitch{id, 67},
        transcribe::ChangeScoreDuration{id, 1.25},
        transcribe::ChangeScoreFingering{id, 1, 1}};
    editor.apply(transaction);
    project.score = editor.document();
    project.exports.clear();
    project.annotations = {{"phrase", id, "Keep this expressive onset."},
                           {"project-notes", {}, "Original take; no embedded audio."}};
    transcribe::ProjectFingeringAlternative alternative;
    alternative.cost = 2.5;
    alternative.cost_components[2] = 2.5;
    alternative.positions = {{id, 2, 6}};
    project.fingering_alternatives = {alternative};
    project.fingering_alternatives_truncated = true;
    project.analysis.quantization = "evidence";
    project.analysis.quantization_strength = .25;
    project.analysis.minimum_rhythm_evidence = .8;
    project.analysis.retain_review_evidence = true;
    project.analysis.bounded_inference = true;
    project.analysis.model_identifier = "local-model";
    project.analysis.model_reference = "models/model.onnx";
    project.analysis.model_version = "hand-verified-v1";
    project.analysis.provider = "cpu";
    const auto serialized = transcribe::serialize_project_document(project);
    const auto restored = transcribe::deserialize_project_document(serialized);
    REQUIRE(transcribe::serialize_score_document(restored.score) ==
            transcribe::serialize_score_document(project.score));
    REQUIRE(transcribe::serialize_project_document(restored) == serialized);
    REQUIRE(restored.score.notes.front().measured_onset_seconds == .125);
    REQUIRE(restored.score.notes.front().duration_quarters == 1.25);
    REQUIRE(restored.score.notes.front().sounding_midi_pitch == 67);
    REQUIRE(restored.score.tab_notes.front().string_number == 1);
    REQUIRE(restored.score.tab_notes.front().fret == 1);
    REQUIRE(restored.score.instrument.capo_fret == 2);
    REQUIRE(restored.score.instrument.tuning.back() == 38);
    REQUIRE(restored.score.metadata.time_numerator == 3);
    REQUIRE(restored.score.metadata.key_fifths == 2);
    REQUIRE(restored.annotations.front().note_id == id);
    REQUIRE(restored.fingering_alternatives.front().positions.front().fret == 6);
    REQUIRE(restored.fingering_alternatives.front().cost_components[2] == 2.5);
    REQUIRE(restored.fingering_alternatives_truncated);
    REQUIRE(restored.analysis.quantization_strength == .25);
    REQUIRE(restored.analysis.minimum_rhythm_evidence == .8);
    REQUIRE(restored.analysis.retain_review_evidence);
    REQUIRE(restored.analysis.bounded_inference);
    REQUIRE(restored.analysis.model_version == "hand-verified-v1");
    transcribe::ScoreEditor reopened(restored.score);
    REQUIRE_THROWS(reopened.undo(restored.score.revision));
}

TEST_CASE("version one project wrappers explicitly migrate to current settings",
          "[project][migration]") {
    const auto legacy =
        "{\"format\":\"transcriber-project\",\"project_schema_version\":1,"
        "\"id\":\"v1-project\",\"title\":\"Version one\",\"last_saved_path\":\"\","
        "\"audio\":{\"policy\":\"external-reference\",\"reference_path\":\"original.wav\","
        "\"relative_to_project\":true,\"sample_rate\":48000,\"channels\":1,\"frame_count\":96000},"
        "\"analysis\":{\"backend_identifier\":\"builtin:yin\",\"model_identifier\":\"builtin:yin\","
        "\"model_reference\":\"\",\"instrument_prior\":\"guitar\",\"live_backend\":\"yin\","
        "\"polyphonic\":false,\"fast_preview\":false,\"preserve_expressive_timing\":true,"
        "\"latency_ms\":30},\"exports\":[],\"score\":" +
        transcribe::serialize_score_document(sample_project().score) + "}";
    const auto project = transcribe::deserialize_project_document(legacy);
    REQUIRE(project.schema_version == 2);
    REQUIRE(project.audio.original_reference == "original.wav");
    REQUIRE(project.audio.relative_to_project);
    REQUIRE(project.analysis.quantization == "legacy");
    REQUIRE(project.analysis.preserve_expressive_timing);
    REQUIRE(project.annotations.empty());
    REQUIRE(project.fingering_alternatives.empty());
    REQUIRE_NOTHROW(transcribe::deserialize_project_document(
        transcribe::serialize_project_document(project)));
}

TEST_CASE("project integrity rejects valid JSON with corrupted musical metadata",
          "[project][validation]") {
    auto json = transcribe::serialize_project_document(sample_project());
    const auto title = json.find("Fixture project");
    REQUIRE(title != std::string::npos);
    json[title] = 'X';
    REQUIRE_THROWS_WITH(transcribe::deserialize_project_document(json),
        "Project integrity check failed. Open a recovery copy or restore a known-good project.");
    json = transcribe::serialize_project_document(sample_project());
    const auto checksum = json.rfind("\"value\":\"");
    REQUIRE(checksum != std::string::npos);
    json[checksum + 9] = json[checksum + 9] == '0' ? '1' : '0';
    REQUIRE_THROWS(transcribe::deserialize_project_document(json));
    json = transcribe::serialize_project_document(sample_project());
    json.resize(json.size() / 2);
    REQUIRE_THROWS(transcribe::deserialize_project_document(json));
}

TEST_CASE("projects reject embedded paths invalid settings and oversized content",
          "[project][validation]") {
    for (const auto& path : std::vector<std::string>{"../outside.wav", "..\\outside.wav", "/absolute.wav",
                                   "C:\\absolute.wav", "data:audio/wav,AA", std::string("audio\0.wav", 10)}) {
        auto project = sample_project();
        project.audio.reference_path = path;
        REQUIRE_THROWS(transcribe::serialize_project_document(project));
    }
    auto project = sample_project();
    project.audio.policy = transcribe::ProjectAudioPolicy::ProjectCopy;
    project.audio.relative_to_project = false;
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    project = sample_project();
    project.analysis.quantization_strength = std::numeric_limits<double>::quiet_NaN();
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    project = sample_project();
    project.analysis.live_backend = "arbitrary-plugin";
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    project = sample_project();
    project.annotations = {{"annotation", "missing-note", "Unanchored?"}};
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    project.annotations = {{"annotation", {}, std::string(16385, 'x')}};
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    project.annotations = {{"same", {}, "one"}, {"same", {}, "two"}};
    REQUIRE_THROWS(transcribe::serialize_project_document(project));
    REQUIRE_THROWS(transcribe::deserialize_project_document(std::string(
        transcribe::maximum_project_bytes + 1, ' ')));
}

TEST_CASE("explicit audio copies are portable and failed saves preserve older assets",
          "[project][audio][persistence]") {
    const auto root = temporary_base("portable");
    const auto moved = temporary_base("relocated");
    std::filesystem::create_directories(root);
    const auto source = root / std::filesystem::path(u8"original \u00e9.wav");
    const auto path = root / std::filesystem::path(u8"project \u00e9.transcriber");
    transcribe::AudioBuffer audio;
    audio.sample_rate = 48000;
    audio.samples = {.1f, .2f, -.3f};
    transcribe::write_audio_wav_atomic(source, audio);
    auto project = sample_project();
    project.audio.frame_count = audio.samples.size();
    transcribe::relink_project_audio(project, path, source);
    const auto saved = transcribe::save_project_with_audio_copy(path, project, source);
    REQUIRE(saved.audio.policy == transcribe::ProjectAudioPolicy::ProjectCopy);
    REQUIRE(saved.audio.relative_to_project);
    REQUIRE_FALSE(saved.audio.original_reference.empty());
    const auto asset = transcribe::resolve_project_audio_path(saved, path);
    REQUIRE(asset != source);
    REQUIRE(transcribe::read_wav(asset).samples == audio.samples);
    const auto again = transcribe::save_project_with_audio_copy(path, saved, source);
    REQUIRE(again.audio.reference_path != saved.audio.reference_path);
    REQUIRE(std::filesystem::is_regular_file(asset));
    REQUIRE_THROWS(transcribe::save_project_with_audio_copy(source, project, source));
    REQUIRE(transcribe::read_wav(source).samples == audio.samples);
    REQUIRE_THROWS(transcribe::save_project_with_audio_copy(path, saved, root / "missing.wav"));
    REQUIRE(transcribe::load_project_file(path).audio.reference_path == again.audio.reference_path);
    const auto files_before = std::distance(std::filesystem::directory_iterator(root),
                                          std::filesystem::directory_iterator());
    REQUIRE_THROWS(transcribe::save_project_with_audio_copy(root, saved, source));
    REQUIRE(std::distance(std::filesystem::directory_iterator(root),
                          std::filesystem::directory_iterator()) == files_before);
    std::filesystem::remove(source);
    std::filesystem::rename(root, moved);
    const auto relocated_path = moved / path.filename();
    const auto relocated = transcribe::load_project_file(relocated_path);
    REQUIRE(std::filesystem::is_regular_file(
        transcribe::resolve_project_audio_path(relocated, relocated_path)));
    REQUIRE(transcribe::read_wav(transcribe::resolve_project_audio_path(
        relocated, relocated_path)).samples == audio.samples);
    for (const auto& file : std::filesystem::directory_iterator(moved))
        std::filesystem::remove(file.path());
    std::filesystem::remove(moved);
}

TEST_CASE("audio relocation validates WAV identity without changing edited scores",
          "[project][audio]") {
    const auto source = std::filesystem::path(temporary_base("relink").string() + ".wav");
    transcribe::AudioBuffer audio;
    audio.sample_rate = 48000;
    audio.samples = {0, .5f};
    transcribe::write_audio_wav_atomic(source, audio);
    auto project = sample_project();
    project.audio.frame_count = 2;
    const auto score = transcribe::serialize_score_document(project.score);
    transcribe::relink_project_audio(project, source.parent_path() / "take.transcriber", source);
    REQUIRE(transcribe::serialize_score_document(project.score) == score);
    REQUIRE(project.audio.relative_to_project);
    project.audio.frame_count = 3;
    REQUIRE_THROWS(transcribe::relink_project_audio(project, {}, source));
    REQUIRE(project.audio.frame_count == 3);
    project.audio.frame_count = 2;
    project.audio.sample_rate = 44100;
    REQUIRE_THROWS(transcribe::relink_project_audio(project, {}, source));
    std::filesystem::remove(source);
}

TEST_CASE("live saves use immutable assets and do not replace older recovery audio",
          "[project][audio][persistence]") {
    const auto root = temporary_base("live-project");
    std::filesystem::create_directory(root);
    const auto path = root / "live.transcriber";
    transcribe::AudioBuffer audio;
    audio.sample_rate = 48000;
    audio.samples = {.1f, -.2f};
    auto project = sample_project();
    const auto first = transcribe::save_project_with_live_audio(path, project, audio);
    const auto first_audio = transcribe::resolve_project_audio_path(first, path);
    audio.samples = {.8f};
    const auto next = transcribe::save_project_with_live_audio(path, first, audio);
    REQUIRE(first.audio.policy == transcribe::ProjectAudioPolicy::ProjectCopy);
    REQUIRE(next.audio.reference_path != first.audio.reference_path);
    REQUIRE(transcribe::read_wav(first_audio).samples == std::vector<float>{.1f, -.2f});
    project.title.clear();
    REQUIRE_THROWS(transcribe::save_project_with_live_audio(path, project, audio));
    REQUIRE(transcribe::load_project_file(path).audio.reference_path == next.audio.reference_path);
    REQUIRE(transcribe::load_project_file(path).audio.frame_count == 1);
    const auto empty_path = root / "empty.transcriber";
    { std::ofstream empty(empty_path); }
    REQUIRE_THROWS(transcribe::load_project_file(empty_path));
    for (const auto& file : std::filesystem::directory_iterator(root))
        std::filesystem::remove(file.path());
    std::filesystem::remove(root);
}

TEST_CASE("documented guitar project fixture opens and survives canonical round trips",
          "[project][fixture]") {
    const auto path = std::filesystem::path(__FILE__).parent_path() /
        "fixtures" / "projects" / "guitar-phrase.transcriber";
    const auto project = transcribe::load_project_file(path);
    REQUIRE(project.schema_version == 2);
    REQUIRE(project.audio.policy == transcribe::ProjectAudioPolicy::ScoreOnly);
    REQUIRE(project.score.notes.size() == 1);
    REQUIRE(project.score.notes.front().sounding_midi_pitch == 64);
    REQUIRE(project.annotations.front().id == "project-notes");
    const auto serialized = transcribe::serialize_project_document(project);
    REQUIRE(transcribe::serialize_project_document(
        transcribe::deserialize_project_document(serialized)) == serialized);
}

TEST_CASE("project save and import enforce the same byte limit",
          "[project][limits]") {
    auto project = sample_project();
    for (std::size_t index = 0; index < 4096; ++index)
        project.annotations.push_back({std::to_string(index), {}, std::string(16384, 'x')});
    REQUIRE_THROWS_AS(transcribe::serialize_project_document(project), std::length_error);
}

TEST_CASE("versioned projects round trip score, audio, configuration and exports",
          "[project][serialization]") {
    const auto original = sample_project();
    const auto restored =
        transcribe::deserialize_project_document(
            transcribe::serialize_project_document(original));
    REQUIRE(restored.schema_version == transcribe::project_schema_version);
    REQUIRE(restored.id == original.id);
    REQUIRE(restored.title == original.title);
    REQUIRE(restored.audio.policy ==
            transcribe::ProjectAudioPolicy::ExternalReference);
    REQUIRE(restored.audio.reference_path == "audio/source.wav");
    REQUIRE(restored.audio.relative_to_project);
    REQUIRE(restored.audio.sample_rate == 48000);
    REQUIRE(restored.audio.frame_count == 144000);
    REQUIRE(restored.analysis.live_backend == "spectral");
    REQUIRE_FALSE(restored.analysis.tempo_automatic);
    REQUIRE_FALSE(restored.analysis.meter_automatic);
    REQUIRE_FALSE(restored.analysis.key_automatic);
    REQUIRE(restored.analysis.preserve_expressive_timing);
    REQUIRE(restored.exports.size() == 1);
    REQUIRE(restored.exports.front().score_revision == original.score.revision);
    REQUIRE(restored.score.id == original.score.id);
    REQUIRE(restored.score.history.size() == original.score.history.size());
    REQUIRE(restored.score.history.back().reason ==
            original.score.history.back().reason);
}

TEST_CASE("legacy score-only documents migrate to an explicit linked project",
          "[project][migration]") {
    auto score = transcribe::make_empty_score_document("legacy-score");
    score.source_id = "old-take.wav";
    const auto project =
        transcribe::deserialize_project_document(
            transcribe::serialize_score_document(score));
    REQUIRE(project.id == "legacy-score-project");
    REQUIRE(project.audio.policy ==
            transcribe::ProjectAudioPolicy::ExternalReference);
    REQUIRE(project.audio.reference_path == "old-take.wav");
    REQUIRE(project.audio.relative_to_project);
    REQUIRE(project.score.id == "legacy-score");
    REQUIRE(project.exports.empty());
}

TEST_CASE("project schema version zero migrates without losing the score",
          "[project][migration]") {
    const auto score = transcribe::serialize_score_document(
        transcribe::make_empty_score_document("v0-score"));
    const auto legacy =
        "{\"format\":\"transcriber-project\","
        "\"project_schema_version\":0,\"id\":\"v0-project\","
        "\"title\":\"Old project\",\"score\":" + score + "}";
    const auto migrated = transcribe::deserialize_project_document(legacy);
    REQUIRE(migrated.id == "v0-project");
    REQUIRE(migrated.title == "Old project");
    REQUIRE(migrated.schema_version == transcribe::project_schema_version);
    REQUIRE(migrated.score.id == "v0-score");
}

TEST_CASE("project loading rejects malformed and unsupported project data",
          "[project][validation]") {
    REQUIRE_THROWS(transcribe::deserialize_project_document("{"));
    REQUIRE_THROWS(transcribe::deserialize_project_document(
        "{\"format\":\"transcriber-project\","
        "\"project_schema_version\":99}"));
    REQUIRE_THROWS(transcribe::deserialize_project_document(
        "{\"format\":\"not-a-transcriber-project\"}"));

    auto invalid = sample_project();
    invalid.audio.reference_path.clear();
    REQUIRE_THROWS(transcribe::serialize_project_document(invalid));
    invalid = sample_project();
    invalid.exports.front().score_revision = invalid.score.revision + 1;
    REQUIRE_THROWS(transcribe::serialize_project_document(invalid));
}

TEST_CASE("atomic project saves preserve the previous file and ignore stale temps",
          "[project][persistence]") {
    const auto path = temporary_base("project").string() + ".transcriber";
    const auto stale_path = path + ".tmp-stale";
    const auto cleanup = [&] {
        std::error_code error;
        std::filesystem::remove(path, error);
        std::filesystem::remove(stale_path, error);
    };
    cleanup();
    transcribe::save_project_file_atomic(path, sample_project());
    const auto first = transcribe::load_project_file(path);
    REQUIRE(first.id == "project-fixture");

    {
        std::ofstream stale(stale_path, std::ios::binary);
        stale << "interrupted write";
    }
    auto invalid = sample_project();
    invalid.title.clear();
    REQUIRE_THROWS(transcribe::save_project_file_atomic(path, invalid));
    REQUIRE(transcribe::load_project_file(path).title == "Fixture project");

    auto updated = sample_project();
    updated.title = "Saved revision";
    transcribe::save_project_file_atomic(path, updated);
    REQUIRE(transcribe::load_project_file(path).title == "Saved revision");
    cleanup();
}

TEST_CASE("live audio assets are saved as readable IEEE-float WAV files",
          "[project][audio]") {
    const auto path = temporary_base("audio").string() + ".wav";
    transcribe::AudioBuffer original;
    original.sample_rate = 44100;
    original.samples = {-1.0f, -0.25f, 0.0f, 0.5f, 1.0f};
    transcribe::write_audio_wav_atomic(path, original);
    const auto restored = transcribe::read_wav(path);
    REQUIRE(restored.sample_rate == original.sample_rate);
    REQUIRE(restored.samples.size() == original.samples.size());
    for (std::size_t index = 0; index < original.samples.size(); ++index) {
        REQUIRE(restored.samples[index] == Catch::Approx(original.samples[index]));
    }
    std::error_code error;
    std::filesystem::remove(path, error);
}
