// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "messages/Message.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "widgets/BaseWidget.hpp"

#include <boost/signals2/connection.hpp>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QPaintEvent>
#include <QPointer>
#include <QPropertyAnimation>
#include <QSet>
#include <QTextEdit>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <optional>
#include <vector>

class QNetworkReply;
class QCompleter;
class QDialog;
class QPushButton;

namespace chatterino {

class Split;
class EmotePopup;
class GiveawayPopup;
class InputCompletionPopup;
class InputHighlighter;
class MessageView;
class ChatIdentityPopupHost;
class KickChatIdentityPopup;
class StreamDatabaseBadgePickerPopup;
class KickChannel;
class TwitchChannel;
class LabelButton;
class ResizingTextEdit;
class ChannelView;
class EmoteBar;
class PlatformSwitchButton;
class SvgButton;
class SpellCheckHighlighter;
enum class CompletionKind;

class SplitInput : public BaseWidget
{
    Q_OBJECT

public:
    struct SendPlatformSelection {
        MessagePlatform selectedPlatform = MessagePlatform::AnyOrTwitch;
        bool allPlatforms = false;
        std::vector<MessagePlatform> customPlatforms;
        std::vector<MessagePlatform> enabledPlatforms;
    };

    SplitInput(Split *_chatWidget, bool enableInlineReplying = true);
    SplitInput(QWidget *parent, Split *_chatWidget, ChannelView *_channelView,
               bool enableInlineReplying = true);

    bool hasSelection() const;
    void clearSelection() const;

    bool isEditFirstWord() const;
    QString getInputText() const;
    void insertText(const QString &text);

    void setReply(MessagePtr target);
    void setPlaceholderText(const QString &text);
    void updatePlatformSelector(bool animate = false);
    void applyActiveAccountProviderDefault();
    SendPlatformSelection sendPlatformSelection() const;
    void restoreSendPlatformSelection(const SendPlatformSelection &selection);
    std::optional<MessagePlatform> selectedSendPlatform() const;
    QString selectedSendPlatformDisplayName() const;
    QString selectedSendAccountName() const;

    /**
     * @brief Hide the widget
     *
     * This is a no-op if the SplitInput is already hidden
     **/
    void hide();

    /**
     * @brief Show the widget
     *
     * This is a no-op if the SplitInput is already shown
     **/
    void show();

    /**
     * @brief Returns the hidden or shown state of the SplitInput
     *
     * Hidden in this context means "has 0 height", meaning it won't be visible
     * but Qt still treats the widget as "technically visible" so we receive events
     * as if the widget is visible
     **/
    bool isHidden() const;

    /**
     * @brief Sets the text of this input
     *
     * This method should only be used in tests
     */
    void setInputText(const QString &newInputText);

    /**
     * @brief Updates a platform's timeout or slow-mode countdown
     *
     * The visible status follows the currently selected merged-chat targets.
     */
    void setSendWaitStatus(MessagePlatform platform, int secondsRemaining);
    void clearSendWaitStatuses();

    void triggerSelfMessageReceived();
    void openGiveawayPopup();

    std::optional<bool> checkSpellingOverride() const;
    void setCheckSpellingOverride(std::optional<bool> override);

    pajlada::Signals::Signal<const QString &> textChanged;
    pajlada::Signals::NoArgSignal selectionChanged;

protected:
    void scaleChangedEvent(float scale_) override;
    void themeChangedEvent() override;

    void paintEvent(QPaintEvent * /*event*/) override;
    void resizeEvent(QResizeEvent * /*event*/) override;

    void mousePressEvent(QMouseEvent *event) override;

    virtual void giveFocus(Qt::FocusReason reason);

    QString handleSendMessage(const std::vector<QString> &arguments);
    void postMessageSend(const QString &message,
                         const std::vector<QString> &arguments);

    /// Clears the input box and any active reply target
    void clearInput();

