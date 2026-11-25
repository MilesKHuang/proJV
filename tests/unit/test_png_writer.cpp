// png_writer: the studio's --screenshot encoder (design 08.6).
//
// A screenshot that cannot be opened is worse than no screenshot, and the only
// way to check this by eye is to open one every time. These cases read the bytes
// back: the chunk framing, the IHDR the decoder reads its dimensions from, and
// the stored-block zlib stream (which is what makes the writer dependency-free
// in the first place).
#include "doctest.h"

#include "png_writer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::vector<unsigned char> readFile(const std::string& path) {
    std::vector<unsigned char> out;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return out;
    unsigned char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return out;
}

unsigned be32(const std::vector<unsigned char>& b, size_t at) {
    return (static_cast<unsigned>(b[at]) << 24) | (static_cast<unsigned>(b[at + 1]) << 16) |
           (static_cast<unsigned>(b[at + 2]) << 8) | static_cast<unsigned>(b[at + 3]);
}

bool hasChunk(const std::vector<unsigned char>& png, const char* type) {
    size_t at = 8;
    while (at + 8 <= png.size()) {
        const unsigned len = be32(png, at);
        const std::string name(reinterpret_cast<const char*>(png.data() + at + 4), 4);
        if (name == type) return true;
        at += 12u + len;
    }
    return false;
}

std::vector<unsigned char> chunkData(const std::vector<unsigned char>& png, const char* type) {
    size_t at = 8;
    while (at + 8 <= png.size()) {
        const unsigned len = be32(png, at);
        const std::string name(reinterpret_cast<const char*>(png.data() + at + 4), 4);
        if (name == type) return std::vector<unsigned char>(png.begin() + at + 8, png.begin() + at + 8 + len);
        at += 12u + len;
    }
    return {};
}

std::string tempPng() { return "test_png_writer_tmp.png"; }

} // namespace

TEST_CASE("png_writer: the file is framed the way a decoder reads it") {
    // 3x2 pixels, one distinct colour per row so a row swap would be visible.
    std::vector<unsigned char> rgb;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 3; ++x) {
            rgb.push_back(static_cast<unsigned char>(y == 0 ? 0xFF : 0x00));
            rgb.push_back(static_cast<unsigned char>(x * 40));
            rgb.push_back(0x20);
        }
    REQUIRE(studio::writePng(tempPng(), 3, 2, rgb.data()));

    const auto png = readFile(tempPng());
    std::remove(tempPng().c_str());
    REQUIRE(png.size() > 8);
    CHECK(png[0] == 0x89);
    CHECK(std::string(png.begin() + 1, png.begin() + 4) == "PNG");

    const auto ihdr = chunkData(png, "IHDR");
    REQUIRE(ihdr.size() == 13);
    CHECK(be32(ihdr, 0) == 3);  // width
    CHECK(be32(ihdr, 4) == 2);  // height
    CHECK(ihdr[8] == 8);        // 8 bits per channel
    CHECK(ihdr[9] == 2);        // truecolour
    CHECK(hasChunk(png, "IDAT"));
    CHECK(hasChunk(png, "IEND"));
}

TEST_CASE("png_writer: the pixel stream is stored deflate, one filter byte per row") {
    std::vector<unsigned char> rgb(3 * 2 * 3, 0x7F); // 3 wide, 2 tall
    REQUIRE(studio::writePng(tempPng(), 3, 2, rgb.data()));
    const auto idat = chunkData(readFile(tempPng()), "IDAT");
    std::remove(tempPng().c_str());

    REQUIRE(idat.size() > 6);
    CHECK(idat[0] == 0x78); // zlib CMF
    CHECK(idat[1] == 0x01); // FLG: no preset dictionary
    CHECK(idat[2] == 0x01); // BFINAL=1, BTYPE=00 (stored)
    const unsigned len = static_cast<unsigned>(idat[3]) | (static_cast<unsigned>(idat[4]) << 8);
    CHECK(len == 2u * (1u + 3u * 3u)); // 2 rows of (filter byte + 9 bytes)
    // zlib header (2) + block header (1) + LEN/NLEN (4) = the data starts at 7.
    CHECK(idat[7] == 0);    // the filter byte of row 0
    CHECK(idat[8] == 0x7F); // first pixel, red
}

TEST_CASE("png_writer: the same pixels always produce the same bytes") {
    std::vector<unsigned char> rgb(4 * 4 * 3);
    for (size_t i = 0; i < rgb.size(); ++i) rgb[i] = static_cast<unsigned char>(i * 7);
    REQUIRE(studio::writePng(tempPng(), 4, 4, rgb.data()));
    const auto a = readFile(tempPng());
    REQUIRE(studio::writePng(tempPng(), 4, 4, rgb.data()));
    const auto b = readFile(tempPng());
    std::remove(tempPng().c_str());
    CHECK(a == b);
    CHECK(!a.empty());
}

TEST_CASE("png_writer: nonsense is refused, not written") {
    std::vector<unsigned char> one(3, 0);
    CHECK(!studio::writePng(tempPng(), 0, 4, one.data()));
    CHECK(!studio::writePng(tempPng(), 4, 0, one.data()));
    CHECK(!studio::writePng(tempPng(), 1, 1, nullptr));
    CHECK(readFile(tempPng()).empty());
}
