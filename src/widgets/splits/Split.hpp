// SPDX-FileCopyrightText: 2016 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Aliases.hpp"
#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/splits/SplitCommon.hpp"

#include <boost/signals2.hpp>
#include <pajlada/signals/signalholder.hpp>
#include <QFont>
#include <QPointer>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdint>

namespace chatterino {

class ChannelView;
class SplitHeader;
class SplitInput;
class StreamDatabaseBadgeBar;
class TwitchPollsAndPredictionsBar;
class PinnedMessageBar;
class SplitContainer;
class SplitOverlay;
class SelectChannelDialog;
class OverlayWindow;
class SplitSettingsDialog;
enum class ActivityTimeDisplayMode : std::uint8_t;
enum class PlatformIndicatorMode : std::uint8_t;

// Each ChatWidget consists of three sub-elements that handle their own part of
// the chat widget: ChatWidgetHeader
//   - Responsible for rendering which channel the ChatWidget is in, and the
//   menu in the top-left of
//     the chat widget
// ChatWidgetView
//   - Responsible for rendering all chat messages, and the scrollbar
// ChatWidgetInput
//   - Responsible for rendering and handling user text input
//
// Each sub-element has a reference to the parent Chat Widget
class Split : public BaseWidget
{
    friend class SplitInput;

    Q_OBJECT

public:
    explicit Split(QWidget *parent);

    ~Split() override;

    pajlada::Signals::NoArgSignal channelChanged;
    pajlada::Signals::NoArgSignal focused;
    pajlada::Signals::NoArgSignal focusLost;

    ChannelView &getChannelView();
    SplitInput &getInput();
    bool inputEnabled() const;
    bool isActivityPane() const;
    bool hasLinkedActivityPane();
    QString activityPaneTitle() const;
    qreal activityMessageScale() const;
    bool collapseGiftedSubscriptions() const;
    ActivityTimeDisplayMode activityTimeDisplayMode() const;
    bool slowerChatEnabled() const;
    qreal slowerChatMessagesPerSecond() const;
    bool slowerChatMessageAnimations() const;
    bool streamDatabaseBadgeFeedVisible() const;
    bool titleSettingsButtonVisible() const;
    bool chatModeIndicatorVisible() const;
    PlatformIndicatorMode platformIndicatorMode() const;
    uint32_t twitchActivityMinimumBits() const;
    uint32_t kickActivityMinimumKicks() const;
    uint32_t tiktokActivityMinimumDiamonds() const;
    bool viewerCountEnabled() const;
    std::optional<bool> viewerCountEnabledOverride() const;
    bool filterActivity() const;
    bool filterActivityExplicit() const;

    IndirectChannel getIndirectChannel();
    ChannelPtr getChannel() const;
    void setChannel(IndirectChannel newChannel);

    void setFilters(const QList<QUuid> ids);
    QList<QUuid> getFilters() const;

    void setModerationMode(bool value);
    bool getModerationMode() const;
    void setInputEnabled(bool enabled);
    bool hasHiddenPinnedMessage() const;
    void unhidePinnedMessages();
    void setActivityMessageScale(qreal value);
    void setCollapseGiftedSubscriptions(bool value);
    void setActivityTimeDisplayMode(ActivityTimeDisplayMode value);
    void setSlowerChatEnabled(bool value);
    void setSlowerChatMessagesPerSecond(qreal value);
    void setSlowerChatMessageAnimations(bool value);
    void setStreamDatabaseBadgeFeedVisible(bool value);
    void setTitleSettingsButtonVisible(bool value);
    void setChatModeIndicatorVisible(bool value);
    void setPlatformIndicatorMode(PlatformIndicatorMode value);
    void setTwitchActivityMinimumBits(uint32_t value);
    void setKickActivityMinimumKicks(uint32_t value);
    void setTikTokActivityMinimumDiamonds(uint32_t value);
    void setViewerCountEnabled(bool value);
    void setViewerCountEnabledOverride(std::optional<bool> value);
    void setFilterActivity(bool value, bool explicitPreference = false);

    std::optional<bool> checkSpellingOverride() const;
    void setCheckSpellingOverride(std::optional<bool> override);

    void insertTextToInput(const QString &text);

    void showChangeChannelPopup(const char *dialogTitle, bool empty,
                                std::function<void(bool)> callback);
    void updateGifEmotes();
    void updateLastReadMessage();
    void updateHeaderIcons();
    void setIsTopRightSplit(bool value);

    void drag();

    bool isInContainer() const;

    void setContainer(SplitContainer *container);

    void setInputReply(const MessagePtr &reply);
    void showSettingsDialog();

    // This is called on window focus lost
    void unpause();

    OverlayWindow *overlayWindow();

