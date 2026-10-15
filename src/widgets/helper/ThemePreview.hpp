// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/ThemeEffect.hpp"
#include "widgets/helper/ThemeBackdrop.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QTimer>
#include <QWidget>

#include <memory>

namespace chatterino {

struct Emote;

/// A miniature chat window painted with a theme, including its animated
/// backdrop, so themes can be judged before (or without) applying them.
class ThemePreview final : public QWidget
{
public:
    explicit ThemePreview(QWidget *parent = nullptr);

    /// @param theme A theme already resolved for the mode it is shown in
    void setTheme(const QJsonObject &theme);

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Colors {
        QColor accent;
        QColor window;
        QColor windowText;
        QColor tab;
        QColor tabText;
        QColor selectedTab;
        QColor selectedTabText;
        QColor newMessageTabText;
        QColor header;
        QColor headerText;
        QColor headerBorder;
        QColor chat;
        QColor alternate;
        QColor text;
        QColor system;
        QColor link;
        QColor input;
        QColor inputBorder;
        QColor placeholder;
        bool light{};
    };

    void updateTimer();

    Colors colors_;
    ThemeEffect effect_;
    ThemeBackdrop backdrop_;
    QTimer frameTimer_;
    QElapsedTimer clock_;
    std::shared_ptr<const Emote> skullEmote_;
    std::shared_ptr<const Emote> moodEmote_;
    pajlada::Signals::SignalHolder connections_;
};

}  // namespace chatterino
