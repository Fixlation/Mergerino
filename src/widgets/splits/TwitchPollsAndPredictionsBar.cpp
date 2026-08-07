// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "util/Helpers.hpp"

#include <QFontMetrics>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSizePolicy>

#include <algorithm>
#include <array>
#include <cmath>

namespace chatterino {
namespace {

constexpr int REFRESH_WITH_ACTIVE_MS = 15 * 1000;
constexpr int REFRESH_WHEN_IDLE_MS = 60 * 1000;
constexpr int REFRESH_WAIT_FOR_ROOM_ID_MS = 2 * 1000;
constexpr qint64 LOCAL_POLL_EXTRA_RETENTION_MS = 30 * 1000;
constexpr qint64 LOCAL_PREDICTION_RETENTION_MS = 24LL * 60 * 60 * 1000;
constexpr std::array<int, 3> REFRESH_AFTER_LOCAL_CHANGE_MS = {
    2 * 1000,
    5 * 1000,
    10 * 1000,
};

struct LocalPollState {
    QString title;
    QStringList choices;
    QDateTime createdAt;
    int durationSeconds = 0;
};

struct LocalPredictionOutcomeState {
    QString id;
    QString title;
    int users = 0;
    int channelPoints = 0;
};

struct LocalPredictionState {
    QString id;
    QString title;
    std::vector<LocalPredictionOutcomeState> outcomes;
    QDateTime createdAt;
    QDateTime lockedAt;
    QDateTime endedAt;
    QString winningOutcomeID;
    QString status = QStringLiteral("ACTIVE");
    int durationSeconds = 0;
};

QHash<QString, LocalPollState> localPolls;
QHash<QString, LocalPredictionState> localPredictions;

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

QString statusTitle(QString status)
{
    if (status.isEmpty())
    {
        return {};
    }

    status = status.toLower();
    status[0] = status[0].toUpper();
    return status;
}

QString voteText(int votes)
{
    return votes == 1 ? QStringLiteral("1 vote")
                      : QStringLiteral("%1 votes").arg(localizeNumbers(votes));
}

void fillRoundedRect(QPainter &painter, const QRectF &rect,
                     const QColor &color, qreal radius)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(rect, radius, radius);
}

QColor mutedFromTheme(const Theme *theme, const QColor &text)
{
    QColor muted = theme->messages.textColors.system;
    if (!muted.isValid() || muted.alpha() == 0)
    {
        muted = text;
    }

    muted.setAlpha(theme->isLightTheme() ? 185 : 205);
    return muted;
}

QString percentText(int value, int total)
{
    if (total <= 0)
    {
        return QStringLiteral("--");
    }

    return QStringLiteral("%1%").arg(
        localizeNumbers(int(std::round(value * 100.0 / total))));
}

void drawChannelPointsIcon(QPainter &painter, const QRect &rect,
                           const QColor &fallback)
{
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(QRectF{1, 1, 22, 22});
    ring.addEllipse(QRectF{3, 3, 18, 18});

    QPainterPath innerArc;
    innerArc.moveTo(12, 5);
    innerArc.lineTo(12, 7);
    innerArc.arcTo(QRectF{7, 7, 10, 10}, 90, -90);
    innerArc.lineTo(19, 12);
    innerArc.arcTo(QRectF{5, 5, 14, 14}, 0, 90);
    innerArc.closeSubpath();

    painter.save();
    painter.translate(rect.x(), rect.y());
    painter.scale(rect.width() / 24.0, rect.height() / 24.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fallback);
    painter.drawPath(ring);
    painter.drawPath(innerArc);
    painter.restore();
}

void drawUsersIcon(QPainter &painter, const QRect &rect, const QColor &color)
{
    const auto lineWidth = std::max(2, int(std::round(rect.width() / 6.0)));
    painter.setPen(QPen(color, lineWidth, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);

    const qreal x = rect.x();
    const qreal y = rect.y();
    const qreal w = rect.width();
    const qreal h = rect.height();

    painter.drawEllipse(QRectF{x + w * 0.43, y + h * 0.14, w * 0.28,
                               h * 0.28});
    painter.drawEllipse(QRectF{x + w * 0.20, y + h * 0.23, w * 0.23,
                               h * 0.23});
    painter.drawArc(QRectF{x + w * 0.30, y + h * 0.50, w * 0.54, h * 0.42},
                    22 * 16, 136 * 16);
    painter.drawArc(QRectF{x + w * 0.08, y + h * 0.57, w * 0.44, h * 0.34},
                    28 * 16, 124 * 16);
}

enum class MetricIcon {
    ChannelPoints,
    Users,
};

int drawIconMetric(QPainter &painter, int right, int centerY,
                   const QString &text, MetricIcon icon,
                   const QColor &textColor, const QColor &iconColor,
                   const QFontMetrics &metrics, float scale)
{
    const int iconSize =
        icon == MetricIcon::Users ? std::max(13, int(std::round(14 * scale)))
                                  : std::max(11, int(std::round(12 * scale)));
    const int gap = int(std::round(3 * scale));
    const int textWidth = metrics.horizontalAdvance(text);
    const int textHeight = metrics.height();
    QRect textRect{right - textWidth, centerY - textHeight / 2, textWidth,
                   textHeight};
    QRect iconRect{textRect.left() - gap - iconSize, centerY - iconSize / 2,
                   iconSize, iconSize};
    if (icon == MetricIcon::Users)
    {
        iconRect.translate(0, int(std::round(2 * scale)));
    }

    if (icon == MetricIcon::ChannelPoints)
    {
        drawChannelPointsIcon(painter, iconRect, iconColor);
    }
    else
    {
        drawUsersIcon(painter, iconRect, iconColor);
    }

    painter.setPen(textColor);
    painter.drawText(textRect, Qt::AlignCenter, text);
    return iconRect.left();
}

int drawPredictionMetrics(QPainter &painter, const QRect &rect, int points,
                          int users, const QColor &textColor,
                          const QColor &iconColor, const QFontMetrics &metrics,
                          float scale)
{
    const int centerY = rect.center().y();
    const int groupGap = int(std::round(9 * scale));
    int left = drawIconMetric(painter, rect.right(), centerY,
                              localizeNumbers(users), MetricIcon::Users,
                              textColor, iconColor, metrics, scale);
    left = drawIconMetric(painter, left - groupGap, centerY,
                          localizeNumbers(points), MetricIcon::ChannelPoints,
                          textColor, iconColor, metrics, scale);
    return left;
}

QString pollDetailText(const QString &votes, int value, int total)
{
    if (total <= 0)
    {
        return votes;
    }

    return percentText(value, total) + QStringLiteral("  ") + votes;
}

std::shared_ptr<KickChannel> resolveKickChannel(const ChannelPtr &channel)
{
    if (!channel)
    {
        return nullptr;
    }

    if (auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        return kick;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<KickChannel>(merged->kickChannel());
    }

    return nullptr;
}

std::shared_ptr<TwitchChannel> resolveTwitchChannel(const ChannelPtr &channel)
{
    if (!channel)
    {
        return nullptr;
    }

    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        return twitch;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<TwitchChannel>(
            merged->twitchChannel());
    }

    return nullptr;
}

QString jsonStringValue(const QJsonObject &object, const QString &key,
                        const QString &fallback = {})
{
    const auto value = object.value(key).toString().trimmed();
    return value.isEmpty() ? fallback : value;
}

int jsonIntValue(const QJsonObject &object, const QString &key,
                 int fallback = 0)
{
    const auto value = object.value(key);
    return value.isDouble() ? value.toInt() : fallback;
}

QDateTime jsonDateTimeValue(const QJsonObject &object, const QString &key)
{
    const auto value =
        QDateTime::fromString(object.value(key).toString(), Qt::ISODate);
    return value.isValid() ? value.toUTC() : QDateTime{};
}

QString normalizedPredictionStatus(QString status)
{
    status = status.trimmed().toUpper();
    return status.isEmpty() ? QStringLiteral("ACTIVE") : status;
}

bool localPredictionExpired(const LocalPredictionState &state,
                            const QDateTime &now)
{
    if (!state.createdAt.isValid())
    {
        return true;
    }

    const auto elapsedMs = std::max<qint64>(0, state.createdAt.msecsTo(now));
    return elapsedMs > LOCAL_PREDICTION_RETENTION_MS;
}

QString computedLocalPredictionStatus(const LocalPredictionState &state,
                                      const QDateTime &now)
{
    const auto status = normalizedPredictionStatus(state.status);
    if (status != QStringLiteral("ACTIVE"))
    {
        return status;
    }

    const auto submissionsOpenMs =
        static_cast<qint64>(std::max(state.durationSeconds, 1)) * 1000;
    const auto elapsedMs = std::max<qint64>(0, state.createdAt.msecsTo(now));
    return elapsedMs <= submissionsOpenMs ? QStringLiteral("ACTIVE")
                                          : QStringLiteral("LOCKED");
}

QJsonArray localPredictionOutcomesJson(const LocalPredictionState &state)
{
    QJsonArray outcomes;
    for (const auto &outcome : state.outcomes)
    {
        QJsonObject object;
        object.insert(QStringLiteral("id"), outcome.id);
        object.insert(QStringLiteral("title"), outcome.title);
        object.insert(QStringLiteral("users"), outcome.users);
        object.insert(QStringLiteral("channel_points"), outcome.channelPoints);
        outcomes.append(object);
    }
    return outcomes;
}

QJsonObject localPredictionJsonObject(const LocalPredictionState &state,
                                      const QDateTime &now)
{
    const auto status = computedLocalPredictionStatus(state, now);
    auto lockedAt = state.lockedAt;
    if (status == QStringLiteral("LOCKED") && !lockedAt.isValid() &&
        state.createdAt.isValid())
    {
        lockedAt = state.createdAt.addSecs(std::max(state.durationSeconds, 1));
    }

    QJsonObject object;
    object.insert(QStringLiteral("id"), state.id);
    object.insert(QStringLiteral("title"), state.title);
    object.insert(QStringLiteral("winning_outcome_id"),
                  state.winningOutcomeID);
    object.insert(QStringLiteral("status"), status);
    object.insert(QStringLiteral("created_at"),
                  state.createdAt.isValid()
                      ? state.createdAt.toUTC().toString(Qt::ISODate)
                      : QString{});
    object.insert(QStringLiteral("locked_at"),
                  lockedAt.isValid()
                      ? lockedAt.toUTC().toString(Qt::ISODate)
                      : QString{});
    object.insert(QStringLiteral("ended_at"),
                  state.endedAt.isValid()
                      ? state.endedAt.toUTC().toString(Qt::ISODate)
                      : QString{});
    object.insert(QStringLiteral("prediction_window"),
                  std::max(state.durationSeconds, 1));
    object.insert(QStringLiteral("outcomes"),
                  localPredictionOutcomesJson(state));
    return object;
}

std::vector<LocalPredictionOutcomeState> localPredictionOutcomes(
    const QStringList &outcomeTitles, const QJsonObject &predictionObject)
{
    const auto rawOutcomes =
        predictionObject.value(QStringLiteral("outcomes")).toArray();
    const int count = std::max(static_cast<int>(outcomeTitles.size()),
                               static_cast<int>(rawOutcomes.size()));

    std::vector<LocalPredictionOutcomeState> outcomes;
    outcomes.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const auto rawOutcome =
            i < rawOutcomes.size() ? rawOutcomes.at(i).toObject()
                                   : QJsonObject{};
        auto title = jsonStringValue(rawOutcome, QStringLiteral("title"));
        if (title.isEmpty() && i < outcomeTitles.size())
        {
            title = outcomeTitles.at(i).trimmed();
        }
        if (title.isEmpty())
        {
            continue;
        }

        outcomes.push_back({
            jsonStringValue(rawOutcome, QStringLiteral("id")),
            title,
            jsonIntValue(rawOutcome, QStringLiteral("users")),
            jsonIntValue(rawOutcome, QStringLiteral("channel_points")),
        });
    }

