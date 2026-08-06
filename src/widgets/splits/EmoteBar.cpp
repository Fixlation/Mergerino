// SPDX-FileCopyrightText: 2026 Mergerino Contributors
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/EmoteBar.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QAbstractButton>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QParallelAnimationGroup>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>

namespace chatterino {

namespace {

constexpr int EMOTE_BAR_HISTORY_LIMIT = 100;
constexpr int EMOTE_BAR_USAGE_LIMIT = 200;
constexpr int EMOTE_BAR_CHANNEL_LIMIT = 64;
constexpr int EMOTE_BAR_MOST_USED_MIN_COUNT = 3;

struct StoredEntry {
    QString id;
    QString name;
    QString provider;
    QString platform;
    int count = 0;
};

struct ChannelData {
    qint64 updatedAt = 0;
    std::vector<StoredEntry> recent;
    std::vector<StoredEntry> usage;
};

struct EmoteBarStore {
    QHash<QString, ChannelData> channels;
};

struct Candidate {
    StoredEntry entry;
    EmotePtr emote;
};

struct CandidateMaps {
    QHash<QString, Candidate> byIdentity;
    QHash<QString, Candidate> byName;
};

struct DisplayEntry {
    QString identity;
    QString token;
    EmotePtr emote;
    std::optional<int> count;
};

QString entryIdentity(const StoredEntry &entry)
{
    return entry.provider + QChar(0x1f) + entry.platform + QChar(0x1f) +
           entry.id;
}

bool isSupportedProvider(const QString &provider)
{
    return provider == QStringLiteral("7TV") ||
           provider == QStringLiteral("BTTV") ||
           provider == QStringLiteral("FFZ") ||
           provider == QStringLiteral("PLATFORM");
}

StoredEntry entryFromJson(const QJsonObject &object, bool withCount)
{
    StoredEntry entry{
        .id = object.value(QStringLiteral("id")).toString().trimmed(),
        .name = object.value(QStringLiteral("name")).toString().trimmed(),
        .provider =
            object.value(QStringLiteral("provider")).toString().trimmed(),
        .platform =
            object.value(QStringLiteral("platform")).toString().trimmed(),
    };
    if (withCount)
    {
        const auto rawCount = object.value(QStringLiteral("count")).toDouble();
        entry.count = static_cast<int>(std::clamp(
            rawCount, 0.0, static_cast<double>(std::numeric_limits<int>::max())));
    }
    return entry;
}

std::vector<StoredEntry> entriesFromJson(const QJsonArray &array,
                                         bool withCount, int limit)
{
    std::vector<StoredEntry> entries;
    QSet<QString> seen;
    for (const auto &value : array)
    {
        if (!value.isObject())
        {
            continue;
        }
        auto entry = entryFromJson(value.toObject(), withCount);
        if (entry.id.isEmpty() || entry.name.isEmpty() ||
            !isSupportedProvider(entry.provider) ||
            (withCount && entry.count <= 0))
        {
            continue;
        }
        const auto identity = entryIdentity(entry);
        if (seen.contains(identity))
        {
            continue;
        }
        seen.insert(identity);
        entries.push_back(std::move(entry));
        if (static_cast<int>(entries.size()) >= limit)
        {
            break;
        }
    }
    return entries;
}

EmoteBarStore parseStore(const QString &json)
{
    EmoteBarStore store;
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return store;
    }
    const auto channels =
        document.object().value(QStringLiteral("channels")).toArray();
    for (const auto &value : channels)
    {
        if (!value.isObject())
        {
            continue;
        }
        const auto object = value.toObject();
        const auto key =
            object.value(QStringLiteral("key")).toString().trimmed();
        if (key.isEmpty())
        {
            continue;
        }
        ChannelData data;
        data.updatedAt = static_cast<qint64>(
            object.value(QStringLiteral("updatedAt")).toDouble());
        data.recent = entriesFromJson(
            object.value(QStringLiteral("recent")).toArray(), false,
            EMOTE_BAR_HISTORY_LIMIT);
        data.usage = entriesFromJson(
            object.value(QStringLiteral("usage")).toArray(), true,
            EMOTE_BAR_USAGE_LIMIT);
        if (!data.recent.empty() || !data.usage.empty())
        {
            store.channels.insert(key, std::move(data));
        }
    }
    return store;
}

QJsonObject entryToJson(const StoredEntry &entry, bool withCount)
{
    QJsonObject object{
        {QStringLiteral("id"), entry.id},
        {QStringLiteral("name"), entry.name},
        {QStringLiteral("provider"), entry.provider},
    };
    if (!entry.platform.isEmpty())
    {
        object.insert(QStringLiteral("platform"), entry.platform);
    }
    if (withCount)
    {
        object.insert(QStringLiteral("count"), entry.count);
    }
    return object;
}

QString serializeStore(const EmoteBarStore &store)
{
    std::vector<std::pair<QString, ChannelData>> channels;
    channels.reserve(store.channels.size());
    for (auto it = store.channels.cbegin(); it != store.channels.cend(); ++it)
    {
        if (!it->recent.empty() || !it->usage.empty())
        {
            channels.emplace_back(it.key(), it.value());
        }
    }
    std::sort(channels.begin(), channels.end(), [](const auto &left,
                                                    const auto &right) {
        return left.second.updatedAt > right.second.updatedAt;
    });
    if (channels.size() > EMOTE_BAR_CHANNEL_LIMIT)
    {
        channels.resize(EMOTE_BAR_CHANNEL_LIMIT);
    }
    QJsonArray channelArray;
    for (const auto &[key, data] : channels)
    {
        QJsonArray recent;
        for (const auto &entry : data.recent)
        {
            recent.append(entryToJson(entry, false));
        }
        QJsonArray usage;
        for (const auto &entry : data.usage)
        {
            usage.append(entryToJson(entry, true));
        }
        channelArray.append(QJsonObject{
            {QStringLiteral("key"), key},
            {QStringLiteral("updatedAt"), static_cast<double>(data.updatedAt)},
            {QStringLiteral("recent"), recent},
            {QStringLiteral("usage"), usage},
        });
    }
    return QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1},
        {QStringLiteral("channels"), channelArray},
    }).toJson(QJsonDocument::Compact));
}

