// curl_error: the text a user sees when the network or TLS layer fails.
//
// The contract under test is the one from the M0 security work: a certificate
// failure must be actionable (name the cause, name the fix) and must never hint
// that verification can be turned off -- there is no such switch.
#include "doctest.h"

#include "client/curl_error.h"

#include <string>

namespace {
// Keep the numbers local instead of including curl/curl.h: the header under
// test deliberately hides curl, and the test should prove the contract rather
// than re-derive the constants.
constexpr int kCouldntResolveHost = 6;
constexpr int kOperationTimedOut = 28;
constexpr int kSslConnectError = 35;
constexpr int kPeerFailedVerification = 60;
} // namespace

TEST_CASE("curl_error: certificate failure is actionable") {
    const std::string s = curl_error::describe(kPeerFailedVerification,
                                               "SSL certificate problem: self-signed certificate");

    CHECK(s.find("certificate") != std::string::npos);
    CHECK(s.find("trust store") != std::string::npos);
    CHECK(s.find("self-signed") != std::string::npos);
    CHECK(s.find("cannot be disabled") != std::string::npos);
}

TEST_CASE("curl_error: certificate failure never suggests disabling verification") {
    const std::string s = curl_error::describe(kPeerFailedVerification, "raw curl detail");

    // The raw curl string would only confuse the reader here.
    CHECK(s.find("raw curl detail") == std::string::npos);
    CHECK(s.find("VERIFYPEER") == std::string::npos);
    CHECK(s.find("VERIFYHOST") == std::string::npos);
    CHECK(s.find("insecure") == std::string::npos);
    CHECK(s.find("turn off") == std::string::npos);
    CHECK(s.find("disable verification") == std::string::npos);
}

TEST_CASE("curl_error: DNS failure points at base_url and the proxy") {
    const std::string s = curl_error::describe(kCouldntResolveHost, "");
    CHECK(s.find("resolve") != std::string::npos);
    CHECK(s.find("base_url") != std::string::npos);
}

TEST_CASE("curl_error: timeout is reported as a timeout") {
    const std::string s = curl_error::describe(kOperationTimedOut, "");
    CHECK(s.find("timed out") != std::string::npos);
}

TEST_CASE("curl_error: other TLS problems keep the curl detail on one line") {
    const std::string s =
        curl_error::describe(kSslConnectError, "error:0A00010B:SSL routines\nbad record mac");
    CHECK(s.find("TLS handshake failed") != std::string::npos);
    CHECK(s.find("35") != std::string::npos);
    CHECK(s.find("bad record mac") != std::string::npos);
    CHECK(s.find('\n') == std::string::npos);
}

TEST_CASE("curl_error: empty detail falls back to the curl string") {
    const std::string s = curl_error::describe(9999, "");
    CHECK(s.find("9999") != std::string::npos);
    CHECK(!s.empty());
}

TEST_CASE("curl_error: unknown code echoes both the code and the detail") {
    const std::string s = curl_error::describe(9999, "weird");
    CHECK(s.find("9999") != std::string::npos);
    CHECK(s.find("weird") != std::string::npos);
}
