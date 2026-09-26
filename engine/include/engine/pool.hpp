#pragma once

#include "engine/node.hpp"
#include <cassert>
#include <vector>
namespace engine {

class NodePool {
  public:
    explicit NodePool(std::size_t capacity) : nodes_(capacity) {
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            free_link(&nodes_[i]) = &nodes_[i + 1];
        }
        free_head_ = capacity > 0 ? &nodes_[0] : nullptr;
    }

    NodePool(const NodePool &) = delete;
    NodePool &operator=(const NodePool &) = delete;

    Node *acquire() {
        if (free_head_ == nullptr)
            return nullptr; // pool exhausted
        Node *node = free_head_;
        free_head_ = free_link(node);
        *node = Node{};
        return node;
    }

    void release(Node *node) {
        assert(node >= nodes_.data() && node < nodes_.data() + nodes_.size());

        node->prev = nullptr;
        free_link(node) = free_head_;
        free_head_ = node;
    }

  private:
    std::vector<Node> nodes_;
    Node *free_head_ = nullptr;
    static Node *&free_link(Node *node) { return node->next; }
};
}; // namespace engine