QString channelKey(const ChannelPtr &channel)
{
    if (!channel)
    {
        return {};
    }
    if (const auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        QString twitchID;
        if (const auto twitch =
                std::dynamic_pointer_cast<TwitchChannel>(merged->twitchChannel()))
        {
            twitchID = twitch->roomId();
        }
        QString kickID;
        if (const auto kick =
                std::dynamic_pointer_cast<KickChannel>(merged->kickChannel()))
        {
            kickID = QString::number(kick->channelID());
        }
        return QStringLiteral("merged:%1:%2:%3")
            .arg(twitchID, kickID,
                 channel->getName().trimmed().toLower());
    }
    if (const auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        const auto identity = twitch->roomId().isEmpty()
                                  ? twitch->getName().trimmed().toLower()
                                  : twitch->roomId();
        return QStringLiteral("twitch:") + identity;
    }
    if (const auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        const auto identity = kick->channelID() == 0
                                  ? kick->slug().trimmed().toLower()
                                  : QString::number(kick->channelID());
        return QStringLiteral("kick:") + identity;
    }
    return QStringLiteral("channel:") + channel->getName().trimmed().toLower();
}

void addEmoteMap(CandidateMaps &candidates,
                 const std::shared_ptr<const EmoteMap> &map,
                 const QString &provider, const QString &platform = {})
{
    if (!map)
    {
        return;
    }
    for (const auto &[mapName, emote] : *map)
    {
        if (!emote)
        {
            continue;
        }
        Candidate candidate{
            .entry =
                {
                    .id = emote->id.string,
                    .name = mapName.string,
                    .provider = provider,
                    .platform = platform,
                },
            .emote = emote,
        };
        candidates.byIdentity.insert(entryIdentity(candidate.entry), candidate);
        candidates.byName.insert(candidate.entry.name, std::move(candidate));
    }
}

CandidateMaps buildCandidates(const std::vector<ChannelPtr> &channels)
{
    CandidateMaps candidates;
    auto *app = getApp();
    auto *settings = getSettings();
    bool hasTwitch = false;
    bool hasKick = false;
    for (const auto &channel : channels)
    {
        hasTwitch = hasTwitch ||
                    std::dynamic_pointer_cast<TwitchChannel>(channel) != nullptr;
        hasKick = hasKick ||
                  std::dynamic_pointer_cast<KickChannel>(channel) != nullptr;
    }

    if ((hasTwitch || hasKick) && settings->enableSevenTVGlobalEmotes)
    {
        addEmoteMap(candidates, app->getSeventvEmotes()->globalEmotes(),
                    QStringLiteral("7TV"));
    }
    if (hasTwitch && settings->enableBTTVGlobalEmotes)
    {
        addEmoteMap(candidates, app->getBttvEmotes()->emotes(),
                    QStringLiteral("BTTV"));
    }
    if (hasTwitch && settings->enableFFZGlobalEmotes)
    {
        addEmoteMap(candidates, app->getFfzEmotes()->emotes(),
                    QStringLiteral("FFZ"));
    }

    for (const auto &channel : channels)
    {
        if (const auto twitch =
                std::dynamic_pointer_cast<TwitchChannel>(channel))
        {
            if (settings->enableSevenTVChannelEmotes)
            {
                addEmoteMap(candidates, twitch->seventvEmotes(),
                            QStringLiteral("7TV"));
            }
            if (settings->enableBTTVChannelEmotes)
            {
                addEmoteMap(candidates, twitch->bttvEmotes(),
                            QStringLiteral("BTTV"));
            }
            if (settings->enableFFZChannelEmotes)
            {
                addEmoteMap(candidates, twitch->ffzEmotes(),
                            QStringLiteral("FFZ"));
            }

            const auto twitchAccount = app->getAccounts()->twitch.getCurrent();
            if (twitchAccount && !twitchAccount->isAnon())
            {
                if (settings->enableSevenTVPersonalEmotes)
                {
                    for (const auto &map : app->getSeventvPersonalEmotes()
                                               ->getEmoteSetsForTwitchUser(
                                                   twitchAccount->getUserId()))
                    {
                        addEmoteMap(candidates, map, QStringLiteral("7TV"));
                    }
                }
                const auto accessEmotes = twitchAccount->accessEmotes();
                addEmoteMap(candidates, *accessEmotes,
                            QStringLiteral("PLATFORM"),
                            QStringLiteral("TWITCH"));
            }
            addEmoteMap(candidates, twitch->localTwitchEmotes(),
                        QStringLiteral("PLATFORM"),
                        QStringLiteral("TWITCH"));
            continue;
        }

        if (const auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
        {
            if (settings->enableSevenTVChannelEmotes)
            {
                addEmoteMap(candidates, kick->seventvEmotes(),
                            QStringLiteral("7TV"));
            }
            const auto kickAccount = app->getAccounts()->kick.current();
            if (kickAccount && !kickAccount->isAnonymous() &&
                settings->enableSevenTVPersonalEmotes)
            {
                for (const auto &map :
                     app->getSeventvPersonalEmotes()->getEmoteSetsForKickUser(
                         kickAccount->userID()))
                {
                    addEmoteMap(candidates, map, QStringLiteral("7TV"));
                }
            }
            addEmoteMap(candidates, app->getKickChatServer()->globalEmotes(),
                        QStringLiteral("PLATFORM"), QStringLiteral("KICK"));
            addEmoteMap(candidates, kick->kickChannelEmotes(),
                        QStringLiteral("PLATFORM"), QStringLiteral("KICK"));
        }
    }
    return candidates;
}

QString compactCount(int count)
{
    if (count < 1000)
    {
        return QString::number(count);
    }
    if (count < 10000)
    {
        return QString::number(count / 1000.0, 'f', 1)
                   .replace(QStringLiteral(".0"), QString()) +
               QStringLiteral("k");
    }
    return QString::number(count / 1000) + QStringLiteral("k");
}

class EmoteBarButton final : public QAbstractButton
{
public:
    EmoteBarButton(DisplayEntry entry, QWidget *parent)
        : QAbstractButton(parent)
        , entry_(std::move(entry))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFocusPolicy(Qt::NoFocus);
        this->setAttribute(Qt::WA_Hover);
        this->hoverAnimation_.setDuration(110);
        this->hoverAnimation_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(
            &this->hoverAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                this->hoverProgress_ = value.toReal();
                this->update();
            });
        this->updateTooltip();
    }

    const QString &identity() const
    {
        return this->entry_.identity;
    }

    const QString &token() const
    {
        return this->entry_.token;
    }

    void setEntry(DisplayEntry entry)
    {
        this->entry_ = std::move(entry);
        this->updateTooltip();
        this->update();
    }

    void setDisplayScale(float scale)
    {
        this->scale_ = scale;
        this->setFixedSize(std::max(28, qRound(32 * scale)),
                           std::max(24, qRound(26 * scale)));
        this->update();
    }

    void setAppearance(QColor text, QColor hover, QColor focus)
    {
        this->textColor_ = std::move(text);
        this->hoverColor_ = std::move(hover);
        this->focusColor_ = std::move(focus);
        this->update();
    }

