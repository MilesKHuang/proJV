// proJV studio -- the file tree's model: entries, Explorer order, visible rows.
//
// The tree is the Files panel's whole state. It is ImGui-free on purpose: the
// ordering rule and the flattening are where the bugs live (a row drawn one place
// and its path read from another is the classic file-tree defect), so both are
// pinned by tests without a window.
//
// Three decisions, all taken from the reference panel this was designed from:
//
//   1. Explorer order, not dictionary order: folders first, and a digit run
//      compares as a NUMBER, so b2 sorts before b10. StrCmpLogicalW was the
//      tempting shortcut and the wrong one: it cannot be tested without the OS and
//      hides the rule inside a system call.
//   2. Children are read lazily, only for directories the user expanded, one level
//      at a time. A tree that walks itself is a hang waiting for a symlink loop.
//   3. A row is a value: name, path, depth, flags. The panel draws rows and reads
//      paths from the same struct, so the two cannot disagree.
#pragma once

#include <set>
#include <string>
#include <vector>

namespace studio {
namespace ftree {

struct Entry {
    std::string name;
    std::string path;
    bool isDir = false;
};

// The ordering rule, exposed so the panel and the tests use the same one.
// Text runs sort before number runs at the same position (the reference's tag
// ordering); numbers compare by value, never by text.
bool entryLess(const Entry& a, const Entry& b);
void sortEntries(std::vector<Entry>& entries);

// One level of a directory. `error` is set when the directory could not be read
// (permission, vanished); the node still draws, with the reason.
struct Node {
    Entry entry;
    bool loaded = false;
    std::string error;
    bool truncated = false; // more than kMaxChildren entries; the rest is not listed
    std::vector<Node> children;
};

// Cap on one directory's children: a node_modules must not cost a frame. The
// reference has no cap because a Qt model streams; a GUI that lays out every row
// does.
constexpr int kMaxChildren = 2000;

// Read one directory (sorted). Never throws; an unreadable path fills `err`.
std::vector<Entry> listDirectory(const std::string& dir, std::string* err = nullptr);

// One visible row of the panel.
struct Row {
    std::string name;
    std::string path;
    int depth = 0;
    bool isDir = false;
    bool expanded = false;
    bool outside = false; // filled by the panel: outside the agent's allowed roots
};

// The whole tree: a root, which directories are expanded, and their children.
class Tree {
  public:
    // Point the tree at `dir` and read its first level. False when it is not a
    // readable directory; `err` says why.
    bool setRoot(const std::string& dir, std::string* err = nullptr);
    const std::string& root() const { return root_; }
    const std::string& rootError() const { return rootError_; }

    // Lazy read of one directory. Called when a folder is expanded.
    bool load(const std::string& dir, std::string* err = nullptr);
    bool isExpanded(const std::string& dir) const;
    void expand(const std::string& dir);
    void collapse(const std::string& dir);
    void toggle(const std::string& dir);

    // Re-read every expanded directory; collapses what no longer exists.
    void refresh();

    // The rows to draw, top to bottom (collapsed folders hide their children).
    std::vector<Row> rows() const;
    // Rows the last rows() call produced -- the panel's scroll bound.
    int count() const { return count_; }

  private:
    Node* find(Node& node, const std::string& dir);
    void flatten(const Node& node, int depth, std::vector<Row>& out) const;
    void refreshNode(Node& node);

    std::string root_;
    std::string rootError_;
    Node rootNode_;
    std::set<std::string> expanded_;
    mutable int count_ = 0;
};

} // namespace ftree
} // namespace studio
