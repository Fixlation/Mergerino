// SPDX-FileCopyrightText: 2023 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/completion/sources/Source.hpp"
#include "controllers/completion/strategies/Strategy.hpp"

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace chatterino {

class Channel;
enum class MessagePlatform : std::uint8_t;

}  // namespace chatterino

namespace chatterino::completion {

struct CommandItem {
    QString name{};
    QString prefix{};
    std::vector<MessagePlatform> platforms{};
};

class CommandSource : public Source
{
public:
    using ActionCallback = std::function<void(const QString &)>;
    using PlatformActionCallback =
        std::function<void(const QString &, MessagePlatform)>;
    using CommandStrategy = Strategy<CommandItem>;

    /// @brief Initializes a source for CommandItems.
    /// @param strategy Strategy to apply
    /// @param callback ActionCallback to invoke upon InputCompletionItem selection.
    /// See InputCompletionItem::action(). Can be nullptr.
    CommandSource(
        std::unique_ptr<CommandStrategy> strategy,
        ActionCallback callback = nullptr, const Channel *channel = nullptr,
        bool slashCommandsOnly = false,
        std::vector<MessagePlatform> platformFilter = {},
        PlatformActionCallback platformCallback = nullptr);

    void update(const QString &query) override;
    void addToListModel(GenericListModel &model,
                        size_t maxCount = 0) const override;
    void addToStringList(QStringList &list, size_t maxCount = 0,
                         bool isFirstWord = false) const override;

    const std::vector<CommandItem> &output() const;

private:
    void initializeItems();

    std::unique_ptr<CommandStrategy> strategy_;
    ActionCallback callback_;
    PlatformActionCallback platformCallback_;
    const Channel *channel_{};
    bool slashCommandsOnly_{};
    std::vector<MessagePlatform> platformFilter_{};

    std::vector<CommandItem> items_{};
    std::vector<CommandItem> output_{};
};

}  // namespace chatterino::completion