protected:
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave)
        {
            this->hoverAnimation_.stop();
            this->hoverAnimation_.setStartValue(this->hoverProgress_);
            this->hoverAnimation_.setEndValue(event->type() == QEvent::Enter
                                                  ? 1.0
                                                  : 0.0);
            this->hoverAnimation_.start();
        }
        return QAbstractButton::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (this->hoverProgress_ > 0.0 || this->isDown())
        {
            auto background =
                this->isDown() ? this->focusColor_ : this->hoverColor_;
            if (!this->isDown())
            {
                background.setAlphaF(background.alphaF() *
                                     this->hoverProgress_);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(this->rect().adjusted(1, 1, -1, -1),
                                    4 * this->scale_, 4 * this->scale_);
        }
        if (this->entry_.emote)
        {
            const auto image =
                this->entry_.emote->images.getImageOrLoaded(2.0F);
            if (image)
            {
                if (const auto pixmap = image->pixmapOrLoad())
                {
                    const QRectF available = QRectF(this->rect()).adjusted(
                        3.0 * this->scale_, 2.0 * this->scale_,
                        -3.0 * this->scale_, -2.0 * this->scale_);
                    QSizeF size = pixmap->size();
                    size.scale(available.size(), Qt::KeepAspectRatio);
                    size *= 1.05;
                    const auto center =
                        available.center() + QPointF(0.0, 0.25 * this->scale_);
                    QRectF target(center.x() - size.width() / 2.0,
                                  center.y() - size.height() / 2.0,
                                  size.width(), size.height());
                    target.translate(
                        0.0, -this->hoverProgress_ * this->scale_);
                    painter.setRenderHint(QPainter::SmoothPixmapTransform);
                    painter.drawPixmap(target, *pixmap,
                                       QRectF(pixmap->rect()));
                }
            }
        }
        if (this->entry_.count)
        {
            auto font = painter.font();
            font.setBold(true);
            font.setPixelSize(std::max(8, qRound(9 * this->scale_)));
            const auto text = compactCount(*this->entry_.count);
            const qreal diameter =
                std::max<qreal>(13.0, 14.0 * this->scale_);
            const QRectF badgeRect(this->scale_, this->scale_, diameter,
                                   diameter);
            const qreal availableTextWidth =
                diameter - std::max<qreal>(2.0, 2.0 * this->scale_);
            const qreal textWidth =
                QFontMetricsF(font).horizontalAdvance(text);
            if (textWidth > availableTextWidth)
            {
                font.setPixelSize(std::max(
                    6, static_cast<int>(std::floor(
                           font.pixelSize() * availableTextWidth / textWidth))));
            }
            painter.setFont(font);
            painter.setPen(
                QPen(QColor(24, 24, 28, 220),
                     std::max<qreal>(1.0, this->scale_)));
            painter.setBrush(QColor(255, 255, 255));
            painter.drawEllipse(badgeRect);
            painter.setPen(QColor(8, 8, 10));
            painter.save();
            painter.translate(
                0.0, -std::max<qreal>(1.0, this->scale_));
            painter.drawText(badgeRect, Qt::AlignCenter, text);
            painter.restore();
        }
    }

private:
    void updateTooltip()
    {
        auto tooltip = this->entry_.token +
                       QStringLiteral(" — Click to send. Ctrl+Click or "
                                      "Alt+Click to insert.");
        if (this->entry_.count)
        {
            tooltip = QStringLiteral("%1 — Sent %2 time%3 in this channel. "
                                     "Click to send. Ctrl+Click or Alt+Click "
                                     "to insert.")
                          .arg(this->entry_.token)
                          .arg(*this->entry_.count)
                          .arg(*this->entry_.count == 1 ? QString()
                                                       : QStringLiteral("s"));
        }
        this->setToolTip(tooltip);
    }

    DisplayEntry entry_;
    QVariantAnimation hoverAnimation_;
    qreal hoverProgress_ = 0.0;
    float scale_ = 1.F;
    QColor textColor_ = Qt::white;
    QColor hoverColor_{255, 255, 255, 24};
    QColor focusColor_{255, 255, 255, 36};
};

class EmoteBarRow final : public QScrollArea
{
public:
    using ActivateCallback = EmoteBar::ActivateCallback;

    EmoteBarRow(ActivateCallback activate, QWidget *parent)
        : QScrollArea(parent)
        , activate_(std::move(activate))
        , content_(new QWidget)
        , layout_(new QHBoxLayout(this->content_))
    {
        this->setFrameShape(QFrame::NoFrame);
        this->setWidgetResizable(false);
        this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setFocusPolicy(Qt::NoFocus);
        this->setAutoFillBackground(false);
        this->viewport()->setFocusPolicy(Qt::NoFocus);
        this->viewport()->setAutoFillBackground(false);
        this->viewport()->setAttribute(Qt::WA_TranslucentBackground);
        this->content_->setFocusPolicy(Qt::NoFocus);
        this->content_->setAutoFillBackground(false);
        this->content_->setAttribute(Qt::WA_TranslucentBackground);
        auto transparentPalette = this->palette();
        transparentPalette.setColor(QPalette::Base, Qt::transparent);
        transparentPalette.setColor(QPalette::Window, Qt::transparent);
        this->setPalette(transparentPalette);
        this->viewport()->setPalette(transparentPalette);
        this->content_->setPalette(transparentPalette);
        this->setWidget(this->content_);
        this->layout_->setContentsMargins(6, 3, 6, 3);
        this->layout_->setSpacing(4);
        this->layout_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }

