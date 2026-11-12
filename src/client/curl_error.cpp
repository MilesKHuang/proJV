#include "curl_error.h"

#include <curl/curl.h>

#include <string>

namespace curl_error {
namespace {

// 60 is the one code that means "this certificate is not trusted". Keeping it
// separate from the other TLS codes matters: only this one gets the long
// "install the root CA" advice, and only this one drops the raw curl string.
bool isCertificateVerification(int code) {
    return code == CURLE_PEER_FAILED_VERIFICATION;
}

// Handshake / local-key / engine problems: TLS, but not "the server identity
// is unverified". The raw string is useful here (protocol mismatch, expired
// client cert, engine missing).
bool isOtherTlsProblem(int code) {
    switch (code) {
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_SSL_ENGINE_NOTFOUND:
    case CURLE_SSL_ENGINE_SETFAILED:
    case CURLE_SSL_ENGINE_INITFAILED:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
    case CURLE_SSL_CLIENTCERT:
        return true;
    default:
        return false;
    }
}

std::string certificateText() {
    return "TLS certificate verification failed: the identity of the server could not be "
           "verified.\n"
           "  Common causes: a proxy or gateway that re-signs traffic without its root CA "
           "installed, an expired or self-signed certificate, a wrong system clock, or "
           "base_url pointing at a self-signed service.\n"
           "  Fix: install that root certificate into the OS trust store (Windows: Trusted "
           "Root Certification Authorities; Linux: /usr/local/share/ca-certificates + "
           "update-ca-certificates), or point base_url at an endpoint with a publicly "
           "trusted certificate.\n"
           "  Certificate verification cannot be disabled.";
}

std::string shortDetail(const std::string& detail, int code) {
    std::string d = detail;
    if (d.empty()) d = curl_easy_strerror((CURLcode)code);
    if (d.size() > 200) {
        d.resize(200);
        d += "...";
    }
    // One line: these strings end up in a status bar / tool result.
    for (char& c : d)
        if (c == '\n' || c == '\r') c = ' ';
    return d;
}

} // namespace

std::string describe(int code, const std::string& detail) {
    if (isCertificateVerification(code)) return certificateText();

    if (isOtherTlsProblem(code)) {
        return "TLS handshake failed (curl " + std::to_string(code) +
               "): " + shortDetail(detail, code);
    }

    switch (code) {
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "Cannot resolve the proxy host configured for this shell. Check HTTP(S)_PROXY "
               "or the system proxy settings.";
    case CURLE_COULDNT_RESOLVE_HOST:
        return "Cannot resolve the API host name. Check base_url and the DNS/proxy settings.";
    case CURLE_COULDNT_CONNECT:
        return "Cannot connect to the API host. Is it reachable from this machine, and does "
               "the proxy allow it?";
    case CURLE_OPERATION_TIMEDOUT:
        return "The request timed out. The connection or the server is too slow; check the "
               "proxy and try again.";
    case CURLE_TOO_MANY_REDIRECTS:
        return "Too many redirects. base_url probably points at a proxy that keeps "
               "redirecting.";
    case CURLE_GOT_NOTHING:
        return "The server closed the connection without replying. Usually a proxy or "
               "gateway dropping the request.";
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
        return "The connection dropped while transferring data. Usually a proxy, a firewall "
               "or an unstable network.";
    default:
        return "Network request failed (curl " + std::to_string(code) +
               "): " + shortDetail(detail, code);
    }
}

} // namespace curl_error
