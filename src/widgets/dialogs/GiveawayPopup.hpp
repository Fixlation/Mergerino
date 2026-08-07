// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "ForwardDecl.hpp"
#include "messages/Message.hpp"
#include "widgets/BasePopup.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <QHash>
#include <QSet>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

class QCloseEvent;
class QCheckBox;
class QGraphicsOpacityEffect;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QSlider;
class QSpinBox;
class QStackedWidget;
class QTextBrowser;
class QToolButton;
class QVariantAnimation;
class QVBoxLayout;
class QWidget;

namespace chatterino {

class GiveawayPopup final : public BasePopup
{
public:
    explicit GiveawayPopup(QWidget *parent = nullptr);

    void setContext(ChannelPtr channel,
                    std::vector<MessagePlatform> availablePlatforms);

protected:
    void themeChangedEvent() override;
    void showEvent(QShowEvent *event) override;

private:
    enum class Mode : uint8_t {
        Keyword,
        Number,
    };

    enum class NumberVisibility : uint8_t {
        Visible,
        Hidden,
    };

    struct CapturedMessage {
        QString text;
        QString timestamp;
        QString author;
        MessagePlatform platform = MessagePlatform::AnyOrTwitch;
    };

    struct Participant {
        QString key;
        QString author;
        MessagePlatform platform = MessagePlatform::AnyOrTwitch;
        int entryCount = 1;
        QVector<CapturedMessage> messages;
    };

    void buildUi();
    void applyTheme();
    void setMode(Mode mode);
    void updateModeControls();
    void updatePlatformControls();
    void updateRoundControls();
    void resetParticipants();
    void removeParticipant(const QString &participantKey);
    void updateEntrantList();
    void startOrStopRound();
    void startRound();
    void stopRound();
    void rollKeywordWinner();
    void randomizeTarget();
    void toggleNumberVisibility();
    void applyNumberVisibility(NumberVisibility visibility);
    void handleMessage(const MessagePtr &message);
    bool acceptsPlatform(MessagePlatform platform) const;
    bool isEligibleMessage(const Message &message) const;
    bool messageIsSubscriber(const Message &message) const;
    QString participantKey(const Message &message) const;
    Participant participantFromMessage(const Message &message) const;
    CapturedMessage capturedMessage(const Message &message) const;
    void appendParticipantMessage(Participant &participant,
                                  const Message &message);
    void chooseWinner(const QString &participantKey,
                      const QString &reason = {});
    void clearWinner();
    void renderWinnerMessages();
    void appendWinnerMessage(const CapturedMessage &message);
    void setStatus(const QString &text, bool error = false);
    QString selectedPlatformsText() const;
    ChannelPtr channel_;
    std::vector<MessagePlatform> availablePlatforms_;
    pajlada::Signals::SignalHolder channelConnections_;

    Mode mode_ = Mode::Keyword;
    NumberVisibility numberVisibility_ = NumberVisibility::Visible;
    std::optional<NumberVisibility> pendingNumberVisibility_;
    bool numberVisibilitySwapped_ = false;
    bool running_ = false;
    QString activeKeyword_;
    int activeTarget_ = 0;
    int activeSubscriberMultiplier_ = 1;
    QHash<QString, Participant> participants_;
    QVector<QString> participantOrder_;
    QSet<QString> previousWinnerKeys_;
    QSet<QString> removedParticipantKeys_;
    QString winnerKey_;

    QWidget *root_ = nullptr;
    QHash<MessagePlatform, QToolButton *> platformButtons_;
    QPushButton *keywordModeButton_ = nullptr;
    QPushButton *numberModeButton_ = nullptr;
    QStackedWidget *modeStack_ = nullptr;
    QLineEdit *keywordInput_ = nullptr;
    QSlider *subscriberLuck_ = nullptr;
    QLabel *subscriberLuckValue_ = nullptr;
    QCheckBox *excludePreviousWinners_ = nullptr;
    QSpinBox *rangeMinimum_ = nullptr;
    QSpinBox *rangeMaximum_ = nullptr;
    QLineEdit *targetInput_ = nullptr;
    QPushButton *randomizeButton_ = nullptr;
    QPushButton *numberVisibilityButton_ = nullptr;
    QGraphicsOpacityEffect *numberVisibilityOpacity_ = nullptr;
    QVariantAnimation *numberVisibilityAnimation_ = nullptr;
    QLabel *statusLabel_ = nullptr;
    QLabel *entrantCountLabel_ = nullptr;
    QScrollArea *entrantScrollArea_ = nullptr;
    QWidget *entrantListWidget_ = nullptr;
    QVBoxLayout *entrantListLayout_ = nullptr;
    QLabel *winnerLabel_ = nullptr;
    QTextBrowser *winnerMessages_ = nullptr;
    QPushButton *startButton_ = nullptr;
    QPushButton *rollButton_ = nullptr;
};

}  // namespace chatterino