    void setEntries(std::vector<DisplayEntry> entries, QString emptyText)
    {
        this->entries_ = std::move(entries);
        this->emptyText_ = std::move(emptyText);
        this->rebuild();
    }

    void setDisplayScale(float scale)
    {
        this->scale_ = scale;
        this->rowHeight_ = std::max(30, qRound(32 * scale));
        this->setFixedHeight(this->rowHeight_);
        this->rebuild();
    }

    void setAppearance(QColor text, QColor muted, QColor hover, QColor focus,
                       QFont font)
    {
        this->textColor_ = std::move(text);
        this->mutedColor_ = std::move(muted);
        this->hoverColor_ = std::move(hover);
        this->focusColor_ = std::move(focus);
        this->font_ = std::move(font);
        this->applyAppearance();
    }

    void refreshFrames()
    {
        for (auto *button : this->buttons_)
        {
            button->update();
        }
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QScrollArea::resizeEvent(event);
        this->resizeContent();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int delta = event->angleDelta().y() != 0
                              ? event->angleDelta().y()
                              : event->angleDelta().x();
        if (delta != 0)
        {
            this->horizontalScrollBar()->setValue(
                this->horizontalScrollBar()->value() - delta);
            event->accept();
            return;
        }
        QScrollArea::wheelEvent(event);
    }

private:
    void cancelTransition()
    {
        if (this->transition_)
        {
            this->transition_->stop();
            this->transition_->deleteLater();
            this->transition_ = nullptr;
        }
        for (auto *button : this->buttons_)
        {
            if (button->graphicsEffect())
            {
                button->setGraphicsEffect(nullptr);
            }
        }
        if (this->emptyLabel_ && this->emptyLabel_->graphicsEffect())
        {
            this->emptyLabel_->setGraphicsEffect(nullptr);
        }
        for (const auto &widget : this->outgoingWidgets_)
        {
            if (widget)
            {
                widget->deleteLater();
            }
        }
        this->outgoingWidgets_.clear();
    }

    void rebuild()
    {
        this->cancelTransition();

        QHash<QString, EmoteBarButton *> availableButtons;
        QHash<EmoteBarButton *, QRect> previousGeometries;
        for (auto *button : this->buttons_)
        {
            availableButtons.insert(button->identity(), button);
            previousGeometries.insert(button, button->geometry());
        }
        auto *previousEmptyLabel = this->emptyLabel_;
        const auto previousEmptyGeometry =
            previousEmptyLabel != nullptr ? previousEmptyLabel->geometry()
                                          : QRect{};

        while (auto *item = this->layout_->takeAt(0))
        {
            delete item;
        }
        this->buttons_.clear();
        this->emptyLabel_ = nullptr;

        const int horizontalMargin = std::max(4, qRound(5 * this->scale_));
        const int verticalMargin = std::max(2, qRound(2 * this->scale_));
        this->layout_->setContentsMargins(horizontalMargin, verticalMargin,
                                          horizontalMargin, verticalMargin);
        this->layout_->setSpacing(std::max(2, qRound(3 * this->scale_)));

        std::vector<EmoteBarButton *> newButtons;
        for (const auto &entry : this->entries_)
        {
            EmoteBarButton *button = nullptr;
            const auto found = availableButtons.find(entry.identity);
            if (found != availableButtons.end())
            {
                button = found.value();
                availableButtons.erase(found);
                button->setEntry(entry);
            }
            else
            {
                button = new EmoteBarButton(entry, this->content_);
                QObject::connect(
                    button, &QAbstractButton::clicked, button,
                    [this, button] {
                        this->activate_(
                            button->token(),
                            QGuiApplication::keyboardModifiers());
                    });
            }
            button->setDisplayScale(this->scale_);
            button->setEnabled(true);
            button->show();
            newButtons.push_back(button);
        }
        this->buttons_ = std::move(newButtons);

        QHash<QWidget *, QRect> outgoingGeometries;
        for (auto *button : availableButtons)
        {
            outgoingGeometries.insert(button,
                                      previousGeometries.value(button));
            button->setEnabled(false);
            this->outgoingWidgets_.push_back(button);
        }

        int desiredWidth = this->layout_->contentsMargins().left() +
                           this->layout_->contentsMargins().right();
        if (this->entries_.empty())
        {
            this->emptyLabel_ = previousEmptyLabel != nullptr
                                    ? previousEmptyLabel
                                    : new QLabel(this->content_);
            this->emptyLabel_->setText(this->emptyText_);
            this->emptyLabel_->setWordWrap(false);
            this->emptyLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);
            this->emptyLabel_->show();
        }
        else if (previousEmptyLabel != nullptr)
        {
            outgoingGeometries.insert(previousEmptyLabel,
                                      previousEmptyGeometry);
            this->outgoingWidgets_.push_back(previousEmptyLabel);
        }

        this->applyAppearance();
        if (this->emptyLabel_ != nullptr)
        {
            this->layout_->addWidget(this->emptyLabel_, 0,
                                     Qt::AlignLeft | Qt::AlignVCenter);
            desiredWidth += this->emptyLabel_->sizeHint().width();
        }
        else
        {
            for (auto *button : this->buttons_)
            {
                this->layout_->addWidget(button, 0,
                                         Qt::AlignLeft | Qt::AlignVCenter);
                desiredWidth += button->width();
            }
            desiredWidth += std::max(
                0, static_cast<int>(this->buttons_.size()) - 1) *
                            this->layout_->spacing();
        }

