#include "delete_guard.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace delete_guard {
namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string unquote(const std::string& s) {
    if (s.size() >= 2) {
        const char f = s.front();
        if ((f == '"' || f == '\'') && s.back() == f) return s.substr(1, s.size() - 2);
    }
    return s;
}

// `/bin/rm` -> `rm`, `C:\Windows\System32\format.com` -> `format.com`.
std::string baseName(const std::string& s) {
    const size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? s : s.substr(p + 1);
}

std::string normalizeToken(const std::string& raw) {
    std::string t = lowerCopy(baseName(unquote(trim(raw))));
    // `format.com`, `rm.exe`: the shell resolves PATHEXT.
    for (const char* ext : {".exe", ".com", ".bat", ".cmd"}) {
        const size_t n = std::string(ext).size();
        if (t.size() > n && t.compare(t.size() - n, n, ext) == 0) {
            t.resize(t.size() - n);
            break;
        }
    }
    return t;
}

// Split a segment into words, keeping quoted groups together. Quotes are
// stripped, so `cp "a b" c` yields [cp, a b, c].
std::vector<std::string> words(const std::string& segment) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < segment.size(); ++i) {
        const char c = segment[i];
        if (quote) {
            if (c == quote) {
                quote = 0;
                out.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

const std::set<std::string>& deleteCommands() {
    static const std::set<std::string> k = {"rm", "del",    "erase",       "rmdir",
                                            "rd", "unlink", "remove-item", "ri"};
    return k;
}

const std::set<std::string>& overwriteCommands() {
    static const std::set<std::string> k = {"mv",    "move",     "cp",  "copy",
                                            "xcopy", "robocopy", "tee", "truncate"};
    return k;
}

bool isMkfs(const std::string& token) {
    return token.rfind("mkfs", 0) == 0;
}

bool startsWith(const std::string& s, const char* p) {
    const std::string pre(p);
    return s.size() >= pre.size() && s.compare(0, pre.size(), pre) == 0;
}

// `dd if=x of=/dev/sda` and friends: the argument list, not the command name,
// is what makes them fatal.
bool isDeviceWipe(const std::string& token, const std::string& loweredSegment) {
    if (token == "dd") return loweredSegment.find("of=/dev/") != std::string::npos;
    if (token == "diskutil")
        return loweredSegment.find("erasedisk") != std::string::npos ||
               loweredSegment.find("partitiondisk") != std::string::npos;
    return false;
}

// Segment-level: is there a redirect that truncates or appends to a real file?
// `2>&1` and `>/dev/null` / `>nul` are not destructive.
bool hasFileRedirect(const std::string& segment) {
    char quote = 0;
    for (size_t i = 0; i < segment.size(); ++i) {
        const char c = segment[i];
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c != '>') continue;
        size_t j = i + 1;
        if (j < segment.size() && segment[j] == '>') ++j; // >>
        if (j < segment.size() && segment[j] == '&') continue;
        while (j < segment.size() && (segment[j] == ' ' || segment[j] == '\t'))
            ++j;
        std::string target;
        while (j < segment.size() && segment[j] != ' ' && segment[j] != '\t' && segment[j] != ';' &&
               segment[j] != '|' && segment[j] != '&') {
            target += segment[j];
            ++j;
        }
        const std::string t = lowerCopy(unquote(target));
        if (t.empty() || t == "/dev/null" || t == "nul" || startsWith(t, "/dev/null")) continue;
        return true;
    }
    return false;
}

// Peeling wrappers is where bypasses hide: `cmd /c del x` and `sudo rm -rf /`
// must reach `del` / `rm`. Too many layers means we stop guessing and ask.
constexpr int kMaxDepth = 6;

std::string peel(const std::string& segment, bool& tooDeep) {
    std::vector<std::string> w = words(segment);
    size_t i = 0;
    tooDeep = false;

    // Leading VAR=value assignments (bash) carry no command.
    auto isAssignment = [](const std::string& t) {
        const size_t eq = t.find('=');
        if (eq == std::string::npos || eq == 0) return false;
        for (size_t k = 0; k < eq; ++k)
            if (!(std::isalnum((unsigned char)t[k]) || t[k] == '_')) return false;
        return true;
    };

    for (int depth = 0; depth < kMaxDepth; ++depth) {
        while (i < w.size() && isAssignment(w[i]))
            ++i;
        if (i >= w.size()) return std::string();

        const std::string tok = normalizeToken(w[i]);
        ++i;

        auto skipFlags = [&](const char* prefix) {
            while (i < w.size() && startsWith(w[i], prefix))
                ++i;
        };

        // A wrapper can swallow a whole quoted command line: `sh -c "rm -rf x"`
        // arrives as one token. Re-tokenize what is left so the next round sees
        // `rm` as the command.
        auto resplit = [&]() {
            std::string rest;
            for (size_t k = i; k < w.size(); ++k) {
                if (!rest.empty()) rest += ' ';
                rest += w[k];
            }
            w = words(rest);
            i = 0;
        };

        if (tok == "cmd") {
            skipFlags("/");
            resplit();
            continue;
        }
        if (tok == "powershell" || tok == "pwsh" || tok == "sh" || tok == "bash" || tok == "zsh" ||
            tok == "dash" || tok == "ksh" || tok == "fish") {
            skipFlags("-");
            resplit();
            continue;
        }
        if (tok == "sudo" || tok == "doas") {
            skipFlags("-");
            resplit();
            continue;
        }
        if (tok == "env" || tok == "nohup" || tok == "command" || tok == "exec") {
            skipFlags("-");
            resplit();
            continue;
        }
        // A wrapper that swallowed the command (e.g. `sudo cmd /c ...`) lands
        // here; keep the token and let the caller classify it.
        return tok;
    }
    tooDeep = true;
    return std::string();
}

} // namespace

std::vector<std::string> splitSegments(const std::string& command) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur += c;
            continue;
        }
        // Only shell metacharacters are escaped; a bare backslash is a Windows
        // path separator and must survive (`rm C:\tmp\x`).
        if (c == '\\' && i + 1 < command.size()) {
            const char n = command[i + 1];
            if (n == ';' || n == '|' || n == '&' || n == '\\') {
                cur += n;
                ++i;
                continue;
            }
            cur += c;
            continue;
        }
        if (c == ';' || c == '\n') {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        if (c == '&' && i + 1 < command.size() && command[i + 1] == '&') {
            out.push_back(cur);
            cur.clear();
            ++i;
            continue;
        }
        if (c == '|') {
            out.push_back(cur);
            cur.clear();
            if (i + 1 < command.size() && command[i + 1] == '|') ++i;
            continue;
        }
        cur += c;
    }
    out.push_back(cur);

    std::vector<std::string> cleaned;
    for (const std::string& s : out) {
        const std::string t = trim(s);
        if (!t.empty()) cleaned.push_back(t);
    }
    return cleaned;
}

