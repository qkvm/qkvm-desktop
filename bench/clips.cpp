#include "clips.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <system_error>

namespace qkvm::bench {

namespace {

// The largest made file is the 4K clip, some tens of megabytes.
constexpr std::uintmax_t largestFile = std::uintmax_t{512} * 1024 * 1024;

// The kinds of unit in an H.264 stream that matter for cutting it.
constexpr int keyFrameSlice = 5;
constexpr int delimiter = 9;

} // namespace

base::Result<std::vector<std::byte>> readFile(const std::filesystem::path& file)
{
    std::error_code failed;
    const std::uintmax_t size = std::filesystem::file_size(file, failed);
    if (failed) {
        return base::Error{"not_found", file.string() + " cannot be read: " + failed.message()};
    }
    if (size > largestFile) {
        return base::Error{"too_large", file.string() + " is larger than a made clip can be."};
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return base::Error{"not_found", file.string() + " cannot be read."};
    }
    return bytes;
}

std::vector<Cut> cutIntoPictures(std::span<const std::byte> stream)
{
    std::vector<Cut> cuts;
    const auto at = [&](std::size_t index) { return std::to_integer<int>(stream[index]); };

    Cut current;
    bool open = false;
    // A unit begins behind 00 00 01; the five low bits of its first byte say what it is.
    for (std::size_t index = 0; index + 3 < stream.size(); ++index) {
        if (at(index) != 0 || at(index + 1) != 0 || at(index + 2) != 1) {
            continue;
        }
        const int kind = at(index + 3) & 0x1F;
        if (kind == delimiter || !open) {
            // The zero that may stand before the three bytes belongs to the picture it begins.
            const std::size_t from = index > 0 && at(index - 1) == 0 ? index - 1 : index;
            if (open) {
                current.size = from - current.from;
                cuts.push_back(current);
            }
            current = Cut{from, 0, false};
            open = true;
        }
        if (kind == keyFrameSlice) {
            current.keyFrame = true;
        }
        index += 2;
    }
    if (open) {
        current.size = stream.size() - current.from;
        cuts.push_back(current);
    }
    return cuts;
}

std::vector<std::filesystem::path> jpegsIn(const std::filesystem::path& folder)
{
    std::vector<std::filesystem::path> files;
    std::error_code failed;
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(folder, failed)) {
        if (entry.is_regular_file(failed) && entry.path().extension() == ".jpg") {
            files.push_back(entry.path());
        }
    }
    std::ranges::sort(files);
    return files;
}

} // namespace qkvm::bench
