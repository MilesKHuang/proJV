// proJV studio -- the file tree's model (see file_tree.h).
#include "file_tree.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace studio {
namespace ftree {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr bool kCaseInsensitive = true;
#else
constexpr bool kCaseInsensitive = false;
#endif

std::string folded(const std::string& s) {
    if (!kCaseInsensitive) return s;
    std::string out = s;
    for (char& c : out) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 128) c = static_cast<char>(std::tolower(u));
    }
    return out;
}

struct Run {
    bool number = false;
    std::string text;             // folded, when !number
    unsigned long long value = 0; // when number (saturating on overflow)
};

std::vector<Run> splitRuns(const std::string& name) {
    std::vector<Run> runs;
    std::string text;
    std::string digits;
    const auto flushText = [&] {
        if (!text.empty()) {
            Run r;
            r.text = folded(text);
            runs.push_back(std::move(r));
            text.clear();
        }
    };
    const auto flushDigits = [&] {
        if (!digits.empty()) {
            Run r;
            r.number = true;
            unsigned long long value = 0;
            bool saturated = false;
            for (char c : digits) {
                const unsigned long long digit = static_cast<unsigned long long>(c - '0');
                if (!saturated && value > (~0ull - digit) / 10u) saturated = true;
                else if (!saturated) value = value * 10u + digit;
            }
            r.value = saturated ? ~0ull : value;
            runs.push_back(std::move(r));
            digits.clear();
        }
    };
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u >= '0' && u <= '9') {
            flushText();
            digits.push_back(c);
        } else {
            flushDigits();
            text.push_back(c);
        }
    }
    flushText();
    flushDigits();
    return runs;
}

} // namespace

bool entryLess(const Entry& a, const Entry& b) {
    if (a.isDir != b.isDir) return a.isDir;
    const std::vector<Run> ra = splitRuns(a.name);
    const std::vector<Run> rb = splitRuns(b.name);
    const size_t n = std::min(ra.size(), rb.size());
    for (size_t i = 0; i < n; ++i) {
        if (ra[i].number != rb[i].number) return !ra[i].number; // text before number
        if (ra[i].number) {
            if (ra[i].value != rb[i].value) return ra[i].value < rb[i].value;
        } else if (ra[i].text != rb[i].text) {
            return ra[i].text < rb[i].text;
        }
    }
    if (ra.size() != rb.size()) return ra.size() < rb.size();
    return a.name < b.name; // same key: deterministic, never a tie in the sort
}

void sortEntries(std::vector<Entry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), entryLess);
}

