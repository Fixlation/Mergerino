// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QSet>
#include <QTimer>
#include <QVariantAnimation>

#include <utility>
#include <vector>

class QEvent;
class QMouseEvent;
class QPaintEvent;

namespace chatterino {

class LinkInfo;
class TooltipWidget;

class PinnedMessageBar final : public BaseWidget
{
public:
    explicit PinnedMessageBar(QWidget *parent = nullptr);

    void setChannel(const ChannelPtr &channel);
    void setDisplayEnabled(bool enabled);
    void setMultiplePlatformsSelected(bool multiplePlatformsSelected);
    bool hasHiddenPinnedMessage() const;
    void unhidePinnedMessages();
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

private:
    struct Row {
        ChannelPtr channel;
        PinnedChatMessage message;
        QRect rect;
        QRect menuRect;
        std::vector<std::pair<QRectF, QString>> linkHitboxes;
        bool bodyElided = false;
    };

    void rebuildObservedChannels();
    void rebuildRows();
    void updateFixedHeight();
    void refreshTwitchState();
    void showMenu(int rowIndex, const QPoint &globalPosition);
    void showLinkMenu(const QString &url, const QPoint &globalPosition);
    void showLinkTooltip(const QString &url, const QPoint &globalPosition);
    int rowAt(const QPoint &position) const;
    int collapsedRowHeight() const;
    int expandedRowHeight(const Row &row) const;
    int rowHeight(const Row &row) const;
    int expandedBodyHeight(const Row &row) const;
    QString bodyText(const Row &row) const;
    bool rowExpanded(const Row &row) const;
    void animateRowExpansion(const Row &row, bool expand);
    QString remainingText(const Row &row) const;

    ChannelPtr channel_;
    std::vector<ChannelPtr> observedChannels_;
    std::vector<Row> rows_;
    TooltipWidget *tooltipWidget_ = nullptr;
    QPointer<LinkInfo> hoveredLinkInfo_;
    QString hoveredLinkUrl_;
    pajlada::Signals::SignalHolder channelSignals_;
    QTimer displayTimer_;
    QTimer refreshTimer_;
    QVariantAnimation rowExpansionAnimation_;
    QSet<QString> hiddenMessageKeys_;
    QSet<QString> expandedMessageKeys_;
    QString animatedMessageKey_;
    int animatedRowHeight_ = 0;
    bool animationTargetExpanded_ = false;
    bool displayEnabled_ = true;
    bool multiplePlatformsSelected_ = false;
    int hoveredMenuRow_ = -1;
};

}  // namespace chatterino