    static pajlada::Signals::Signal<Qt::KeyboardModifiers>
        modifierStatusChanged;
    static Qt::KeyboardModifiers modifierStatus;

    enum class Action {
        RefreshTab,
        ResetMouseStatus,
        AppendNewSplit,
        Delete,

        SelectSplitLeft,
        SelectSplitRight,
        SelectSplitAbove,
        SelectSplitBelow,
    };

    pajlada::Signals::Signal<Action> actionRequested;
    pajlada::Signals::Signal<ChannelPtr> openSplitRequested;

    pajlada::Signals::Signal<SplitDirection, Split *> insertSplitRequested;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void enterEvent(QEnterEvent * /*event*/) override;
    void leaveEvent(QEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void channelNameUpdated(const QString &newChannelName);
    void handleModifiers(Qt::KeyboardModifiers modifiers);
    void updateYouTubeDiscoveryForeground(bool foreground);
    void refreshYouTubeDiscoveryForeground();
    void updateInputVisibility();
    void updateInputPlaceholder();
    void addShortcuts() override;

    /**
     * @brief Opens a Twitch channel's stream in your default browser's player (opens a formatted link)
     */
    void openChannelInBrowserPlayer(ChannelPtr channel);
    /**
     * @brief Opens a Twitch channel's stream in streamlink (if the stream's live, and streamlink's installed)
     */
    void openChannelInStreamlink(const QString channelName);
    /**
     * @brief Opens a Twitch channel's stream in your custom player (if the stream's live, and the custom player protocol's set)
     */
    void openChannelInCustomPlayer(QString channelName);
    /**
     * @brief Opens a Twitch channel's chat in a new tab
     */
    void joinChannelInNewTab(const ChannelPtr &channel);

    /**
     * @brief Refresh moderation mode layouts/buttons
     *
     * Should be called after after the moderation mode is changed or
     * moderation actions have been changed
     **/
    void refreshModerationMode();

    IndirectChannel channel_;

    bool moderationMode_{};
    bool isTopRightSplit_{};
    bool inputEnabled_{true};
    bool filterActivity_{false};
    bool filterActivityExplicit_{false};
    qreal activityMessageScale_{0.9};
    bool collapseGiftedSubscriptions_{true};
    ActivityTimeDisplayMode activityTimeDisplayMode_;
    bool slowerChatEnabled_{false};
    qreal slowerChatMessagesPerSecond_{5.0};
    bool slowerChatMessageAnimations_{true};
    bool streamDatabaseBadgeFeedVisible_{true};
    bool titleSettingsButtonVisible_{true};
    bool chatModeIndicatorVisible_{true};
    uint32_t twitchActivityMinimumBits_{100};
    uint32_t kickActivityMinimumKicks_{100};
    uint32_t tiktokActivityMinimumDiamonds_{0};
    std::optional<bool> viewerCountEnabledOverride_;
    PlatformIndicatorMode platformIndicatorMode_;

    bool isMouseOver_{};
    bool isDragging_{};

    QVBoxLayout *const vbox_;
    SplitHeader *const header_;
    StreamDatabaseBadgeBar *streamDatabaseBadgeBar_{};
    TwitchPollsAndPredictionsBar *const twitchPollsAndPredictionsBar_;
    PinnedMessageBar *const pinnedMessageBar_;
    ChannelView *const view_;
    SplitInput *const input_;
    SplitOverlay *const overlay_;

    QPointer<OverlayWindow> overlayWindow_;

    QPointer<SelectChannelDialog> selectChannelDialog_;
    QPointer<SplitSettingsDialog> splitSettingsDialog_;

    pajlada::Signals::Connection channelIDChangedConnection_;
    pajlada::Signals::Connection usermodeChangedConnection_;
    pajlada::Signals::Connection roomModeChangedConnection_;

    pajlada::Signals::Connection indirectChannelChangedConnection_;

    // This signal-holder is cleared whenever this split changes the underlying channel
    pajlada::Signals::SignalHolder channelSignalHolder_;

    pajlada::Signals::SignalHolder signalHolder_;
    std::vector<boost::signals2::scoped_connection> bSignals_;

public Q_SLOTS:
    void addSibling();
    void deleteFromContainer();
    void changeChannel();
    void openAlertsPane();
    void explainMoving();
    void explainSplitting();
    void popup();
    void showOverlayWindow();
    void clear();
    void openInBrowser();
    void openModViewInBrowser();
    void openWhispersInBrowser();
    void openBrowserPlayer();
    void openInStreamlink();
    void openWithCustomScheme();
    void setFiltersDialog();
    void showSearch(bool singleChannel);
    void openChatterList();
    void openSubPage();
    void reconnect();
};

}  // namespace chatterino

QDebug operator<<(QDebug dbg, const chatterino::Split &split);
QDebug operator<<(QDebug dbg, const chatterino::Split *split);