    return outcomes;
}

}  // namespace

TwitchPollsAndPredictionsBar::TwitchPollsAndPredictionsBar(QWidget *parent)
    : BaseWidget(parent)
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    this->setFixedHeight(0);
    this->hide();

    this->refreshTimer_.setSingleShot(true);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this, [this] {
        this->refresh();
    });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->scheduleRefresh(0);
        }));
    this->moderationAuthSignalHolder_.managedConnect(
        TwitchModerationAuth::accountChanged(), [this] {
            this->refreshNow();
        });
}

void TwitchPollsAndPredictionsBar::rememberLocalPoll(QString broadcasterID,
                                                     QString title,
                                                     QStringList choices,
                                                     int durationSeconds)
{
    broadcasterID = broadcasterID.trimmed();
    title = title.trimmed();
    choices.removeAll(QString{});
    if (broadcasterID.isEmpty() || title.isEmpty() || choices.isEmpty())
    {
        return;
    }

    localPolls.insert(
        broadcasterID,
        {
            std::move(title),
            std::move(choices),
            QDateTime::currentDateTimeUtc(),
            std::max(durationSeconds, 1),
        });
}

void TwitchPollsAndPredictionsBar::rememberLocalPrediction(
    QString broadcasterID, QString title, QStringList outcomes,
    int durationSeconds, QString predictionID, QJsonObject predictionObject)
{
    broadcasterID = broadcasterID.trimmed();
    title = title.trimmed();
    outcomes.removeAll(QString{});
    if (broadcasterID.isEmpty() || title.isEmpty() || outcomes.isEmpty())
    {
        return;
    }

    const auto createdAt =
        jsonDateTimeValue(predictionObject, QStringLiteral("created_at"));
    const auto lockedAt =
        jsonDateTimeValue(predictionObject, QStringLiteral("locked_at"));
    const auto endedAt =
        jsonDateTimeValue(predictionObject, QStringLiteral("ended_at"));
    const auto predictionWindow =
        jsonIntValue(predictionObject, QStringLiteral("prediction_window"),
                     durationSeconds);

    localPredictions.insert(
        broadcasterID,
        {
            jsonStringValue(predictionObject, QStringLiteral("id"),
                            predictionID.trimmed()),
            std::move(title),
            localPredictionOutcomes(outcomes, predictionObject),
            createdAt.isValid() ? createdAt : QDateTime::currentDateTimeUtc(),
            lockedAt,
            endedAt,
            jsonStringValue(predictionObject,
                            QStringLiteral("winning_outcome_id")),
            normalizedPredictionStatus(jsonStringValue(
                predictionObject, QStringLiteral("status"))),
            std::max(predictionWindow, 1),
        });
}

