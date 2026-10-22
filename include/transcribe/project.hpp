#pragma once

#include "transcribe/score.hpp"
#include "transcribe/audio.hpp"

#include <cstdint>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace transcribe {

inline constexpr std::uint32_t project_schema_version = 2;
inline constexpr std::size_t maximum_project_bytes = 64U * 1024U * 1024U;

enum class ProjectAudioPolicy { ScoreOnly, ExternalReference, ProjectCopy };

struct ProjectAudio {
    ProjectAudioPolicy policy{ProjectAudioPolicy::ScoreOnly};
    std::string reference_path;
    bool relative_to_project{};
    std::uint32_t sample_rate{};
    std::uint32_t channels{1};
    std::uint64_t frame_count{};
    std::string original_reference;
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
    std::string quantization{"legacy"};
    double quantization_strength{1.0};
    double minimum_rhythm_evidence{0.65};
    bool retain_review_evidence{};
    bool bounded_inference{};
    std::size_t fingering_alternatives{5};
    int maximum_fret{24};
    int preferred_position{5};
    std::string provider{"cpu"};
    std::string model_version;
};

struct ProjectAnnotation {
    std::string id;
    std::string note_id;
    std::string text;
};

struct ProjectFingeringPosition {
    std::string note_id;
    int string_number{};
    int fret{};
};

struct ProjectFingeringAlternative {
    double cost{};
    std::array<double, 9> cost_components{};
    std::vector<ProjectFingeringPosition> positions;
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
    std::vector<ProjectAnnotation> annotations;
    std::vector<ProjectFingeringAlternative> fingering_alternatives;
    bool fingering_alternatives_truncated{};
};

void validate_project_document(const ProjectDocument& project);
std::string serialize_project_document(const ProjectDocument& project);
ProjectDocument deserialize_project_document(std::string_view json);
std::filesystem::path resolve_project_audio_path(
    const ProjectDocument& project, const std::filesystem::path& project_path);
void save_project_file_atomic(const std::filesystem::path& path,
                              const ProjectDocument& project);
ProjectDocument load_project_file(const std::filesystem::path& path);
ProjectDocument save_project_with_audio_copy(
    const std::filesystem::path& path, ProjectDocument project,
    const std::filesystem::path& source_audio);
ProjectDocument save_project_with_live_audio(
    const std::filesystem::path& path, ProjectDocument project, const AudioBuffer& audio);
void relink_project_audio(ProjectDocument& project,
                          const std::filesystem::path& project_path,
                          const std::filesystem::path& source_audio);
void write_audio_wav_atomic(const std::filesystem::path& path,
                            const AudioBuffer& audio);

} // namespace transcribe
