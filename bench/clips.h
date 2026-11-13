#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include <qkvm/base/result.h>

// The pictures tools/make_testdata.ps1 makes, read back: for the measuring rig, and for the tests
// of the engine. The engine itself is handed pictures that are cut already (DESIGN.md section 9).
namespace qkvm::bench {

// A file, whole. Refused when it is larger than any of the made ones can be.
base::Result<std::vector<std::byte>> readFile(const std::filesystem::path& file);

// One picture of an H.264 file: where its bytes are in the file, and whether it is a key frame.
struct Cut {
    std::size_t from = 0;
    std::size_t size = 0;
    bool keyFrame = false;
};

// Cuts an H.264 stream in Annex B form into its pictures, at the delimiter that stands before each.
// A stream without delimiters comes back as one piece: this is no parser, the files are made to
// be cut this way.
std::vector<Cut> cutIntoPictures(std::span<const std::byte> stream);

// The JPEGs of a folder, in the order of their names.
std::vector<std::filesystem::path> jpegsIn(const std::filesystem::path& folder);

} // namespace qkvm::bench