    void addShortcuts() override;
    void initLayout();
    QCompleter *createCompleter(ChannelPtr channel);
    bool eventFilter(QObject *obj, QEvent *event) override;
    void installTextEditEvents();
    void onCursorPositionChanged();
    void onTextChanged();
    void updateBadgeButton();
    int badgeButtonTargetWidth() const;
    void setBadgeButtonShown(bool shown, bool animate);
    int sendWaitLockTargetWidth() const;
    void updateSendWaitLockIcon();
    void positionSendWaitLockIcon();
    void setSendWaitLockShown(bool shown, bool animate);
    void updateEmoteButton();
    void updateCompletionPopup();
    void updatePlatformButtonLayout(int platformCount = 1);
    void showCompletionPopup(const QString &text, CompletionKind kind);
    void hideCompletionPopup();
    void insertCompletionText(const QString &input_) const;
    void updatePollPredictionButtons();
    void refreshResubNotification();
    void updateResubCallout();
    void openShareResubDialog();
    void openPollDialog();
    void openTwitchPollDialog();
    void openKickPollDialog();
    void openPredictionDialog();
    void openTwitchPredictionDialog();
    void openKickPredictionDialog();
    void openEmotePopup();
    void updateEmotePopupChannel();
    void activateEmoteBarToken(const QString &token,
                               Qt::KeyboardModifiers modifiers);
    std::vector<ChannelPtr> emoteBarSendChannels() const;
    void openBadgePickerPopup();
    void updateBadgePickerContext();
    void ensureChatIdentityPopupHost();
    void ensureTwitchIdentityPopup();
    void ensureKickIdentityPopup();
    void switchChatIdentityPlatform(MessagePlatform platform);
    void updateSeventvCosmeticsForInput();
    void resetBadgeIdentityButtonFetch(bool clearBadges);
    void requestBadgeIdentityForCurrentTwitchChannel(
        const std::shared_ptr<TwitchChannel> &twitch);
    void requestKickIdentityForCurrentChannel(
        const std::shared_ptr<KickChannel> &kick);
    void clearReplyTarget();

    void updateCancelReplyButton();

    // scaledMaxHeight returns the height in pixels that this widget can grow to
    // This does not take hidden into account, so callers must take hidden into account themselves
    int scaledMaxHeight() const;

    // Returns true if the channel this input is connected to is a Twitch channel,
    // the user's setting is set to Prevent, and the given text goes beyond the Twitch message length limit
    bool shouldPreventInput(const QString &text) const;

    int marginForTheme() const;

    void applyOuterMargin();

    int replyMessageWidth() const;

    std::vector<MessagePlatform> availableSendPlatforms() const;
    std::vector<MessagePlatform> cycleSendPlatforms(
        const std::vector<MessagePlatform> &availablePlatforms) const;
    std::optional<MessagePlatform> replySendPlatform() const;
    std::vector<MessagePlatform> storedSelectedSendPlatforms(
        const std::vector<MessagePlatform> &availablePlatforms) const;
    std::vector<MessagePlatform> selectedSendPlatforms() const;
    ChannelPtr channelForSendPlatform(MessagePlatform platform) const;
    std::vector<MessagePlatform> giveawayPlatforms() const;
    bool canSendToPlatform(MessagePlatform platform) const;
    void normalizeSelectedSendPlatforms(
        const std::vector<MessagePlatform> &availablePlatforms);
    void setSelectedSendPlatforms(std::vector<MessagePlatform> platforms);
    void showPlatformSelectionMenu();
    void selectSendPlatform(MessagePlatform platform);
    void selectAllSendPlatforms();
    bool tryCycleSendPlatform();
    void cycleSendPlatform();

    Split *const split_;
    bool hasSendWait(MessagePlatform platform) const;
    void refreshSendWaitStatus();

    ChannelView *const channelView_;
    QPointer<EmotePopup> emotePopup_;
    QPointer<GiveawayPopup> giveawayPopup_;
    QPointer<ChatIdentityPopupHost> chatIdentityPopupHost_;
    QPointer<StreamDatabaseBadgePickerPopup> badgePickerPopup_;
    QPointer<KickChatIdentityPopup> kickIdentityPopup_;
    QPointer<InputCompletionPopup> inputCompletionPopup_;
    QNetworkAccessManager badgeIdentityNetwork_;
    QHash<QString, QNetworkReply *> pendingBadgeIdentityRequests_;
    QSet<QString> failedBadgeIdentityChannels_;
    int badgeIdentityRequestGeneration_ = 0;
    std::optional<KickChatIdentity> kickChatIdentity_;
    QString kickChatIdentityKey_;
    QString kickIdentityFailedKey_;
    bool kickIdentityRequestPending_ = false;
    int kickIdentityRequestGeneration_ = 0;
    MessagePlatform chatIdentityPlatform_ = MessagePlatform::AnyOrTwitch;