void TwitchPollsAndPredictionsBar::rememberLocalPrediction(
    QString broadcasterID, const HelixPrediction &prediction)
{
    if (prediction.status != QStringLiteral("ACTIVE") &&
        prediction.status != QStringLiteral("LOCKED"))
    {
        TwitchPollsAndPredictionsBar::clearLocalPrediction(broadcasterID);
        return;
    }

    std::vector<LocalPredictionOutcomeState> outcomes;
    outcomes.reserve(prediction.outcomes.size());
    for (const auto &outcome : prediction.outcomes)
    {
        outcomes.push_back({
            outcome.id,
            outcome.title,
            outcome.users,
            outcome.channelPoints,
        });
    }

    broadcasterID = broadcasterID.trimmed();
    if (broadcasterID.isEmpty() || prediction.id.isEmpty() ||
        prediction.title.isEmpty() || outcomes.empty())
    {
        return;
    }

    localPredictions.insert(
        broadcasterID,
        {
            prediction.id,
            prediction.title,
            std::move(outcomes),
            prediction.createdAt.isValid()
                ? prediction.createdAt.toUTC()
                : QDateTime::currentDateTimeUtc(),
            prediction.lockedAt.isValid() ? prediction.lockedAt.toUTC()
                                          : QDateTime{},
            prediction.endedAt.isValid() ? prediction.endedAt.toUTC()
                                         : QDateTime{},
            prediction.winningOutcomeID,
            normalizedPredictionStatus(prediction.status),
            std::max(prediction.predictionWindow, 1),
        });
}

void TwitchPollsAndPredictionsBar::clearLocalPrediction(
    const QString &broadcasterID)
{
    localPredictions.remove(broadcasterID.trimmed());
}

