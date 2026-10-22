#pragma once

#include <filesystem>
#include <string>

namespace transcribe {

inline std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {encoded.begin(), encoded.end()};
}

} // namespace transcribe
