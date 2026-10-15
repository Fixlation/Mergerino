#pragma once

#include <cstddef>
#include <deque>
#include <unordered_set>
#include <utility>

namespace chatterino {

// FIFO duplicate window: repeats do not grow storage or evict other recent IDs.
template <typename Key, typename Hash = std::hash<Key>>
class BoundedUniqueSet
{
public:
    explicit BoundedUniqueSet(size_t capacity)
        : capacity_(capacity)
    {
    }
    bool contains(const Key &key) const
    {
        return this->keys_.contains(key);
    }
    bool insert(const Key &key)
    {
        if (this->keys_.contains(key))
            return false;
        if (this->capacity_ == 0)
            return true;
        this->keys_.insert(key);
        this->order_.push_back(key);
        if (this->order_.size() > this->capacity_)
        {
            this->keys_.erase(this->order_.front());
            this->order_.pop_front();
        }
        return true;
    }
    void clear()
    {
        this->keys_.clear();
        this->order_.clear();
    }
    size_t size() const
    {
        return this->keys_.size();
    }

private:
    size_t capacity_;
    std::unordered_set<Key, Hash> keys_;
    std::deque<Key> order_;
};

}  // namespace chatterino
