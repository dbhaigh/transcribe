#include "transcribe/preprocessing.hpp"
#include "transcribe/path.hpp"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace transcribe {
namespace {

AudioFileIdentity identify(const std::filesystem::path& path) {
    return {path, std::filesystem::file_size(path), std::filesystem::last_write_time(path)};
}

void check_cancelled(std::stop_token cancellation) {
    if (cancellation.stop_requested()) throw PreprocessingCancelled();
}

void validate_samples(const std::filesystem::path& path, std::stop_token cancellation) {
    read_wav_blocks(path, 16384, [&](const AudioBlockView& block) {
        check_cancelled(cancellation);
        if (std::any_of(block.samples.begin(), block.samples.end(),
                        [](float sample) { return !std::isfinite(sample); })) {
            throw std::invalid_argument("Preprocessing audio contains nonfinite samples: " + path_utf8(path));
        }
    });
}

} // namespace

void verify_prepared_audio(const PreparedAudio& prepared) {
    for (const auto& identity : prepared.identities) {
        if (std::filesystem::file_size(identity.path) != identity.bytes ||
            std::filesystem::last_write_time(identity.path) != identity.modified) {
            throw std::runtime_error("Audio source changed during preprocessing/transcription: " +
                                     path_utf8(identity.path) + "; retry with immutable source files.");
        }
    }
}

void protect_preprocessing_inputs(const PreprocessingRequest& request,
                                  std::span<const std::filesystem::path> outputs) {
    for (const auto& input : {request.original, request.isolated}) {
        if (input.empty()) continue;
        for (const auto& output : outputs) {
            if (output.empty()) continue;
            std::error_code error;
            bool same = std::filesystem::equivalent(input, output, error) && !error;
            auto a = std::filesystem::weakly_canonical(input).native();
            auto b = std::filesystem::weakly_canonical(output).native();
#ifdef _WIN32
            const auto lower = [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); };
            std::transform(a.begin(), a.end(), a.begin(), lower);
            std::transform(b.begin(), b.end(), b.begin(), lower);
#endif
            if (same || a == b) {
                throw std::invalid_argument("Output would overwrite mixture or isolated audio: " + path_utf8(output));
            }
        }
    }
}

PreparedAudio prepare_audio(const PreprocessingRequest& request,
                            std::stop_token cancellation, AudioPreprocessor* backend) {
    check_cancelled(cancellation);
    PreparedAudio result{request.original, false, "Preprocessing disabled.", {}};
    if (request.isolated.empty() && !backend) return result;
    if (request.target != "guitar" && request.target != "bass" &&
        request.target != "piano" && request.target != "voice") {
        throw std::invalid_argument("Isolated audio requires an explicit guitar, bass, piano or voice profile.");
    }
    result.identities.push_back(identify(request.original));
    const auto original = inspect_wav(request.original);
    std::filesystem::path stem;
    try {
        stem = backend ? backend->isolate(request, cancellation) : request.isolated;
        check_cancelled(cancellation);
        if (!std::filesystem::exists(stem)) {
            throw PreprocessingUnavailable("Isolated audio is unavailable: " + path_utf8(stem));
        }
    } catch (const PreprocessingUnavailable& error) {
        check_cancelled(cancellation);
        if (!request.fallback_to_original) throw;
        verify_prepared_audio(result);
        result.diagnostic = std::string(error.what()) +
            "; explicit fallback: transcribing the original mixture, not the target source.";
        return result;
    }
    if (std::filesystem::equivalent(request.original, stem)) {
        throw std::invalid_argument("Isolated audio must be a different file from the original mixture.");
    }
    result.identities.push_back(identify(stem));
    const auto isolated = inspect_wav(stem);
    if (original.sample_rate != isolated.sample_rate || original.frames != isolated.frames ||
        original.frames == 0) {
        throw std::invalid_argument(
            "Isolated WAV must preserve the original sample rate and exact frame count, starting at sample zero. "
            "Trim separator latency/padding explicitly; shortened or resampled stems cannot be aligned implicitly.");
    }
    validate_samples(stem, cancellation);
    check_cancelled(cancellation);
    verify_prepared_audio(result);
    result.analysis_path = stem;
    result.isolated = true;
    result.diagnostic = "Using user-supplied " + request.target +
        " isolation at original timestamps. Equal length/rate do not prove acoustic alignment or source identity; "
        "audition and compare against the mixture. Separation can suppress notes or introduce false attacks.";
    return result;
}

} // namespace transcribe
