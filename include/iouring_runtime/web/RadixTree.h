#pragma once

#include <iouring_runtime/web/HttpMethod.h>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace iouring_runtime::web {

struct RequestContext;
using HttpHandler = std::function<void(RequestContext&)>;

struct MatchResult {
    HttpHandler* handler = nullptr;
    std::vector<std::pair<std::string_view, std::string_view>> params;
    bool path_exists = false;
};

class RadixTree {
public:
    RadixTree();
    ~RadixTree();

    RadixTree(const RadixTree&) = delete;
    RadixTree& operator=(const RadixTree&) = delete;
    RadixTree(RadixTree&&) noexcept;
    RadixTree& operator=(RadixTree&&) noexcept;

    void Insert(HttpMethod method, const std::string& path, HttpHandler handler);
    MatchResult Match(HttpMethod method, std::string_view path) const;

private:
    static constexpr std::size_t kMethodCount =
        static_cast<std::size_t>(HttpMethod::kUnknown) + 1;

    struct Node {
        std::string prefix;
        std::vector<std::unique_ptr<Node>> children;
        std::unique_ptr<Node> param_child;
        std::string param_name;
        std::unique_ptr<Node> wildcard_child;
        std::string wildcard_name;
        std::array<HttpHandler, kMethodCount> handlers{};

        bool HasAnyHandler() const;
    };

    static std::vector<std::string> SplitPath(const std::string& path);
    static std::vector<std::string_view> SplitPathView(std::string_view path);

    void InsertSegments(Node* node, const std::vector<std::string>& segments,
                        std::size_t index, HttpMethod method,
                        HttpHandler handler);
    void MatchNode(const Node* node,
                   const std::vector<std::string_view>& segments,
                   std::size_t index, HttpMethod method, MatchResult& result,
                   std::vector<std::pair<std::string_view, std::string_view>>& params) const;

    std::unique_ptr<Node> root_;
};

} // namespace iouring_runtime::web
