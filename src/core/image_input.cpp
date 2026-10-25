// proJV -- vision input: "@<path>" marker parsing (see image_input.h).
#include "image_input.h"

#include "debug_log.h"
#include "tools/base64.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace image_input {
namespace {

std::string trimCopy(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// C++20: std::filesystem::path::u8string() yields std::u8string.
std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

bool isAbsolutePath(const std::string& p) {
    if (p.empty()) return false;
    if (p[0] == '/' || p[0] == '\\') return true;
    if (p.size() > 1 && std::isalpha(static_cast<unsigned char>(p[0])) && p[1] == ':') return true;
    return false;
}

// Relative markers resolve against the workspace; absolute ones pass through.
// The result is lexically normalized so "./a.png" and "a.png" agree.
std::string resolvePath(const std::string& raw, const std::string& workspacePath) {
    std::string joined;
    if (isAbsolutePath(raw) || workspacePath.empty()) {
        joined = raw;
    } else {
        std::string ws = workspacePath;
        if (!ws.empty() && (ws.back() == '/' || ws.back() == '\\'))
            joined = ws + raw;
        else
            joined = ws + "/" + raw;
    }
    try {
        return toUtf8(fs::u8path(joined).lexically_normal());
    } catch (...) {
        return joined;
    }
}

// Collapse runs of blanks left behind by removed markers ("a  @x  b" -> "a b").
// Newlines are preserved: they carry structure in user messages.
std::string collapseBlanks(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool prevBlank = false;
    for (char c : s) {
        const bool blank = (c == ' ' || c == '\t');
        if (blank) {
            if (!prevBlank) out += ' ';
            prevBlank = true;
        } else {
            out += c;
            prevBlank = false;
        }
    }
    return out;
}

bool readHead(const std::string& path, std::string& head) {
    std::string bytes;
    if (!imgutil::readFileBytes(path, bytes)) return false;
    head = bytes.substr(0, 16);
    return true;
}

// Append one validated image. Enforces the per-file and total size budgets.
bool addImage(const std::string& fullPath, std::vector<ImageAttachment>& images, std::string& err) {
    if (images.size() >= kMaxImagesPerMessage) {
        err = "[image] too many images: at most " + std::to_string(kMaxImagesPerMessage) +
              " per message";
        return false;
    }

    std::error_code ec;
    const auto size = fs::file_size(fs::u8path(fullPath), ec);
    if (ec) {
        err = "[image] cannot stat: " + fullPath;
        return false;
    }
    if (size > kMaxImageBytes) {
        err = "[image] file exceeds 30 MiB: " + fullPath;
        return false;
    }

    std::string head;
    if (!readHead(fullPath, head)) {
        err = "[image] cannot read: " + fullPath;
        return false;
    }
    const std::string mime = imgutil::sniffImageMime(head);
    if (mime.empty()) {
        err = "[image] unsupported format (JPEG/PNG/GIF/WebP only): " + fullPath;
        return false;
    }

    ImageAttachment img;
    img.path = fullPath;
    img.detail = "auto";
    img.mime = mime;
    img.dataUrl = imgutil::makeImageDataUrl(fullPath, img.mime);
    if (img.dataUrl.empty()) {
        err = "[image] cannot encode: " + fullPath;
        return false;
    }

    size_t total = img.dataUrl.size();
    for (const auto& existing : images)
        total += existing.dataUrl.size();
    if (total > kMaxTotalEncodedBytes) {
        err = "[image] total image payload exceeds 44 MiB; remove some images";
        return false;
    }

    images.push_back(std::move(img));
    return true;
}

// Expand one marker (file or directory) into `images`.
bool expandMarker(const std::string& rawPath, const std::string& workspacePath,
                  std::vector<ImageAttachment>& images, std::string& err) {
    const std::string full = resolvePath(rawPath, workspacePath);
    std::error_code ec;
    if (!fs::exists(fs::u8path(full), ec)) {
        err = "[image] not found: " + rawPath;
        return false;
    }

    if (fs::is_directory(fs::u8path(full), ec)) {
        // Top level only, never recursive; unknown files are skipped silently.
        std::vector<fs::path> files;
        for (auto it = fs::directory_iterator(fs::u8path(full), ec);
             !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            files.push_back(it->path());
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) {
            return toUtf8(a.filename()) < toUtf8(b.filename());
        });

        const size_t before = images.size();
        for (const auto& f : files) {
            const std::string p = toUtf8(f);
            std::string head;
            if (!readHead(p, head)) continue;
            if (imgutil::sniffImageMime(head).empty()) continue;
            if (!addImage(p, images, err)) return false;
        }
        if (images.size() == before) {
            err = "[image] no supported images in directory: " + rawPath;
            return false;
        }
        return true;
    }

    if (!fs::is_regular_file(fs::u8path(full), ec)) {
        err = "[image] not a file or directory: " + rawPath;
        return false;
    }
    return addImage(full, images, err);
}

} // namespace

ParseResult parseMarkers(const std::string& text, const std::string& workspacePath) {
    ParseResult out;
    std::string cleaned;
    size_t i = 0;
    size_t copiedUpTo = 0;

    while (i < text.size()) {
        if (text[i] != '@') {
            ++i;
            continue;
        }

        size_t pathStart = 0, pathEnd = 0, markerEnd = 0;
        bool found = false;

        if (i + 1 < text.size() && text[i + 1] == '"') {
            // Quoted form: @"<path>" -- required for paths with spaces.
            const size_t close = text.find('"', i + 2);
            if (close != std::string::npos && close > i + 2) {
                pathStart = i + 2;
                pathEnd = close;
                markerEnd = close + 1;
                found = true;
            }
        } else {
            // Bare form: only the three documented path shapes trigger.
            const size_t p = i + 1;
            if (p >= text.size()) break;
            const unsigned char c0 = static_cast<unsigned char>(text[p]);
            const bool drive = std::isalpha(c0) && p + 1 < text.size() && text[p + 1] == ':';
            const bool root = (c0 == '/');
            const bool dotSlash = (c0 == '.' && p + 1 < text.size() && text[p + 1] == '/');
            const bool dotDot = (c0 == '.' && p + 2 < text.size() && text[p + 1] == '.' &&
                                 (text[p + 2] == '/' || text[p + 2] == '\\'));
            if (drive || root || dotSlash || dotDot) {
                size_t e = p;
                while (e < text.size() && !std::isspace(static_cast<unsigned char>(text[e])))
                    ++e;
                if (e > p) {
                    pathStart = p;
                    pathEnd = e;
                    markerEnd = e;
                    found = true;
                }
            }
        }

        if (!found) {
            ++i;
            continue;
        }

        out.hadMarkers = true;
        const std::string rawPath = text.substr(pathStart, pathEnd - pathStart);

        if (!expandMarker(rawPath, workspacePath, out.images, out.error)) {
            debugLogf("[Image] marker rejected: %s", out.error.c_str());
            return out; // stop at the first problem, send nothing
        }

        // Strip the marker itself from the text.
        cleaned.append(text, copiedUpTo, i - copiedUpTo);
        copiedUpTo = markerEnd;
        i = markerEnd;
    }

    if (copiedUpTo == 0) {
        out.text = trimCopy(text);
    } else {
        cleaned.append(text, copiedUpTo, std::string::npos);
        out.text = trimCopy(collapseBlanks(cleaned));
    }
    return out;
}

} // namespace image_input
