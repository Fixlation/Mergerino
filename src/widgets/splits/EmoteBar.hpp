// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"

#include <functional>
#include <memory>
#include <vector>

class QContextMenuEvent;
class QPaintEvent;

namespace chatterino {

class EmoteBar final : public BaseWidget
{
public:
    using ChannelProvider = std::function<ChannelPtr()>;
    using SendChannelsProvider = std::function<std::vector<ChannelPtr>()>;
    using ActivateCallback =
        std::function<void(const QString &, Qt::KeyboardModifiers)>;
    using LayoutChangedCallback = std::function<void()>;

    EmoteBar(QWidget *parent, ChannelProvider channelProvider,
             SendChannelsProvider sendChannelsProvider,
             ActivateCallback activateCallback,
             LayoutChangedCallback layoutChangedCallback);
    ~EmoteBar() override;

    void refresh();
    void recordMessage(const QString &message);
    int preferredHeight() const;
    int contentTop() const;

protected:
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;
    void paintEvent(QPaintEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chatterino
