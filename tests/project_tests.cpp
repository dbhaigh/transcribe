#include "transcribe/project.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