std::optional<QJsonObject> TwitchPollsAndPredictionsBar::localPredictionJson(
    const QString &broadcasterID)
{
    auto it = localPredictions.find(broadcasterID.trimmed());
    if (it == localPredictions.end())
    {
        return std::nullopt;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    if (localPredictionExpired(*it, now))
    {
        localPredictions.erase(it);
        return std::nullopt;
    }

    return localPredictionJsonObject(*it, now);
}

void TwitchPollsAndPredictionsBar::setChannel(const ChannelPtr &channel)
{
    this->requestGeneration_++;
    this->pendingRequests_ = 0;
    this->pendingPoll_.reset();
    this->pendingPrediction_.reset();
    this->suppressedTwitchPollID_.clear();
    this->refreshTimer_.stop();
    this->channelSignalHolder_.clear();
    this->twitchChannel_ = resolveTwitchChannel(channel);
    this->kickChannel_ = resolveKickChannel(channel);
    this->clearItems();

    if (auto kick = this->kickChannel_.lock())
    {
        this->channelSignalHolder_.managedConnect(
            kick->predictionChanged, [this] {
                this->updateKickPollItem();
                this->updateKickPredictionItem();
                this->predictionStateChanged.invoke();
            });
        this->updateKickPollItem();
        this->updateKickPredictionItem();
        kick->refreshPrediction();
    }

    if (auto twitch = this->twitchChannel_.lock())
    {
        this->channelSignalHolder_.managedConnect(
            twitch->streamStatusChanged, [this] {
                this->refreshNow();
            });
        this->channelSignalHolder_.managedConnect(
            twitch->userStateChanged, [this] {
                this->refreshNow();
            });
        this->scheduleRefresh(250);
    }
}

void TwitchPollsAndPredictionsBar::refreshNow()
{
    auto twitch = this->twitchChannel_.lock();
    if (!twitch)
    {
        return;
    }

    this->scheduleRefresh(0);
    for (const auto delayMs : REFRESH_AFTER_LOCAL_CHANGE_MS)
    {
        QTimer::singleShot(delayMs, this, [this, twitch] {
            if (this->twitchChannel_.lock() == twitch)
            {
                this->scheduleRefresh(0);
            }
        });
    }
}

void TwitchPollsAndPredictionsBar::markTwitchPollEnded(
    const QString &broadcasterID, const QString &pollID)
{
    localPolls.remove(broadcasterID.trimmed());
    this->suppressedTwitchPollID_ = pollID.trimmed();

    // Ignore any older in-flight response which still describes the poll as
    // active, then remove it immediately instead of waiting for the next API
    // refresh to catch up with the successful end mutation.
    this->requestGeneration_++;
    this->pendingRequests_ = 0;
    this->pendingPoll_.reset();
    std::erase_if(this->items_, [](const Item &item) {
        return item.kind == ItemKind::Poll &&
               item.platform == ItemPlatform::Twitch;
    });
    this->updateFixedHeight();
    this->update();
    this->refreshNow();
}

bool TwitchPollsAndPredictionsBar::hasActiveTwitchPoll() const
{
    for (const auto &item : this->items_)
    {
        if (item.kind == ItemKind::Poll &&
            item.platform == ItemPlatform::Twitch)
        {
            return true;
        }
    }

    return false;
}

bool TwitchPollsAndPredictionsBar::hasActiveKickPoll() const
{
    for (const auto &item : this->items_)
    {
        if (item.kind == ItemKind::Poll &&
            item.platform == ItemPlatform::Kick)
        {
            return true;
        }
    }

    return false;
}

bool TwitchPollsAndPredictionsBar::hasOpenTwitchPrediction() const
{
    for (const auto &item : this->items_)
    {
        if (item.kind == ItemKind::Prediction &&
            item.platform == ItemPlatform::Twitch)
        {
            return true;
        }
    }

    return false;
}

bool TwitchPollsAndPredictionsBar::hasOpenKickPrediction() const
{
    for (const auto &item : this->items_)
    {
        if (item.kind == ItemKind::Prediction &&
            item.platform == ItemPlatform::Kick)
        {
            return true;
        }
    }

    return false;
}

QString TwitchPollsAndPredictionsBar::predictionButtonTooltip(
    bool canManage) const
{
    if (!canManage)
    {
        return QStringLiteral("Make Twitch prediction");
    }

    for (const auto &item : this->items_)
    {
        if (item.kind != ItemKind::Prediction ||
            item.platform != ItemPlatform::Twitch)
        {
            continue;
        }

        if (item.status == QStringLiteral("Locked"))
        {
            return QStringLiteral("Manage locked Twitch prediction");
        }
        if (item.status == QStringLiteral("Active"))
        {
            return QStringLiteral("Manage active Twitch prediction");
        }

        return QStringLiteral("Manage Twitch prediction");
    }

    return QStringLiteral("Start Twitch prediction");
}

QString TwitchPollsAndPredictionsBar::pollButtonTooltip(bool canManage) const
{
    if (!canManage)
    {
        return QStringLiteral("Open Twitch poll");
    }

    for (const auto &item : this->items_)
    {
        if (item.kind == ItemKind::Poll &&
            item.platform == ItemPlatform::Twitch)
        {
            return QStringLiteral("Twitch poll already active");
        }
    }

    return QStringLiteral("Start Twitch poll");
}

QSize TwitchPollsAndPredictionsBar::sizeHint() const
{
    return {0, this->barHeight()};
}

void TwitchPollsAndPredictionsBar::paintEvent(QPaintEvent * /*event*/)
{
    if (this->items_.empty())
    {
        return;
    }

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(this->rect(), this->theme->messages.backgrounds.regular);

    int y = 0;
    for (const auto &item : this->items_)
    {
        const auto height = this->itemHeight(item);
        this->drawItem(painter, item, QRect{0, y, this->width(), height});
        y += height;
    }
}

void TwitchPollsAndPredictionsBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        int y = 0;
        for (const auto &item : this->items_)
        {
            const auto height = this->itemHeight(item);
            if (QRect{0, y, this->width(), height}.contains(
                    event->position().toPoint()))
            {
                if (item.kind == ItemKind::Poll)
                {
                    if (item.platform == ItemPlatform::Kick)
                    {
                        this->kickPollClicked.invoke();
                    }
                    else
                    {
                        this->pollClicked.invoke();
                    }
                }
                else if (item.platform == ItemPlatform::Kick)
                {
                    this->kickPredictionClicked.invoke();
                }
                else
                {
                    this->predictionClicked.invoke();
                }
                event->accept();
                return;
            }
            y += height;
        }
    }

    BaseWidget::mousePressEvent(event);
}

