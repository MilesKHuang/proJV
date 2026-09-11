// proJV TUI -- Markdown text model implementation (1:1 port of legacy rules).
#include "markdown_text.h"

#include <cctype>
#include <string>
#include <vector>

namespace markdown_text {

namespace {

// skipSpace: advance past spaces/tabs.
size_t skipSpace(const std::string& s, size_t pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
    return pos;
}

bool startsWith(const std::string& s, size_t pos, const char* prefix) {
    while (*prefix) {
        if (pos >= s.size() || s[pos] != *prefix) return false;
        ++pos;
        ++prefix;
    }
    return true;
}

// --- Inline parsing (1:1 with parseInlineSegs) ---------------------------
std::vector<Segment> parseInline(const std::string& line) {
    std::vector<Segment> out;
    size_t p = 0;
    size_t segStart = 0;

    auto pushNormal = [&](size_t end) {
        if (end > segStart) {
            Segment s;
            s.text = line.substr(segStart, end - segStart);
            s.style = Style::Normal;
            out.push_back(std::move(s));
        }
        segStart = end;
    };

    while (p < line.size()) {
        // `code`
        if (line[p] == '`') {
            size_t e = p + 1;
            while (e < line.size() && line[e] != '`') ++e;
            if (e < line.size() && e > p + 1) {
                pushNormal(p);
                Segment s;
                s.text = line.substr(p + 1, e - p - 1);
                s.style = Style::Code;
                out.push_back(std::move(s));
                segStart = p = e + 1;
                continue;
            }
        }
        // **bold**
        if (p + 1 < line.size() && line[p] == '*' && line[p + 1] == '*') {
            size_t e = p + 2;
            while (e + 1 < line.size() && !(line[e] == '*' && line[e + 1] == '*')) ++e;
            if (e + 1 < line.size() && e > p + 2) {
                pushNormal(p);
                Segment s;
                s.text = line.substr(p + 2, e - p - 2);
                s.style = Style::Bold;
                out.push_back(std::move(s));
                segStart = p = e + 2;
                continue;
            }
        }
        // *italic* (single star, not adjacent to another star)
        if (line[p] == '*' && (p == 0 || line[p - 1] != '*') &&
            (p + 1 >= line.size() || line[p + 1] != '*')) {
            size_t e = p + 1;
            while (e < line.size() && line[e] != '*') ++e;
            if (e < line.size() && e > p + 1) {
                pushNormal(p);
                Segment s;
                s.text = line.substr(p + 1, e - p - 1);
                s.style = Style::Italic;
                out.push_back(std::move(s));
                segStart = p = e + 1;
                continue;
            }
        }
        // [text](url)
        if (line[p] == '[') {
            size_t rb = p + 1;
            while (rb < line.size() && line[rb] != ']') ++rb;
            if (rb < line.size() && rb > p + 1 &&
                rb + 1 < line.size() && line[rb + 1] == '(') {
                size_t rp = rb + 2;
                while (rp < line.size() && line[rp] != ')') ++rp;
                if (rp < line.size()) {
                    pushNormal(p);
                    Segment s;
                    s.text = line.substr(p + 1, rb - p - 1);
                    s.style = Style::Link;
                    s.url = line.substr(rb + 2, rp - rb - 2);
                    out.push_back(std::move(s));
                    segStart = p = rp + 1;
                    continue;
                }
            }
        }
        ++p;
    }
    pushNormal(line.size());
    return out;
}

// --- Table helpers (1:1 with parseTableRow/isTableSep/parseTableAlign) ----

std::vector<std::string> parseTableRow(const std::string& line) {
    std::vector<std::string> cells;
    size_t p = 0;

    // Tables without a leading '|': the segment before the first '|' is the
    // first cell (previously it was silently dropped).
    if (!line.empty() && line[0] != '|') {
        size_t firstPipe = line.find('|');
        if (firstPipe != std::string::npos) {
            size_t s = 0, e = firstPipe;
            while (s < e && (line[s] == ' ' || line[s] == '\t')) ++s;
            while (e > s && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
            cells.emplace_back(line.substr(s, e - s));
            p = firstPipe;
        }
    }

    while (p < line.size()) {
        if (line[p] == '|') {
            ++p;
            size_t start = p;
            while (p < line.size() && line[p] != '|') ++p;
            while (start < p && line[start] == ' ') ++start;
            size_t end = p;
            while (end > start && line[end - 1] == ' ') --end;
            cells.emplace_back(line.substr(start, end - start));
        } else {
            ++p;
        }
    }
    while (!cells.empty() && cells.back().empty()) cells.pop_back();
    return cells;
}

bool isTableSep(const std::string& line) {
    bool hasPipe = false;
    for (char c : line) {
        if (c == '|') { hasPipe = true; continue; }
        if (c == ' ' || c == '-' || c == ':' || c == '\t') continue;
        return false;
    }
    return hasPipe;
}

std::vector<int> parseTableAlign(const std::string& line, int expectedCols) {
    std::vector<int> result;
    size_t p = 0;
    while (p < line.size()) {
        if (line[p] == '|') {
            ++p;
            size_t segStart = p;
            while (p < line.size() && line[p] != '|') ++p;
            while (segStart < p && line[segStart] == ' ') ++segStart;
            size_t segEnd = p;
            while (segEnd > segStart && line[segEnd - 1] == ' ') --segEnd;
            bool lc = (segStart < segEnd && line[segStart] == ':');
            bool rc = (segStart < segEnd && line[segEnd - 1] == ':');
            if (lc && rc) result.push_back(1);
            else if (rc) result.push_back(2);
            else result.push_back(0);
            if (static_cast<int>(result.size()) >= expectedCols) break;
        } else {
            ++p;
        }
    }
    return result;
}

// Map header level to a style (level>=3 collapses to H3, mirroring legacy).
Style headerStyle(int level) {
    if (level == 1) return Style::H1;
    if (level == 2) return Style::H2;
    return Style::H3;
}

} // namespace

std::vector<Line> parseMarkdown(const std::string& text) {
    std::vector<Line> out;
    if (text.empty()) return out;

    // Split into lines.
    std::vector<std::string> lines;
    size_t s = 0;
    while (s < text.size()) {
        size_t nl = text.find('\n', s);
        if (nl == std::string::npos) {
            lines.push_back(text.substr(s));
            break;
        }
        lines.push_back(text.substr(s, nl - s));
        s = nl + 1;
    }

    enum class State { Normal, CodeBlock, Table };
    State state = State::Normal;
    std::vector<std::string> codeLines;
    std::vector<std::string> tableHeaders;
    std::vector<std::vector<std::string>> tableRows;
    std::vector<int> tableAlign;

    auto flushCode = [&]() {
        for (const auto& cl : codeLines) {
            Line l;
            Segment seg;
            seg.text = cl;
            seg.style = Style::CodeBlock;
            l.segs.push_back(std::move(seg));
            out.push_back(std::move(l));
        }
        codeLines.clear();
    };

    auto flushTable = [&]() {
        if (tableHeaders.empty()) return;
        {
            Line l;
            for (const auto& h : tableHeaders) {
                Segment seg;
                seg.text = h;
                seg.style = Style::TableHeader;
                l.segs.push_back(std::move(seg));
            }
            l.tableAlign = tableAlign;
            out.push_back(std::move(l));
        }
        for (const auto& row : tableRows) {
            Line l;
            for (const auto& c : row) {
                // Keep inline styles inside cells.
                auto segs = parseInline(c);
                for (auto& sg : segs) {
                    if (sg.style == Style::Normal) sg.style = Style::TableCell;
                    l.segs.push_back(std::move(sg));
                }
            }
            out.push_back(std::move(l));
        }
        tableHeaders.clear();
        tableRows.clear();
        tableAlign.clear();
    };

    auto emitLine = [&](Line l) { out.push_back(std::move(l)); };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];

