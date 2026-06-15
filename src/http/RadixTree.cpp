#include <iouring/http/RadixTree.h>

namespace iouring::http {

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

bool RouteEntry::HasHandler() const {
    return static_cast<bool>(handler);
}

bool RouteEntry::HasStreamHandler() const {
    return stream_handler.on_headers || stream_handler.on_body ||
           stream_handler.on_complete || stream_handler.on_abort;
}

bool RouteEntry::HasAnyHandler() const {
    return HasHandler() || HasStreamHandler();
}

bool RadixTree::Node::HasAnyHandler() const {
    for (const auto& route : routes) {
        if (route.HasAnyHandler()) {
            return true;
        }
    }
    return false;
}

std::string RadixTree::Node::AllowHeaderValue() const {
    std::string allow;

    auto append_method = [&allow](std::string_view method) {
        if (!allow.empty()) {
            allow += ", ";
        }
        allow += method;
    };

    const bool allow_get =
        routes[static_cast<std::size_t>(HttpMethod::kGet)].HasAnyHandler();
    const bool allow_head =
        routes[static_cast<std::size_t>(HttpMethod::kHead)].HasAnyHandler();
    if (allow_get) {
        append_method("GET");
    }
    if (allow_head || allow_get) {
        append_method("HEAD");
    }
    if (routes[static_cast<std::size_t>(HttpMethod::kPost)].HasAnyHandler()) {
        append_method("POST");
    }
    if (routes[static_cast<std::size_t>(HttpMethod::kPut)].HasAnyHandler()) {
        append_method("PUT");
    }
    if (routes[static_cast<std::size_t>(HttpMethod::kDelete)].HasAnyHandler()) {
        append_method("DELETE");
    }
    if (routes[static_cast<std::size_t>(HttpMethod::kOptions)].HasAnyHandler()) {
        append_method("OPTIONS");
    }
    if (routes[static_cast<std::size_t>(HttpMethod::kPatch)].HasAnyHandler()) {
        append_method("PATCH");
    }

    return allow;
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
                       RouteEntry entry) {
    if (path == "/") {
        root_->routes[static_cast<std::size_t>(method)] = std::move(entry);
        return;
    }

    auto segments = SplitPath(path);
    InsertSegments(root_.get(), segments, 0, method, std::move(entry));
}

void RadixTree::InsertSegments(Node* node,
                               const std::vector<std::string>& segments,
                               std::size_t index, HttpMethod method,
                               RouteEntry entry) {
    if (index >= segments.size()) {
        node->routes[static_cast<std::size_t>(method)] = std::move(entry);
        return;
    }

    const auto& segment = segments[index];
    if (segment.size() > 1 && segment[0] == '*') {
        if (!node->wildcard_child) {
            node->wildcard_child = std::make_unique<Node>();
            node->wildcard_name = segment.substr(1);
        }
        node->wildcard_child->routes[static_cast<std::size_t>(method)] =
            std::move(entry);
        return;
    }

    if (segment.size() > 1 && segment[0] == ':') {
        if (!node->param_child) {
            node->param_child = std::make_unique<Node>();
            node->param_name = segment.substr(1);
        }
        InsertSegments(node->param_child.get(), segments, index + 1, method,
                       std::move(entry));
        return;
    }

    for (auto& child : node->children) {
        if (child->prefix == segment) {
            InsertSegments(child.get(), segments, index + 1, method,
                           std::move(entry));
            return;
        }
    }

    auto child = std::make_unique<Node>();
    child->prefix = segment;
    auto* raw = child.get();
    node->children.push_back(std::move(child));
    InsertSegments(raw, segments, index + 1, method, std::move(entry));
}

MatchResult RadixTree::Match(HttpMethod method, std::string_view path) const {
    MatchResult result;
    if (path == "/") {
        auto& route = root_->routes[static_cast<std::size_t>(method)];
        if (route.HasAnyHandler()) {
            result.entry = const_cast<RouteEntry*>(&route);
            result.path_exists = true;
        } else if (root_->HasAnyHandler()) {
            result.path_exists = true;
            result.allow_header = root_->AllowHeaderValue();
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
        auto& route = node->routes[static_cast<std::size_t>(method)];
        if (route.HasAnyHandler()) {
            result.entry = const_cast<RouteEntry*>(&route);
            result.params = params;
            result.path_exists = true;
            result.allow_header = node->AllowHeaderValue();
        } else if (node->HasAnyHandler()) {
            result.path_exists = true;
            result.allow_header = node->AllowHeaderValue();
        }
        return;
    }

    if (result.entry) {
        return;
    }

    const auto segment = segments[index];
    for (const auto& child : node->children) {
        if (child->prefix == segment) {
            MatchNode(child.get(), segments, index + 1, method, result, params);
            if (result.entry) {
                return;
            }
            break;
        }
    }

    if (node->param_child && !result.entry) {
        params.emplace_back(std::string_view(node->param_name), segment);
        MatchNode(node->param_child.get(), segments, index + 1, method, result,
                  params);
        if (result.entry) {
            return;
        }
        params.pop_back();
    }

    if (node->wildcard_child && !result.entry) {
        const auto first = segments[index];
        const auto last = segments.back();
        std::string_view wildcard_value(
            first.data(),
            static_cast<std::size_t>(last.data() + last.size() - first.data()));

        auto& route =
            node->wildcard_child->routes[static_cast<std::size_t>(method)];
        if (route.HasAnyHandler()) {
            params.emplace_back(std::string_view(node->wildcard_name),
                                wildcard_value);
            result.entry = const_cast<RouteEntry*>(&route);
            result.params = params;
            result.path_exists = true;
            result.allow_header = node->wildcard_child->AllowHeaderValue();
        } else if (node->wildcard_child->HasAnyHandler()) {
            result.path_exists = true;
            result.allow_header = node->wildcard_child->AllowHeaderValue();
        }
    }
}

} // namespace iouring::http
