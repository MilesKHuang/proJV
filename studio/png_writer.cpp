// proJV studio -- a minimal PNG writer (see png_writer.h).
#include "png_writer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace studio {
namespace {

unsigned crc32Of(const unsigned char* data, size_t n) {
    static unsigned table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned i = 0; i < 256; ++i) {
            unsigned c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

unsigned adler32Of(const unsigned char* data, size_t n) {
    unsigned a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void be32(std::vector<unsigned char>& out, unsigned v) {
    out.push_back(static_cast<unsigned char>(v >> 24));
    out.push_back(static_cast<unsigned char>(v >> 16));
    out.push_back(static_cast<unsigned char>(v >> 8));
    out.push_back(static_cast<unsigned char>(v));
}

void chunk(std::vector<unsigned char>& out, const char* type, const unsigned char* data,
           size_t n) {
    be32(out, static_cast<unsigned>(n));
    std::vector<unsigned char> body;
    body.reserve(n + 4);
    body.insert(body.end(), type, type + 4);
    if (n > 0) body.insert(body.end(), data, data + n);
    out.insert(out.end(), body.begin(), body.end());
    be32(out, crc32Of(body.data(), body.size()));
}

} // namespace

bool writePng(const std::string& path, int width, int height, const unsigned char* rgb) {
    if (!rgb || width <= 0 || height <= 0) return false;
    const size_t stride = static_cast<size_t>(width) * 3u;

    // Raw scanlines: one filter byte (0 = none) in front of every row.
    std::vector<unsigned char> raw;
    raw.reserve((stride + 1u) * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb + stride * static_cast<size_t>(y),
                   rgb + stride * static_cast<size_t>(y) + stride);
    }

    // zlib stream: header, stored blocks (<= 65535 bytes each), Adler32.
    std::vector<unsigned char> z;
    z.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    z.push_back(0x78); // CMF: deflate, 32K window
    z.push_back(0x01); // FLG: no dictionary, fastest
    size_t pos = 0;
    while (pos < raw.size() || pos == 0) {
        const size_t take = std::min<size_t>(65535u, raw.size() - pos);
        const bool last = (pos + take) >= raw.size();
        z.push_back(last ? 0x01 : 0x00); // BFINAL + BTYPE=00 (stored)
        z.push_back(static_cast<unsigned char>(take & 0xFFu));
        z.push_back(static_cast<unsigned char>((take >> 8) & 0xFFu));
        z.push_back(static_cast<unsigned char>(~take & 0xFFu));
        z.push_back(static_cast<unsigned char>((~take >> 8) & 0xFFu));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos),
                 raw.begin() + static_cast<long>(pos + take));
        pos += take;
        if (last) break;
    }
    be32(z, adler32Of(raw.data(), raw.size()));

    std::vector<unsigned char> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    unsigned char ihdr[13] = {};
    ihdr[0] = static_cast<unsigned char>(width >> 24);
    ihdr[1] = static_cast<unsigned char>(width >> 16);
    ihdr[2] = static_cast<unsigned char>(width >> 8);
    ihdr[3] = static_cast<unsigned char>(width);
    ihdr[4] = static_cast<unsigned char>(height >> 24);
    ihdr[5] = static_cast<unsigned char>(height >> 16);
    ihdr[6] = static_cast<unsigned char>(height >> 8);
    ihdr[7] = static_cast<unsigned char>(height);
    ihdr[8] = 8; // bit depth
    ihdr[9] = 2; // colour type: truecolour RGB
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    chunk(png, "IHDR", ihdr, sizeof(ihdr));
    chunk(png, "IDAT", z.data(), z.size());
    chunk(png, "IEND", nullptr, 0);

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const size_t written = std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    return written == png.size();
}

} // namespace studio
