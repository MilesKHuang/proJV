// proJV unit tests -- OSC 7501 "Program Status Protocol" (sender side).
//
// The wire format is the contract with the host terminal, so these tests pin
// the exact bytes: prefix, field order, separators, base64, limits, dedupe and
// the TTY gate (a redirected stdout must receive 0 bytes).
#include "doctest/doctest.h"

#include "core/terminal_status.h"

#include <string>
#include <vector>

namespace {

using terminal_status::encode;

// Capture everything the module would have written to the console.
struct Sink {
    std::vector<std::string> writes;

    void attach() {
        terminal_status::test::setSink([this](const std::string& s) { writes.push_back(s); });
    }
};

bool startsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}
bool endsWith(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& s, const std::string& p) {
    return s.find(p) != std::string::npos;
}

// Every test starts from a clean module: no sink, no probe, no pending state.
struct Fresh {
    Fresh() { terminal_status::test::reset(); }
    ~Fresh() { terminal_status::test::reset(); }
};

} // namespace

TEST_CASE("osc7501: encode framing and field order") {
    const std::string seq = encode("working", "", "thinking", "projv", "main", -1);
    CHECK(startsWith(seq, "\x1b]7501;"));
    CHECK(endsWith(seq, "\x1b\\"));
    CHECK(contains(seq, "state=working"));
    CHECK(contains(seq, "app=projv"));
    CHECK(contains(seq, "id=main"));
    // state comes first after the OSC introducer.
    CHECK(seq.compare(std::string("\x1b]7501;").size(), 13, "state=working") == 0);
    // No stray spaces: the spec has none, and hosts are strict parsers.
    CHECK(seq.find(' ') == std::string::npos);
}

TEST_CASE("osc7501: msg is standard base64 with padding") {
    CHECK(contains(encode("working", "", "thinking", "projv", "main", -1), "msg=dGhpbmtpbmc="));
    CHECK(contains(encode("blocked", "permission", "permission", "projv", "main", -1),
                   "msg=cGVybWlzc2lvbg=="));
}

TEST_CASE("osc7501: control characters become spaces before encoding") {
    // "a\nb\tc" -> "a b c" -> YSBiIGM=
    const std::string seq = encode("working", "", "a\nb\tc", "projv", "main", -1);
    CHECK(contains(seq, "msg=YSBiIGM="));
    CHECK(seq.find('\n') == std::string::npos);
    CHECK(seq.find('\t') == std::string::npos);
}

TEST_CASE("osc7501: C1 controls (U+0080..U+009F) never reach the host") {
    // The spec bans U+0000..U+001F, U+007F and U+0080..U+009F after decoding.
    // A byte-wise filter misses the C1 range, which travels as 0xC2 0x80..0x9F;
    // a host that finds a control codepoint drops the whole record, so the two
    // bytes must collapse into a single space.
    std::string msg = "a";
    msg += "\xC2\x80"; // U+0080
    msg += "b";
    msg += "\xC2\x9F"; // U+009F
    msg += "c";

    const std::string seq = encode("working", "", msg, "projv", "main", -1);
    // "a b c" -- the same payload the C0 test expects.
    CHECK(contains(seq, "msg=YSBiIGM="));
    CHECK(seq.find('\xC2') == std::string::npos);
}

TEST_CASE("osc7501: non-control UTF-8 above the C1 range survives") {
    // U+00A0 (0xC2 0xA0) shares the 0xC2 lead byte but is legal payload: the
    // filter must key on the codepoint, not on the lead byte.
    const std::string seq = encode("working", "",
                                   "a\xC2\xA0"
                                   "b",
                                   "projv", "main", -1);
    CHECK(contains(seq, "msg=YcKgYg=="));
}

TEST_CASE("osc7501: state and kind validation") {
    // Unknown state: nothing is emitted (a host would drop it anyway).
    CHECK(encode("busy", "", "x", "projv", "main", -1).empty());

    // blocked always carries a kind; an invalid one degrades to permission
    // instead of losing the one record that needs a human.
    CHECK(contains(encode("blocked", "permission", "m", "projv", "main", -1), "kind=permission"));
    CHECK(contains(encode("blocked", "question", "m", "projv", "main", -1), "kind=question"));
    CHECK(contains(encode("blocked", "bogus", "m", "projv", "main", -1), "kind=permission"));
    CHECK(contains(encode("blocked", "", "m", "projv", "main", -1), "kind=permission"));

    // kind is only meaningful for blocked.
    CHECK(!contains(encode("working", "permission", "m", "projv", "main", -1), "kind="));
}

TEST_CASE("osc7501: progress only for working, and only in range") {
    CHECK(contains(encode("working", "", "m", "projv", "main", 42), "progress=42"));
    CHECK(contains(encode("working", "", "m", "projv", "main", 0), "progress=0"));
    CHECK(!contains(encode("working", "", "m", "projv", "main", -1), "progress="));
    CHECK(!contains(encode("working", "", "m", "projv", "main", 101), "progress="));
    CHECK(!contains(encode("done", "", "m", "projv", "main", 42), "progress="));
}

TEST_CASE("osc7501: clear carries state, id and app, nothing else") {
    const std::string seq = encode("clear", "", "", "projv", "main", -1);
    // app is mandatory on every record (rev 0.4), clear included.
    CHECK(endsWith(seq, "state=clear:id=main:app=projv\x1b\\"));
    CHECK(!contains(seq, "msg="));
    CHECK(!contains(seq, "progress="));
}

TEST_CASE("osc7501: an empty app still emits the mandatory field") {
    CHECK(contains(encode("working", "", "m", "", "main", -1), ":app=projv"));
    CHECK(contains(encode("clear", "", "", "", "main", -1), ":app=projv"));
}

