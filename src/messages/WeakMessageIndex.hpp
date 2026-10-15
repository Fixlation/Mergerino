#pragma once

#include "messages/Message.hpp"
#include "util/QStringHash.hpp"

#include <unordered_map>

namespace chatterino {

// Allows updates to messages kept by history/replies/cards without owning them.
class WeakMessageIndex
{
public:
    MessagePtr find(const QString &key) const
    {
        const auto it = this->messages_.find(key);
        return it == this->messages_.end() ? nullptr : it->second.lock();
    }
    bool contains(const QString &key) const
    {
        return this->find(key) != nullptr;
    }
    void insert(const QString &key, const MessagePtr &message)
    {
        this->messages_[key] = message;
        if (++this->insertions_ >= 256)
        {
            this->pruneExpired();
            this->insertions_ = 0;
        }
    }
    void erase(const QString &key)
    {
        this->messages_.erase(key);
    }
    void pruneExpired()
    {
        std::erase_if(this->messages_, [](const auto &entry) {
            return entry.second.expired();
        });
    }
    size_t size() const
    {
        return this->messages_.size();
    }

private:
    std::unordered_map<QString, std::weak_ptr<const Message>> messages_;
    size_t insertions_ = 0;
};

}  // namespace chatterino
