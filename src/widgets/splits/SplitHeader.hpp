// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWidget.hpp"
#include "widgets/TooltipWidget.hpp"

#include <boost/signals2.hpp>
#include <pajlada/settings/setting.hpp>
#include <pajlada/signals/connection.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QElapsedTimer>
#include <QMenu>
#include <QPoint>

#include <memory>
#include <vector>

namespace chatterino {

class SvgButton;
class DrawnButton;
class LabelButton;
class Label;
class Split;
class MergedChannel;

class SplitHeader final : public BaseWidget
{
    Q_OBJECT

public:
    explicit SplitHeader(Split *split);

    void setAddButtonVisible(bool value);
    void setSlowChatQueueIndicatorReady(bool value);

    void updateChannelText();
    void updateIcons();
    // Invoked when SplitHeader should update anything refering to a TwitchChannel's mode
    // has changed (e.g. sub mode toggled)
    void updateRoomModes();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    void initializeLayout();
    std::unique_ptr<QMenu> createMainMenu();
    std::unique_ptr<QMenu> createChatModeMenu();
    void exportChat();
    void popupMainMenu(const QPoint &globalPosition);
    void openTabSettings();
    void popupTitleSettingsButtonMenu(const QPoint &globalPosition);
    void setTitleSettingsButtonVisible(bool visible);
    void positionTitleSettingsButton();
    bool isTitleSettingsButtonHoverArea() const;
    void showHoverTooltip(QWidget *target, const QString &text, bool wordWrap);
    void hideHoverTooltip();
    QString mergedStreamPreviewTooltip(MergedChannel *mergedChannel);
    void updateThumbnail(const QString &url, bool followRedirects);

    /**
     * @brief   Reset the thumbnail data and timer so a new
     *          thumbnail can be fetched
     **/
    void resetThumbnail();

    void handleChannelChanged();

    Split *const split_{};
    QString tooltipText_{};
    QString viewerCountTooltipText_{};
    QString modeTooltipText_{};
    TooltipWidget *const tooltipWidget_{};
    bool isLive_{false};
    QString thumbnail_;
    QString thumbnailUrl_;
    QElapsedTimer lastThumbnail_;
    std::chrono::steady_clock::time_point lastReloadedChannelEmotes_;
    std::chrono::steady_clock::time_point lastReloadedSubEmotes_;

    // ui
    DrawnButton *dropdownButton_{};
    Label *titleLabel_{};
    SvgButton *titleSettingsButton_{};
    BaseWidget *viewerCountContainer_{};
    BaseWidget *viewerCountIcon_{};
    Label *viewerCountLabel_{};

    LabelButton *modeButton_{};
    Label *queuedSlowChatCountLabel_{};
    QAction *modeActionSetEmote{};
    QAction *modeActionSetSub{};
    QAction *modeActionSetSlow{};
    QAction *modeActionSetR9k{};
    QAction *modeActionSetFollowers{};

    SvgButton *alertsButton_{};
    SvgButton *priorityChatButton_{};
    SvgButton *moderationButton_{};
    SvgButton *chattersButton_{};
    SvgButton *clearActivityButton_{};
    DrawnButton *addButton_{};

    // states
    QPoint dragStart_{};
    bool dragging_{false};
    bool doubleClicked_{false};
    bool menuVisible_{false};
    bool slowChatQueueIndicatorReady_{false};

    // managedConnections_ contains connections for signals that are not managed by us
    // and don't change when the parent Split changes its underlying channel
    pajlada::Signals::SignalHolder managedConnections_;
    pajlada::Signals::SignalHolder channelConnections_;
    std::vector<boost::signals2::scoped_connection> bSignals_;

public Q_SLOTS:
    void reloadChannelEmotes();
    void reloadSubscriberEmotes();
    void reconnect();
};

}  // namespace chatterino
