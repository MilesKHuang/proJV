// proJV -- image helpers: standard base64 + magic-number sniffing.
//
// Header-only (inline) so both core/ and client/ can use it without adding a
// translation unit or a third-party dependency. No OpenSSL, no libbase64.
#pragma once

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

namespace imgutil {

// RFC 4648 standard alphabet, with padding. Used for data: URLs.
inline std::string base64Encode(const unsigned char* data, size_t len) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16)
                         | (static_cast<uint32_t>(data[i + 1]) << 8)
                         |  static_cast<uint32_t>(data[i + 2]);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += tbl[v & 0x3F];
    }
    if (i + 1 == len) {                      // one leftover byte
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += "==";
    } else if (i + 2 == len) {               // two leftover bytes
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16)
                         | (static_cast<uint32_t>(data[i + 1]) << 8);
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6) & 0x3F];
        out += '=';
    }
    return out;
}

inline std::string base64Encode(const std::string& bytes) {
    return base64Encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
}

// Detect the container format from the leading bytes.
// Returns "image/jpeg", "image/png", "image/gif", "image/webp", or "" if the
// data is not one of the four supported formats (JPEG / PNG / GIF / WebP).
inline std::string sniffImageMime(const std::string& head) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(head.data());
    const size_t n = head.size();

    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G' &&
        p[4] == 0x0D && p[5] == 0x0A && p[6] == 0x1A && p[7] == 0x0A) return "image/png";
    if (n >= 6 && head.compare(0, 6, "GIF87a") == 0) return "image/gif";
    if (n >= 6 && head.compare(0, 6, "GIF89a") == 0) return "image/gif";
    if (n >= 12 && head.compare(0, 4, "RIFF") == 0 && head.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    return "";
}

// Read an entire file as bytes. Returns false if it cannot be opened/read.
inline bool readFileBytes(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    return true;
}

// Build a "data:<mime>;base64,<...>" URL from a file on disk.
// Returns "" (and leaves mimeOut untouched) when the file is unreadable or is
// not one of the supported image formats.
inline std::string makeImageDataUrl(const std::string& path, std::string& mimeOut) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return "";
    std::string bytes;
    bytes.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
    if (bytes.empty()) return "";

    std::string mime = sniffImageMime(bytes.substr(0, 16));
    if (mime.empty()) return "";
    mimeOut = mime;
    return "data:" + mime + ";base64," + base64Encode(bytes);
}

} // namespace imgutil
