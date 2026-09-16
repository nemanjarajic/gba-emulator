#include "host/png.h"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace gba::host {
namespace {

uint32_t crcTable(uint32_t n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    return c;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < len; ++i) crc = crcTable((crc ^ data[i]) & 0xFF) ^ (crc >> 8);
    return crc;
}

void be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& body) {
    be32(out, uint32_t(body.size()));
    const size_t start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32(out.data() + start, out.size() - start) ^ 0xFFFFFFFFu);
}

}  // namespace

bool writePng(const std::string& path, const std::vector<uint8_t>& rgb, uint32_t width,
              uint32_t height) {
    if (rgb.size() != size_t(width) * height * 3) return false;

    // Raw scanlines, each prefixed with filter type 0 (none).
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (1 + size_t(width) * 3));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        const uint8_t* row = rgb.data() + size_t(y) * width * 3;
        raw.insert(raw.end(), row, row + size_t(width) * 3);
    }

    // zlib stream: header, stored deflate blocks, Adler-32 of the raw data.
    std::vector<uint8_t> z{0x78, 0x01};
    for (size_t off = 0; off < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - off);
        const bool last = (off + n) == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n));
        z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n));
        z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) {
        a = (a + v) % 65521;
        b = (b + a) % 65521;
    }
    be32(z, (b << 16) | a);

    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> ihdr;
    be32(ihdr, width);
    be32(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(2);  // colour type: truecolour RGB
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", {});

    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
    return f.good();
}

std::vector<uint8_t> bgr555ToRgb(const std::vector<uint32_t>& fb, uint32_t width,
                                 uint32_t height) {
    std::vector<uint8_t> rgb(size_t(width) * height * 3, 0);
    for (uint32_t i = 0; i < width * height; ++i) {
        const uint32_t c = (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
        // BGR555 -> RGB888. Replicating the top bits into the low ones makes
        // full-scale 31 map to 255 rather than 248.
        const uint32_t r = (c >> 0) & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
        rgb[i * 3 + 0] = uint8_t((r << 3) | (r >> 2));
        rgb[i * 3 + 1] = uint8_t((g << 3) | (g >> 2));
        rgb[i * 3 + 2] = uint8_t((b << 3) | (b >> 2));
    }
    return rgb;
}

}  // namespace gba::host
