#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gba::host {

// Writes an RGB888 image as a PNG. Returns false if the file cannot be opened.
//
// Uses stored (uncompressed) deflate blocks, so there is no zlib dependency for
// the sake of dumping a 240x160 frame. Files are larger than they need to be
// and nothing here cares.
bool writePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t width,
              uint32_t height);

// Converts a framebuffer of native BGR555 (two pixels per word) to RGB888.
std::vector<uint8_t> bgr555ToRgb(const std::vector<uint32_t>& fb, uint32_t width, uint32_t height);

}  // namespace gba::host
