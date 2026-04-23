#include <iouring_runtime/web/RadixTree.h>

namespace iouring_runtime::web {

namespace {

std::size_t EstimateSegmentCount(std::string_view path) {
    if (path.empty() || path == "/") {
        return 0;
    }

    std::size_t count = 0;
    bool in_segment = false;
    for (char ch : path) {
        if (ch == '/') {
            in_segment = false;
            continue;
        }
        if (!in_segment) {
            ++count;
            in_segment = true;
        }
    }
    if (path.size() > 1 && path.back() == '/') {
        ++count;
    }
    return count;
}

} // namespace

bool RadixTree::Node::HasAnyHandler() const {
    for (const auto& handler : handlers) {
        if (handler) {
            return true;
        }
    }
    return false;
}

RadixTree::RadixTree()
    : root_(std::make_unique<Node>()) {}

RadixTree::~RadixTree() = default;
RadixTree::RadixTree(RadixTree&&) noexcept = default;
RadixTree& RadixTree::operator=(RadixTree&&) noexcept = default;

std::vector<std::string> RadixTree::SplitPath(const std::string& path) {
    std::vector<std::string> segments;
    segments.reserve(EstimateSegmentCount(path));

    std::size_t start = 0;
    if (!path.empty() && path[0] == '/') {
        start = 1;
    }

    while (start < path.size()) {
        const auto pos = path.find('/', start);
        if (pos == std::string::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        if (pos > start) {
            segments.push_back(path.substr(start, pos - start));
        }
        start = pos + 1;
    }

    return segments;
}

std::vector<std::string_view> RadixTree::SplitPathView(std::string_view path) {
    std::vector<std::string_view> segments;
    segments.reserve(EstimateSegmentCount(path));

    std::size_t start = 0;
    if (!path.empty() && path[0] == '/') {
        start = 1;
    }

    while (start < path.size()) {
        const auto pos = path.find('/', start);
        if (pos == std::string_view::npos) {
            segments.push_back(path.substr(start));
            break;
        }
        if (pos > start) {
            segments.push_back(path.substr(start, pos - start));
        }
        start = pos + 1;
    }

    if (path.size() > 1 && path.back() == '/') {
        segments.emplace_back();
    }

    return segments;
}

void RadixTree::Insert(HttpMethod method, const std::string& path,
                       HttpHandler handler) {
    if (path == "/") {
        root_->handlers[static_cast<std::size_t>(method)] = std::move(handler);
        return;
    }

    auto segments = SplitPath(path);
    InsertSegments(root_.get(), segments, 0, method, std::move(handler));
}

void RadixTree::InsertSegments(Node* node,
                               const std::vector<std::string>& segments,
                               std::size_t index, HttpMethod method,
                               HttpHandler handler) {
    if (index >= segments.size()) {
        node->handlers[static_cast<std::size_t>(method)] = std::move(handler);
        return;
    }

    const auto& segment = segments[index];
    if (segment.size() > 1 && segment[0] == '*') {
        if (!node->wildcard_child) {
            node->wildcard_child = std::make_unique<Node>();
            node->wildcard_name = segment.substr(1);
        }
        node->wildcard_child->handlers[static_cast<std::size_t>(method)] =
            std::move(handler);
        return;
    }

    if (segment.size() > 1 && segment[0] == ':') {
        if (!node->param_child) {
            node->param_child = std::make_unique<Node>();
            node->param_name = segment.substr(1);
        }
        InsertSegments(node->param_child.get(), segments, index + 1, method,
                       std::move(handler));
        return;
    }

    for (auto& child : node->children) {
        if (child->prefix == segment) {
            InsertSegments(child.get(), segments, index + 1, method,
                           std::move(handler));
            return;
        }
    }

    auto child = std::make_unique<Node>();
    child->prefix = segment;
    auto* raw = child.get();
    node->children.push_back(std::move(child));
    InsertSegments(raw, segments, index + 1, method, std::move(handler));
}

MatchResult RadixTree::Match(HttpMethod method, std::string_view path) const {
    MatchResult result;
    if (path == "/") {
        auto& handler = root_->handlers[static_cast<std::size_t>(method)];
        if (handler) {
            result.handler = const_cast<HttpHandler*>(&handler);
            result.path_exists = true;
        } else if (root_->HasAnyHandler()) {
            result.path_exists = true;
        }
        return result;
    }

    auto segments = SplitPathView(path);
    std::vector<std::pair<std::string_view, std::string_view>> params;
    params.reserve(segments.size());
    MatchNode(root_.get(), segments, 0, method, result, params);
    return result;
}

void RadixTree::MatchNode(
    const Node* node, const std::vector<std::string_view>& segments,
    std::size_t index, HttpMethod method, MatchResult& result,
    std::vector<std::pair<std::string_view, std::string_view>>& params) const {
    if (index >= segments.size()) {
        auto& handler = node->handlers[static_cast<std::size_t>(method)];
        if (handler) {
            result.handler = const_cast<HttpHandler*>(&handler);
            result.params = params;
            result.path_exists = true;
        } else if (node->HasAnyHandler()) {
            result.path_exists = true;
        }
        return;
    }

    if (result.handler) {
        return;
    }

    const auto segment = segments[index];
    for (const auto& child : node->children) {
        if (child->prefix == segment) {
            MatchNode(child.get(), segments, index + 1, method, result, params);
            if (result.handler) {
                return;
            }
            break;
        }
    }

    if (node->param_child && !result.handler) {
        params.emplace_back(std::string_view(node->param_name), segment);
        MatchNode(node->param_child.get(), segments, index + 1, method, result,
                  params);
        if (result.handler) {
            return;
        }
        params.pop_back();
    }

    if (node->wildcard_child && !result.handler) {
        const auto first = segments[index];
        const auto last = segments.back();
        std::string_view wildcard_value(
            first.data(),
            static_cast<std::size_t>(last.data() + last.size() - first.data()));

        auto& handler =
            node->wildcard_child->handlers[static_cast<std::size_t>(method)];
        if (handler) {
            params.emplace_back(std::string_view(node->wildcard_name),
                                wildcard_value);
            result.handler = const_cast<HttpHandler*>(&handler);
            result.params = params;
            result.path_exists = true;
        } else if (node->wildcard_child->HasAnyHandler()) {
            result.path_exists = true;
        }
    }
}

} // namespace iouring_runtime::web
