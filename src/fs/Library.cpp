#include "Library.hpp"
#include <algorithm>
#include <functional>

namespace qp {
namespace fs = std::filesystem;

void Library::setRoot(const std::string& root) {
    nodes_.clear();
    rootPath_ = root;
    TreeNode n;
    n.path = root;
    n.name = root;
    n.isDir = true;
    n.expanded = true;
    n.parent = -1;
    nodes_.push_back(n);
    rootIdx_ = 0;
    loadChildren(rootIdx_);
}

TreeNode& Library::ref(int i) { return nodes_[size_t(i)]; }

int Library::depth(int node) const {
    int d = 0;
    int i = node;
    while (i >= 0) { i = nodes_[size_t(i)].parent; d++; }
    return d - 1; // root has depth 0
}

int Library::visibleCount() const {
    std::vector<int> v;
    std::function<void(int)> walk = [&](int n) {
        if (n != rootIdx_) v.push_back(n);
        const TreeNode& t = nodes_[size_t(n)];
        if (t.isDir && t.expanded)
            for (int ch : t.children) walk(ch);
    };
    walk(rootIdx_);
    return int(v.size());
}

void Library::loadChildren(int idx) {
    auto entries = scan(idx);
    graft(idx, entries);
}

std::vector<Library::DirEntry> Library::scan(int idx) const {
    std::vector<DirEntry> kids;
    if (idx < 0 || size_t(idx) >= nodes_.size()) return kids;
    fs::path dir = nodes_[size_t(idx)].path; // copy: no ref escapes
    std::error_code ec;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        const fs::directory_entry& e = *it;
        std::string name = e.path().filename().string();
        if (!name.empty() && name[0] == '.') continue;
        bool isDir = e.is_directory(ec);
        kids.push_back(DirEntry{ name, e.path().string(), isDir });
    }
    std::sort(kids.begin(), kids.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir;
        std::string an = a.name, bn = b.name;
        std::transform(an.begin(), an.end(), an.begin(), [](unsigned char c){ return std::tolower(c); });
        std::transform(bn.begin(), bn.end(), bn.begin(), [](unsigned char c){ return std::tolower(c); });
        return an < bn;
    });
    return kids;
}

// UI-thread only
void Library::graft(int idx, const std::vector<DirEntry>& entries) {
    if (idx < 0 || size_t(idx) >= nodes_.size()) return;
    TreeNode& n = nodes_[size_t(idx)];
    if (!n.children.empty()) { n.loading = false; n.expanded = true; return; }
    int parent = idx;
    for (const auto& e : entries) {
        int id = int(nodes_.size());
        TreeNode c;
        c.path = e.path;
        c.name = e.name;
        c.isDir = e.isDir;
        c.parent = parent;
        nodes_.push_back(std::move(c));
        nodes_[size_t(parent)].children.push_back(id); // re-fetch, never hold ref
    }
    nodes_[size_t(parent)].loading = false;
    nodes_[size_t(parent)].expanded = true;
}

void Library::expand(int idx) {
    if (idx < 0 || size_t(idx) >= nodes_.size()) return;
    TreeNode& n = nodes_[size_t(idx)];
    if (!n.isDir) return;
    if (n.expanded) { n.expanded = false; return; } // toggle: collapse via same call
    if (n.children.empty()) {
        // caller must schedule loadChildren on a worker and re-expand after
        n.loading = true;
        n.expanded = false;
        return;
    }
    n.expanded = true;
}

void Library::collapse(int idx) {
    if (idx >= 0 && size_t(idx) < nodes_.size()) nodes_[size_t(idx)].expanded = false;
}

std::vector<int> Library::visible(int top, int rowCount) const {
    std::vector<int> out;
    out.reserve(size_t(rowCount) * 2);
    std::function<void(int)> walk = [&](int n) {
        if (int(out.size()) >= top + rowCount) return;
        if (n != rootIdx_) out.push_back(n);
        const TreeNode& t = nodes_[size_t(n)];
        if (t.isDir && t.expanded)
            for (int ch : t.children) walk(ch);
    };
    walk(rootIdx_);
    if (top >= int(out.size())) return {};
    size_t from = size_t(top);
    size_t to = std::min(out.size(), size_t(top) + size_t(rowCount));
    return std::vector<int>(out.begin() + long(from), out.begin() + long(to));
}

} // namespace qp