void TwitchPollsAndPredictionsBar::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    this->updateFixedHeight();
    this->update();
}

void TwitchPollsAndPredictionsBar::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->update();
}

void TwitchPollsAndPredictionsBar::clearItems()
{
    this->items_.clear();
    this->updateFixedHeight();
    this->update();
}

void TwitchPollsAndPredictionsBar::scheduleRefresh(int delayMs)
{
    if (this->twitchChannel_.expired())
    {
        return;
    }

    this->refreshTimer_.start(std::max(delayMs, 0));
}

void TwitchPollsAndPredictionsBar::refresh()
{
    auto twitch = this->twitchChannel_.lock();
    if (!twitch)
    {
        this->pendingPoll_.reset();
        this->pendingPrediction_.reset();
        this->updateItems();
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser == nullptr || currentUser->isAnon())
    {
        this->pendingPoll_.reset();
        this->pendingPrediction_.reset();
        this->updateItems();
        this->scheduleRefresh(REFRESH_WHEN_IDLE_MS);
        return;
    }

    const auto roomId = twitch->roomId();
    if (roomId.isEmpty())
    {
        this->pendingPoll_.reset();
        this->pendingPrediction_.reset();
        this->updateItems();
        this->scheduleRefresh(REFRESH_WAIT_FOR_ROOM_ID_MS);
        return;
    }

    std::optional<TwitchModerationAuth::Account> moderationAccount;
    if (twitch->isMod() && !twitch->isBroadcaster())
    {
        auto account =
            TwitchModerationAuth::resolveForCurrentUser(currentUser->getUserId());
        if (account.isValid())
        {
            moderationAccount = std::move(account);
        }
    }

    auto oauthClient = currentUser->getOAuthClient();
    auto oauthToken = currentUser->getOAuthToken();
    if (moderationAccount)
    {
        oauthClient = moderationAccount->clientId;
        oauthToken = moderationAccount->oauthToken;
    }

    const int generation = ++this->requestGeneration_;
    this->pendingRequests_ = 1;
    this->pendingPoll_.reset();
    this->pendingPrediction_.reset();
    for (const auto &item : this->items_)
    {
        if (item.platform != ItemPlatform::Twitch)
        {
            continue;
        }
        if (item.kind == ItemKind::Poll)
        {
            this->pendingPoll_ = item;
        }
        else
        {
            this->pendingPrediction_ = item;
        }
    }
    if (auto item = TwitchPollsAndPredictionsBar::makeLocalPollItem(roomId))
    {
        this->pendingPoll_ = std::move(item);
    }
    if (auto item =
            TwitchPollsAndPredictionsBar::makeLocalPredictionItem(roomId))
    {
        this->pendingPrediction_ = std::move(item);
    }
    if (this->pendingPoll_ || this->pendingPrediction_)
    {
        // Preview cached Twitch state without consuming it, while retaining
        // the Kick banners until the network refresh completes.
        std::vector<Item> refreshedItems;
        if (this->pendingPoll_)
        {
            refreshedItems.push_back(*this->pendingPoll_);
        }
        if (this->pendingPrediction_)
        {
            refreshedItems.push_back(*this->pendingPrediction_);
        }
        for (const auto &item : this->items_)
        {
            if (item.platform == ItemPlatform::Kick)
            {
                refreshedItems.push_back(item);
            }
        }
        this->items_ = std::move(refreshedItems);
        this->updateFixedHeight();
        this->update();
    }

    QPointer<TwitchPollsAndPredictionsBar> guard(this);
    TwitchWebApi::getActivePollAndPredictions(
            roomId, oauthClient, oauthToken,
            [guard, generation](const HelixPolls &polls,
                                const HelixPredictions &predictions) {
                if (!guard || guard->requestGeneration_ != generation)
                {
                    return;
                }

                guard->pendingPoll_.reset();
                guard->pendingPrediction_.reset();
                for (const auto &poll : polls.polls)
                {
                    if (!guard->suppressedTwitchPollID_.isEmpty() &&
                        poll.id == guard->suppressedTwitchPollID_)
                    {
                        continue;
                    }
                    if (auto item =
                            TwitchPollsAndPredictionsBar::makePollItem(poll))
                    {
                        guard->suppressedTwitchPollID_.clear();
                        guard->pendingPoll_ = std::move(item);
                        break;
                    }
                }
                for (const auto &prediction : predictions.predictions)
                {
                    if (auto item =
                            TwitchPollsAndPredictionsBar::makePredictionItem(
                                prediction))
                    {
                        guard->pendingPrediction_ = std::move(item);
                        break;
                    }
                }
                guard->finishRequest(generation);
            },
            [guard, generation](const QString &) {
                if (!guard || guard->requestGeneration_ != generation)
                {
                    return;
                }

                guard->finishRequest(generation);
            });
}

void TwitchPollsAndPredictionsBar::finishRequest(int generation)
{
    if (generation != this->requestGeneration_)
    {
        return;
    }

    if (--this->pendingRequests_ > 0)
    {
        return;
    }

    this->updateItems();
    this->scheduleRefresh(this->items_.empty() ? REFRESH_WHEN_IDLE_MS
                                               : REFRESH_WITH_ACTIVE_MS);
}