        if (state == State::CodeBlock) {
            size_t cs = skipSpace(ln, 0);
            if (startsWith(ln, cs, "```")) {
                flushCode();
                state = State::Normal;
                continue;
            }
            codeLines.push_back(ln);
            continue;
        }

        if (state == State::Table) {
            size_t ts = skipSpace(ln, 0);
            // Support table rows without a leading '|' (e.g. "1 | 2").
            bool rowHasPipe = ln.find('|', ts) != std::string::npos;
            if (rowHasPipe) {
                if (isTableSep(ln.substr(ts))) continue;
                auto cells = parseTableRow(ln.substr(ts));
                bool any = false;
                for (auto& c : cells) if (!c.empty()) { any = true; break; }
                if (any) {
                    tableRows.push_back(std::move(cells));
                    continue;
                }
            }
            flushTable();
            state = State::Normal;
        }

        size_t ls = skipSpace(ln, 0);
        size_t trimmedLen = ln.size() - ls;

        // Empty line.
        if (trimmedLen == 0) {
            flushCode();
            flushTable();
            emitLine(Line{});
            continue;
        }

        // Code fence.
        if (startsWith(ln, ls, "```")) {
            flushTable();
            state = State::CodeBlock;
            codeLines.clear();
            continue;
        }

        // HR: >=3 identical chars (with trailing spaces allowed).
        if (trimmedLen >= 3) {
            char ch = ln[ls];
            if ((ch == '-' || ch == '*' || ch == '_') &&
                ln[ls] == ch && ln[ls + 1] == ch && ln[ls + 2] == ch) {
                bool allSame = true;
                for (size_t k = 3; k < trimmedLen; ++k) {
                    if (ln[ls + k] != ch && ln[ls + k] != ' ') { allSame = false; break; }
                }
                if (allSame) {
                    flushTable();
                    Line l;
                    Segment seg;
                    seg.text = "";
                    seg.style = Style::HR;
                    l.segs.push_back(std::move(seg));
                    emitLine(std::move(l));
                    continue;
                }
            }
        }

