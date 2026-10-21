#include "transcribe/project.hpp"

#include "transcribe/audio.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace transcribe {
namespace {

constexpr std::size_t maximum_project_bytes = 64U * 1024U * 1024U;
std::atomic<std::uint64_t> temporary_file_sequence{0};

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string utf8;
    utf8.reserve(value.size());
    for (const unsigned char character : value) {
        utf8.push_back(static_cast<char8_t>(character));
    }
    return std::filesystem::path(utf8);
}

std::filesystem::path temporary_path_for(const std::filesystem::path& path) {
    const auto sequence = temporary_file_sequence.fetch_add(1);
#ifdef _WIN32
    const auto process = static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    const auto process = static_cast<std::uint64_t>(getpid());
#endif
    return path.parent_path() /
        (path.filename().native() + std::filesystem::path(
            ".tmp-" + std::to_string(process) + "-" +
            std::to_string(sequence)).native());
}

void replace_file_atomically(const std::filesystem::path& path,
                             std::string_view bytes) {
    if (path.empty()) throw std::invalid_argument("Project output path must not be empty.");
    const auto parent = path.has_parent_path()
        ? path.parent_path() : std::filesystem::current_path();
    std::error_code filesystem_error;
    if (!std::filesystem::is_directory(parent, filesystem_error) ||
        filesystem_error) {
        throw std::filesystem::filesystem_error(
            "Project output folder does not exist.", parent,
            filesystem_error ? filesystem_error :
                std::make_error_code(std::errc::not_a_directory));
    }

    const auto temporary = temporary_path_for(path);
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(),
                                "Could not create a temporary project file");
    }
    bool file_open = true;
    try {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto request = static_cast<DWORD>(std::min<std::size_t>(
                bytes.size() - written, std::numeric_limits<DWORD>::max()));
            DWORD count = 0;
            if (!WriteFile(file, bytes.data() + written, request, &count, nullptr) ||
                count == 0) {
                throw std::system_error(static_cast<int>(GetLastError()),
                                        std::system_category(),
                                        "Could not write the project file");
            }
            written += count;
        }
        if (!FlushFileBuffers(file)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Could not flush the project file");
        }
        if (!CloseHandle(file)) {
            file_open = false;
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Could not close the project file");
        }
        file_open = false;
        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(),
                                    "Could not atomically replace the project file");
        }
    } catch (...) {
        if (file_open) CloseHandle(file);
        std::filesystem::remove(temporary, filesystem_error);
        throw;
    }
#else
    const int descriptor = ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY,
                                  S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "Could not create a temporary project file");
    }
    bool file_open = true;
    try {
        std::size_t written = 0;
        while (written < bytes.size()) {
            const auto count = ::write(descriptor, bytes.data() + written,
                                       bytes.size() - written);
            if (count < 0) {
                if (errno == EINTR) continue;
                throw std::system_error(errno, std::generic_category(),
                                        "Could not write the project file");
            }
            if (count == 0) {
                throw std::runtime_error("Could not complete the project file write.");
            }
            written += static_cast<std::size_t>(count);
        }
        if (::fsync(descriptor) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "Could not flush the project file");
        }
        if (::close(descriptor) != 0) {
            file_open = false;
            throw std::system_error(errno, std::generic_category(),
                                    "Could not close the project file");
        }
        file_open = false;
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            throw std::system_error(errno, std::generic_category(),
                                    "Could not atomically replace the project file");
        }
        const int directory = ::open(parent.c_str(), O_RDONLY);
        if (directory >= 0) {
            (void)::fsync(directory);
            (void)::close(directory);
        }
    } catch (...) {
        if (file_open) ::close(descriptor);
        std::filesystem::remove(temporary, filesystem_error);
        throw;
    }
#endif
}

void append_u16(std::string& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<char>(value & 0xff));
    bytes.push_back(static_cast<char>((value >> 8) & 0xff));
}

void append_u32(std::string& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<char>((value >> shift) & 0xff));
    }
}

} // namespace

std::filesystem::path resolve_project_audio_path(
    const ProjectDocument& project, const std::filesystem::path& project_path) {
    if (project.audio.policy != ProjectAudioPolicy::ExternalReference ||
        project.audio.reference_path.empty()) {
        return {};
    }
    auto audio_path = path_from_utf8(project.audio.reference_path);
    if (project.audio.relative_to_project) {
        audio_path = project_path.parent_path() / audio_path;
    }
    return std::filesystem::absolute(audio_path).lexically_normal();
}

void save_project_file_atomic(const std::filesystem::path& path,
                              const ProjectDocument& project) {
    const auto serialized = serialize_project_document(project);
    replace_file_atomically(path, serialized);
}

ProjectDocument load_project_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw std::filesystem::filesystem_error(
            "Could not read project file size.", path, error);
    }
    if (size == 0 || size > maximum_project_bytes) {
        throw std::length_error("Project file is empty or exceeds the 64 MiB limit.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open project file.");
    std::string content(static_cast<std::size_t>(size), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Could not read the complete project file.");
    }
    return deserialize_project_document(content);
}

void write_audio_wav_atomic(const std::filesystem::path& path,
                            const AudioBuffer& audio) {
    if (audio.sample_rate == 0 ||
        audio.samples.size() >
            (std::numeric_limits<std::uint32_t>::max() - 36U) / sizeof(float)) {
        throw std::invalid_argument("Live audio cannot be represented as a WAV file.");
    }
    std::string bytes;
    bytes.reserve(44 + audio.samples.size() * sizeof(float));
    bytes.append("RIFF", 4);
    append_u32(bytes, static_cast<std::uint32_t>(36 + audio.samples.size() * 4));
    bytes.append("WAVEfmt ", 8);
    append_u32(bytes, 16);
    append_u16(bytes, 3);
    append_u16(bytes, 1);
    append_u32(bytes, audio.sample_rate);
    append_u32(bytes, audio.sample_rate * 4);
    append_u16(bytes, 4);
    append_u16(bytes, 32);
    bytes.append("data", 4);
    append_u32(bytes, static_cast<std::uint32_t>(audio.samples.size() * 4));
    for (const float sample : audio.samples) {
        append_u32(bytes, std::bit_cast<std::uint32_t>(
                              std::clamp(sample, -1.0f, 1.0f)));
    }
    replace_file_atomically(path, bytes);
}

} // namespace transcribe