void TwitchPollsAndPredictionsBar::updateItems()
{
    this->items_.clear();
    if (this->pendingPoll_)
    {
        this->items_.push_back(std::move(*this->pendingPoll_));
    }
    if (this->pendingPrediction_)
    {
        this->items_.push_back(std::move(*this->pendingPrediction_));
    }
    this->pendingPoll_.reset();
    this->pendingPrediction_.reset();

    if (auto kick = this->kickChannel_.lock(); kick != nullptr)
    {
        if (kick->activePoll())
        {
            if (auto item = TwitchPollsAndPredictionsBar::makeKickPollItem(
                    *kick->activePoll()))
            {
                this->items_.push_back(std::move(*item));
            }
        }
        if (kick->activePrediction())
        {
            if (auto item =
                    TwitchPollsAndPredictionsBar::makeKickPredictionItem(
                        *kick->activePrediction()))
            {
                this->items_.push_back(std::move(*item));
            }
        }
    }

    this->updateFixedHeight();
    this->update();
}

void TwitchPollsAndPredictionsBar::updateKickPollItem()
{
    std::erase_if(this->items_, [](const Item &item) {
        return item.kind == ItemKind::Poll &&
               item.platform == ItemPlatform::Kick;
    });

    if (auto kick = this->kickChannel_.lock();
        kick != nullptr && kick->activePoll())
    {
        if (auto item = TwitchPollsAndPredictionsBar::makeKickPollItem(
                *kick->activePoll()))
        {
            this->items_.push_back(std::move(*item));
        }
    }

    this->updateFixedHeight();
    this->update();
}

void TwitchPollsAndPredictionsBar::updateKickPredictionItem()
{
    std::erase_if(this->items_, [](const Item &item) {
        return item.kind == ItemKind::Prediction &&
               item.platform == ItemPlatform::Kick;
    });

    if (auto kick = this->kickChannel_.lock();
        kick != nullptr && kick->activePrediction())
    {
        if (auto item = TwitchPollsAndPredictionsBar::makeKickPredictionItem(
                *kick->activePrediction()))
        {
            this->items_.push_back(std::move(*item));
        }
    }

    this->updateFixedHeight();
    this->update();
}

