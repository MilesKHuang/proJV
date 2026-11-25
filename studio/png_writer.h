// proJV studio -- a minimal PNG writer (design 08.6: --screenshot).
//
// No dependency and no vendored encoder: a screenshot needs one still frame, not
// an image library. Stored (uncompressed) deflate blocks are legal zlib, so this
// is a CRC32, an Adler32 and a chunk loop -- and it is testable without a GPU or
// a window (tests/unit/test_png_writer.cpp checks the bytes a decoder reads).
#pragma once

#include <string>

namespace studio {

// `rgb` is width*height*3 bytes, 8 bits per channel, FIRST ROW AT THE TOP (a
// caller reading back from OpenGL has to flip). Returns false when the file
// cannot be written or the arguments make no sense.
bool writePng(const std::string& path, int width, int height, const unsigned char* rgb);

} // namespace studio
