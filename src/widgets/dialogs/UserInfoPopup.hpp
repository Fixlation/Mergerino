// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/Paths.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/DraggablePopup.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <pajlada/signals/signal.hpp>
#include <QPixmap>
#include <QPointer>

#include <chrono>
#include <cstdint>

class QCheckBox;
class QCloseEvent;
class QMovie;

namespace chatterino {

class Channel;
using ChannelPtr = std::shared_ptr<Channel>;
class Label;
class MarkdownLabel;
class EditUserNotesDialog;
class TwitchModCommentsDialog;
class ChannelView;
class Split;
struct HelixUser;
class LabelButton;
class PixmapButton;
class LiveIndicator;
class YouTubeLiveChat;
enum class MessagePlatform : std::uint8_t;

class UserInfoPopup final : public DraggablePopup
{
    Q_OBJECT

public:
    /**
     * @param closeAutomatically Decides whether the popup should close when it loses focus
     * @param split Will be used as the popup's parent. Must not be null
     */
    UserInfoPopup(bool closeAutomatically, Split *split);
    ~UserInfoPopup() override
    {
        this->prepareForClose();
    }

    void setData(const QString &name, const ChannelPtr &channel);
    void setData(const QString &name, const ChannelPtr &contextChannel,
                 const ChannelPtr &openingChannel);
    void setData(const QString &name, const ChannelPtr &contextChannel,
                 const ChannelPtr &openingChannel, MessagePlatform platform,
                 const QString &platformUserID = {});

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    bool event(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void windowDeactivationEvent() override;

private:
    void installEvents();
    void updateUserData();
    void updateGenericPlatformUserData(const QString &platformUserID);
    void updateLatestMessages();
    void updateNotes();
    void updateLogUserButton();
    void updateTargetRoleStateFromMessages();
    void refreshChannelModerationPermissions();
    void openModComments();
    void prepareForClose();
    bool canModerateTargetUser() const;
    bool isCurrentPlatformUser() const;
    void sendModerationCommand(const QString &command);
    void sendModerationAction(const QString &action, int durationSeconds = 0);
    void sendYouTubeModerationAction(const QString &action,
                                     int durationSeconds);

    void loadAvatar(const QString &userID, const QString &pictureURL,
                    bool isKick, bool loadSevenTV = true);

    void loadSevenTVAvatar(const QString &userID, bool isKick);
    void setSevenTVAvatar(const QString &filename, const QByteArray &format);

    void saveCacheAvatar(const QByteArray &avatar,
                         const QString &filename) const;

    void updateAvatarUrl();

    void updateKickUserData();
    void onKickProfilePictureClick(Qt::MouseButton button);

    QStringView platformName() const;
    YouTubeLiveChat *youtubeLiveChat() const;

    void appendCommonProfileActions(QMenu *menu);

    bool isMod_{};
    bool isBroadcaster_{};

    Split *split_;

    QString userName_;
    QString userId_;
    QString platformUserID_;
    QString avatarUrl_;
    QString helixAvatarUrl_;
    QString seventvAvatarUrl_;
    QString seventvUserID_;
    QString genericProfileUrl_;

    QString kickUserSlug_;

    // The channel the popup was opened from (e.g. /mentions or #forsen). Can be a special channel.
    ChannelPtr channel_;

    // The channel the messages are rendered from (e.g. #forsen). Can be a special channel, but will try to not be where possible.
    ChannelPtr underlyingChannel_;

    pajlada::Signals::NoArgSignal userStateChanged_;

    std::unique_ptr<pajlada::Signals::ScopedConnection> refreshConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        userDataUpdatedConnection_;
    std::unique_ptr<pajlada::Signals::ScopedConnection>
        youtubeModerationConnection_;

    TwitchChannelModerationPermissions channelModerationPermissions_;
    int channelModerationPermissionsGeneration_ = 0;
    bool targetIsModerator_ = false;
    bool targetIsVip_ = false;
    bool targetIsBroadcaster_ = false;

    // If we should close the dialog automatically if the user clicks out
    // Set based on the "Automatically close usercard when it loses focus" setting
    // Pinned status is tracked in DraggablePopup::isPinned_.
    const bool closeAutomatically_;

    class TimeoutWidget;
    struct {
        PixmapButton *avatarButton = nullptr;
        PixmapButton *localizedNameCopyButton = nullptr;

        Label *nameLabel = nullptr;
        Label *localizedNameLabel = nullptr;
        Label *followerCountLabel = nullptr;
        Label *createdDateLabel = nullptr;
        Label *userIDLabel = nullptr;
        Label *followageLabel = nullptr;
        Label *subageLabel = nullptr;

        LiveIndicator *liveIndicator = nullptr;

        QCheckBox *block = nullptr;
        QCheckBox *ignoreHighlights = nullptr;
        MarkdownLabel *notesPreview = nullptr;
        LabelButton *notesAdd = nullptr;
        LabelButton *modComments = nullptr;
        QCheckBox *logUser = nullptr;

        Label *noMessagesLabel = nullptr;
        ChannelView *latestMessages = nullptr;

        LabelButton *usercardLabel = nullptr;
        LabelButton *switchAvatars = nullptr;
        LabelButton *moderatorToggle = nullptr;
        LabelButton *vipToggle = nullptr;
        // Preserve the established UI struct layout for the project's
        // incremental changed-file build workflow.
        QWidget *roleToggleLayoutPadding1 = nullptr;
        QWidget *roleToggleLayoutPadding2 = nullptr;

        TimeoutWidget *timeoutWidget = nullptr;
    } ui_;

    QMovie *seventvAvatar_ = nullptr;
    bool isTwitchAvatarShown_ = true;
    QPixmap avatarPixmap_;
    QPointer<EditUserNotesDialog> editUserNotesDialog_;
    QPointer<TwitchModCommentsDialog> twitchModCommentsDialog_;

    bool isKick_ = false;
    bool isGenericPlatform_ = false;
    bool preparedForClose_ = false;
    MessagePlatform platform_;
    uint64_t kickUserID_ = 0;

    class TimeoutWidget : public BaseWidget
    {
    public:
        enum Action { Ban, Unban, Timeout };

        TimeoutWidget();

        pajlada::Signals::Signal<std::pair<Action, int>> buttonClicked;

        void setMinTimeout(int minSecs);

    protected:
        void paintEvent(QPaintEvent *event) override;

    private:
        std::vector<std::pair<QWidget *, int>> timeoutButtons;
    };
};

}  // namespace chatterino
