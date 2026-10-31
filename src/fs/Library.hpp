// QPanel — library module.
// Lazy-loaded hierarchical directory tree. Node children load on expand in a
// background worker; tree traversal renders a scroll window of visible rows.
#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

#include "../media/Media.hpp"

namespace qp {

struct TreeNode {
    std::filesystem::path path;
    std::string name;
    bool isDir = false;
    bool expanded = false;
    bool loading = false;
    int parent = -1;
    std::vector<int> children;

    bool playable() const { return !isDir && Media::playable(path.string()); }
};

class Library {
public:
    void setRoot(const std::string& root); // synchronous, single root level
    std::string root() const { return rootPath_; }

    // expanded career: render path; returns number of nodes in view
    std::vector<int> visible(int top, int count) const; // indices of rows [top, top+count)

    // DFS-visible depth calculations
    int visibleCount() const;
    int depth(int node) const;

    void expand(int idx);          // marks loading; caller fills children later
    void collapse(int idx);
    void loadChildren(int idx);    // SYNCHRONOUS fill (careful: bumps nodes_)
    bool isLoading(int idx) const { return at(idx).loading; }

    // worker-safe scan of a dir (copies node, touches no tree state)
    // pairs: (name,path,isDir) ready for graft()
    struct DirEntry { std::string name; std::string path; bool isDir; };
    std::vector<DirEntry> scan(int idx) const;

    // UI-thread-only: graft scanned children
    void graft(int idx, const std::vector<DirEntry>& entries);

    const TreeNode& at(int i) const { return nodes_[size_t(i)]; }
    int count() const { return int(nodes_.size()); }

    int rootIdx() const { return rootIdx_; }
    TreeNode& ref(int i);

private:
    int rootIdx_ = -1;
    std::vector<TreeNode> nodes_;
    std::string rootPath_;
};

// thread-safe wrapper managed by App (tree data is owned by UI thread;
// loads happen on workers and publish back via events)
struct TreeLoadResult {
    int node = -1;
    std::vector<TreeNode> children;
};

} // namespace qp