TEST_CASE("osc7501: msg truncation stays inside the spec limits") {
    // 3000 ASCII bytes -> plaintext capped at 2048, base64 at 2732.
    const std::string huge(3000, 'x');
    const std::string seq = encode("working", "", huge, "projv", "main", -1);
    CHECK(seq.size() <= terminal_status::kMaxSequence);
    const std::string key = "msg=";
    const size_t at = seq.find(key);
    REQUIRE(at != std::string::npos);
    const size_t start = at + key.size();
    const size_t end = seq.find("\x1b\\", start);
    REQUIRE(end != std::string::npos);
    const std::string payload = seq.substr(start, end - start);
    CHECK(payload.size() == terminal_status::kMaxMsgEncoded);
    CHECK(payload.size() % 4 == 0);

    // A multi-byte character must not be cut in half.
    std::string mixed(2047, 'a');
    mixed += "\xE4\xB8\xAD"; // U+4E2D
    const std::string mixedSeq = encode("working", "", mixed, "projv", "main", -1);
    CHECK(mixedSeq.size() <= terminal_status::kMaxSequence);
    CHECK(contains(mixedSeq, "msg="));
}

TEST_CASE("osc7501: id and app are capped") {
    const std::string longId(300, 'i');
    const std::string seq = encode("done", "", "m", "projv", longId, -1);
    const size_t at = seq.find("id=");
    REQUIRE(at != std::string::npos);
    const size_t end = seq.find(':', at);
    const std::string idVal =
        seq.substr(at + 3, end == std::string::npos ? std::string::npos : end - at - 3);
    CHECK(idVal.size() <= terminal_status::kMaxId);
}

TEST_CASE("osc7501: not a TTY -> every call is a no-op") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return false; });

    CHECK(terminal_status::init(true) == false);
    CHECK(terminal_status::available() == false);

    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    terminal_status::blocked("permission", "rm -rf build");
    terminal_status::flushIfDirty();
    terminal_status::done("turn finished");
    terminal_status::flushIfDirty();
    terminal_status::shutdown();

    // The whole point: redirecting the TUI to a file must add zero bytes.
    CHECK(sink.writes.empty());
}

TEST_CASE("osc7501: disabled by config -> no-op") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });

    CHECK(terminal_status::init(false) == false);
    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    CHECK(sink.writes.empty());
}

TEST_CASE("osc7501: dedupe -- identical state twice writes once") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });
    REQUIRE(terminal_status::init(true));
    REQUIRE(terminal_status::available());

    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    CHECK(sink.writes.size() == 1);

    // working -> blocked -> working is three distinct reports.
    terminal_status::blocked("permission", "approve shell: rm -rf build");
    terminal_status::flushIfDirty();
    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    REQUIRE(sink.writes.size() == 3);
    CHECK(contains(sink.writes[0], "state=working"));
    CHECK(contains(sink.writes[1], "state=blocked"));
    CHECK(contains(sink.writes[1], "kind=permission"));
    CHECK(contains(sink.writes[2], "state=working"));
}

TEST_CASE("osc7501: flush without a change writes nothing") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });
    REQUIRE(terminal_status::init(true));

    terminal_status::flushIfDirty(); // nothing pending yet
    terminal_status::done("turn finished");
    terminal_status::flushIfDirty();
    terminal_status::flushIfDirty(); // already flushed, not dirty any more
    CHECK(sink.writes.size() == 1);
}

TEST_CASE("osc7501: shutdown emits clear exactly once") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });
    REQUIRE(terminal_status::init(true));

    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    terminal_status::shutdown();
    terminal_status::shutdown(); // idempotent: no second clear

    REQUIRE(sink.writes.size() == 2);
    CHECK(contains(sink.writes.back(), "state=clear"));
    CHECK(contains(sink.writes.back(), "id=main"));
    CHECK(terminal_status::available() == false);
}

TEST_CASE("osc7501: abandon leaves error as the last record, never clear") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });
    REQUIRE(terminal_status::init(true));

    terminal_status::working("thinking");
    terminal_status::flushIfDirty();
    terminal_status::abandon("interrupted");
    terminal_status::abandon("interrupted again"); // idempotent: one record only

    REQUIRE(sink.writes.size() == 2);
    CHECK(contains(sink.writes.back(), "state=error"));
    CHECK(contains(sink.writes.back(), "app=projv"));
    CHECK(!contains(sink.writes.back(), "state=clear"));
    CHECK(terminal_status::available() == false);
}

TEST_CASE("osc7501: abandon after shutdown writes nothing") {
    Fresh fresh;
    Sink sink;
    sink.attach();
    terminal_status::test::setTtyProbe([] { return true; });
    REQUIRE(terminal_status::init(true));

    terminal_status::shutdown();
    const size_t afterShutdown = sink.writes.size();
    terminal_status::abandon("late");
    CHECK(sink.writes.size() == afterShutdown);
}

TEST_CASE("osc7501: no raw newline can survive the encoder") {
    const std::string seq =
        encode("error", "", "HTTP 500 from api.deepseek.com\nstack line 2", "projv", "main", -1);
    CHECK(contains(seq, "msg="));
    CHECK(seq.find('\n') == std::string::npos);
}

TEST_CASE("osc7501: every produced sequence fits the 4096 byte cap") {
    const std::string worstMsg(4096, 'z');
    const std::string worstId(512, 'i');
    const std::string seq = encode("blocked", "permission", worstMsg, "projv", worstId, 100);
    CHECK(seq.size() <= terminal_status::kMaxSequence);
}
