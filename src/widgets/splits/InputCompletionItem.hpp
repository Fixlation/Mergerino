// SPDX-FileCopyrightText: 2020 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/listview/GenericListItem.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace chatterino {

enum class MessagePlatform : std::uint8_t;
struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

class InputCompletionItem : public GenericListItem
{
    using ActionCallback = std::function<void(const QString &)>;
    using PlatformActionCallback =
        std::function<void(const QString &, MessagePlatform)>;

public:
    InputCompletionItem(
        const EmotePtr &emote, const QString &text, ActionCallback action,
        std::vector<MessagePlatform> platforms = {},
        PlatformActionCallback platformAction = nullptr);

    // GenericListItem interface
    void action() override;
    void actionAt(const QPoint &position, const QRect &rect) override;
    void paint(QPainter *painter, const QRect &rect) const override;
    QSize sizeHint(const QRect &rect) const override;

private:
    EmotePtr emote_;
    QString text_;
    QString actionText() const;
    std::vector<QRect> platformIconRects(const QRect &rect) const;

    ActionCallback action_;
    std::vector<MessagePlatform> platforms_;
    PlatformActionCallback platformAction_;
};

}  // namespace chatterino