        this->desiredWidth_ = desiredWidth;
        this->content_->setFixedHeight(this->rowHeight_);
        this->resizeContent();
        this->layout_->invalidate();
        this->layout_->activate();

        auto *transition = new QParallelAnimationGroup(this);
        this->transition_ = transition;
        const int offset = std::max(2, qRound(3 * this->scale_));
        const int inset = std::max(2, qRound(3 * this->scale_));

        auto animateGeometry = [transition](QWidget *widget,
                                            const QRect &start,
                                            const QRect &end) {
            if (start == end)
            {
                widget->setGeometry(end);
                return;
            }
            widget->setGeometry(start);
            auto *animation = new QPropertyAnimation(widget, "geometry");
            animation->setDuration(180);
            animation->setStartValue(start);
            animation->setEndValue(end);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            transition->addAnimation(animation);
        };
        auto animateOpacity = [transition](QWidget *widget, qreal start,
                                           qreal end) {
            auto *effect = new QGraphicsOpacityEffect(widget);
            effect->setOpacity(start);
            widget->setGraphicsEffect(effect);
            auto *animation = new QPropertyAnimation(effect, "opacity");
            animation->setDuration(180);
            animation->setStartValue(start);
            animation->setEndValue(end);
            animation->setEasingCurve(QEasingCurve::OutCubic);
            transition->addAnimation(animation);
        };

        for (auto *button : this->buttons_)
        {
            const auto target = button->geometry();
            if (previousGeometries.contains(button))
            {
                animateGeometry(button, previousGeometries.value(button),
                                target);
            }
            else
            {
                auto start = target.adjusted(inset, inset, -inset, -inset);
                start.translate(0, offset);
                animateGeometry(button, start, target);
                animateOpacity(button, 0.0, 1.0);
            }
        }
        if (this->emptyLabel_ != nullptr && previousEmptyLabel == nullptr)
        {
            const auto target = this->emptyLabel_->geometry();
            auto start = target;
            start.translate(0, offset);
            animateGeometry(this->emptyLabel_, start, target);
            animateOpacity(this->emptyLabel_, 0.0, 1.0);
        }

        for (const auto &widgetPointer : this->outgoingWidgets_)
        {
            auto *widget = widgetPointer.data();
            if (widget == nullptr)
            {
                continue;
            }
            const auto start = outgoingGeometries.value(widget,
                                                        widget->geometry());
            auto end = start.adjusted(inset, inset, -inset, -inset);
            end.translate(0, offset);
            widget->raise();
            animateGeometry(widget, start, end);
            animateOpacity(widget, 1.0, 0.0);
        }

        QObject::connect(
            transition, &QParallelAnimationGroup::finished, this,
            [this, transition] {
                for (auto *button : this->buttons_)
                {
                    if (button->graphicsEffect())
                    {
                        button->setGraphicsEffect(nullptr);
                    }
                }
                if (this->emptyLabel_ != nullptr &&
                    this->emptyLabel_->graphicsEffect())
                {
                    this->emptyLabel_->setGraphicsEffect(nullptr);
                }
                for (const auto &widget : this->outgoingWidgets_)
                {
                    if (widget)
                    {
                        widget->deleteLater();
                    }
                }
                this->outgoingWidgets_.clear();
                if (this->transition_ == transition)
                {
                    this->transition_ = nullptr;
                }
                transition->deleteLater();
            });
        transition->start();
    }

    void resizeContent()
    {
        this->content_->resize(
            std::max(this->desiredWidth_, this->viewport()->width()),
            this->rowHeight_);
    }

    void applyAppearance()
    {
        if (this->emptyLabel_)
        {
            auto palette = this->emptyLabel_->palette();
            palette.setColor(QPalette::WindowText, this->mutedColor_);
            this->emptyLabel_->setPalette(palette);
            this->emptyLabel_->setFont(this->font_);
        }
        for (auto *button : this->buttons_)
        {
            button->setAppearance(this->textColor_, this->hoverColor_,
                                  this->focusColor_);
        }
    }

    ActivateCallback activate_;
    QWidget *content_;
    QHBoxLayout *layout_;
    QLabel *emptyLabel_ = nullptr;
    QPointer<QParallelAnimationGroup> transition_;
    std::vector<QPointer<QWidget>> outgoingWidgets_;
    std::vector<DisplayEntry> entries_;
    std::vector<EmoteBarButton *> buttons_;
    QString emptyText_;
    float scale_ = 1.F;
    int rowHeight_ = 32;
    int desiredWidth_ = 0;
    QColor textColor_ = Qt::white;
    QColor mutedColor_{190, 190, 190};
    QColor hoverColor_{255, 255, 255, 24};
    QColor focusColor_{255, 255, 255, 36};
    QFont font_;
};

std::vector<DisplayEntry> resolveEntries(
    const std::vector<StoredEntry> &stored, const CandidateMaps &candidates,
    EmoteBarScope scope, int maximum, bool showCounts,
    const QSet<QString> &excluded = {})
{
    std::vector<DisplayEntry> resolved;
    for (const auto &entry : stored)
    {
        if (scope == EmoteBarScope::SevenTV &&
            entry.provider != QStringLiteral("7TV"))
        {
            continue;
        }
        const auto identity = entryIdentity(entry);
        if (excluded.contains(identity))
        {
            continue;
        }
        const auto found = candidates.byIdentity.constFind(identity);
        if (found == candidates.byIdentity.cend())
        {
            continue;
        }
        resolved.push_back({
            .identity = identity,
            .token = found->entry.name,
            .emote = found->emote,
            .count = showCounts ? std::optional<int>(entry.count)
                                : std::nullopt,
        });
        if (static_cast<int>(resolved.size()) >= maximum)
        {
            break;
        }
    }
    return resolved;
}

QString cleanToken(QString token)
{
    static const auto edgePunctuation =
        QStringLiteral("`~!@#$%^&*()-+=[]{}\\|;:'\",.<>/?");
    while (!token.isEmpty() && edgePunctuation.contains(token.front()))
    {
        token.remove(0, 1);
    }
    while (!token.isEmpty() && edgePunctuation.contains(token.back()))
    {
        token.chop(1);
    }
    return token;
}