    QPointer<QDialog> resubDialog_;
    int resubRequestGeneration_ = 0;
    QString resubChannelLogin_;
    std::optional<TwitchResubNotification> resubNotification_;
    boost::signals2::scoped_connection resubAccountConnection_;

    struct {
        // vbox for all components
        QVBoxLayout *vbox;

        // Twitch subscription anniversary callout
        QWidget *resubCalloutWrapper = nullptr;
        QLabel *resubCalloutLabel = nullptr;
        QPushButton *resubCalloutButton = nullptr;

        // Recent and most-used emotes
        EmoteBar *emoteBar = nullptr;

        // reply widgets
        QWidget *replyWrapper;
        QVBoxLayout *replyVbox;
        QHBoxLayout *replyHbox;
        MessageView *replyMessage;
        QLabel *replyLabel;
        SvgButton *cancelReplyButton;

        // input widgets
        QWidget *inputWrapper;
        QHBoxLayout *inputHbox;
        QWidget *badgeButtonWrapper = nullptr;
        QToolButton *badgeButton = nullptr;
        QWidget *sendWaitLockWrapper = nullptr;
        QLabel *sendWaitLockIcon = nullptr;
        ResizingTextEdit *textEdit;
        QLabel *textEditLength;
        LabelButton *sendButton;
        QVBoxLayout *rightVbox;
        QHBoxLayout *buttonHbox;
        PlatformSwitchButton *platformButton;
        SvgButton *predictionButton = nullptr;
        SvgButton *pollButton = nullptr;
        SvgButton *seventvButton = nullptr;
        SvgButton *giveawayButton = nullptr;
        SvgButton *emoteButton;
    } ui_;

    MessagePtr replyTarget_ = nullptr;
    bool enableInlineReplying_;

    pajlada::Signals::SignalHolder managedConnections_;
    QStringList prevMsg_;
    QString currMsg_;
    int prevIndex_ = 0;
    MessagePlatform selectedSendPlatform_ = MessagePlatform::AnyOrTwitch;
    bool selectedSendAllPlatforms_ = false;
    std::vector<MessagePlatform> customSelectedSendPlatforms_;
    std::vector<MessagePlatform> enabledSendPlatforms_;
    QHash<MessagePlatform, int> sendWaitStatuses_;
    QString placeholderText_;

    // Hidden denotes whether this split input should be hidden or not
    // This is used instead of the regular QWidget::hide/show because
    // focus events don't work as expected, so instead we use this bool and
    // set the height of the split input to 0 if we're supposed to be hidden instead
    bool hidden{false};

    /// Updates the text edit palette using the current theme
    /// and current "backgroundColor" property
    void updateTextEditPalette();

    // the background color defines the current background color of this split input
    // instead of reading straight from the theme, we store a property here
    // to be used by a property to be able to pulse a highlight color on demand
    Q_PROPERTY(
        QColor backgroundColor READ backgroundColor WRITE setBackgroundColor);

    QColor backgroundColor_{"#000000"};
    QColor backgroundColor() const;
    void setBackgroundColor(QColor newColor);

    QPropertyAnimation backgroundColorAnimation;
    QVariantAnimation badgeButtonVisibilityAnimation_;
    bool badgeButtonVisibilityInitialized_ = false;
    bool badgeButtonShown_ = true;
    QVariantAnimation sendWaitLockVisibilityAnimation_;
    bool sendWaitLockShown_ = false;

    std::optional<bool> checkSpellingOverride_;
    bool shouldCheckSpelling() const;
    void checkSpellingChanged();

    InputHighlighter *inputHighlighter = nullptr;

    void updateFonts();

    pajlada::Signals::NoArgSignal sendPlatformChanged;

private Q_SLOTS:
    void editTextChanged();

    friend class Split;
    friend class ReplyThreadPopup;
};

}  // namespace chatterino
