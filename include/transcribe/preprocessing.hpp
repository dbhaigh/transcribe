#pragma once

#include "transcribe/audio.hpp"

#include <stdexcept>
#include <string>

namespace transcribe {

class PreprocessingUnavailable : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PreprocessingCancelled : public std::runtime_error {
public:
    PreprocessingCancelled() : std::runtime_error("Audio preprocessing was cancelled.") {}
};

struct PreprocessingRequest {
    std::filesystem::path original;
    std::filesystem::path isolated;
    std::string target;
    bool fallback_to_original{};
};

struct AudioFileIdentity {
    std::filesystem::path path;
    std::uintmax_t bytes{};
    std::filesystem::file_time_type modified;
};

struct PreparedAudio {
    std::filesystem::path analysis_path;
    bool isolated{};
    std::string diagnostic;
    std::vector<AudioFileIdentity> identities;
};

// Local backends own their temporary files. Returned stems must start at sample
// zero, with exactly the original rate and frame count; no implicit resampling.
class AudioPreprocessor {
public:
    virtual ~AudioPreprocessor() = default;
    virtual std::filesystem::path isolate(const PreprocessingRequest& request,
                                         std::stop_token cancellation) = 0;
};

PreparedAudio prepare_audio(const PreprocessingRequest& request,
                            std::stop_token cancellation = {},
                            AudioPreprocessor* backend = nullptr);
void verify_prepared_audio(const PreparedAudio& prepared);
void protect_preprocessing_inputs(const PreprocessingRequest& request,
                                  std::span<const std::filesystem::path> outputs);

} // namespace transcribe