class EmoteBarIntroduction final : public QWidget
{
public:
    explicit EmoteBarIntroduction(QWidget *parent)
        : QWidget(parent)
        , layout_(new QHBoxLayout(this))
        , label_(new QLabel(
              QStringLiteral(
                  "<b>Emote bar</b><br>Your most-used and recent 7TV emotes "
                  "appear here after you send them."),
              this))
        , dismissButton_(new QPushButton(QStringLiteral("Dismiss"), this))
        , disableButton_(new QPushButton(QStringLiteral("Disable"), this))
    {
        this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->layout_->setSpacing(6);
        this->label_->setTextFormat(Qt::RichText);
        this->label_->setWordWrap(true);
        this->label_->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Preferred);
        for (auto *button : {this->dismissButton_, this->disableButton_})
        {
            button->setCursor(Qt::PointingHandCursor);
            button->setFocusPolicy(Qt::NoFocus);
        }
        this->layout_->addWidget(this->label_, 1);
        this->layout_->addWidget(this->dismissButton_, 0, Qt::AlignVCenter);
        this->layout_->addWidget(this->disableButton_, 0, Qt::AlignVCenter);

        QObject::connect(this->dismissButton_, &QPushButton::clicked, this, [] {
            emoteBarIntroductionDismissedSetting() = true;
        });
        QObject::connect(this->disableButton_, &QPushButton::clicked, this, [] {
            emoteBarIntroductionDismissedSetting() = true;
            emoteBarModeSetting().setValue(QStringLiteral("disabled"));
        });
        this->setDisplayScale(1.0F);
    }

    int preferredHeight() const
    {
        return std::max(74, qRound(78 * this->scale_));
    }

    void setDisplayScale(float scale)
    {
        this->scale_ = scale;
        const int horizontal = std::max(10, qRound(12 * scale));
        this->layout_->setContentsMargins(
            horizontal, std::max(7, qRound(8 * scale)), horizontal,
            std::max(12, qRound(14 * scale)));
        const int buttonHeight = std::max(28, qRound(30 * scale));
        const int buttonWidth = std::max(76, qRound(80 * scale));
        this->dismissButton_->setFixedHeight(buttonHeight);
        this->dismissButton_->setMinimumWidth(buttonWidth);
        this->disableButton_->setFixedHeight(buttonHeight);
        this->disableButton_->setMinimumWidth(buttonWidth);
        this->setFixedHeight(this->preferredHeight());
        this->update();
    }

    void setAppearance(QColor background, QColor border, QColor text,
                       const QFont &font)
    {
        this->background_ = std::move(background);
        this->border_ = std::move(border);
        auto palette = this->label_->palette();
        palette.setColor(QPalette::WindowText, text);
        this->label_->setPalette(palette);
        this->label_->setFont(font);
        this->dismissButton_->setFont(font);
        this->disableButton_->setFont(font);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const qreal arrowHeight = std::max<qreal>(7.0, 8.0 * this->scale_);
        const auto bubble =
            QRectF(this->rect()).adjusted(4, 1, -4, -arrowHeight - 1);
        QPainterPath bubblePath;
        bubblePath.addRoundedRect(bubble, 6 * this->scale_,
                                  6 * this->scale_);
        const qreal arrowLeft = bubble.left() + 24 * this->scale_;
        QPainterPath arrowPath;
        arrowPath.moveTo(arrowLeft, bubble.bottom() - 1);
        arrowPath.lineTo(arrowLeft + 8 * this->scale_,
                         this->height() - 1);
        arrowPath.lineTo(arrowLeft + 16 * this->scale_,
                         bubble.bottom() - 1);
        arrowPath.closeSubpath();

        painter.setPen(QPen(this->border_, 1));
        painter.setBrush(this->background_);
        painter.drawPath(bubblePath.united(arrowPath));
    }

private:
    QHBoxLayout *layout_;
    QLabel *label_;
    QPushButton *dismissButton_;
    QPushButton *disableButton_;
    float scale_ = 1.0F;
    QColor background_;
    QColor border_;
};

}  // namespace

struct EmoteBar::Impl {
    Impl(EmoteBar *owner, ChannelProvider channelProvider,
         SendChannelsProvider sendChannelsProvider,
         ActivateCallback activateCallback,
         LayoutChangedCallback layoutChangedCallback)
        : owner(owner)
        , channelProvider(std::move(channelProvider))
        , sendChannelsProvider(std::move(sendChannelsProvider))
        , layoutChanged(std::move(layoutChangedCallback))
        , layout(new QVBoxLayout(owner))
        , introduction(new EmoteBarIntroduction(owner))
        , mostUsedRow(new EmoteBarRow(activateCallback, owner))
        , recentRow(new EmoteBarRow(std::move(activateCallback), owner))
    {
        this->layout->setContentsMargins(0, 0, 0, 0);
        this->layout->setSpacing(0);
        this->layout->addWidget(this->introduction);
        this->layout->addWidget(this->mostUsedRow);
        this->layout->addWidget(this->recentRow);
        owner->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

        const std::function<void()> onSettingChanged = [this] {
            this->owner->refresh();
        };
        emoteBarModeSetting().connect(onSettingChanged, this->connections,
                                      false);
        emoteBarScopeSetting().connect(onSettingChanged, this->connections,
                                       false);
        emoteBarMaxEmotesSetting().connect(onSettingChanged,
                                           this->connections, false);
        emoteBarHistoryJsonSetting().connect(onSettingChanged,
                                             this->connections, false);
        emoteBarIntroductionDismissedSetting().connect(
            onSettingChanged, this->connections, false);
        this->connections.managedConnect(
            getApp()->getFonts()->fontChanged, [this] {
                this->applyAppearance();
            });
        this->connections.managedConnect(
            getApp()->getWindows()->layoutRequested, [this](Channel *) {
                this->owner->refresh();
            });
        this->connections.managedConnect(
            getApp()->getWindows()->gifRepaintRequested, [this] {
                if (this->owner->isVisible())
                {
                    this->mostUsedRow->refreshFrames();
                    this->recentRow->refreshFrames();
                }
            });
    }

