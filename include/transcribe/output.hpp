#pragma once

#include <filesystem>
#include <string_view>

namespace transcribe {

// Replaces one file only after its complete contents have been written and flushed.
void write_output_atomic(const std::filesystem::path& path, std::string_view bytes);

} // namespace transcribe