void TwitchPollsAndPredictionsBar::updateFixedHeight()
{
    if (this->items_.empty())
    {
        this->unsetCursor();
        this->setFixedHeight(0);
        this->hide();
        return;
    }

    this->setCursor(Qt::PointingHandCursor);
    this->setFixedHeight(this->barHeight());
    this->show();
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makePollItem(const HelixPoll &poll)
{
    if (poll.status != QStringLiteral("ACTIVE") || poll.choices.empty())
    {
        return std::nullopt;
    }

    Item item;
    item.kind = ItemKind::Poll;
    item.title = poll.title;
    item.status = statusTitle(poll.status);
    item.choices.reserve(poll.choices.size());

    for (const auto &choice : poll.choices)
    {
        item.choices.push_back({
            .title = choice.title,
            .weight = std::max(choice.votes, 0),
            .detail = voteText(choice.votes),
        });
    }

    return item;
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makePredictionItem(
        const HelixPrediction &prediction)
{
    if ((prediction.status != QStringLiteral("ACTIVE") &&
         prediction.status != QStringLiteral("LOCKED")) ||
        prediction.outcomes.empty())
    {
        return std::nullopt;
    }

    int totalPoints = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        totalPoints += std::max(outcome.channelPoints, 0);
    }

    Item item;
    item.kind = ItemKind::Prediction;
    item.title = prediction.title;
    item.status = statusTitle(prediction.status);
    item.choices.reserve(prediction.outcomes.size());

    for (const auto &outcome : prediction.outcomes)
    {
        item.choices.push_back({
            .title = outcome.title,
            .weight = totalPoints > 0 ? std::max(outcome.channelPoints, 0)
                                      : std::max(outcome.users, 0),
            .points = std::max(outcome.channelPoints, 0),
            .users = std::max(outcome.users, 0),
            .showPredictionMetrics = true,
        });
    }

    return item;
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makeKickPollItem(const KickPoll &poll)
{
    if (!poll.isVisible() || poll.options.empty())
    {
        return std::nullopt;
    }

    Item item;
    item.kind = ItemKind::Poll;
    item.platform = ItemPlatform::Kick;
    item.title = poll.title;
    item.status = statusTitle(poll.state);
    item.choices.reserve(poll.options.size());

    for (const auto &option : poll.options)
    {
        item.choices.push_back({
            .title = option.title,
            .weight = std::max(option.votes, 0),
            .detail = voteText(option.votes),
        });
    }

    return item;
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makeKickPredictionItem(
        const KickPrediction &prediction)
{
    if (!prediction.isOpen() || prediction.outcomes.empty())
    {
        return std::nullopt;
    }

    int totalPoints = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        totalPoints += std::max(outcome.totalVoteAmount, 0);
    }

    Item item;
    item.kind = ItemKind::Prediction;
    item.platform = ItemPlatform::Kick;
    item.title = prediction.title;
    item.status = statusTitle(prediction.state);
    item.choices.reserve(prediction.outcomes.size());

    for (const auto &outcome : prediction.outcomes)
    {
        item.choices.push_back({
            .title = outcome.title,
            .weight = totalPoints > 0 ? std::max(outcome.totalVoteAmount, 0)
                                      : std::max(outcome.voteCount, 0),
            .points = std::max(outcome.totalVoteAmount, 0),
            .users = std::max(outcome.voteCount, 0),
            .showPredictionMetrics = true,
        });
    }

    return item;
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makeLocalPollItem(
        const QString &broadcasterID)
{
    auto it = localPolls.find(broadcasterID);
    if (it == localPolls.end())
    {
        return std::nullopt;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto elapsedMs = std::max<qint64>(0, it->createdAt.msecsTo(now));
    const auto lifetimeMs =
        static_cast<qint64>(std::max(it->durationSeconds, 1)) * 1000 +
        LOCAL_POLL_EXTRA_RETENTION_MS;
    if (!it->createdAt.isValid() || elapsedMs > lifetimeMs)
    {
        localPolls.erase(it);
        return std::nullopt;
    }

    Item item;
    item.kind = ItemKind::Poll;
    item.title = it->title;
    item.status = statusTitle(QStringLiteral("ACTIVE"));
    item.choices.reserve(it->choices.size());
    for (const auto &choiceTitle : it->choices)
    {
        item.choices.push_back({
            .title = choiceTitle,
            .weight = 0,
            .detail = voteText(0),
        });
    }

    return item;
}

std::optional<TwitchPollsAndPredictionsBar::Item>
    TwitchPollsAndPredictionsBar::makeLocalPredictionItem(
        const QString &broadcasterID)
{
    const auto predictionJson =
        TwitchPollsAndPredictionsBar::localPredictionJson(broadcasterID);
    if (!predictionJson)
    {
        return std::nullopt;
    }

    return TwitchPollsAndPredictionsBar::makePredictionItem(
        HelixPrediction(*predictionJson));
}

int TwitchPollsAndPredictionsBar::barHeight() const
{
    if (this->items_.empty())
    {
        return 0;
    }

    int height = 0;
    for (const auto &item : this->items_)
    {
        height += this->itemHeight(item);
    }
    return height;
}

int TwitchPollsAndPredictionsBar::itemHeight(const Item &item) const
{
    const auto scale = this->scale();
    auto *fonts = getApp()->getFonts();
    const QFontMetrics titleMetrics(
        fonts->getFont(FontStyle::UiMediumBold, scale));
    const QFontMetrics detailMetrics(fonts->getFont(FontStyle::UiMedium, scale));

    const int outerInset = std::max(1, int(std::round(1 * scale)));
    const int verticalPadding = int(std::round(4 * scale));
    const int headerHeight =
        std::max(int(std::round(18 * scale)), titleMetrics.height() + 2);
    const int choiceHeight =
        std::max(int(std::round(17 * scale)), detailMetrics.height() + 2);
    const int headerGap = int(std::round(2 * scale));
    const int choiceGap = std::max(1, int(std::round(2 * scale)));
    const int choicesHeight =
        static_cast<int>(item.choices.size()) * choiceHeight +
        std::max(0, static_cast<int>(item.choices.size()) - 1) * choiceGap;

    return outerInset * 2 + verticalPadding * 2 + headerHeight + headerGap +
           choicesHeight;
}

void TwitchPollsAndPredictionsBar::drawItem(QPainter &painter,
                                            const Item &item,
                                            QRect rect) const
{
    const auto scale = this->scale();
    const int outerInset = std::max(1, int(std::round(1 * scale)));
    const int paddingX = int(std::round(8 * scale));
    const int paddingY = int(std::round(4 * scale));
    const int railWidth = std::max(2, int(std::round(3 * scale)));
    const int headerGap = int(std::round(2 * scale));
    const int choiceGap = std::max(1, int(std::round(2 * scale)));

    const QColor accent =
        item.platform == ItemPlatform::Kick
            ? QColor("#53FC18")
            : QColor("#9146FF");
    const bool light = this->theme->isLightTheme();
    const QColor text = this->theme->messages.textColors.regular;
    const QColor mutedText = mutedFromTheme(this->theme, text);

    QColor surface = this->theme->messages.backgrounds.alternate;
    if (surface == this->theme->messages.backgrounds.regular)
    {
        surface = this->theme->splits.input.background;
    }

    QColor separator = this->theme->splits.messageSeperator;
    if (!separator.isValid())
    {
        separator = this->theme->splits.header.border;
    }
    separator.setAlpha(light ? 135 : 150);

    const QColor progressFill = withAlpha(accent, light ? 145 : 125);
    const QColor labelText = withAlpha(accent, light ? 255 : 230);
    const bool locked =
        item.status.compare(QStringLiteral("Locked"), Qt::CaseInsensitive) == 0;
    const QColor lockedStatusAccent = withAlpha(text, light ? 145 : 185);
    const QColor statusAccent =
        locked ? lockedStatusAccent : QColor("#35D49D");
    const QColor statusFill =
        locked ? withAlpha(text, light ? 16 : 24)
               : withAlpha(statusAccent, light ? 32 : 38);

    auto *fonts = getApp()->getFonts();
    const auto titleFont = fonts->getFont(FontStyle::UiMediumBold, scale);
    const auto detailFont = fonts->getFont(FontStyle::UiMedium, scale);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics detailMetrics(detailFont);
    const int headerHeight =
        std::max(int(std::round(18 * scale)), titleMetrics.height() + 2);
    const int choiceHeight =
        std::max(int(std::round(17 * scale)), detailMetrics.height() + 2);

    const QRect itemRect = rect.adjusted(0, outerInset, -1, -outerInset)
                               .normalized();

    painter.fillRect(itemRect, surface);
    painter.fillRect(QRect{itemRect.left(), itemRect.top(), itemRect.width(), 1},
                     separator);
    painter.fillRect(
        QRect{itemRect.left(), itemRect.bottom(), itemRect.width(), 1},
        separator);
    painter.fillRect(
        QRect{itemRect.left(), itemRect.top(), railWidth, itemRect.height()},
        withAlpha(accent, light ? 210 : 190));

    const int contentLeft = itemRect.left() + paddingX + railWidth +
                            int(std::round(5 * scale));
    const int contentRight = itemRect.right() - paddingX;
    if (contentRight <= contentLeft)
    {
        return;
    }

    int y = itemRect.top() + paddingY;

    const QString kindText = item.kind == ItemKind::Poll
                                 ? QStringLiteral("POLL")
                                 : QStringLiteral("PREDICTION");
    const QString statusText = item.status.toUpper();
    const int chipPaddingX = int(std::round(6 * scale));
    const int statusWidth =
        detailMetrics.horizontalAdvance(statusText) + chipPaddingX * 2;
    QRect statusRect{contentRight - statusWidth,
                     y + std::max(0, (headerHeight - detailMetrics.height()) / 2),
                     statusWidth, detailMetrics.height()};
    const int labelWidth = detailMetrics.horizontalAdvance(kindText);
    QRect labelRect{contentLeft, y, labelWidth, headerHeight};
    const int titleLeft = labelRect.right() + int(std::round(8 * scale));
    QRect titleRect{titleLeft, y,
                    std::max(0, statusRect.left() - titleLeft -
                                    int(std::round(10 * scale))),
                    headerHeight};

    painter.setFont(detailFont);
    painter.setPen(labelText);
    painter.drawText(labelRect, Qt::AlignVCenter | Qt::AlignLeft, kindText);

    fillRoundedRect(painter, QRectF(statusRect), statusFill,
                    statusRect.height() / 2.0);
    QRect statusTextRect = statusRect;
    statusTextRect.adjust(chipPaddingX, 0, -chipPaddingX, 0);
    painter.setPen(withAlpha(statusAccent, 245));
    painter.setFont(detailFont);
    painter.drawText(statusTextRect, Qt::AlignVCenter | Qt::AlignRight,
                     statusText);

    painter.setPen(text);
    painter.setFont(titleFont);
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                     titleMetrics.elidedText(item.title, Qt::ElideRight,
                                             titleRect.width()));

    int totalWeight = 0;
    for (const auto &choice : item.choices)
    {
        totalWeight += choice.weight;
    }

    y = itemRect.top() + paddingY + headerHeight + headerGap;

    const int detailWidth =
        std::clamp(int(std::round(112 * scale)),
                   int(std::round(78 * scale)),
                   std::max(int(std::round(78 * scale)),
                            (contentRight - contentLeft) / 2));
    int choiceIndex = 1;

    for (const auto &choice : item.choices)
    {
        QRect rowRect{contentLeft, y, contentRight - contentLeft,
                      choiceHeight};

        const int trackHeight = std::max(2, int(std::round(2 * scale)));
        QRect trackRect{rowRect.left(), rowRect.bottom() - trackHeight,
                        rowRect.width(),
                        trackHeight};
        if (totalWeight > 0 && choice.weight > 0)
        {
            const int filledTrackWidth =
                std::max(1, int(std::round(trackRect.width() *
                                           (double(choice.weight) /
                                            double(totalWeight)))));
            QRect activeTrackRect = trackRect;
            activeTrackRect.setWidth(
                std::min(activeTrackRect.width(), filledTrackWidth));
            painter.fillRect(activeTrackRect, progressFill);
        }
        painter.fillRect(QRect{rowRect.left(), rowRect.bottom(),
                               rowRect.width(), 1},
                         withAlpha(separator, light ? 95 : 85));

        const QRect textLineRect = rowRect.adjusted(0, 0, 0, -1);
        const int indexWidth = int(std::round(17 * scale));
        QRect indexRect{textLineRect.left(), textLineRect.top(), indexWidth,
                        textLineRect.height()};

        painter.setFont(detailFont);
        painter.setPen(withAlpha(accent, light ? 225 : 210));
        painter.drawText(indexRect, Qt::AlignVCenter | Qt::AlignLeft,
                         QString::number(choiceIndex));

        int metricLeft = rowRect.right() - int(std::round(5 * scale)) -
                         detailWidth;

        painter.setFont(detailFont);
        if (choice.showPredictionMetrics)
        {
            QRect metricRect{rowRect.right() - detailWidth,
                             textLineRect.top(), detailWidth,
                             textLineRect.height()};
            metricLeft = drawPredictionMetrics(
                painter, metricRect, choice.points, choice.users, mutedText,
                withAlpha(mutedText, light ? 210 : 225), detailMetrics, scale);
        }
        else
        {
            QRect detailRect{rowRect.right() - detailWidth,
                             textLineRect.top(), detailWidth,
                             textLineRect.height()};
            painter.setPen(mutedText);
            painter.drawText(detailRect, Qt::AlignVCenter | Qt::AlignRight,
                             detailMetrics.elidedText(
                                 pollDetailText(choice.detail, choice.weight,
                                                totalWeight),
                                 Qt::ElideRight, detailRect.width()));
            metricLeft = detailRect.left();
        }

        QRect choiceTitleRect{
            indexRect.right(),
            textLineRect.top(),
            std::max(0, metricLeft - indexRect.right() -
                            int(std::round(8 * scale))),
            textLineRect.height()};

        painter.setPen(text);
        painter.drawText(choiceTitleRect, Qt::AlignVCenter | Qt::AlignLeft,
                         detailMetrics.elidedText(choice.title,
                                                  Qt::ElideRight,
                                                  choiceTitleRect.width()));

        y += choiceHeight + choiceGap;
        choiceIndex++;
    }
}

}  // namespace chatterino
