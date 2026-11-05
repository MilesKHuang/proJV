// proJV -- OSC 7501 status reporter (sender side). See terminal_status.h.
#include "core/terminal_status.h"

#include "debug_log.h"
#include "tools/base64.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace terminal_status {
namespace {

// The one record proJV keeps: the newest state, plus the last sequence we
// actually put on the wire (that is the dedupe key).
struct Pending {
    std::string state;
    std::string kind;
    std::string msg;
    int progress = -1;
    bool dirty = false;
};

struct Globals {
    std::mutex mutex;
    bool enabled = false;
    bool available = false;
    Pending pending;
    std::string lastSent;
    std::function<bool()> ttyProbe;                // test seam
    std::function<void(const std::string&)> sink;  // test seam
};

Globals& g() {
    static Globals inst;
    return inst;
}

bool realTtyProbe() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

bool probeTty() {
    auto& s = g();
    if (s.ttyProbe) return s.ttyProbe();
    return realTtyProbe();
}

// Raw byte write. Deliberately not std::cout: OSC is a control sequence, it
// must reach the terminal unmodified and in one piece.
void writeRaw(const std::string& bytes) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return;
    DWORD written = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
#else
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = ::write(STDOUT_FILENO, bytes.data() + off, bytes.size() - off);
        if (n <= 0) return;
        off += static_cast<size_t>(n);
    }
#endif
}

void emit(const std::string& bytes) {
    auto& s = g();
    if (s.sink) {
        s.sink(bytes);
        return;
    }
    writeRaw(bytes);
}

bool isKnownState(const std::string& s) {
    return s == "idle" || s == "working" || s == "blocked" || s == "done" || s == "error" ||
           s == "clear";
}

bool isKnownKind(const std::string& s) {
    return s == "permission" || s == "question" || s == "auth";
}

// Control characters must not travel even base64-encoded (the decoder on the
// host side would put a newline in a one-line label). Replace, never strip:
// stripping would glue words together.
//
// The spec forbids U+0000..U+001F, U+007F and U+0080..U+009F after decoding, so
// this works on codepoints, not bytes: the C1 range arrives as the two-byte
// sequence 0xC2 0x80..0x9F, and a byte-wise check would let it through. Both
// bytes then collapse into ONE space -- one control character, one space.
std::string sanitizeControl(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        if (c < 0x20 || c == 0x7f) {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (c == 0xC2 && i + 1 < in.size()) {
            const unsigned char c2 = static_cast<unsigned char>(in[i + 1]);
            if (c2 >= 0x80 && c2 <= 0x9F) { // U+0080..U+009F
                out.push_back(' ');
                i += 2;
                continue;
            }
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

// Truncate on a UTF-8 boundary so the host never decodes a broken codepoint.
std::string truncateUtf8(const std::string& in, size_t maxBytes) {
    if (in.size() <= maxBytes) return in;
    size_t cut = maxBytes;
    // Walk back while the byte at `cut` is a continuation byte (10xxxxxx).
    while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80) --cut;
    return in.substr(0, cut);
}

std::string wrap(const std::string& body) {
    std::string seq = std::string(kStart) + body + kEnd;
    if (seq.size() > kMaxSequence) {
        debugLogf("[OSC7501] sequence of %zu bytes exceeds %zu -- dropped", seq.size(),
                  kMaxSequence);
        return std::string();
    }
    return seq;
}

// Caller holds g().mutex.
void setState(const std::string& state, const std::string& kind, const std::string& msg,
              int progress) {
    auto& s = g();
    if (!s.enabled || !s.available) return; // dropped before it can cost anything
    s.pending.state = state;
    s.pending.kind = kind;
    s.pending.msg = msg;
    s.pending.progress = progress;
    s.pending.dirty = true;
}

} // namespace

std::string encode(const std::string& state, const std::string& kind, const std::string& msg,
                   const std::string& app, const std::string& id, int progress) {
    if (!isKnownState(state)) {
        debugLogf("[OSC7501] unknown state '%s' -- not sent", state.c_str());
        return std::string();
    }

    std::string body = "state=" + state;

    if (state == "blocked") {
        std::string k = kind;
        if (!isKnownKind(k)) {
            // The host uses kind to decide whether a human is needed, so a
            // blocked record without a valid kind is useless. Default to the
            // one proJV actually produces today instead of dropping the record.
            if (!k.empty()) debugLogf("[OSC7501] unknown kind '%s' -- using permission", k.c_str());
            k = "permission";
        }
        body += ":kind=" + k;
    }

    if (!id.empty()) body += ":id=" + truncateUtf8(id, kMaxId);

    if (state == "clear") return wrap(body); // clear carries nothing else

    if (!app.empty()) body += ":app=" + truncateUtf8(app, kMaxApp);

    const std::string plain = truncateUtf8(sanitizeControl(msg), kMaxMsgPlain);
    if (!plain.empty()) {
        const std::string encoded = imgutil::base64Encode(plain);
        if (encoded.size() > kMaxMsgEncoded) {
            debugLogf("[OSC7501] encoded msg of %zu bytes exceeds %zu -- msg omitted",
                      encoded.size(), kMaxMsgEncoded);
        } else {
            body += ":msg=" + encoded;
        }
    }

    if (state == "working" && progress >= 0 && progress <= 100)
        body += ":progress=" + std::to_string(progress);

    return wrap(body);
}

bool init(bool enabled) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.pending = Pending{};
    s.lastSent.clear();

    bool on = enabled;
    if (on) {
        const char* noOsc = std::getenv("PROJV_NO_OSC");
        if (noOsc && *noOsc && std::strcmp(noOsc, "0") != 0) {
            debugLog("[OSC7501] PROJV_NO_OSC is set -- status reporting disabled");
            on = false;
        }
    }

    s.enabled = on;
    if (!on) {
        s.available = false;
        return false;
    }

    s.available = probeTty();
    if (!s.available)
        debugLog("[OSC7501] stdout is not a TTY -- status reporting disabled (clean redirect)");
    return s.available;
}

