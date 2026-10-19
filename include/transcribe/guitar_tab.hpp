#pragma once

#include "transcribe/transcriber.hpp"

#include <filesystem>
#include <vector>

namespace transcribe {

void write_guitar_tab(const std::filesystem::path& path, const std::vector<Note>& notes);

} // namespace transcribe
