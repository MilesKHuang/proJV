// proJV -- vision input: "@<path>" marker parsing and image expansion.
//
// Syntax (see IMAGE_INPUT.md):
//   @C:\a.png            single file (drive form)
//   @/abs/path.png       absolute path
//   @./rel or @..\rel    relative path (resolved against the workspace)
//   @"C:\my folder\a.png" quoted form, required when the path has spaces
//   @C:\dir              a directory expands to its top-level images (sorted)
//
// An "@" that does not match one of the path shapes above is ordinary text, so
// emails (a@b.com) and code (@param) are never touched.
#pragma once

#include "models.h"

#include <cstddef>
#include <string>
#include <vector>

namespace image_input {

// Hard limits from the DeepSeek vision docs, with a conservative product cap.
inline constexpr size_t kMaxImagesPerMessage = 8;                    // global per message
inline constexpr size_t kMaxImageBytes = 30ull * 1024 * 1024;        // 30 MiB per file
inline constexpr size_t kMaxTotalEncodedBytes = 44ull * 1024 * 1024; // 44 MiB encoded
inline constexpr int kImageTokens = 1024;                            // per image, upper bound

struct ParseResult {
    std::string text;                    // markers stripped, then trimmed
    std::vector<ImageAttachment> images; // resolved + encoded (dataUrl filled)
    std::string error;                   // non-empty => refuse to send
    bool hadMarkers = false;             // at least one marker was recognized
    bool ok() const { return error.empty(); }
};

// Parse every image marker out of a user message.
// `workspacePath` resolves relative markers (@./x, @..\x); pass "" to resolve
// against the current directory.
ParseResult parseMarkers(const std::string& text, const std::string& workspacePath);

// Tokens to charge for a message's attachments (context-pressure accounting).
inline size_t imageTokens(size_t count) {
    return count * kImageTokens;
}

// Resolve + validate + encode an explicit list of paths (files, or directories
// that expand to their top-level images). This is the entry point for callers
// that already have paths -- the view_image tool -- so it goes through exactly
// the same count / per-file / total / mime checks as the "@path" markers and the
// two can never disagree.
// Relative paths resolve against `workspacePath`. On failure `err` carries the
// reason and `out` is left empty: nothing is sent.
bool loadAttachments(const std::vector<std::string>& paths, const std::string& workspacePath,
                     std::vector<ImageAttachment>& out, std::string& err);

} // namespace image_input