    int preferredHeight() const
    {
        const auto mode = emoteBarModeSetting().getEnum();
        if (mode == EmoteBarMode::Disabled)
        {
            return 0;
        }
        const int rows = mode == EmoteBarMode::Combined ? 2 : 1;
        return rows * std::max(30, qRound(32 * this->owner->scale())) +
               this->contentTop();
    }

    int contentTop() const
    {
        return emoteBarIntroductionDismissedSetting().getValue()
                   ? 0
                   : this->introduction->preferredHeight();
    }

    void applyAppearance()
    {
        auto font = getApp()->getFonts()->getFont(
            FontStyle::ChatMediumSmall, this->owner->scale());
        font.setPointSizeF(font.pointSizeF() * 0.94);
        this->mostUsedRow->setAppearance(this->textColor, this->mutedColor,
                                         this->hoverColor, this->focusColor,
                                         font);
        this->recentRow->setAppearance(this->textColor, this->mutedColor,
                                       this->hoverColor, this->focusColor,
                                       font);
        auto introductionFont = getApp()->getFonts()->getFont(
            FontStyle::ChatMedium, this->owner->scale());
        this->introduction->setAppearance(
            this->introductionBackgroundColor, this->introductionBorderColor,
            this->textColor, introductionFont);
    }

    void refresh()
    {
        const auto mode = emoteBarModeSetting().getEnum();
        const int oldHeight = this->lastPreferredHeight;
        this->lastPreferredHeight = this->preferredHeight();
        this->owner->setVisible(mode != EmoteBarMode::Disabled);
        if (mode == EmoteBarMode::Disabled)
        {
            if (oldHeight != this->lastPreferredHeight && this->layoutChanged)
            {
                this->layoutChanged();
            }
            return;
        }

        this->owner->setFixedHeight(this->lastPreferredHeight);
        this->introduction->setVisible(
            !emoteBarIntroductionDismissedSetting().getValue());
        const bool showMostUsed = mode == EmoteBarMode::MostUsed ||
                                  mode == EmoteBarMode::Combined;
        const bool showRecent = mode == EmoteBarMode::Recent ||
                                mode == EmoteBarMode::Combined;
        this->mostUsedRow->setVisible(showMostUsed);
        this->recentRow->setVisible(showRecent);

        const auto store = parseStore(emoteBarHistoryJsonSetting().getValue());
        const auto data =
            store.channels.value(channelKey(this->channelProvider()));
        auto usage = data.usage;
        usage.erase(
            std::remove_if(usage.begin(), usage.end(), [](const auto &entry) {
                return entry.count < EMOTE_BAR_MOST_USED_MIN_COUNT;
            }),
            usage.end());
        std::sort(usage.begin(), usage.end(), [](const auto &left,
                                                 const auto &right) {
            if (left.count != right.count)
            {
                return left.count > right.count;
            }
            return left.name.compare(right.name, Qt::CaseInsensitive) < 0;
        });

        const auto candidates = buildCandidates(this->sendChannelsProvider());
        const auto scope = emoteBarScopeSetting().getEnum();
        const int maximum =
            std::clamp(emoteBarMaxEmotesSetting().getValue(), 1, 20);
        auto mostUsed = resolveEntries(usage, candidates, scope, maximum, true);
        QSet<QString> usedIdentities;
        if (showMostUsed && showRecent)
        {
            for (const auto &entry : mostUsed)
            {
                usedIdentities.insert(entry.identity);
            }
        }
        auto recent = resolveEntries(data.recent, candidates, scope, maximum,
                                     false, usedIdentities);

        this->mostUsedRow->setEntries(
            std::move(mostUsed),
            QStringLiteral(
                "Most-used emotes appear here after you send them."));
        this->recentRow->setEntries(
            std::move(recent),
            QStringLiteral(
                "Recent emotes appear here after you send them."));
        this->applyAppearance();
        this->owner->update();
        if (oldHeight != this->lastPreferredHeight && this->layoutChanged)
        {
            this->layoutChanged();
        }
    }

    void recordMessage(const QString &message)
    {
        const auto key = channelKey(this->channelProvider());
        if (key.isEmpty() || message.trimmed().isEmpty())
        {
            return;
        }
        const auto candidates = buildCandidates(this->sendChannelsProvider());
        std::vector<StoredEntry> resolved;
        const auto tokens =
            message.split(QRegularExpression(QStringLiteral("\\s+")),
                          Qt::SkipEmptyParts);
        for (auto token : tokens)
        {
            token = cleanToken(std::move(token));
            const auto found = candidates.byName.constFind(token);
            if (found != candidates.byName.cend())
            {
                resolved.push_back(found->entry);
            }
        }
        if (resolved.empty())
        {
            return;
        }

        auto store = parseStore(emoteBarHistoryJsonSetting().getValue());
        auto &data = store.channels[key];
        for (const auto &entry : resolved)
        {
            const auto identity = entryIdentity(entry);
            data.recent.erase(
                std::remove_if(data.recent.begin(), data.recent.end(),
                               [&identity](const auto &item) {
                                   return entryIdentity(item) == identity;
                               }),
                data.recent.end());
            data.recent.insert(data.recent.begin(), entry);

            const auto usage = std::find_if(
                data.usage.begin(), data.usage.end(),
                [&identity](const auto &item) {
                    return entryIdentity(item) == identity;
                });
            if (usage == data.usage.end())
            {
                auto usageEntry = entry;
                usageEntry.count = 1;
                data.usage.push_back(std::move(usageEntry));
            }
            else
            {
                usage->name = entry.name;
                usage->count = std::min(std::numeric_limits<int>::max(),
                                        usage->count + 1);
            }
        }
        if (data.recent.size() > EMOTE_BAR_HISTORY_LIMIT)
        {
            data.recent.resize(EMOTE_BAR_HISTORY_LIMIT);
        }
        std::sort(data.usage.begin(), data.usage.end(),
                  [](const auto &left, const auto &right) {
                      if (left.count != right.count)
                      {
                          return left.count > right.count;
                      }
                      return left.name.compare(right.name,
                                               Qt::CaseInsensitive) < 0;
                  });
        if (data.usage.size() > EMOTE_BAR_USAGE_LIMIT)
        {
            data.usage.resize(EMOTE_BAR_USAGE_LIMIT);
        }
        data.updatedAt = QDateTime::currentMSecsSinceEpoch();
        emoteBarHistoryJsonSetting() = serializeStore(store);
    }