bool available() {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    return s.enabled && s.available;
}

void shutdown() {
    auto& s = g();
    std::string seq;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.enabled || !s.available) {
            s.enabled = false;
            s.available = false;
            s.pending = Pending{};
            return;
        }
        seq = encode("clear", "", "", "projv", "main", -1);
        s.enabled = false;
        s.available = false;
        s.pending = Pending{};
        s.lastSent.clear();
    }
    if (!seq.empty()) emit(seq);
}

void working(const std::string& msg, int progress) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("working", "", msg, progress);
}

void blocked(const std::string& kind, const std::string& msg) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("blocked", kind, msg, -1);
}

void idle(const std::string& msg) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("idle", "", msg, -1);
}

void done(const std::string& msg) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("done", "", msg, -1);
}

void error(const std::string& msg) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("error", "", msg, -1);
}

void clear() {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    setState("clear", "", "", -1);
}

void flushIfDirty() {
    auto& s = g();
    std::string seq;
    {
        std::lock_guard<std::mutex> lk(s.mutex);
        if (!s.enabled || !s.available || !s.pending.dirty) return;
        s.pending.dirty = false;
        seq = encode(s.pending.state, s.pending.kind, s.pending.msg, "projv", "main",
                     s.pending.progress);
        if (seq.empty()) return;
        if (seq == s.lastSent) return; // same state twice: nothing to say
        s.lastSent = seq;
    }
    emit(seq);
}

namespace test {

void setTtyProbe(std::function<bool()> probe) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.ttyProbe = std::move(probe);
}

void setSink(std::function<void(const std::string&)> sink) {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.sink = std::move(sink);
}

void reset() {
    auto& s = g();
    std::lock_guard<std::mutex> lk(s.mutex);
    s.enabled = false;
    s.available = false;
    s.pending = Pending{};
    s.lastSent.clear();
    s.ttyProbe = nullptr;
    s.sink = nullptr;
}

} // namespace test

} // namespace terminal_status
