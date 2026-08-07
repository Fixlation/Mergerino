// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "common/Channel.hpp"
#include "widgets/BaseWidget.hpp"

#include <pajlada/signals/signalholder.hpp>

#include <boost/signals2/connection.hpp>
#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <memory>
#include <optional>
#include <vector>

class QPaintEvent;
class QPainter;
class QMouseEvent;

namespace chatterino {

struct HelixPoll;
struct HelixPrediction;
struct KickPoll;
struct KickPrediction;
class KickChannel;
class TwitchChannel;

class TwitchPollsAndPredictionsBar final : public BaseWidget
{
public:
    explicit TwitchPollsAndPredictionsBar(QWidget *parent = nullptr);

    static void rememberLocalPoll(QString broadcasterID, QString title,
                                  QStringList choices, int durationSeconds);
    static void rememberLocalPrediction(QString broadcasterID, QString title,
                                        QStringList outcomes,
                                        int durationSeconds,
                                        QString predictionID = {},
                                        QJsonObject predictionObject = {});
    static void rememberLocalPrediction(QString broadcasterID,
                                        const HelixPrediction &prediction);
    static void clearLocalPrediction(const QString &broadcasterID);
    [[nodiscard]] static std::optional<QJsonObject> localPredictionJson(
        const QString &broadcasterID);

    void setChannel(const ChannelPtr &channel);
    void refreshNow();
    void markTwitchPollEnded(const QString &broadcasterID,
                             const QString &pollID);
    [[nodiscard]] bool hasActiveTwitchPoll() const;
    [[nodiscard]] bool hasActiveKickPoll() const;
    [[nodiscard]] bool hasOpenTwitchPrediction() const;
    [[nodiscard]] bool hasOpenKickPrediction() const;
    [[nodiscard]] QString predictionButtonTooltip(bool canManage) const;
    [[nodiscard]] QString pollButtonTooltip(bool canManage) const;

    pajlada::Signals::NoArgSignal pollClicked;
    pajlada::Signals::NoArgSignal kickPollClicked;
    pajlada::Signals::NoArgSignal predictionClicked;
    pajlada::Signals::NoArgSignal kickPredictionClicked;
    pajlada::Signals::NoArgSignal predictionStateChanged;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void scaleChangedEvent(float scale) override;
    void themeChangedEvent() override;

private:
    enum class ItemKind {
        Poll,
        Prediction,
    };
    enum class ItemPlatform {
        Twitch,
        Kick,
    };

    struct Choice {
        QString title;
        int weight = 0;
        QString detail;
        int points = 0;
        int users = 0;
        bool showPredictionMetrics = false;
    };

    struct Item {
        ItemKind kind = ItemKind::Poll;
        ItemPlatform platform = ItemPlatform::Twitch;
        QString title;
        QString status;
        std::vector<Choice> choices;
    };

    void clearItems();
    void scheduleRefresh(int delayMs);
    void refresh();
    void finishRequest(int generation);
    void updateItems();
    void updateKickPollItem();
    void updateKickPredictionItem();
    void updateFixedHeight();

    [[nodiscard]] static std::optional<Item> makePollItem(
        const HelixPoll &poll);
    [[nodiscard]] static std::optional<Item> makePredictionItem(
        const HelixPrediction &prediction);
    [[nodiscard]] static std::optional<Item> makeKickPollItem(
        const KickPoll &poll);
    [[nodiscard]] static std::optional<Item> makeKickPredictionItem(
        const KickPrediction &prediction);
    [[nodiscard]] static std::optional<Item> makeLocalPollItem(
        const QString &broadcasterID);
    [[nodiscard]] static std::optional<Item> makeLocalPredictionItem(
        const QString &broadcasterID);
    [[nodiscard]] int barHeight() const;
    [[nodiscard]] int itemHeight(const Item &item) const;
    void drawItem(QPainter &painter, const Item &item, QRect rect) const;

    std::weak_ptr<TwitchChannel> twitchChannel_;
    std::weak_ptr<KickChannel> kickChannel_;
    pajlada::Signals::SignalHolder channelSignalHolder_;
    pajlada::Signals::SignalHolder moderationAuthSignalHolder_;
    QTimer refreshTimer_;
    std::vector<Item> items_;
    std::optional<Item> pendingPoll_;
    std::optional<Item> pendingPrediction_;
    QString suppressedTwitchPollID_;
    int pendingRequests_ = 0;
    int requestGeneration_ = 0;
    std::vector<boost::signals2::scoped_connection> bSignals_;
};

}  // namespace chatterino