    void showContextMenu(QContextMenuEvent *event)
    {
        auto store = parseStore(emoteBarHistoryJsonSetting().getValue());
        const auto key = channelKey(this->channelProvider());
        const auto found = store.channels.constFind(key);
        QMenu menu(this->owner);
        auto *clearRecent = menu.addAction(
            QStringLiteral("Clear recent emotes for this channel"));
        auto *clearMostUsed = menu.addAction(
            QStringLiteral("Clear most-used emotes for this channel"));
        clearRecent->setEnabled(found != store.channels.cend() &&
                                !found->recent.empty());
        clearMostUsed->setEnabled(found != store.channels.cend() &&
                                  !found->usage.empty());
        menu.addSeparator();
        auto *clearAll =
            menu.addAction(QStringLiteral("Clear all emote bar history"));
        clearAll->setEnabled(!store.channels.isEmpty());
        const auto *selected = menu.exec(event->globalPos());
        if (!selected)
        {
            return;
        }
        if (selected == clearAll)
        {
            const auto answer = QMessageBox::question(
                this->owner, QStringLiteral("Clear emote bar history?"),
                QStringLiteral("Clear recent and most-used emotes for every "
                               "channel? This cannot be undone."),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (answer == QMessageBox::Yes)
            {
                emoteBarHistoryJsonSetting() =
                    QStringLiteral(R"({"version":1,"channels":[]})");
            }
            return;
        }

        auto mutableFound = store.channels.find(key);
        if (mutableFound == store.channels.end())
        {
            return;
        }
        if (selected == clearRecent)
        {
            mutableFound->recent.clear();
        }
        else if (selected == clearMostUsed)
        {
            mutableFound->usage.clear();
        }
        if (mutableFound->recent.empty() && mutableFound->usage.empty())
        {
            store.channels.erase(mutableFound);
        }
        emoteBarHistoryJsonSetting() = serializeStore(store);
    }

    EmoteBar *owner;
    ChannelProvider channelProvider;
    SendChannelsProvider sendChannelsProvider;
    LayoutChangedCallback layoutChanged;
    QVBoxLayout *layout;
    EmoteBarIntroduction *introduction;
    EmoteBarRow *mostUsedRow;
    EmoteBarRow *recentRow;
    pajlada::Signals::SignalHolder connections;
    int lastPreferredHeight = 0;
    QColor backgroundColor;
    QColor borderColor;
    QColor textColor;
    QColor mutedColor;
    QColor hoverColor;
    QColor focusColor;
    QColor introductionBackgroundColor;
    QColor introductionBorderColor;
};

EmoteBar::EmoteBar(QWidget *parent, ChannelProvider channelProvider,
                   SendChannelsProvider sendChannelsProvider,
                   ActivateCallback activateCallback,
                   LayoutChangedCallback layoutChangedCallback)
    : BaseWidget(parent)
    , impl_(std::make_unique<Impl>(
          this, std::move(channelProvider), std::move(sendChannelsProvider),
          std::move(activateCallback), std::move(layoutChangedCallback)))
{
    this->impl_->introduction->setDisplayScale(this->scale());
    this->impl_->mostUsedRow->setDisplayScale(this->scale());
    this->impl_->recentRow->setDisplayScale(this->scale());
    this->themeChangedEvent();
    this->refresh();
}

EmoteBar::~EmoteBar() = default;

void EmoteBar::refresh()
{
    this->impl_->refresh();
}

void EmoteBar::recordMessage(const QString &message)
{
    this->impl_->recordMessage(message);
}

int EmoteBar::preferredHeight() const
{
    return this->impl_->preferredHeight();
}

int EmoteBar::contentTop() const
{
    return this->impl_->contentTop();
}

void EmoteBar::scaleChangedEvent(float scale)
{
    this->impl_->introduction->setDisplayScale(scale);
    this->impl_->mostUsedRow->setDisplayScale(scale);
    this->impl_->recentRow->setDisplayScale(scale);
    this->impl_->refresh();
}

void EmoteBar::themeChangedEvent()
{
    this->impl_->backgroundColor = this->theme->splits.input.background;
    this->impl_->borderColor = this->theme->splits.header.border;
    this->impl_->textColor = this->theme->splits.input.text;
    this->impl_->mutedColor = this->theme->splits.input.text;
    this->impl_->mutedColor.setAlpha(115);
    this->impl_->borderColor.setAlpha(70);
    this->impl_->hoverColor = this->theme->splits.header.focusedBackground;
    this->impl_->hoverColor.setAlpha(90);
    this->impl_->focusColor = this->theme->splits.header.focusedBorder;
    this->impl_->focusColor.setAlpha(115);
    this->impl_->introductionBackgroundColor =
        this->theme->splits.header.background;
    this->impl_->introductionBorderColor =
        this->theme->splits.header.focusedBorder;
    this->impl_->applyAppearance();
    this->update();
}

void EmoteBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setPen(QPen(this->impl_->borderColor, 1));
    if (emoteBarModeSetting().getEnum() == EmoteBarMode::Combined)
    {
        const int introductionHeight = this->impl_->contentTop();
        const int rowAreaHeight = this->height() - introductionHeight;
        painter.drawLine(0, introductionHeight + rowAreaHeight / 2,
                         this->width(),
                         introductionHeight + rowAreaHeight / 2);
    }
    painter.drawLine(0, this->height() - 1, this->width(),
                     this->height() - 1);
}

void EmoteBar::contextMenuEvent(QContextMenuEvent *event)
{
    this->impl_->showContextMenu(event);
    event->accept();
}

}  // namespace chatterino
