#pragma once

#include "transcribe/score.hpp"
#include "transcribe/audio.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace transcribe {

inline constexpr std::uint32_t project_schema_version = 1;

enum class ProjectAudioPolicy { ScoreOnly, ExternalReference };

struct ProjectAudio {
    ProjectAudioPolicy policy{ProjectAudioPolicy::ScoreOnly};
    std::string reference_path;
    bool relative_to_project{};
    std::uint32_t sample_rate{};
    std::uint32_t channels{1};
    std::uint64_t frame_count{};
};

struct ProjectAnalysisSettings {
    std::string backend_identifier{"builtin:yin"};
    std::string model_identifier{"builtin:yin"};
    std::string model_reference;
    std::string instrument_prior{"auto"};
    std::string live_backend{"yin"};
    bool tempo_automatic{true};
    bool meter_automatic{true};
    bool key_automatic{true};
    bool polyphonic{};
    bool fast_preview{};
    bool preserve_expressive_timing{};
    double latency_ms{30.0};
};

struct ProjectExportStatus {
    std::string format;
    std::string path;
    std::uint64_t score_revision{};
    bool success{};
    std::string error;
};

struct ProjectDocument {
    std::uint32_t schema_version{project_schema_version};
    std::string id;
    std::string title{"Untitled project"};
    std::string last_saved_path;
    ProjectAudio audio;
    ProjectAnalysisSettings analysis;
    ScoreDocument score;
    std::vector<ProjectExportStatus> exports;
};

void validate_project_document(const ProjectDocument& project);
std::string serialize_project_document(const ProjectDocument& project);
ProjectDocument deserialize_project_document(std::string_view json);
std::filesystem::path resolve_project_audio_path(
    const ProjectDocument& project, const std::filesystem::path& project_path);
void save_project_file_atomic(const std::filesystem::path& path,
                              const ProjectDocument& project);
ProjectDocument load_project_file(const std::filesystem::path& path);
void write_audio_wav_atomic(const std::filesystem::path& path,
                            const AudioBuffer& audio);

} // namespace transcribe