std::string firstCommandToken(const std::string& segment) {
    bool tooDeep = false;
    return peel(segment, tooDeep);
}

Verdict evaluateSegment(const std::string& segment) {
    Verdict v;
    const std::string lowered = lowerCopy(segment);
    bool tooDeep = false;
    const std::string cmd = peel(segment, tooDeep);
    if (tooDeep) {
        v.decision = Decision::Ask;
        return v;
    }
    if (cmd.empty()) return v;

    // 1. Red lines: disk formatting and partitioning. Not askable -- an
    //    overwritten partition table is not something a backup restores.
    static const std::set<std::string> kRedLine = {"format",     "diskpart",        "format-volume",
                                                   "clear-disk", "initialize-disk", "fdisk",
                                                   "parted",     "wipefs",          "sgdisk"};
    if (kRedLine.count(cmd) || isMkfs(cmd) || isDeviceWipe(cmd, lowered)) {
        v.decision = Decision::Deny;
        v.reason =
            "`" + cmd + "` is blocked by policy: disk formatting and partitioning cannot be undone";
        return v;
    }

    // 2. Deleting something that already exists.
    if (deleteCommands().count(cmd)) {
        v.decision = Decision::Ask;
        return v;
    }
    if (cmd == "find" && (lowered.find("-delete") != std::string::npos ||
                          lowered.find("-exec rm") != std::string::npos)) {
        v.decision = Decision::Ask;
        return v;
    }
    if (cmd == "git") {
        if (lowered.find("clean") != std::string::npos ||
            lowered.find("reset --hard") != std::string::npos ||
            lowered.find("checkout --") != std::string::npos ||
            lowered.find("stash drop") != std::string::npos ||
            lowered.find("stash clear") != std::string::npos ||
            lowered.find("restore") != std::string::npos) {
            v.decision = Decision::Ask;
            return v;
        }
        // commit rewrites history, push publishes it. Neither deletes a file, so
        // neither was gated -- but both are irreversible in practice, and the user
        // must see the exact command the model is about to run.
        if (lowered.find("commit") != std::string::npos ||
            lowered.find("push") != std::string::npos) {
            v.decision = Decision::Ask;
            return v;
        }
    }

    // Inline interpreter code (`python -c`, `node -e`) used to ask unconditionally:
    // "there is no way to read it reliably". In practice that punished read-only
    // one-liners -- inspecting a SQLite file, parsing JSON -- as hard as a delete,
    // and the user paid a dialog per call. Same rule as the shell now: scan the
    // payload for the primitives that can destroy data and ask only for those.
    // Irreducible limit: obfuscation (getattr(__import__('os'),'remove')) gets
    // through. The command is still visible in the transcript, and `rm`-equivalent
    // shell text keeps asking.
    static const std::set<std::string> kInterpreters = {"python", "python3", "py", "node",
                                                        "perl",   "ruby",    "php"};
    if (kInterpreters.count(cmd)) {
        const std::vector<std::string> w = words(segment);
        bool destructive = false;
        for (size_t i = 0; i < w.size() && !destructive; ++i) {
            if (w[i] != "-c" && w[i] != "-e") continue;
            // The payload is everything after the flag; join it back so quoting
            // does not hide a keyword from the scan.
            std::string payload;
            for (size_t j = i + 1; j < w.size(); ++j)
                payload += " " + w[j];
            const std::string p = lowerCopy(payload);
            // API-qualified names only: bare "remove" also matches list.remove(),
            // and bare "format" matches "{}".format(), which would put an approval
            // dialog in front of every harmless one-liner again.
            static const char* kDestructive[] = {
                "os.remove",  "rmtree",     "unlink",      "shutil",       "os.rename",
                "os.replace", "truncate",   "shred",       "os.system",    "subprocess",
                "popen",      "os.kill",    "exec(",       "eval(",        "chmod",
                "chown",      "drop table", "delete from", "drop database"};
            for (const char* pat : kDestructive) {
                if (p.find(pat) != std::string::npos) {
                    destructive = true;
                    break;
                }
            }
            // A write-mode open() has no fixed token: open(f,'w') / open(f,"a").
            if (!destructive &&
                (p.find("'w'") != std::string::npos || p.find("\"w\"") != std::string::npos ||
                 p.find("'a'") != std::string::npos || p.find("\"a\"") != std::string::npos))
                destructive = true;
            break; // one payload is enough; flags after it are not scanned twice
        }
        if (destructive) {
            v.decision = Decision::Ask;
            return v;
        }
        // Read-only inline code falls through to the redirect check below: a
        // `python -c "print(1)" > log.txt` is still an overwrite.
    }

    // 3. Overwriting an existing file. mv/cp only matter when the target is
    //    already there, and that is a stat the caller has to do.
    if (overwriteCommands().count(cmd)) {
        v.decision = Decision::Ask;
        const std::vector<std::string> w = words(segment);
        if (w.size() >= 3) {
            v.needsExistenceCheck = true;
            v.checkTarget = w.back();
        }
        return v;
    }

    // 4. Redirects: `echo x > f` truncates f just as well as rm does. Functional,
    //    not destructive: this is how builds and tests are logged, and gating it
    //    made a normal session a dozen dialogs. The workspace snapshot still runs
    //    per turn, so /undo covers a redirect that ate a source file.
    if (hasFileRedirect(segment)) {
        v.decision = Decision::Ask;
        v.kind = Kind::Functional;
        return v;
    }

    return v;
}

Verdict evaluate(const std::string& command) {
    // Any segment can make the whole line dangerous; Deny outranks Ask.
    Verdict worst;
    for (const std::string& seg : splitSegments(command)) {
        const Verdict v = evaluateSegment(seg);
        if (v.decision == Decision::Deny) return v;
        if (v.decision == Decision::Ask && worst.decision == Decision::Allow) worst = v;
    }
    return worst;
}

} // namespace delete_guard
