#pragma once

#include <string>

namespace curl_error {

// Turn a CURLcode into text a user can act on.
//
// `code` is an int on purpose: this header must not include curl/curl.h.
// deepseek.h already hides every curl type behind void* (see
// createEasyHandle), and pulling the vendored curl version into a public
// header would leak it into every translation unit that includes the client.
//
// `detail` is curl's own error string (curl_easy_strerror). It is echoed for
// non-certificate failures only; for a certificate failure it is dropped from
// the user-facing text because it says nothing actionable, and the caller logs
// it instead.
//
// Contract for CURLE_PEER_FAILED_VERIFICATION (60): the text must name the
// failure, list the common causes and the fix (install the root CA in the OS
// trust store), and must NEVER suggest turning verification off -- there is no
// such switch in this program and suggesting one would be a security hole.
std::string describe(int code, const std::string& detail);

} // namespace curl_error