std::vector<Entry> listDirectory(const std::string& dir, std::string* err) {
    std::vector<Entry> out;
    if (err) err->clear();
    if (dir.empty()) {
        if (err) *err = "empty path";
        return out;
    }
    std::error_code ec;
    fs::directory_iterator it(fs::path(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        if (err) *err = ec.message();
        return out;
    }
    const fs::directory_iterator end;
    for (; it != end; it.increment(ec)) {
        if (ec) {
            if (err) *err = ec.message();
            break;
        }
        const fs::directory_entry& de = *it;
        std::string name = de.path().filename().string();
        if (name.empty() || name == "." || name == "..") continue;
        std::error_code deEc;
        const bool isDir = de.is_directory(deEc);
        Entry e;
        e.name = std::move(name);
        e.path = de.path().string();
        e.isDir = isDir && !deEc;
        out.push_back(std::move(e));
        if (static_cast<int>(out.size()) >= kMaxChildren * 4) break; // hard stop
    }
    sortEntries(out);
    if (static_cast<int>(out.size()) > kMaxChildren) out.resize(kMaxChildren);
    return out;
}

// ---- Tree ----------------------------------------------------------------

bool Tree::setRoot(const std::string& dir, std::string* err) {
    if (dir.empty()) {
        if (err) *err = "empty path";
        return false;
    }
    std::error_code ec;
    if (!fs::is_directory(fs::path(dir), ec) || ec) {
        if (err) *err = "not a directory: " + dir;
        return false;
    }
    root_ = fs::path(dir).lexically_normal().string();
    rootError_.clear();
    rootNode_ = Node{};
    rootNode_.entry.path = root_;
    rootNode_.entry.name = fs::path(root_).filename().string();
    if (rootNode_.entry.name.empty()) rootNode_.entry.name = root_;
    rootNode_.entry.isDir = true;
    expanded_.clear();
    refreshNode(rootNode_);
    rootError_ = rootNode_.error;
    return true;
}

void Tree::refreshNode(Node& node) {
    node.children.clear();
    node.loaded = false;
    node.error.clear();
    node.truncated = false;
    if (!node.entry.isDir) return;
    std::string err;
    const std::vector<Entry> entries = listDirectory(node.entry.path, &err);
    node.error = err;
    node.loaded = true;
    node.truncated = static_cast<int>(entries.size()) >= kMaxChildren;
    node.children.reserve(entries.size());
    for (const Entry& e : entries) {
        Node child;
        child.entry = e;
        node.children.push_back(std::move(child));
    }
    // Nested directories stay unread until one is expanded: refresh() re-reads the
    // ones that were open, and nothing else.
}

Node* Tree::find(Node& node, const std::string& dir) {
    if (node.entry.path == dir) return &node;
    for (Node& child : node.children) {
        if (!child.entry.isDir) continue;
        if (child.entry.path == dir) return &child;
        if (dir.size() > child.entry.path.size() + 1 &&
            dir.compare(0, child.entry.path.size(), child.entry.path) == 0) {
            if (Node* hit = find(child, dir)) return hit;
        }
    }
    return nullptr;
}

bool Tree::load(const std::string& dir, std::string* err) {
    Node* node = find(rootNode_, dir);
    if (!node) {
        if (err) *err = "not in the tree: " + dir;
        return false;
    }
    if (!node->loaded) refreshNode(*node);
    if (err) *err = node->error;
    return node->error.empty();
}

bool Tree::isExpanded(const std::string& dir) const { return expanded_.count(dir) != 0; }

void Tree::expand(const std::string& dir) {
    expanded_.insert(dir);
    std::string err;
    load(dir, &err);
}

void Tree::collapse(const std::string& dir) { expanded_.erase(dir); }

void Tree::toggle(const std::string& dir) {
    if (isExpanded(dir)) collapse(dir);
    else expand(dir);
}

void Tree::refresh() {
    // Directories that vanished while open are dropped instead of drawn as errors
    // for ever; the rest keep their expansion state.
    std::vector<std::string> keep;
    keep.reserve(expanded_.size());
    for (const std::string& dir : expanded_) {
        std::error_code ec;
        if (fs::is_directory(fs::path(dir), ec) && !ec) keep.push_back(dir);
    }
    expanded_.clear();
    for (const std::string& dir : keep) expanded_.insert(dir);

    refreshNode(rootNode_);
    rootError_ = rootNode_.error;
    for (const std::string& dir : expanded_) {
        Node* node = find(rootNode_, dir);
        if (node && !node->loaded) refreshNode(*node);
    }
}

void Tree::flatten(const Node& node, int depth, std::vector<Row>& out) const {
    for (const Node& child : node.children) {
        Row row;
        row.name = child.entry.name;
        row.path = child.entry.path;
        row.depth = depth;
        row.isDir = child.entry.isDir;
        row.expanded = child.entry.isDir && isExpanded(child.entry.path);
        out.push_back(std::move(row));
        if (row.expanded) flatten(child, depth + 1, out);
    }
}

std::vector<Row> Tree::rows() const {
    std::vector<Row> out;
    if (root_.empty()) {
        count_ = 0;
        return out;
    }
    flatten(rootNode_, 0, out);
    count_ = static_cast<int>(out.size());
    return out;
}

} // namespace ftree
} // namespace studio