        // Header.
        if (ln[ls] == '#') {
            flushTable();
            int level = 0;
            size_t h = ls;
            while (h < ln.size() && ln[h] == '#') { ++level; ++h; }
            if (h < ln.size() && ln[h] == ' ' && level <= 6) {
                ++h;
                Line l;
                Segment seg;
                seg.text = ln.substr(h);
                seg.style = headerStyle(level);
                l.segs.push_back(std::move(seg));
                emitLine(std::move(l));
                continue;
            }
            ls = skipSpace(ln, 0);
        }

        // Blockquote.
        if (ln[ls] == '>') {
            flushTable();
            size_t q = ls + 1;
            if (q < ln.size() && ln[q] == ' ') ++q;
            Line l;
            for (auto& sg : parseInline(ln.substr(q))) {
                if (sg.style == Style::Normal) sg.style = Style::Quote;
                l.segs.push_back(std::move(sg));
            }
            emitLine(std::move(l));
            continue;
        }

        // Table row: a '|' at the line start, or a pipe-delimited line whose
        // next line is a separator row (supports tables without a leading '|').
        bool hasPipe = ln.find('|', ls) != std::string::npos;
        if (hasPipe) {
            flushCode();
            if (state != State::Table) {
                bool looksLikeTable = false;
                if (i + 1 < lines.size()) {
                    size_t nls = skipSpace(lines[i + 1], 0);
                    const std::string& nl = lines[i + 1];
                    if (nl.find('|', nls) != std::string::npos && isTableSep(nl.substr(nls))) {
                        looksLikeTable = true;
                    }
                }
                if (!looksLikeTable) {
                    Line l;
                    l.segs = parseInline(ln.substr(ls));
                    emitLine(std::move(l));
                    continue;
                }
                state = State::Table;
                tableHeaders = parseTableRow(ln.substr(ls));
                tableRows.clear();
                if (i + 1 < lines.size()) {
                    size_t sls = skipSpace(lines[i + 1], 0);
                    tableAlign = parseTableAlign(lines[i + 1].substr(sls),
                        static_cast<int>(tableHeaders.size()));
                }
                continue;
            }
            continue;
        }

        // Bullet: '-' or '*' followed by space.
        if ((ln[ls] == '-' || ln[ls] == '*') && ls + 1 < ln.size() && ln[ls + 1] == ' ') {
            flushTable();
            Line l;
            l.segs = parseInline(ln.substr(ls + 2));
            for (auto& sg : l.segs) if (sg.style == Style::Normal) sg.style = Style::Bullet;
            emitLine(std::move(l));
            continue;
        }

        // Ordered list: digits + '. '.
        {
            size_t ns = ls;
            while (ns < ln.size() && std::isdigit(static_cast<unsigned char>(ln[ns]))) ++ns;
            if (ns > ls && ns < ln.size() && ln[ns] == '.' &&
                ns + 1 < ln.size() && ln[ns + 1] == ' ') {
                flushTable();
                Line l;
                Segment num;
                num.text = ln.substr(ls, ns - ls + 1); // "N."
                num.style = Style::Ordered;
                l.segs.push_back(std::move(num));
                for (auto& sg : parseInline(ln.substr(ns + 2))) {
                    if (sg.style == Style::Normal) sg.style = Style::Ordered;
                    l.segs.push_back(std::move(sg));
                }
                emitLine(std::move(l));
                continue;
            }
        }

        // Paragraph (default).
        flushCode();
        flushTable();
        {
            Line l;
            l.segs = parseInline(ln.substr(ls));
            emitLine(std::move(l));
        }
    }

    flushCode();
    flushTable();
    return out;
}

} // namespace markdown_text
