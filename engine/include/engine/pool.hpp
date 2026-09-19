#pragma once

#include "engine/node.hpp"
#include <vector>
namespace engine {

class NodePool {
  public:
    explicit NodePool(std::size_t capacity) : slots_(capacity) {
        for (std::size_t i = 0; i + 1 < capacity; ++i) {
            slots_[i].next_free = static_cast<int>(i) + 1;
        }
        if (capacity > 0)
            slots_[capacity - 1].next_free = -1;
        free_head_ = capacity > 0 ? 0 : -1;
    }

    Node *acquire() {
        if (free_head_ == -1)
            return nullptr; // pool exhausted
        int idx = free_head_;
        free_head_ = slots_[idx].next_free;
        Node *node = &slots_[idx].node;
        *node = Node{};
        return node;
    }

    void release(Node *node) {
        int idx = static_cast<int>(
            reinterpret_cast<Slot *>(reinterpret_cast<char *>(node)) -
            slots_.data());
        slots_[idx].next_free = free_head_; // link back onto the free list
        free_head_ = idx;
    }

  private:
    union Slot {
        Slot() {}
        ~Slot() {}
        Node node;
        int next_free;
    };
    std::vector<Slot> slots_;
    int free_head_ = -1;
};
}; // namespace engine
