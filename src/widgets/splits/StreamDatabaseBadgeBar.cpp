// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/StreamDatabaseBadgeBar.hpp"

#include "Application.hpp"
#include "common/ChatterinoSetting.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/MessageFlag.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/CurrentUserBadges.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadgeIdentity.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/colors/ColorProvider.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "widgets/splits/Split.hpp"

#include <QAbstractAnimation>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QLinearGradient>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSet>
#include <QSizePolicy>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QTime>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace chatterino {
namespace {

constexpr int TICKER_INTERVAL_MS = 3300;
constexpr int TICKER_SLIDE_DURATION_MS = 260;
constexpr int MANUAL_TICKER_SLIDE_DURATION_MS = 210;
constexpr int NAVIGATION_HOVER_DURATION_MS = 155;
constexpr int EVENTS_REFRESH_INTERVAL_MS = 15000;
constexpr int EVENTS_REQUEST_TIMEOUT_MS = 10000;
constexpr int VISIBLE_BADGE_COUNT = 5;
constexpr auto STREAMDATABASE_LOGIN = "streamdatabase";
constexpr auto STREAMDATABASE_SITE =
    "https://www.streamdatabase.com/";
constexpr auto STREAMDATABASE_EVENTS_API =
    "https://api.streamdatabase.com/events";
constexpr auto STREAMDATABASE_LOGO_URL =
    "https://www.streamdatabase.com/logo.svg";
constexpr auto SETTINGS_MENU_CLOSED_AT_PROPERTY =
    "streamDatabaseSettingsMenuClosedAt";
constexpr auto SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY =
    "streamDatabaseSettingsMenuCursorOverride";
constexpr qint64 SETTINGS_MENU_REOPEN_SUPPRESS_MS = 250;

enum class OwnedBadgeFeedMode {
    ShowAll = 0,
    HideOwned = 1,
    CheckmarkOwned = 2,
};

enum class BadgeActivityFeedMode {
    ActiveOnly = 0,
    All = 1,
    UpcomingOnly = 2,
};

enum class BadgePingMode {
    NewBadges = 0,
    AvailableNow = 1,
    None = 2,
    Both = 3,
};

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
}

BoolSetting &streamDatabaseBadgePingsSetting()
{
    static BoolSetting setting = {
        "/streamdatabase/badgePings",
        true,
    };
    return setting;
}

IntSetting &streamDatabaseBadgePingModeSetting()
{
    static IntSetting setting = {
        "/streamdatabase/badgePingMode",
        streamDatabaseBadgePingsSetting()
            ? static_cast<int>(BadgePingMode::NewBadges)
            : static_cast<int>(BadgePingMode::None),
    };
    return setting;
}

BadgePingMode streamDatabaseBadgePingMode()
{
    const int value = streamDatabaseBadgePingModeSetting();
    switch (value)
    {
        case static_cast<int>(BadgePingMode::NewBadges):
            return BadgePingMode::NewBadges;
        case static_cast<int>(BadgePingMode::AvailableNow):
            return BadgePingMode::AvailableNow;
        case static_cast<int>(BadgePingMode::None):
            return BadgePingMode::None;
        case static_cast<int>(BadgePingMode::Both):
            return BadgePingMode::Both;
        default:
            return BadgePingMode::NewBadges;
    }
}

void setStreamDatabaseBadgePingMode(BadgePingMode mode)
{
    streamDatabaseBadgePingModeSetting() = static_cast<int>(mode);
    streamDatabaseBadgePingsSetting() = mode != BadgePingMode::None;
    if (auto *settings = getSettings())
    {
        (void)settings->requestSave();
    }
}

IntSetting &streamDatabaseOwnedBadgeFeedModeSetting()
{
    static IntSetting setting = {
        "/streamdatabase/ownedBadgeFeedMode",
        static_cast<int>(OwnedBadgeFeedMode::CheckmarkOwned),
    };
    return setting;
}

OwnedBadgeFeedMode streamDatabaseOwnedBadgeFeedMode()
{
    const int value = streamDatabaseOwnedBadgeFeedModeSetting();
    switch (value)
    {
        case static_cast<int>(OwnedBadgeFeedMode::ShowAll):
            return OwnedBadgeFeedMode::ShowAll;
        case static_cast<int>(OwnedBadgeFeedMode::HideOwned):
            return OwnedBadgeFeedMode::HideOwned;
        case static_cast<int>(OwnedBadgeFeedMode::CheckmarkOwned):
            return OwnedBadgeFeedMode::CheckmarkOwned;
        default:
            return OwnedBadgeFeedMode::CheckmarkOwned;
    }
}

void setStreamDatabaseOwnedBadgeFeedMode(OwnedBadgeFeedMode mode)
{
    streamDatabaseOwnedBadgeFeedModeSetting() = static_cast<int>(mode);
    if (auto *settings = getSettings())
    {
        (void)settings->requestSave();
    }
}

IntSetting &streamDatabaseBadgeActivityFeedModeSetting()
{
    static IntSetting setting = {
        "/streamdatabase/badgeActivityFeedMode",
        static_cast<int>(BadgeActivityFeedMode::All),
    };
    return setting;
}

BadgeActivityFeedMode streamDatabaseBadgeActivityFeedMode()
{
    const int value = streamDatabaseBadgeActivityFeedModeSetting();
    switch (value)
    {
        case static_cast<int>(BadgeActivityFeedMode::ActiveOnly):
            return BadgeActivityFeedMode::ActiveOnly;
        case static_cast<int>(BadgeActivityFeedMode::All):
            return BadgeActivityFeedMode::All;
        case static_cast<int>(BadgeActivityFeedMode::UpcomingOnly):
            return BadgeActivityFeedMode::UpcomingOnly;
        default:
            return BadgeActivityFeedMode::All;
    }
}

void setStreamDatabaseBadgeActivityFeedMode(BadgeActivityFeedMode mode)
{
    streamDatabaseBadgeActivityFeedModeSetting() = static_cast<int>(mode);
    if (auto *settings = getSettings())
    {
        (void)settings->requestSave();
    }
}

struct ChannelTextLinkRange {
    int start = 0;
    int length = 0;
    QString login;
    QString url;
};

bool channelLinkBoundary(const QString &text, int index)
{
    if (index < 0 || index >= text.size())
    {
        return true;
    }

    const QChar ch = text.at(index);
    return !(ch.isLetterOrNumber() || ch == QLatin1Char('_'));
}

QUrl twitchChannelUrl(QString login);

std::vector<ChannelTextLinkRange> channelLinkRangesForText(
    const QString &text,
    const std::vector<StreamDatabaseBadgeChannelLink> &channelLinks,
    const std::vector<StreamDatabaseBadgeTextLink> &textLinks)
{
    struct Candidate {
        QString text;
        QString login;
        QString url;
    };

    std::vector<Candidate> candidates;
    candidates.reserve(channelLinks.size() * 2 + textLinks.size());
    for (const auto &link : channelLinks)
    {
        const auto url = twitchChannelUrl(link.login).toString();
        if (!link.name.isEmpty())
        {
            candidates.push_back({link.name, link.login, url});
        }
        if (!link.login.isEmpty() &&
            link.login.compare(link.name, Qt::CaseInsensitive) != 0)
        {
            candidates.push_back({link.login, link.login, url});
        }
    }
    for (const auto &link : textLinks)
    {
        if (!link.text.isEmpty() && !link.url.isEmpty())
        {
            candidates.push_back({link.text, {}, link.url});
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate &a, const Candidate &b) {
                  return a.text.size() > b.text.size();
              });

    std::vector<bool> occupied(static_cast<size_t>(text.size()), false);
    std::vector<ChannelTextLinkRange> ranges;
    for (const auto &candidate : candidates)
    {
        if (candidate.text.isEmpty() || candidate.url.isEmpty())
        {
            continue;
        }

        int from = 0;
        while (from < text.size())
        {
            const int index =
                text.indexOf(candidate.text, from, Qt::CaseInsensitive);
            if (index < 0)
            {
                break;
            }

            const int end = index + candidate.text.size();
            bool overlaps = false;
            for (int i = index; i < end && i < text.size(); ++i)
            {
                if (occupied[static_cast<size_t>(i)])
                {
                    overlaps = true;
                    break;
                }
            }

            if (!overlaps && channelLinkBoundary(text, index - 1) &&
                channelLinkBoundary(text, end))
            {
                for (int i = index; i < end && i < text.size(); ++i)
                {
                    occupied[static_cast<size_t>(i)] = true;
                }
                ranges.push_back(
                    {index, static_cast<int>(candidate.text.size()),
                     candidate.login, candidate.url});
            }

            from = end;
        }
    }

    std::sort(ranges.begin(), ranges.end(),
              [](const ChannelTextLinkRange &a,
                 const ChannelTextLinkRange &b) {
                  return a.start < b.start;
              });
    return ranges;
}

QUrl twitchChannelUrl(QString login)
{
    login = login.trimmed();
    if (login.startsWith(QLatin1Char('@')))
    {
        login.remove(0, 1);
    }
    return QUrl(QStringLiteral("https://www.twitch.tv/%1").arg(login));
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

int scaledInt(float scale, int value)
{
    return int(std::round(value * scale));
}

QFont smallerFont(QFont font, qreal factor)
{
    if (font.pixelSize() > 0)
    {
        font.setPixelSize(
            std::max(1, int(std::round(font.pixelSize() * factor))));
    }
    else if (font.pointSizeF() > 0)
    {
        font.setPointSizeF(std::max<qreal>(1.0, font.pointSizeF() * factor));
    }

    return font;
}

struct ParsedDateTime {
    QDateTime value;
    bool hasTime = false;
};

QDate parseStreamDatabaseDate(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    return QDate::fromString(value.left(10), Qt::ISODate);
}

QTime parseStreamDatabaseTime(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    const int dateSeparator = value.indexOf(QLatin1Char('T'));
    if (dateSeparator >= 0)
    {
        value = value.mid(dateSeparator + 1);
    }

    if (value.endsWith(QLatin1Char('Z')))
    {
        value.chop(1);
    }

    const int plusIndex = value.indexOf(QLatin1Char('+'));
    if (plusIndex >= 0)
    {
        value = value.left(plusIndex);
    }

    const int minusIndex = value.indexOf(QLatin1Char('-'), 1);
    if (minusIndex >= 0)
    {
        value = value.left(minusIndex);
    }

    const int dotIndex = value.indexOf(QLatin1Char('.'));
    if (dotIndex >= 0 && value.size() > dotIndex + 4)
    {
        value = value.left(dotIndex + 4);
    }

    for (const auto &format :
         {QStringLiteral("HH:mm:ss.zzz"), QStringLiteral("HH:mm:ss"),
          QStringLiteral("HH:mm")})
    {
        const auto time = QTime::fromString(value, format);
        if (time.isValid())
        {
            return time;
        }
    }

    return {};
}

ParsedDateTime parseStreamDatabaseDateTime(const QJsonObject &object,
                                           const char *dateKey,
                                           const char *timeKey,
                                           bool endOfDayWhenTimeMissing)
{
    const QDate date = parseStreamDatabaseDate(
        object.value(QString::fromLatin1(dateKey)).toString());
    if (!date.isValid())
    {
        return {};
    }

    QTime time = parseStreamDatabaseTime(
        object.value(QString::fromLatin1(timeKey)).toString());
    const bool hasTime = time.isValid();
    if (!hasTime)
    {
        time = endOfDayWhenTimeMissing ? QTime(23, 59, 59) : QTime(0, 0);
    }

    return {QDateTime(date, time, Qt::UTC), hasTime};
}

bool dateTimeWindowContains(const ParsedDateTime &start,
                            const ParsedDateTime &end,
                            const QDateTime &now)
{
    return (!start.value.isValid() || start.value <= now) &&
           (!end.value.isValid() || now <= end.value);
}

bool dateTimeWindowStartsInFuture(const ParsedDateTime &start,
                                  const ParsedDateTime &end,
                                  const QDateTime &now)
{
    return start.value.isValid() && now < start.value &&
           (!end.value.isValid() || now <= end.value);
}

bool hasDateWindow(const ParsedDateTime &start, const ParsedDateTime &end)
{
    return start.value.isValid() || end.value.isValid();
}

std::vector<QJsonObject> availabilityObjects(const QJsonArray &availability)
{
    std::vector<QJsonObject> result;

    for (const auto &entry : availability)
    {
        const auto object = entry.toObject();
        if (!object.isEmpty())
        {
            result.push_back(object);
        }
    }

    return result;
}

std::vector<QJsonObject> activeAvailabilityObjects(
    const QJsonArray &availability, const QDateTime &now, bool eventActive)
{
    std::vector<QJsonObject> active;

    for (const auto &entry : availability)
    {
        const auto object = entry.toObject();
        if (object.isEmpty())
        {
            continue;
        }

        const auto start =
            parseStreamDatabaseDateTime(object, "start_at_date",
                                        "start_at_time", false);
        const auto end = parseStreamDatabaseDateTime(
            object, "end_at_date", "end_at_time", true);
        const bool availabilityHasDates = hasDateWindow(start, end);
        if ((availabilityHasDates &&
             dateTimeWindowContains(start, end, now)) ||
            (!availabilityHasDates && eventActive))
        {
            active.push_back(object);
        }
    }

    return active;
}

std::vector<QJsonObject> upcomingAvailabilityObjects(
    const QJsonArray &availability, const QDateTime &now, bool eventUpcoming)
{
    std::vector<QJsonObject> upcoming;

    for (const auto &entry : availability)
    {
        const auto object = entry.toObject();
        if (object.isEmpty())
        {
            continue;
        }

        const auto start =
            parseStreamDatabaseDateTime(object, "start_at_date",
                                        "start_at_time", false);
        const auto end = parseStreamDatabaseDateTime(
            object, "end_at_date", "end_at_time", true);
        const bool availabilityHasDates = hasDateWindow(start, end);
        if ((availabilityHasDates &&
             dateTimeWindowStartsInFuture(start, end, now)) ||
            (!availabilityHasDates && eventUpcoming))
        {
            upcoming.push_back(object);
        }
    }

    return upcoming;
}

ParsedDateTime bestEndDateTime(const std::vector<QJsonObject> &availability,
                               const QJsonObject &event)
{
    ParsedDateTime best;
    for (const auto &object : availability)
    {
        const auto end = parseStreamDatabaseDateTime(
            object, "end_at_date", "end_at_time", true);
        if (!end.value.isValid())
        {
            continue;
        }

        if (!best.value.isValid() || end.value < best.value)
        {
            best = end;
        }
    }

    if (best.value.isValid())
    {
        return best;
    }

    return parseStreamDatabaseDateTime(event, "end_at_date", "end_at_time",
                                       true);
}

QString statusTextForBadge(bool active, bool upcoming,
                           const ParsedDateTime &eventStart,
                           const ParsedDateTime &eventEnd,
                           const QDateTime &now)
{
    if (active)
    {
        return QStringLiteral("ACTIVE");
    }
    if (upcoming || (eventStart.value.isValid() && now < eventStart.value))
    {
        return QStringLiteral("UPCOMING");
    }
    if (eventEnd.value.isValid() && eventEnd.value < now)
    {
        return QStringLiteral("INACTIVE");
    }
    return QStringLiteral("INACTIVE");
}

QString formatEndsText(const ParsedDateTime &end)
{
    if (!end.value.isValid())
    {
        return QStringLiteral("ENDS TBA");
    }

    const QString format = end.hasTime ? QStringLiteral("MMM d, HH:mm 'UTC'")
                                       : QStringLiteral("MMM d");
    return QStringLiteral("ENDS ") +
           QLocale::c().toString(end.value.toUTC(), format).toUpper();
}

QString formatAvailabilityWindowText(const ParsedDateTime &end,
                                     bool active, const QDateTime &now)
{
    if (active)
    {
        return formatEndsText(end);
    }
    if (!end.value.isValid())
    {
        return QStringLiteral("DATE TBA");
    }

    const QString format = end.hasTime ? QStringLiteral("MMM d, HH:mm 'UTC'")
                                       : QStringLiteral("MMM d");
    const QString prefix = end.value < now ? QStringLiteral("ENDED ")
                                           : QStringLiteral("ENDS ");
    return prefix + QLocale::c().toString(end.value.toUTC(), format).toUpper();
}

void addUnique(QStringList &list, QSet<QString> &seen, QString value)
{
    value = value.trimmed();
    if (value.isEmpty() || seen.contains(value))
    {
        return;
    }

    seen.insert(value);
    list.push_back(value);
}

QString limitedList(const QStringList &values, int maxItems = 3)
{
    if (values.isEmpty())
    {
        return {};
    }

    if (values.size() <= maxItems)
    {
        return values.join(QStringLiteral(", "));
    }

    QStringList limited = values.mid(0, maxItems);
    return limited.join(QStringLiteral(", ")) +
           QStringLiteral(" + %1 more").arg(values.size() - maxItems);
}

void addUniqueChannelLink(std::vector<StreamDatabaseBadgeChannelLink> &links,
                          QSet<QString> &seen, QString name, QString login)
{
    name = name.trimmed();
    login = login.trimmed();
    if (name.isEmpty())
    {
        name = login;
    }
    if (login.isEmpty())
    {
        login = name;
    }

    const QString key = login.toCaseFolded();
    if (name.isEmpty() || key.isEmpty() || seen.contains(key))
    {
        return;
    }

    seen.insert(key);
    links.push_back({name, login});
}

QString twitchCategorySlug(QString value)
{
    value = value.trimmed().toLower();

    QString slug;
    bool previousDash = false;
    for (const auto ch : value)
    {
        if (ch.isLetterOrNumber())
        {
            slug.append(ch);
            previousDash = false;
        }
        else if (!slug.isEmpty() && !previousDash)
        {
            slug.append(QLatin1Char('-'));
            previousDash = true;
        }
    }

    while (slug.endsWith(QLatin1Char('-')))
    {
        slug.chop(1);
    }
    return slug;
}

QString twitchCategoryUrl(QString nameOrSlug)
{
    const auto slug = twitchCategorySlug(nameOrSlug);
    if (slug.isEmpty())
    {
        return {};
    }

    return QStringLiteral("https://www.twitch.tv/directory/category/%1")
        .arg(QString::fromLatin1(QUrl::toPercentEncoding(slug)));
}

QString availabilityCategoryUrl(const QJsonObject &category,
                                const QJsonObject &game,
                                const QString &name)
{
    QString url = game.value("url").toString().trimmed();
    if (url.isEmpty())
    {
        url = category.value("url").toString().trimmed();
    }
    if (url.startsWith(QStringLiteral("//")))
    {
        return QStringLiteral("https:") + url;
    }
    if (url.startsWith(QLatin1Char('/')))
    {
        return QStringLiteral("https://www.twitch.tv") + url;
    }
    if (url.startsWith(QStringLiteral("http://")) ||
        url.startsWith(QStringLiteral("https://")))
    {
        return url;
    }

    QString slug = game.value("slug").toString().trimmed();
    if (slug.isEmpty())
    {
        slug = category.value("slug").toString().trimmed();
    }
    return twitchCategoryUrl(slug.isEmpty() ? name : slug);
}

void addUniqueTextLink(std::vector<StreamDatabaseBadgeTextLink> &links,
                       QSet<QString> &seen, QString text, QString url)
{
    text = text.trimmed();
    url = url.trimmed();
    const QString key = text.toCaseFolded();
    if (text.isEmpty() || url.isEmpty() || key.isEmpty() || seen.contains(key))
    {
        return;
    }

    seen.insert(key);
    links.push_back({text, url});
}

std::vector<StreamDatabaseBadgeChannelLink> availabilityChannelLinks(
    const std::vector<QJsonObject> &availability)
{
    std::vector<StreamDatabaseBadgeChannelLink> links;
    QSet<QString> seen;

    for (const auto &object : availability)
    {
        for (const auto &channelValue : object.value("channels").toArray())
        {
            const auto channel = channelValue.toObject();
            const auto user = channel.value("user").toObject();
            QString login = user.value("login").toString();
            if (login.isEmpty())
            {
                login = channel.value("login").toString();
            }

            QString name = user.value("display_name").toString();
            if (name.isEmpty())
            {
                name = login;
            }
            if (name.isEmpty())
            {
                name = channel.value("display_name").toString();
            }

            addUniqueChannelLink(links, seen, name, login);
        }
    }

    return links;
}

QStringList availabilityChannelNames(
    const std::vector<QJsonObject> &availability)
{
    QStringList names;
    QSet<QString> seen;
    for (const auto &link : availabilityChannelLinks(availability))
    {
        addUnique(names, seen, link.name);
    }

    return names;
}

std::vector<StreamDatabaseBadgeTextLink> availabilityCategoryLinks(
    const std::vector<QJsonObject> &availability)
{
    std::vector<StreamDatabaseBadgeTextLink> links;
    QSet<QString> seen;

    for (const auto &object : availability)
    {
        for (const auto &categoryValue : object.value("categories").toArray())
        {
            const auto category = categoryValue.toObject();
            const auto game = category.value("game").toObject();
            QString name = game.value("name").toString();
            if (name.isEmpty())
            {
                name = category.value("name").toString();
            }

            addUniqueTextLink(links, seen, name,
                              availabilityCategoryUrl(category, game, name));
        }
    }

    return links;
}

QStringList availabilityCategoryNames(
    const std::vector<QJsonObject> &availability)
{
    QStringList names;
    QSet<QString> seen;
    for (const auto &link : availabilityCategoryLinks(availability))
    {
        addUnique(names, seen, link.text);
    }

    return names;
}

QString cleanStreamDatabaseText(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("\\s+")),
                 QStringLiteral(" "));
    text = text.trimmed();

    constexpr int MAX_LENGTH = 210;
    if (text.size() <= MAX_LENGTH)
    {
        return text;
    }

    const int sentenceEnd = text.lastIndexOf(QLatin1Char('.'), MAX_LENGTH);
    if (sentenceEnd >= 80)
    {
        return text.left(sentenceEnd + 1);
    }

    return text.left(MAX_LENGTH).trimmed() + QStringLiteral("...");
}

QString fallbackRequirementText(const QString &eventContent,
                                const QString &badgeDescription)
{
    QString fallback = cleanStreamDatabaseText(eventContent);
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    fallback = cleanStreamDatabaseText(badgeDescription);
    if (!fallback.isEmpty())
    {
        return fallback;
    }

    return QStringLiteral("Check StreamDatabase for the current badge rules.");
}

bool jsonBool(const QJsonObject &object, const char *key)
{
    return object.value(QString::fromLatin1(key)).toBool(false);
}

QString requirementTextFromAvailability(
    const std::vector<QJsonObject> &availability, const QString &eventContent,
    const QString &badgeDescription)
{
    if (availability.empty())
    {
        return fallbackRequirementText(eventContent, badgeDescription);
    }

    bool subscription = false;
    bool subscriptionGift = false;
    bool watch = false;
    bool clip = false;
    bool bits = false;
    bool twitchcon = false;
    bool turbo = false;
    int watchMinutes = 0;

    for (const auto &object : availability)
    {
        subscription = subscription || jsonBool(object, "subscription");
        subscriptionGift =
            subscriptionGift || jsonBool(object, "subscription_gift");
        watch = watch || jsonBool(object, "watch");
        clip = clip || jsonBool(object, "clip");
        bits = bits || jsonBool(object, "bits");
        twitchcon = twitchcon || jsonBool(object, "twitchcon");
        turbo = turbo || jsonBool(object, "turbo");
        watchMinutes =
            std::max(watchMinutes, object.value("watch_minutes").toInt());
    }

    QString action;
    if (subscription && subscriptionGift)
    {
        action = QStringLiteral("Subscribe once or gift one subscription");
    }
    else if (subscription)
    {
        action = QStringLiteral("Subscribe once");
    }
    else if (subscriptionGift)
    {
        action = QStringLiteral("Gift one subscription");
    }
    else if (watch && clip)
    {
        action = QStringLiteral("Watch a clip, VOD, or live stream");
    }
    else if (watch)
    {
        action = watchMinutes > 0
                     ? QStringLiteral("Watch %1 minutes").arg(watchMinutes)
                     : QStringLiteral("Watch an eligible stream");
    }
    else if (bits)
    {
        action = QStringLiteral("Cheer Bits");
    }
    else if (twitchcon)
    {
        action = QStringLiteral("Attend the TwitchCon event");
    }
    else if (turbo)
    {
        action = QStringLiteral("Have Twitch Turbo");
    }
    else
    {
        action = QStringLiteral("Complete the event requirements");
    }

    const QStringList channels = availabilityChannelNames(availability);
    const QStringList categories = availabilityCategoryNames(availability);
    const QString channelText = limitedList(channels);
    const QString categoryText = limitedList(categories);

    if (subscription || subscriptionGift)
    {
        if (!channelText.isEmpty() && !categoryText.isEmpty())
        {
            action += QStringLiteral(" to %1 while they are in %2")
                          .arg(channelText, categoryText);
        }
        else if (!channelText.isEmpty())
        {
            action += QStringLiteral(" to %1").arg(channelText);
        }
        else if (!categoryText.isEmpty())
        {
            action += QStringLiteral(" to an eligible %1 stream")
                          .arg(categoryText);
        }
    }
    else
    {
        if (!channelText.isEmpty() && !categoryText.isEmpty())
        {
            action += QStringLiteral(" while %1 %2 live in %3")
                          .arg(channelText,
                               channels.size() == 1 ? QStringLiteral("is")
                                                    : QStringLiteral("are"),
                               categoryText);
        }
        else if (!channelText.isEmpty())
        {
            action += QStringLiteral(" while %1 %2 live")
                          .arg(channelText,
                               channels.size() == 1 ? QStringLiteral("is")
                                                    : QStringLiteral("are"));
        }
        else if (!categoryText.isEmpty())
        {
            action += QStringLiteral(" on eligible %1 streams")
                          .arg(categoryText);
        }
    }

    if (!action.endsWith(QLatin1Char('.')))
    {
        action += QLatin1Char('.');
    }

    return action;
}

QString defaultBadgeAccent()
{
    return QStringLiteral("#22C55E");
}

QColor readableBadgeAccent(QColor color)
{
    if (!color.isValid())
    {
        return QColor(defaultBadgeAccent());
    }

    float h = 0.0F;
    float s = 0.0F;
    float l = 0.0F;
    float a = 0.0F;
    color.getHslF(&h, &s, &l, &a);

    l = std::clamp(l, 0.48F, 0.72F);
    if (s < 0.16)
    {
        s = 0.16F;
    }

    return QColor::fromHslF(h, s, l, 1.0);
}

QString accentFromBadgePixmap(const QPixmap &pixmap)
{
    const QImage image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    if (image.isNull())
    {
        return defaultBadgeAccent();
    }

    double totalWeight = 0.0;
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    const int step = std::max(1, std::min(image.width(), image.height()) / 24);

    for (int y = 0; y < image.height(); y += step)
    {
        for (int x = 0; x < image.width(); x += step)
        {
            const QColor pixel = QColor::fromRgba(image.pixel(x, y));
            if (pixel.alpha() < 48)
            {
                continue;
            }

            const qreal saturation = pixel.hslSaturationF();
            const qreal lightness = pixel.lightnessF();
            qreal weight = qreal(pixel.alpha()) / 255.0;
            weight *= 0.35 + saturation;
            weight *= 0.35 + std::sqrt(std::clamp(lightness, 0.0, 1.0));

            if (lightness < 0.08)
            {
                weight *= 0.15;
            }
            else if (lightness > 0.93 && saturation < 0.12)
            {
                weight *= 0.35;
            }

            totalWeight += weight;
            red += pixel.redF() * weight;
            green += pixel.greenF() * weight;
            blue += pixel.blueF() * weight;
        }
    }

    if (totalWeight <= 0.001)
    {
        return defaultBadgeAccent();
    }

    return readableBadgeAccent(QColor::fromRgbF(red / totalWeight,
                                                green / totalWeight,
                                                blue / totalWeight))
        .name(QColor::HexRgb);
}

QString siteUrlForBadge(const QString &setID, const QString &versionID)
{
    if (setID.isEmpty() || versionID.isEmpty())
    {
        return QString::fromLatin1(STREAMDATABASE_SITE);
    }

    return QStringLiteral(
               "https://www.streamdatabase.com/twitch/global-badges/%1/%2")
        .arg(setID, versionID);
}

QString badgeEventKey(const StreamDatabaseBadgeBar::BadgeItem &badge)
{
    auto key = StreamDatabaseBadgeBar::badgeKey(badge);
    if (!badge.eventTitle.trimmed().isEmpty())
    {
        key += QStringLiteral("::event:") + badge.eventTitle.trimmed();
    }
    return key;
}

QString badgeAvailabilityKey(const StreamDatabaseBadgeBar::BadgeItem &badge)
{
    auto key = badgeEventKey(badge);
    if (!badge.endsText.trimmed().isEmpty())
    {
        key += QStringLiteral("::window:") + badge.endsText.trimmed();
    }
    if (!badge.requirementText.trimmed().isEmpty())
    {
        key += QStringLiteral("::rules:") + badge.requirementText.trimmed();
    }
    return key;
}

class StreamDatabaseEventsPoller final : public QObject
{
public:
    StreamDatabaseEventsPoller()
        : network_(this)
    {
        this->timer_.setInterval(EVENTS_REFRESH_INTERVAL_MS);
        this->timer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&this->timer_, &QTimer::timeout, this, [this] {
            this->requestNow();
        });
    }

    void setWantsEvents(StreamDatabaseBadgeBar *bar, bool wantsEvents)
    {
        if (bar == nullptr)
        {
            return;
        }

        if (wantsEvents)
        {
            const bool alreadySubscribed = this->subscribers_.contains(bar);
            this->subscribers_.insert(bar);
            if (!alreadySubscribed && !this->lastEventsData_.isEmpty())
            {
                bar->applyEventsData(this->lastEventsData_, false);
            }
        }
        else
        {
            this->subscribers_.remove(bar);
        }

        this->updateTimer();
    }

    void unregisterBar(StreamDatabaseBadgeBar *bar)
    {
        this->subscribers_.remove(bar);
        this->updateTimer();
    }

    void refresh(StreamDatabaseBadgeBar *bar)
    {
        if (bar != nullptr && this->subscribers_.contains(bar) &&
            !this->lastEventsData_.isEmpty())
        {
            bar->applyEventsData(this->lastEventsData_, false);
        }
        this->requestNow();
    }

    void requestNow()
    {
        if (this->pendingEventsRequest_ != nullptr ||
            this->subscribers_.isEmpty())
        {
            return;
        }

        QNetworkRequest request(
            QUrl(QString::fromLatin1(STREAMDATABASE_EVENTS_API)));
        request.setRawHeader("Accept", "application/json");
        request.setRawHeader("Cache-Control", "no-cache, no-store, max-age=0");
        request.setRawHeader("Pragma", "no-cache");
        request.setRawHeader("Expires", "0");
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
        auto *reply = this->network_.get(request);
        this->pendingEventsRequest_ = reply;

        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
            this->handleEventsFinished(reply);
        });
        QTimer::singleShot(EVENTS_REQUEST_TIMEOUT_MS, reply, [this, reply] {
            if (this->pendingEventsRequest_ == reply && reply->isRunning())
            {
                reply->abort();
            }
        });
    }

private:
    void updateTimer()
    {
        if (this->subscribers_.isEmpty())
        {
            this->timer_.stop();
            if (this->pendingEventsRequest_ != nullptr &&
                this->pendingEventsRequest_->isRunning())
            {
                this->pendingEventsRequest_->abort();
            }
            return;
        }

        if (!this->timer_.isActive())
        {
            this->timer_.start();
        }
        this->requestNow();
    }

    void handleEventsFinished(QNetworkReply *reply)
    {
        if (this->pendingEventsRequest_ == reply)
        {
            this->pendingEventsRequest_ = nullptr;
        }

        if (reply->error() == QNetworkReply::NoError)
        {
            const auto data = reply->readAll();
            this->lastEventsData_ = data;

            std::vector<StreamDatabaseBadgeBar *> subscribers;
            subscribers.reserve(this->subscribers_.size());
            for (auto *bar : this->subscribers_)
            {
                subscribers.push_back(bar);
            }

            StreamDatabaseBadgeBar *notifyTarget =
                subscribers.empty() ? nullptr : subscribers.front();
            for (auto *bar : subscribers)
            {
                if (this->subscribers_.contains(bar))
                {
                    bar->applyEventsData(data, bar == notifyTarget);
                }
            }
        }

        reply->deleteLater();
    }

    QNetworkAccessManager network_;
    QTimer timer_;
    QSet<StreamDatabaseBadgeBar *> subscribers_;
    QByteArray lastEventsData_;
    QNetworkReply *pendingEventsRequest_ = nullptr;
};

StreamDatabaseEventsPoller &streamDatabaseEventsPoller()
{
    static auto *poller = new StreamDatabaseEventsPoller;
    return *poller;
}

}  // namespace

StreamDatabaseBadgeBar::StreamDatabaseBadgeBar(QWidget *parent)
    : BaseWidget(parent)
    , badges_(StreamDatabaseBadgeBar::makeMockBadges())
    , network_(this)
    , tickerSlideAnimation_(this)
    , detailAnimation_(this)
    , navigationHoverAnimation_(this)
{
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    this->setAttribute(Qt::WA_Hover, true);
    this->setMouseTracking(true);
    this->setFixedHeight(0);
    this->hide();

    this->tickerTimer_.setInterval(TICKER_INTERVAL_MS);
    QObject::connect(&this->tickerTimer_, &QTimer::timeout, this, [this] {
        this->advanceTicker();
    });

    this->tickerSlideAnimation_.setDuration(TICKER_SLIDE_DURATION_MS);
    this->tickerSlideAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->tickerSlideAnimation_,
                     &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->tickerSlideProgress_ =
                             std::clamp(value.toReal(), 0.0, 1.0);
                         this->update();
                     });
    QObject::connect(&this->tickerSlideAnimation_,
                     &QVariantAnimation::finished, this, [this] {
                         this->finishTickerSlide();
                     });

    this->detailAnimation_.setDuration(180);
    this->detailAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->detailAnimation_, &QVariantAnimation::valueChanged,
                     this, [this](const QVariant &value) {
                         this->detailVisibleHeight_ =
                             std::max<qreal>(0.0, value.toReal());
                         this->updateFixedHeight();
                         this->update();
                     });
    QObject::connect(&this->detailAnimation_, &QVariantAnimation::finished,
                     this, [this] {
                         this->detailVisibleHeight_ =
                             std::max<qreal>(0.0, this->detailVisibleHeight_);
                         if (this->detailVisibleHeight_ <= 0.001)
                         {
                             this->expandedIndex_ = -1;
                             this->detailVisibleHeight_ = 0.0;
                             this->restartTickerIfNeeded();
                         }
                         this->updateFixedHeight();
                         this->update();
                     });

    this->navigationHoverAnimation_.setDuration(NAVIGATION_HOVER_DURATION_MS);
    this->navigationHoverAnimation_.setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(&this->navigationHoverAnimation_,
                     &QVariantAnimation::valueChanged, this,
                     [this](const QVariant &value) {
                         this->navigationHoverProgress_ =
                             std::clamp(value.toReal(), 0.0, 1.0);
                         this->update();
                     });

    this->ownedBadgeConnections_.managedConnect(
        twitch::streamDatabaseBadgeOwnershipChanged(), [this] {
            QTimer::singleShot(0, this, [this] {
                this->reconcileOwnedBadgeVisibility();
            });
        });
}

StreamDatabaseBadgeBar::~StreamDatabaseBadgeBar()
{
    streamDatabaseEventsPoller().unregisterBar(this);
}

std::vector<StreamDatabaseBadgeBar::BadgeItem>
    StreamDatabaseBadgeBar::makeMockBadges()
{
    return {
        {
            QStringLiteral("QSMP2"),
            QStringLiteral("QSMP Season 2"),
            QStringLiteral("ACTIVE"),
            QStringLiteral("ENDS JUL 1, 07:59 UTC"),
            QStringLiteral(
                "Watch 60 minutes of Quackity, QSMP, or QuackityToo while "
                "they are live in the QSMP category."),
            QStringLiteral("#22C55E"),
            QStringLiteral(
                "https://static-cdn.jtvnw.net/badges/v1/"
                "2fa68fb9-fcdd-4795-bfab-f408e10efaef/2"),
            QStringLiteral(
                "https://www.streamdatabase.com/twitch/global-badges/qsmp2/1"),
            QStringLiteral("qsmp2"),
            QStringLiteral("1"),
            {
                {QStringLiteral("Quackity"), QStringLiteral("quackity")},
                {QStringLiteral("QSMP"), QStringLiteral("qsmp")},
                {QStringLiteral("QuackityToo"), QStringLiteral("quackitytoo")},
            },
        },
        {
            QStringLiteral("BrawlhallAwoo"),
            QStringLiteral("Brawlhalla Fest 2026"),
            QStringLiteral("ACTIVE"),
            QStringLiteral("ENDS JUN 16, 15:58 UTC"),
            QStringLiteral(
                "Subscribe once or gift one subscription to an eligible "
                "Brawlhalla stream."),
            QStringLiteral("#3BA7FF"),
            QStringLiteral(
                "https://static-cdn.jtvnw.net/badges/v1/"
                "a3e4f5e7-a6c9-4458-b8c4-f83a0f725566/2"),
            QStringLiteral(
                "https://www.streamdatabase.com/twitch/global-badges/"
                "brawlhallawoo/1"),
            QStringLiteral("brawlhallawoo"),
            QStringLiteral("1"),
            {},
        },
        {
            QStringLiteral("007 Gun Barrel"),
            QStringLiteral("007 First Light Launch"),
            QStringLiteral("ACTIVE"),
            QStringLiteral("ENDS JUN 23, 13:59 UTC"),
            QStringLiteral(
                "Subscribe once or gift one subscription to an eligible 007 "
                "First Light stream."),
            QStringLiteral("#B7BEC9"),
            QStringLiteral(
                "https://static-cdn.jtvnw.net/badges/v1/"
                "6ae7ce40-99e6-4d83-8487-f8b990bf5f32/2"),
            QStringLiteral(
                "https://www.streamdatabase.com/twitch/global-badges/"
                "007-gun-barrel/1"),
            QStringLiteral("007-gun-barrel"),
            QStringLiteral("1"),
            {},
        },
        {
            QStringLiteral("Mod Founder"),
            QStringLiteral("Mod Appreciation Day"),
            QStringLiteral("ACTIVE"),
            QStringLiteral("ENDS JUN 30"),
            QStringLiteral(
                "Apply and get accepted into the Twitch Moderators Club "
                "before the Mod Appreciation Day 2026 cutoff."),
            QStringLiteral("#7CFC00"),
            QStringLiteral(
                "https://static-cdn.jtvnw.net/badges/v1/"
                "b1d1da41-5616-42a5-b1fa-132753d29f8a/2"),
            QStringLiteral(
                "https://www.streamdatabase.com/twitch/global-badges/"
                "mod-founder/1"),
            QStringLiteral("mod-founder"),
            QStringLiteral("1"),
            {},
        },
        {
            QStringLiteral("RuneScape Skull"),
            QStringLiteral("Deadman All Stars"),
            QStringLiteral("ACTIVE"),
            QStringLiteral("ENDS JUN 21, 00:59 UTC"),
            QStringLiteral(
                "Subscribe once or gift one subscription to an eligible Old "
                "School RuneScape Deadman All Stars creator."),
            QStringLiteral("#C9AD83"),
            QStringLiteral(
                "https://static-cdn.jtvnw.net/badges/v1/"
                "7f382386-e3ce-4c3d-8c23-464851415321/2"),
            QStringLiteral(
                "https://www.streamdatabase.com/twitch/global-badges/"
                "runescape-skull/1"),
            QStringLiteral("runescape-skull"),
            QStringLiteral("1"),
            {},
        },
    };
}

std::vector<StreamDatabaseBadgeBar::BadgeItem>
    StreamDatabaseBadgeBar::parseEventBadges(const QByteArray &data,
                                             bool activeOnly,
                                             bool upcomingOnly)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
    {
        return {};
    }

    const auto root = document.object();
    const auto events = root.value("data").toArray();
    if (events.isEmpty())
    {
        return {};
    }

    const auto now = QDateTime::currentDateTimeUtc();
    std::vector<BadgeItem> parsed;
    QSet<QString> seenBadges;

    for (const auto &eventValue : events)
    {
        const auto event = eventValue.toObject();
        if (event.isEmpty() || event.value("hidden").toBool(false))
        {
            continue;
        }

        const auto eventStart = parseStreamDatabaseDateTime(
            event, "start_at_date", "start_at_time", false);
        const auto eventEnd = parseStreamDatabaseDateTime(
            event, "end_at_date", "end_at_time", true);
        const bool eventHasDates = hasDateWindow(eventStart, eventEnd);
        const bool eventActive =
            eventHasDates && dateTimeWindowContains(eventStart, eventEnd, now);
        const bool eventUpcoming =
            dateTimeWindowStartsInFuture(eventStart, eventEnd, now);

        const auto globalBadges =
            event.value("twitch_global_badges").toArray();
        for (const auto &badgeValue : globalBadges)
        {
            const auto badgeObject = badgeValue.toObject();
            const auto current = badgeObject.value("current").toObject();
            const auto version = current.value("version").toObject();
            const QString setID = current.value("set_id").toString();
            const QString versionID = version.value("id").toString();
            if (setID.isEmpty() || versionID.isEmpty())
            {
                continue;
            }

            const QString dedupeKey = setID + QLatin1Char('/') + versionID;
            if (seenBadges.contains(dedupeKey))
            {
                continue;
            }

            const auto availability =
                badgeObject.value("availability").toArray();
            const auto activeAvailability =
                activeAvailabilityObjects(availability, now, eventActive);
            const auto allAvailability = availabilityObjects(availability);
            const auto upcomingAvailability =
                upcomingAvailabilityObjects(availability, now, eventUpcoming);
            const std::vector<QJsonObject> displayAvailability =
                !activeAvailability.empty()
                    ? activeAvailability
                    : (!upcomingAvailability.empty() ? upcomingAvailability
                                                     : allAvailability);
            const bool badgeActive =
                availability.isEmpty()
                    ? eventActive
                    : !activeAvailability.empty();
            const bool badgeUpcoming =
                !badgeActive && (availability.isEmpty()
                                     ? eventUpcoming
                                     : !upcomingAvailability.empty());
            if (activeOnly && !badgeActive)
            {
                continue;
            }
            if (upcomingOnly && !badgeUpcoming)
            {
                continue;
            }
            const auto end = bestEndDateTime(displayAvailability, event);

            const QString eventTitle = event.value("title").toString();
            QString badgeName = version.value("title").toString();
            if (badgeName.isEmpty())
            {
                badgeName = setID;
            }

            parsed.push_back(BadgeItem{
                badgeName,
                eventTitle,
                statusTextForBadge(badgeActive, badgeUpcoming, eventStart,
                                   eventEnd, now),
                formatAvailabilityWindowText(end, badgeActive, now),
                requirementTextFromAvailability(
                    displayAvailability, event.value("content").toString(),
                    version.value("description").toString()),
                defaultBadgeAccent(),
                version.value("image_url_4x").toString(),
                siteUrlForBadge(setID, versionID),
                setID,
                versionID,
                availabilityChannelLinks(displayAvailability),
                availabilityCategoryLinks(displayAvailability),
                badgeActive,
            });
            seenBadges.insert(dedupeKey);
        }
    }

    return parsed;
}

QString StreamDatabaseBadgeBar::badgeKey(const BadgeItem &badge)
{
    if (!badge.setID.isEmpty() && !badge.versionID.isEmpty())
    {
        return badge.setID + u'/' + badge.versionID;
    }

    if (!badge.siteUrl.isEmpty())
    {
        return badge.siteUrl;
    }

    return badge.eventTitle + QStringLiteral("::") + badge.badgeName;
}

void StreamDatabaseBadgeBar::setChannel(const ChannelPtr &channel)
{
    const auto twitch = resolveTwitchChannel(channel);
    const bool shouldShow =
        twitch != nullptr &&
        twitch->getName().compare(QString::fromLatin1(STREAMDATABASE_LOGIN),
                                  Qt::CaseInsensitive) == 0;

    if (shouldShow)
    {
        this->streamDatabaseChannel_ = channel;

        auto account = getApp()->getAccounts()->twitch.getCurrent();
        if (account == nullptr || account->isAnon())
        {
            twitch::clearCurrentUserOwnedBadges();
        }
        else
        {
            const auto channelName = twitch->getName();
            const auto badgeAuth =
                TwitchModerationAuth::resolveForCurrentUser(account->getUserId());
            if (badgeAuth.supportsWebGql())
            {
                twitch::requestCurrentUserBadgeIdentity(
                    this->network_, channelName, badgeAuth, this,
                    [this,
                     channelName](QVector<twitch::CurrentUserBadgeIdentity> badges) {
                        twitch::updateCurrentUserOwnedBadgesForChannel(
                            channelName, badges);
                        this->reconcileOwnedBadgeVisibility();
                    },
                    [](const QString &) {
                    });
            }
        }
    }
    else
    {
        this->streamDatabaseChannel_.reset();
    }
    this->channelAllowsFeed_ = shouldShow;
    this->setVisibleForChannel(shouldShow && this->badgeFeedVisible_);
}

void StreamDatabaseBadgeBar::setBadgeFeedVisible(bool visible)
{
    if (this->badgeFeedVisible_ == visible)
    {
        this->setVisibleForChannel(this->channelAllowsFeed_ &&
                                   this->badgeFeedVisible_);
        return;
    }

    this->badgeFeedVisible_ = visible;
    this->setVisibleForChannel(this->channelAllowsFeed_ && visible);
}

bool StreamDatabaseBadgeBar::badgeFeedVisible() const
{
    return this->badgeFeedVisible_;
}

QSize StreamDatabaseBadgeBar::sizeHint() const
{
    return {0, this->barHeight()};
}

bool StreamDatabaseBadgeBar::event(QEvent *event)
{
    switch (event->type())
    {
        case QEvent::Enter:
        case QEvent::HoverEnter:
            this->update();
            break;
        case QEvent::Leave:
        case QEvent::HoverLeave:
            this->hoverPaused_ = false;
            this->hoverBadgeIndex_ = -1;
            this->hoverSiteLink_ = false;
            this->hoverSettingsButton_ = false;
            this->hoverSourceLink_ = false;
            this->hoverChannelLink_ = false;
            this->hoverPreviousArrow_ = false;
            this->hoverNextArrow_ = false;
            this->setNavigationHoverVisible(false);
            this->unsetCursor();
            this->restartTickerIfNeeded();
            this->update();
            break;
        case QEvent::Hide:
            this->hoverPaused_ = false;
            this->hoverBadgeIndex_ = -1;
            this->hoverSiteLink_ = false;
            this->hoverSettingsButton_ = false;
            this->hoverSourceLink_ = false;
            this->hoverChannelLink_ = false;
            this->hoverPreviousArrow_ = false;
            this->hoverNextArrow_ = false;
            this->unsetCursor();
            this->tickerTimer_.stop();
            this->navigationHoverAnimation_.stop();
            this->navigationHoverProgress_ = 0.0;
            if (this->tickerSlideAnimation_.state() ==
                QAbstractAnimation::Running)
            {
                this->tickerSlideAnimation_.pause();
            }
            break;
        case QEvent::Show:
            this->restartTickerIfNeeded();
            break;
        case QEvent::HoverMove:
        {
            const QPoint position = this->mapFromGlobal(QCursor::pos());
            this->updateNavigationHover(position);
            this->updateHoverState(position);
            break;
        }
        default:
            break;
    }

    return BaseWidget::event(event);
}

void StreamDatabaseBadgeBar::mouseMoveEvent(QMouseEvent *event)
{
    this->updateNavigationHover(event->pos());
    this->updateHoverState(event->pos());
    BaseWidget::mouseMoveEvent(event);
}

void StreamDatabaseBadgeBar::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton)
    {
        const int badgeIndex = this->badgeIndexAt(event->pos());
        if (badgeIndex >= 0)
        {
            this->showBadgeMenu(badgeIndex,
                                event->globalPosition().toPoint());
            event->accept();
            return;
        }

        this->showSettingsMenu(event->globalPosition().toPoint());
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton)
    {
        BaseWidget::mousePressEvent(event);
        return;
    }

    if (this->previousArrowContains(event->pos()))
    {
        this->stepBadgeFeed(-1);
        event->accept();
        return;
    }

    if (this->nextArrowContains(event->pos()))
    {
        this->stepBadgeFeed(1);
        event->accept();
        return;
    }

    if (this->settingsButtonContains(event->pos()))
    {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto settingsMenuClosedAt =
            this->property(SETTINGS_MENU_CLOSED_AT_PROPERTY).toLongLong();
        if (settingsMenuClosedAt > 0 &&
            now - settingsMenuClosedAt < SETTINGS_MENU_REOPEN_SUPPRESS_MS)
        {
            this->setProperty(SETTINGS_MENU_CLOSED_AT_PROPERTY, 0LL);
            event->accept();
            return;
        }

        this->showSettingsMenu(
            this->mapToGlobal(
                QPoint{this->settingsButtonRect_.right() + 1,
                       this->settingsButtonRect_.bottom() +
                           scaledInt(this->scale(), 4)}),
            true, true);
        event->accept();
        return;
    }

    if (this->sourceLinkContains(event->pos()) && this->expandedIndex_ >= 0 &&
        this->expandedIndex_ < static_cast<int>(this->badges_.size()))
    {
        QDesktopServices::openUrl(
            QUrl(this->badges_[this->expandedIndex_].siteUrl));
        event->accept();
        return;
    }

    const int channelLinkIndex = this->channelLinkIndexAt(event->pos());
    if (channelLinkIndex >= 0)
    {
        const auto &link =
            this->channelLinkRects_[static_cast<size_t>(channelLinkIndex)];
        QDesktopServices::openUrl(
            link.url.isEmpty() ? twitchChannelUrl(link.login) : QUrl(link.url));
        event->accept();
        return;
    }

    if (this->linkContains(event->pos()))
    {
        QDesktopServices::openUrl(
            QUrl(QString::fromLatin1(STREAMDATABASE_SITE)));
        event->accept();
        return;
    }

    const int badgeIndex = this->badgeIndexAt(event->pos());
    if (badgeIndex >= 0)
    {
        this->setExpandedIndex(badgeIndex);
        event->accept();
        return;
    }

    if (this->collapsedBadgeInfoContains(event->pos()))
    {
        const int linkedIndex = this->linkedBadgeIndex();
        if (linkedIndex >= 0)
        {
            this->setExpandedIndex(linkedIndex);
            event->accept();
            return;
        }
    }

    BaseWidget::mousePressEvent(event);
}

void StreamDatabaseBadgeBar::paintEvent(QPaintEvent * /*event*/)
{
    this->badgeRects_.clear();
    this->channelLinkRects_.clear();
    this->siteLinkRect_ = {};
    this->settingsButtonRect_ = {};
    this->sourceLinkRect_ = {};
    this->previousArrowRect_ = {};
    this->nextArrowRect_ = {};

    if (!this->visibleForChannel_ || this->badges_.empty())
    {
        return;
    }

    const auto scale = this->scale();
    const bool light = this->theme->isLightTheme();
    const QColor text = this->theme->messages.textColors.regular;
    const QColor mutedText = mutedFromTheme(this->theme, text);
    const QColor activeGreen = QColor("#22C55E");

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

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(this->rect(), this->theme->messages.backgrounds.regular);

    const int outerInset = std::max(1, scaledInt(scale, 1));
    const int paddingX = scaledInt(scale, 8);
    const int paddingY = scaledInt(scale, 4);
    const int railWidth = std::max(2, scaledInt(scale, 3));
    const int rowHeight = this->collapsedHeight();
    const QRect itemRect =
        this->rect().adjusted(0, outerInset, -1, -outerInset).normalized();

    painter.fillRect(itemRect, surface);
    painter.fillRect(QRect{itemRect.left(), itemRect.top(), itemRect.width(), 1},
                     separator);
    painter.fillRect(
        QRect{itemRect.left(), itemRect.bottom(), itemRect.width(), 1},
        separator);

    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    const int badgeCount = static_cast<int>(this->badges_.size());
    const int slideDirection = this->tickerSlideDirection_ < 0 ? -1 : 1;
    const bool slideSourceValid =
        this->slideSourceTickerIndex_ >= 0 &&
        this->slideSourceTickerIndex_ < badgeCount &&
        !this->badgeHiddenByOwnedMode(this->slideSourceTickerIndex_);
    const bool pendingTickerValid =
        this->pendingTickerIndex_ >= 0 &&
        this->pendingTickerIndex_ < badgeCount &&
        !this->badgeHiddenByOwnedMode(this->pendingTickerIndex_);
    const bool tickerHasPendingBadge =
        visibleBadgeIndices.size() > 1 && slideSourceValid &&
        pendingTickerValid;
    const bool tickerIsSliding =
        tickerHasPendingBadge && this->tickerSlideProgress_ > 0.001;
    const int normalizedTickerIndex =
        this->normalizedTickerIndex(visibleBadgeIndices);
    const int displayBadgeIndex =
        this->expandedIndex_ >= 0 && !this->badgeHiddenByOwnedMode(
                                         this->expandedIndex_)
            ? this->expandedIndex_
            : (tickerIsSliding
                   ? this->slideSourceTickerIndex_
                   : normalizedTickerIndex);
    const auto *tickerBadge = displayBadgeIndex >= 0 &&
                                      displayBadgeIndex < badgeCount
                                  ? &this->badges_[displayBadgeIndex]
                                  : nullptr;
    const int incomingBadgeIndex = tickerHasPendingBadge
                                       ? this->pendingTickerIndex_
                                       : -1;
    const auto *incomingTickerBadge =
        incomingBadgeIndex >= 0 &&
                incomingBadgeIndex < static_cast<int>(this->badges_.size())
            ? &this->badges_[incomingBadgeIndex]
            : nullptr;
    const QColor tickerAccent =
        tickerBadge != nullptr ? QColor(tickerBadge->accent) : activeGreen;
    QLinearGradient railGradient(itemRect.topLeft(), itemRect.bottomLeft());
    railGradient.setColorAt(0.0, withAlpha(activeGreen, light ? 235 : 220));
    railGradient.setColorAt(1.0, withAlpha(tickerAccent, light ? 205 : 185));
    painter.fillRect(
        QRect{itemRect.left(), itemRect.top(), railWidth, itemRect.height()},
        railGradient);

    auto *fonts = getApp()->getFonts();
    const auto titleFont = fonts->getFont(FontStyle::UiMediumBold, scale);
    const auto detailFont = fonts->getFont(FontStyle::UiMedium, scale);
    const auto brandFont =
        smallerFont(fonts->getFont(FontStyle::UiMediumBold, scale), 0.84);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics detailMetrics(detailFont);
    const QFontMetrics brandMetrics(brandFont);

    const int contentLeft =
        itemRect.left() + paddingX + railWidth + scaledInt(scale, 5);
    const int contentRight = itemRect.right() - paddingX;
    if (contentRight <= contentLeft)
    {
        return;
    }

    const int centerY = itemRect.top() + rowHeight / 2;
    const int chipPaddingX = scaledInt(scale, 7);
    const int smallChipHeight =
        std::max(scaledInt(scale, 19), detailMetrics.height() + 2);
    const int brandLogoSize = std::max(20, scaledInt(scale, 24));
    const int brandGap = scaledInt(scale, 5);
    const int brandPaddingX = scaledInt(scale, 4);
    const int brandLineHeight =
        std::max(1, brandMetrics.height() - scaledInt(scale, 2));
    const int brandTextWidth =
        std::max(brandMetrics.horizontalAdvance(QStringLiteral("STREAM")),
                 brandMetrics.horizontalAdvance(QStringLiteral("DATABASE"))) +
        scaledInt(scale, 4);
    const int settingsButtonSize = std::max(18, scaledInt(scale, 20));
    const int settingsButtonGap = scaledInt(scale, 3);
    const int linkHeight =
        std::max({scaledInt(scale, 30), brandLogoSize + scaledInt(scale, 6),
                  brandLineHeight * 2 + scaledInt(scale, 5)});
    const int linkWidth =
        brandPaddingX * 2 + brandLogoSize + brandGap + brandTextWidth;
    const int brandAreaWidth =
        linkWidth + settingsButtonGap + settingsButtonSize;
    this->siteLinkRect_ =
        QRect{contentRight - brandAreaWidth, centerY - linkHeight / 2,
              linkWidth, linkHeight};
    this->settingsButtonRect_ =
        QRect{this->siteLinkRect_.right() + settingsButtonGap + 1,
              centerY - settingsButtonSize / 2, settingsButtonSize,
              settingsButtonSize};

    this->drawStreamDatabaseLink(painter, this->siteLinkRect_, brandFont,
                                 this->hoverSiteLink_);
    const bool settingsButtonActive =
        this->hoverSettingsButton_ ||
        (this->settingsMenu_ != nullptr && this->settingsMenu_->isVisible());
    this->drawSettingsButton(painter, this->settingsButtonRect_,
                             settingsButtonActive);

    const int statusDotPaddingX = scaledInt(scale, 6);
    int x = itemRect.left() + railWidth + statusDotPaddingX;
    const int statusDotSize = std::max(8, scaledInt(scale, 9));
    QRect statusDotRect{x, centerY - statusDotSize / 2, statusDotSize,
                        statusDotSize};
    x = statusDotRect.right() + statusDotPaddingX + 1;

    const int badgeSize = std::max(22, scaledInt(scale, 24));
    const int badgeGap = std::max(1, scaledInt(scale, 5));
    const int badgeStep = badgeSize + badgeGap;
    const int badgeAreaRight = this->siteLinkRect_.left() - scaledInt(scale, 8);
    const int availableBadgeWidth = std::max(0, badgeAreaRight - x + 1);
    const int visibleBadgeSlots = std::min(
        {VISIBLE_BADGE_COUNT, static_cast<int>(visibleBadgeIndices.size()),
         std::max(0, (availableBadgeWidth + badgeGap) / badgeStep)});
    this->badgeRects_.assign(this->badges_.size(), QRect{});

    if (visibleBadgeSlots > 0)
    {
        const int carouselWidth =
            visibleBadgeSlots * badgeSize +
            std::max(0, visibleBadgeSlots - 1) * badgeGap;
        const QRect carouselRect{x, centerY - badgeSize / 2, carouselWidth,
                                 badgeSize};
        const bool canSlide = visibleBadgeIndices.size() > 1;
        const qreal navigationProgress =
            canSlide ? this->navigationHoverProgress_ : 0.0;
        if (navigationProgress < 0.999)
        {
            QColor dotColor = activeGreen;
            dotColor.setAlphaF(std::clamp(1.0 - navigationProgress, 0.0, 1.0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(dotColor);
            painter.drawEllipse(statusDotRect);
        }
        const bool isSliding = canSlide && tickerIsSliding;
        const int drawCount = visibleBadgeSlots + (isSliding ? 1 : 0);
        const int startBadgeIndex =
            isSliding ? this->slideSourceTickerIndex_
                      : (canSlide ? normalizedTickerIndex
                                  : visibleBadgeIndices.front());
        int startPosition = 0;
        for (int i = 0; i < static_cast<int>(visibleBadgeIndices.size()); ++i)
        {
            if (visibleBadgeIndices[static_cast<size_t>(i)] == startBadgeIndex)
            {
                startPosition = i;
                break;
            }
        }
        const qreal slideOffset =
            isSliding ? -qreal(slideDirection) * qreal(badgeStep) *
                            this->tickerSlideProgress_
                      : 0.0;
        const int badgePaintOutset = std::max(1, scaledInt(scale, 2));

        painter.save();
        painter.setClipRect(
            carouselRect.adjusted(-badgePaintOutset, -scaledInt(scale, 3),
                                  badgePaintOutset, scaledInt(scale, 3)));
        for (int slot = 0; slot < drawCount; ++slot)
        {
            const int slotPosition =
                slot + (isSliding && slideDirection < 0 ? -1 : 0);
            const int sourcePosition =
                (startPosition + slotPosition +
                 static_cast<int>(visibleBadgeIndices.size())) %
                static_cast<int>(visibleBadgeIndices.size());
            const int badgeIndex = visibleBadgeIndices[static_cast<size_t>(
                sourcePosition)];
            QRect badgeRect{
                x + int(std::round(qreal(slotPosition * badgeStep) +
                                   slideOffset)),
                centerY - badgeSize / 2,
                badgeSize,
                badgeSize,
            };
            if (!badgeRect.intersects(carouselRect))
            {
                continue;
            }

            this->badgeRects_[badgeIndex] = badgeRect.intersected(carouselRect);
            const bool active =
                badgeIndex == this->expandedIndex_ ||
                (this->expandedIndex_ < 0 &&
                 badgeIndex == displayBadgeIndex) ||
                badgeIndex == this->hoverBadgeIndex_;
            this->drawBadgeIcon(painter, badgeRect, this->badges_[badgeIndex],
                                active);
        }
        painter.restore();

        const int arrowSize = std::max(14, scaledInt(scale, 16));
        const int rightArrowGap = scaledInt(scale, 3);
        const int summaryBaseLeft =
            carouselRect.right() + scaledInt(scale, 5);
        int summaryHoverLeft = summaryBaseLeft;
        QRect rightArrowRect;
        if (canSlide)
        {
            rightArrowRect = QRect{
                carouselRect.right() + rightArrowGap,
                centerY - arrowSize / 2,
                arrowSize,
                arrowSize,
            };
            summaryHoverLeft =
                std::max(summaryBaseLeft,
                         rightArrowRect.right() + scaledInt(scale, 4));
        }

        if (canSlide && navigationProgress > 0.001)
        {
            const QRect leftArrowRect{
                statusDotRect.center().x() - arrowSize / 2,
                centerY - arrowSize / 2,
                arrowSize,
                arrowSize,
            };
            this->previousArrowRect_ = leftArrowRect;
            this->nextArrowRect_ = rightArrowRect;
            const QColor arrowColor = withAlpha(text, light ? 215 : 230);
            this->drawNavigationArrow(painter, leftArrowRect, true,
                                      navigationProgress,
                                      this->hoverPreviousArrow_, arrowColor);
            this->drawNavigationArrow(painter, rightArrowRect, false,
                                      navigationProgress,
                                      this->hoverNextArrow_, arrowColor);
        }

        x = canSlide ? summaryBaseLeft +
                           int(std::round((summaryHoverLeft -
                                           summaryBaseLeft) *
                                          navigationProgress))
                     : carouselRect.right() + scaledInt(scale, 5);
    }
    else
    {
        painter.setPen(Qt::NoPen);
        painter.setBrush(activeGreen);
        painter.drawEllipse(statusDotRect);
        x += scaledInt(scale, 4);
    }

    const int summaryLeft = x;
    const int summaryRight = this->siteLinkRect_.left() - scaledInt(scale, 10);
    const auto drawSummary = [&](const BadgeItem *badge) {
        if (summaryRight <= summaryLeft)
        {
            return;
        }

        painter.save();
        painter.setClipRect(QRect{summaryLeft, itemRect.top(),
                                  summaryRight - summaryLeft, rowHeight});

        int textRight = summaryRight;
        if (badge != nullptr)
        {
            const QColor badgeAccent(badge->accent);
            const int deadlineWidth =
                detailMetrics.horizontalAdvance(badge->endsText) +
                chipPaddingX * 2;
            const int deadlineLeft = textRight - deadlineWidth;
            if (deadlineLeft > summaryLeft + scaledInt(scale, 90))
            {
                QRect deadlineRect{deadlineLeft, centerY - smallChipHeight / 2,
                                   deadlineWidth, smallChipHeight};
                fillRoundedRect(painter, QRectF(deadlineRect),
                                withAlpha(badgeAccent, light ? 38 : 46),
                                smallChipHeight / 2.0);
                painter.setFont(detailFont);
                painter.setPen(withAlpha(badgeAccent, light ? 255 : 230));
                painter.drawText(deadlineRect, Qt::AlignCenter,
                                 badge->endsText);
                textRight = deadlineRect.left() - scaledInt(scale, 8);
            }
        }

        if (textRight > summaryLeft)
        {
            const QRect headlineRect{summaryLeft, itemRect.top() + paddingY,
                                     textRight - summaryLeft,
                                     titleMetrics.height()};
            const QRect detailRect{
                summaryLeft,
                headlineRect.bottom(),
                textRight - summaryLeft,
                rowHeight - paddingY - headlineRect.bottom() + itemRect.top()};

            if (badge != nullptr)
            {
                const QString headline =
                    badge->eventTitle + QStringLiteral(" - ") +
                    badge->badgeName;
                painter.setFont(titleFont);
                painter.setPen(text);
                painter.drawText(
                    headlineRect, Qt::AlignVCenter | Qt::AlignLeft,
                    titleMetrics.elidedText(headline, Qt::ElideRight,
                                            headlineRect.width()));

                painter.setFont(detailFont);
                painter.setPen(mutedText);
                painter.drawText(
                    detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                    detailMetrics.elidedText(badge->requirementText,
                                             Qt::ElideRight,
                                             detailRect.width()));
            }
            else
            {
                painter.setFont(titleFont);
                painter.setPen(text);
                painter.drawText(
                    headlineRect, Qt::AlignVCenter | Qt::AlignLeft,
                    titleMetrics.elidedText(
                        QStringLiteral("No unowned badges to show"),
                        Qt::ElideRight, headlineRect.width()));

                painter.setFont(detailFont);
                painter.setPen(mutedText);
                painter.drawText(
                    detailRect, Qt::AlignVCenter | Qt::AlignLeft,
                    detailMetrics.elidedText(
                        QStringLiteral("Owned badges are hidden in "
                                       "StreamDatabase settings."),
                        Qt::ElideRight, detailRect.width()));
            }
        }

        painter.restore();
    };

    const auto *summaryBadge =
        incomingTickerBadge != nullptr ? incomingTickerBadge : tickerBadge;
    drawSummary(summaryBadge);

    if (this->expandedIndex_ < 0 || this->expandedIndex_ >=
                                      static_cast<int>(this->badges_.size()) ||
        this->badgeHiddenByOwnedMode(this->expandedIndex_) ||
        this->detailVisibleHeight_ <= 0.001)
    {
        return;
    }

    const int detailVisibleHeight =
        std::max(0, int(std::round(this->detailVisibleHeight_)));
    if (detailVisibleHeight <= 0)
    {
        return;
    }

    const QRect detailClip{contentLeft, itemRect.top() + rowHeight,
                           contentRight - contentLeft, detailVisibleHeight};
    const int badgePaintOutset = std::max(1, scaledInt(scale, 2));
    painter.save();
    painter.setClipRect(detailClip.adjusted(-badgePaintOutset, 0, 0, 0));
    painter.fillRect(QRect{contentLeft, detailClip.top(),
                           contentRight - contentLeft, 1},
                     withAlpha(separator, light ? 105 : 95));

    const auto &badge = this->badges_[this->expandedIndex_];
    const QColor badgeAccent(badge.accent);
    const int detailPaddingY = scaledInt(scale, 7);
    const int largeBadgeSize = std::max(22, scaledInt(scale, 26));
    QRect largeBadgeRect{contentLeft, detailClip.top() + detailPaddingY,
                         largeBadgeSize, largeBadgeSize};
    this->drawBadgeIcon(painter, largeBadgeRect, badge, true);

    const int textLeft = largeBadgeRect.right() + scaledInt(scale, 9);
    const int textWidth = std::max(0, contentRight - textLeft);
    QRect titleRect{textLeft, detailClip.top() + detailPaddingY, textWidth,
                    titleMetrics.height()};
    QRect statusLineRect{textLeft, titleRect.bottom() + scaledInt(scale, 1),
                         textWidth, detailMetrics.height()};
    const int requirementHeight = std::max(
        detailMetrics.height(),
        detailMetrics
            .boundingRect(QRect{0, 0, textWidth, 10000},
                          Qt::TextWordWrap | Qt::AlignLeft |
                              Qt::AlignTop,
                          badge.requirementText)
            .height());
    QRect requirementRect{textLeft,
                          statusLineRect.bottom() + scaledInt(scale, 1),
                          textWidth, requirementHeight};
    QRect sourceRect{textLeft,
                     requirementRect.bottom() + scaledInt(scale, 2),
                     textWidth, detailMetrics.height()};

    painter.setFont(titleFont);
    painter.setPen(text);
    painter.drawText(titleRect, Qt::AlignVCenter | Qt::AlignLeft,
                     titleMetrics.elidedText(
                         badge.badgeName + QStringLiteral(" - ") +
                             badge.eventTitle,
                         Qt::ElideRight,
                         titleRect.width()));

    painter.setFont(detailFont);
    painter.setPen(withAlpha(badgeAccent, light ? 235 : 215));
    painter.drawText(
        statusLineRect, Qt::AlignVCenter | Qt::AlignLeft,
        detailMetrics.elidedText(badge.statusText + QStringLiteral(" - ") +
                                     badge.endsText,
                                 Qt::ElideRight, statusLineRect.width()));

    this->drawRequirementText(painter, requirementRect, badge, detailFont,
                              mutedText,
                              withAlpha(badgeAccent, light ? 245 : 225));

    const QString sourceText = badge.siteUrl;
    const QString elidedSourceText =
        detailMetrics.elidedText(sourceText, Qt::ElideRight,
                                 sourceRect.width());
    const int sourceLinkWidth =
        std::min(sourceRect.width(),
                 detailMetrics.horizontalAdvance(elidedSourceText));
    this->sourceLinkRect_ =
        QRect{sourceRect.left(), sourceRect.top(), sourceLinkWidth,
              sourceRect.height()};

    painter.setPen(withAlpha(badgeAccent,
                             this->hoverSourceLink_ ? (light ? 255 : 235)
                                                    : (light ? 235 : 215)));
    painter.drawText(sourceRect, Qt::AlignVCenter | Qt::AlignLeft,
                     elidedSourceText);
    painter.restore();
}

void StreamDatabaseBadgeBar::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    if (this->expandedIndex_ >= 0 && this->detailVisibleHeight_ > 0.0 &&
        this->detailAnimation_.state() == QAbstractAnimation::Stopped)
    {
        this->detailVisibleHeight_ = this->expandedDetailHeight();
    }
    this->updateFixedHeight();
}

void StreamDatabaseBadgeBar::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    this->updateFixedHeight();
    this->update();
}

void StreamDatabaseBadgeBar::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    this->update();
}

void StreamDatabaseBadgeBar::setVisibleForChannel(bool visible)
{
    if (this->visibleForChannel_ == visible)
    {
        this->updateEventsRefreshTimer();
        this->restartTickerIfNeeded();
        return;
    }

    this->visibleForChannel_ = visible;
    this->hoverPaused_ = false;
    this->tickerIndex_ = 0;
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideDirection_ = 1;
    this->navigationHoverProgress_ = 0.0;
    this->expandedIndex_ = -1;
    this->detailVisibleHeight_ = 0.0;
    this->tickerSlideAnimation_.stop();
    this->detailAnimation_.stop();
    this->navigationHoverAnimation_.stop();
    this->hoverBadgeIndex_ = -1;
    this->hoverSiteLink_ = false;
    this->hoverSettingsButton_ = false;
    this->hoverSourceLink_ = false;
    this->hoverChannelLink_ = false;
    this->hoverPreviousArrow_ = false;
    this->hoverNextArrow_ = false;
    this->badgeRects_.clear();
    this->channelLinkRects_.clear();
    this->siteLinkRect_ = {};
    this->settingsButtonRect_ = {};
    this->sourceLinkRect_ = {};
    this->previousArrowRect_ = {};
    this->nextArrowRect_ = {};

    if (!this->visibleForChannel_)
    {
        this->updateEventsRefreshTimer();
        this->tickerTimer_.stop();
        this->tickerSlideAnimation_.stop();
        this->slideSourceTickerIndex_ = -1;
        this->pendingTickerIndex_ = -1;
        this->tickerSlideProgress_ = 0.0;
        this->tickerSlideDirection_ = 1;
        this->navigationHoverAnimation_.stop();
        this->navigationHoverProgress_ = 0.0;
        this->setFixedHeight(0);
        this->hide();
        return;
    }

    this->updateFixedHeight();
    this->show();
    this->updateEventsRefreshTimer();
    this->requestBadgeImages();
    this->requestStreamDatabaseLogo();
    this->restartTickerIfNeeded();
    this->update();
}

void StreamDatabaseBadgeBar::updateEventsRefreshTimer()
{
    const bool shouldPollEvents =
        this->channelAllowsFeed_ &&
        (this->badgeFeedVisible_ ||
         streamDatabaseBadgePingMode() != BadgePingMode::None);
    streamDatabaseEventsPoller().setWantsEvents(this, shouldPollEvents);
}

void StreamDatabaseBadgeBar::restartTickerIfNeeded()
{
    const bool pointerPaused = this->hoverPaused_ || this->underMouse();
    if (!this->canRunTicker())
    {
        this->tickerTimer_.stop();
        if (!pointerPaused &&
            this->tickerSlideAnimation_.state() == QAbstractAnimation::Running)
        {
            this->tickerSlideAnimation_.pause();
        }
        return;
    }

    if (this->tickerSlideAnimation_.state() == QAbstractAnimation::Paused)
    {
        this->tickerSlideAnimation_.resume();
        return;
    }

    if (this->tickerSlideAnimation_.state() != QAbstractAnimation::Stopped)
    {
        this->tickerTimer_.stop();
        return;
    }

    this->tickerTimer_.start();
}

void StreamDatabaseBadgeBar::pauseTickerForPointer()
{
    const bool wasHoverPaused = this->hoverPaused_;
    this->hoverPaused_ = true;
    this->tickerTimer_.stop();
    if (!wasHoverPaused)
    {
        this->cancelTickerSlideToSource();
    }
}

void StreamDatabaseBadgeBar::cancelTickerSlideToSource()
{
    if (this->tickerSlideAnimation_.state() != QAbstractAnimation::Stopped)
    {
        const bool wasBlocked = this->tickerSlideAnimation_.blockSignals(true);
        this->tickerSlideAnimation_.stop();
        this->tickerSlideAnimation_.blockSignals(wasBlocked);
    }

    if (this->slideSourceTickerIndex_ >= 0 &&
        this->slideSourceTickerIndex_ < static_cast<int>(this->badges_.size()) &&
        !this->badgeHiddenByOwnedMode(this->slideSourceTickerIndex_))
    {
        this->tickerIndex_ = this->slideSourceTickerIndex_;
    }
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideDirection_ = 1;
    this->tickerSlideAnimation_.setDuration(TICKER_SLIDE_DURATION_MS);
}

bool StreamDatabaseBadgeBar::beginTickerSlide(int direction, int durationMs)
{
    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    if (direction == 0 || visibleBadgeIndices.size() <= 1)
    {
        this->slideSourceTickerIndex_ = -1;
        this->pendingTickerIndex_ = -1;
        this->tickerSlideProgress_ = 0.0;
        return false;
    }

    const int slideDirection = direction < 0 ? -1 : 1;
    const bool useExpandedSource =
        this->expandedIndex_ >= 0 &&
        !this->badgeHiddenByOwnedMode(this->expandedIndex_);
    const int sourceIndex =
        useExpandedSource ? this->expandedIndex_
                          : this->normalizedTickerIndex(visibleBadgeIndices);
    const int targetIndex =
        this->tickerIndexAfterStep(visibleBadgeIndices, sourceIndex,
                                   slideDirection);
    if (sourceIndex < 0 || targetIndex < 0)
    {
        this->slideSourceTickerIndex_ = -1;
        this->pendingTickerIndex_ = -1;
        this->tickerSlideProgress_ = 0.0;
        return false;
    }

    this->tickerSlideDirection_ = slideDirection;
    this->slideSourceTickerIndex_ = sourceIndex;
    this->pendingTickerIndex_ = targetIndex;
    this->tickerIndex_ = sourceIndex;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideAnimation_.setDuration(durationMs);
    this->tickerSlideAnimation_.setStartValue(0.0);
    this->tickerSlideAnimation_.setEndValue(1.0);
    this->tickerSlideAnimation_.start();
    this->update();
    return true;
}

void StreamDatabaseBadgeBar::advanceTicker()
{
    if (!this->canRunTicker())
    {
        this->restartTickerIfNeeded();
        return;
    }

    if (this->tickerSlideAnimation_.state() != QAbstractAnimation::Stopped)
    {
        return;
    }

    this->tickerTimer_.stop();
    if (!this->beginTickerSlide(1, TICKER_SLIDE_DURATION_MS))
    {
        this->restartTickerIfNeeded();
    }
}

void StreamDatabaseBadgeBar::finishTickerSlide()
{
    this->tickerSlideProgress_ = 0.0;
    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    if (this->pendingTickerIndex_ >= 0 &&
        !this->badgeHiddenByOwnedMode(this->pendingTickerIndex_))
    {
        this->tickerIndex_ = this->pendingTickerIndex_;
    }
    else if (!visibleBadgeIndices.empty())
    {
        this->tickerIndex_ = this->normalizedTickerIndex(visibleBadgeIndices);
    }
    else
    {
        this->tickerIndex_ = -1;
    }
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideDirection_ = 1;
    this->tickerSlideAnimation_.setDuration(TICKER_SLIDE_DURATION_MS);
    if (this->expandedIndex_ >= 0 && this->tickerIndex_ >= 0)
    {
        this->expandedIndex_ = this->tickerIndex_;
        this->animateDetail(this->expandedDetailHeight());
    }
    this->restartTickerIfNeeded();
    this->update();
}

int StreamDatabaseBadgeBar::tickerIndexAfterStep(
    const std::vector<int> &visibleBadgeIndexes, int sourceIndex,
    int direction) const
{
    if (direction == 0 || sourceIndex < 0 || visibleBadgeIndexes.empty())
    {
        return -1;
    }

    int currentPosition = -1;
    for (int i = 0; i < static_cast<int>(visibleBadgeIndexes.size()); ++i)
    {
        if (visibleBadgeIndexes[static_cast<size_t>(i)] == sourceIndex)
        {
            currentPosition = i;
            break;
        }
    }
    if (currentPosition < 0)
    {
        return -1;
    }

    const int count = static_cast<int>(visibleBadgeIndexes.size());
    int nextPosition = (currentPosition + (direction < 0 ? -1 : 1)) % count;
    if (nextPosition < 0)
    {
        nextPosition += count;
    }

    return visibleBadgeIndexes[static_cast<size_t>(nextPosition)];
}

void StreamDatabaseBadgeBar::setNavigationHoverVisible(bool visible)
{
    const qreal target = visible ? 1.0 : 0.0;
    if (std::abs(this->navigationHoverProgress_ - target) < 0.001)
    {
        this->navigationHoverProgress_ = target;
        this->update();
        return;
    }

    this->navigationHoverAnimation_.stop();
    this->navigationHoverAnimation_.setStartValue(
        this->navigationHoverProgress_);
    this->navigationHoverAnimation_.setEndValue(target);
    this->navigationHoverAnimation_.start();
}

void StreamDatabaseBadgeBar::stepBadgeFeed(int direction)
{
    if (direction == 0)
    {
        return;
    }

    if (this->tickerSlideAnimation_.state() != QAbstractAnimation::Stopped)
    {
        const bool wasBlocked = this->tickerSlideAnimation_.blockSignals(true);
        this->tickerSlideAnimation_.stop();
        this->tickerSlideAnimation_.blockSignals(wasBlocked);
        this->finishTickerSlide();
    }

    this->hoverPaused_ = true;
    this->tickerTimer_.stop();
    this->beginTickerSlide(direction, MANUAL_TICKER_SLIDE_DURATION_MS);
}

void StreamDatabaseBadgeBar::setExpandedIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(this->badges_.size()) ||
        this->badgeHiddenByOwnedMode(index))
    {
        return;
    }

    if (this->expandedIndex_ == index && this->detailVisibleHeight_ > 0.0)
    {
        this->animateDetail(0.0);
        return;
    }

    this->expandedIndex_ = index;
    this->tickerIndex_ = index;
    this->tickerTimer_.stop();
    this->tickerSlideAnimation_.stop();
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideDirection_ = 1;
    this->animateDetail(this->expandedDetailHeight());
    this->update();
}

void StreamDatabaseBadgeBar::animateDetail(qreal targetHeight)
{
    this->detailAnimation_.stop();
    this->detailAnimation_.setStartValue(this->detailVisibleHeight_);
    this->detailAnimation_.setEndValue(std::max<qreal>(0.0, targetHeight));
    this->detailAnimation_.start();
}

void StreamDatabaseBadgeBar::updateFixedHeight()
{
    if (!this->visibleForChannel_)
    {
        this->setFixedHeight(0);
        this->hide();
        return;
    }

    this->setFixedHeight(this->barHeight());
    this->show();
}

void StreamDatabaseBadgeBar::updateNavigationHover(const QPoint &position)
{
    const bool overNavigation = this->navigationHoverContains(position);
    if (overNavigation)
    {
        this->pauseTickerForPointer();
    }
    else if (this->hoverPaused_)
    {
        this->hoverPaused_ = false;
        this->restartTickerIfNeeded();
    }

    this->setNavigationHoverVisible(overNavigation);
}

void StreamDatabaseBadgeBar::updateHoverState(const QPoint &position)
{
    const int badgeIndex = this->badgeIndexAt(position);
    const bool overLink = this->linkContains(position);
    const bool overSettingsButton = this->settingsButtonContains(position);
    const bool overSourceLink = this->sourceLinkContains(position);
    const bool overChannelLink = this->channelLinkIndexAt(position) >= 0;
    const bool overPreviousArrow = this->previousArrowContains(position);
    const bool overNextArrow = this->nextArrowContains(position);
    const bool overBadgeInfo = this->collapsedBadgeInfoContains(position);
    if (badgeIndex == this->hoverBadgeIndex_ &&
        overLink == this->hoverSiteLink_ &&
        overSettingsButton == this->hoverSettingsButton_ &&
        overSourceLink == this->hoverSourceLink_ &&
        overChannelLink == this->hoverChannelLink_ &&
        overPreviousArrow == this->hoverPreviousArrow_ &&
        overNextArrow == this->hoverNextArrow_)
    {
        if (overBadgeInfo)
        {
            this->setCursor(Qt::PointingHandCursor);
        }
        else if (badgeIndex < 0 && !overLink && !overSettingsButton &&
                 !overSourceLink && !overChannelLink && !overPreviousArrow &&
                 !overNextArrow)
        {
            this->unsetCursor();
        }
        return;
    }

    this->hoverBadgeIndex_ = badgeIndex;
    this->hoverSiteLink_ = overLink;
    this->hoverSettingsButton_ = overSettingsButton;
    this->hoverSourceLink_ = overSourceLink;
    this->hoverChannelLink_ = overChannelLink;
    this->hoverPreviousArrow_ = overPreviousArrow;
    this->hoverNextArrow_ = overNextArrow;
    if (badgeIndex >= 0 || overLink || overSettingsButton || overSourceLink ||
        overChannelLink || overPreviousArrow || overNextArrow || overBadgeInfo)
    {
        this->setCursor(Qt::PointingHandCursor);
    }
    else
    {
        this->unsetCursor();
    }
    this->update();
}

void StreamDatabaseBadgeBar::showBadgeMenu(int badgeIndex,
                                           const QPoint &globalPosition)
{
    if (badgeIndex < 0 || badgeIndex >= static_cast<int>(this->badges_.size()))
    {
        return;
    }

    const QString siteUrl = this->badges_[badgeIndex].siteUrl;
    QMenu menu(this);

    auto *openAction =
        menu.addAction(QStringLiteral("Open on StreamDatabase"));
    openAction->setEnabled(!siteUrl.isEmpty());
    QObject::connect(openAction, &QAction::triggered, this, [siteUrl] {
        QDesktopServices::openUrl(QUrl(siteUrl));
    });

    menu.exec(globalPosition);
}

void StreamDatabaseBadgeBar::showSettingsMenu(const QPoint &globalPosition,
                                              bool toggleExisting,
                                              bool alignRight)
{
    if (this->settingsMenu_ != nullptr)
    {
        if (this->settingsMenu_->isVisible() && toggleExisting)
        {
            this->settingsMenu_->close();
            return;
        }

        auto *oldMenu = this->settingsMenu_.data();
        this->settingsMenu_.clear();
        oldMenu->close();
        oldMenu->deleteLater();
    }

    auto *menu = new QMenu(this);
    this->settingsMenu_ = menu;
    this->update();

    auto *titleAction =
        menu->addAction(QStringLiteral("StreamDatabase settings"));
    titleAction->setEnabled(false);
    menu->addSeparator();

    auto *feedAction = menu->addAction(QStringLiteral("Badge feed"));
    feedAction->setCheckable(true);
    feedAction->setChecked(this->badgeFeedVisible_);
    QObject::connect(feedAction, &QAction::toggled, this, [this](bool checked) {
        if (auto *split = dynamic_cast<Split *>(this->parentWidget()))
        {
            split->setStreamDatabaseBadgeFeedVisible(checked);
        }
        else
        {
            this->setBadgeFeedVisible(checked);
        }
    });

    auto *activityMenu = menu->addMenu(QStringLiteral("Badge activity"));
    auto *activityGroup = new QActionGroup(menu);
    activityGroup->setExclusive(true);
    const auto currentActivityMode = streamDatabaseBadgeActivityFeedMode();
    auto addActivityModeAction = [&](const QString &label,
                                     BadgeActivityFeedMode mode) {
        auto *action = activityMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(currentActivityMode == mode);
        activityGroup->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, mode] {
            if (streamDatabaseBadgeActivityFeedMode() == mode)
            {
                return;
            }
            setStreamDatabaseBadgeActivityFeedMode(mode);
            this->badges_.clear();
            this->expandedIndex_ = -1;
            this->detailVisibleHeight_ = 0.0;
            this->reconcileOwnedBadgeVisibility();
            this->requestEvents();
        });
    };
    addActivityModeAction(QStringLiteral("Active badges"),
                          BadgeActivityFeedMode::ActiveOnly);
    addActivityModeAction(QStringLiteral("Upcoming badges"),
                          BadgeActivityFeedMode::UpcomingOnly);
    addActivityModeAction(QStringLiteral("All badges"),
                          BadgeActivityFeedMode::All);

    auto *ownedMenu = menu->addMenu(QStringLiteral("Owned badges"));
    auto *ownedGroup = new QActionGroup(menu);
    ownedGroup->setExclusive(true);
    const auto currentOwnedMode = streamDatabaseOwnedBadgeFeedMode();
    auto addOwnedModeAction = [&](const QString &label,
                                  OwnedBadgeFeedMode mode) {
        auto *action = ownedMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(currentOwnedMode == mode);
        ownedGroup->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, mode] {
            setStreamDatabaseOwnedBadgeFeedMode(mode);
            this->reconcileOwnedBadgeVisibility();
        });
    };
    addOwnedModeAction(QStringLiteral("All"), OwnedBadgeFeedMode::ShowAll);
    addOwnedModeAction(QStringLiteral("Hide"), OwnedBadgeFeedMode::HideOwned);
    addOwnedModeAction(QStringLiteral("Checkmark"),
                       OwnedBadgeFeedMode::CheckmarkOwned);

    auto *pingsMenu = menu->addMenu(QStringLiteral("Badge pings"));
    auto *pingsGroup = new QActionGroup(menu);
    pingsGroup->setExclusive(true);
    const auto currentPingMode = streamDatabaseBadgePingMode();
    auto addPingModeAction = [&](const QString &label, BadgePingMode mode) {
        auto *action = pingsMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(currentPingMode == mode);
        pingsGroup->addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, mode] {
            setStreamDatabaseBadgePingMode(mode);
            this->updateEventsRefreshTimer();
        });
    };
    addPingModeAction(QStringLiteral("New badges"), BadgePingMode::NewBadges);
    addPingModeAction(QStringLiteral("Available now badges"),
                      BadgePingMode::AvailableNow);
    addPingModeAction(QStringLiteral("Both"), BadgePingMode::Both);
    addPingModeAction(QStringLiteral("None"), BadgePingMode::None);
    auto updateCursorForSettingsButton = [this] {
        const bool overSettingsButton =
            this->settingsButtonContains(this->mapFromGlobal(QCursor::pos()));
        const bool hasOverride =
            this->property(SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY).toBool();
        if (overSettingsButton && !hasOverride)
        {
            QApplication::setOverrideCursor(Qt::PointingHandCursor);
            this->setProperty(SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY, true);
        }
        else if (!overSettingsButton && hasOverride)
        {
            QApplication::restoreOverrideCursor();
            this->setProperty(SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY, false);
        }
    };
    auto *cursorTimer = new QTimer(menu);
    cursorTimer->setInterval(40);
    QObject::connect(cursorTimer, &QTimer::timeout, this,
                     updateCursorForSettingsButton);
    cursorTimer->start();
    updateCursorForSettingsButton();

    QObject::connect(menu, &QMenu::aboutToHide, this, [this, menu] {
        if (this->property(SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY).toBool())
        {
            QApplication::restoreOverrideCursor();
            this->setProperty(SETTINGS_MENU_CURSOR_OVERRIDE_PROPERTY, false);
        }
        if (this->settingsMenu_ == menu)
        {
            this->settingsMenu_.clear();
            this->setProperty(SETTINGS_MENU_CLOSED_AT_PROPERTY,
                              QDateTime::currentMSecsSinceEpoch());
            this->update();
            menu->deleteLater();
        }
    });

    QPoint popupPosition = globalPosition;
    if (alignRight)
    {
        menu->adjustSize();
        popupPosition.rx() -= menu->sizeHint().width();
    }
    menu->popup(popupPosition);
}

void StreamDatabaseBadgeBar::requestEvents()
{
    streamDatabaseEventsPoller().refresh(this);
}

void StreamDatabaseBadgeBar::applyEventsData(const QByteArray &data,
                                             bool allowPings)
{
    const auto activityMode = streamDatabaseBadgeActivityFeedMode();
    const auto allParsed =
        StreamDatabaseBadgeBar::parseEventBadges(data, false, false);
    auto parsed = activityMode == BadgeActivityFeedMode::All
                      ? allParsed
                      : StreamDatabaseBadgeBar::parseEventBadges(
                            data,
                            activityMode == BadgeActivityFeedMode::ActiveOnly,
                            activityMode == BadgeActivityFeedMode::UpcomingOnly);
    std::vector<BadgeItem> newBadges;
    std::vector<BadgeItem> availableNowBadges;
    for (const auto &badge : allParsed)
    {
        const auto key = badgeEventKey(badge);
        if (this->hasLoadedEvents_ &&
            !this->seenEventBadgeKeys_.contains(key))
        {
            newBadges.push_back(badge);
        }
        if (badge.active)
        {
            const auto activeKey = badgeAvailabilityKey(badge);
            if (this->hasLoadedEvents_ &&
                !this->seenActiveEventBadgeKeys_.contains(activeKey))
            {
                availableNowBadges.push_back(badge);
            }
            this->seenActiveEventBadgeKeys_.insert(activeKey);
        }
        this->seenEventBadgeKeys_.insert(key);
    }
    this->hasLoadedEvents_ = true;

    this->tickerSlideAnimation_.stop();
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideDirection_ = 1;
    this->badges_ = std::move(parsed);
    this->reconcileOwnedBadgeVisibility();
    this->requestBadgeImages();
    if (allowPings)
    {
        this->notifyBadgePings(newBadges, availableNowBadges);
    }
}

void StreamDatabaseBadgeBar::notifyBadgePings(
    const std::vector<BadgeItem> &newBadges,
    const std::vector<BadgeItem> &availableNowBadges)
{
    const auto pingMode = streamDatabaseBadgePingMode();
    if (pingMode == BadgePingMode::None)
    {
        return;
    }

    std::vector<BadgeItem> combinedBadges;
    const std::vector<BadgeItem> *badges = &newBadges;
    if (pingMode == BadgePingMode::AvailableNow)
    {
        badges = &availableNowBadges;
    }
    else if (pingMode == BadgePingMode::Both)
    {
        QSet<QString> seenKeys;
        auto appendUniqueBadge = [&](const BadgeItem &badge) {
            const auto key = badgeEventKey(badge);
            if (seenKeys.contains(key))
            {
                return;
            }
            seenKeys.insert(key);
            combinedBadges.push_back(badge);
        };
        for (const auto &badge : newBadges)
        {
            appendUniqueBadge(badge);
        }
        for (const auto &badge : availableNowBadges)
        {
            appendUniqueBadge(badge);
        }
        badges = &combinedBadges;
    }

    if (badges->empty())
    {
        return;
    }

    auto channel = this->streamDatabaseChannel_.lock();
    if (channel == nullptr)
    {
        return;
    }

    auto badgeText = [](const BadgeItem &badge) {
        QString text = QStringLiteral("%1 - %2").arg(badge.badgeName,
                                                     badge.eventTitle);
        if (!badge.endsText.isEmpty())
        {
            text += QStringLiteral(" (") + badge.endsText + QStringLiteral(")");
        }
        return text;
    };

    QString messageText;
    const bool hasBothPingKinds = pingMode == BadgePingMode::Both &&
                                  !newBadges.empty() &&
                                  !availableNowBadges.empty();
    const auto singlePrefix =
        hasBothPingKinds
            ? QStringLiteral("StreamDatabase badge update")
            : pingMode == BadgePingMode::AvailableNow
            ? QStringLiteral("StreamDatabase badge available now")
            : QStringLiteral("New StreamDatabase badge");
    const auto groupedPrefix =
        hasBothPingKinds
            ? QStringLiteral("StreamDatabase badge updates")
            : pingMode == BadgePingMode::AvailableNow
            ? QStringLiteral("StreamDatabase badges available now")
            : QStringLiteral("New StreamDatabase badges");
    if (badges->size() == 1)
    {
        const auto &badge = badges->front();
        messageText =
            QStringLiteral("%1: %2").arg(singlePrefix, badgeText(badge));
        if (!badge.siteUrl.isEmpty())
        {
            messageText += QStringLiteral(" ") + badge.siteUrl;
        }
    }
    else
    {
        QStringList badgeSummaries;
        constexpr qsizetype MAX_GROUPED_BADGE_SUMMARIES = 4;
        const auto summaryCount =
            std::min<qsizetype>(static_cast<qsizetype>(badges->size()),
                                MAX_GROUPED_BADGE_SUMMARIES);
        for (qsizetype i = 0; i < summaryCount; ++i)
        {
            badgeSummaries.push_back(badgeText((*badges)[i]));
        }

        messageText = QStringLiteral("%1 (%2): %3")
                          .arg(groupedPrefix)
                          .arg(badges->size())
                          .arg(badgeSummaries.join(QStringLiteral("; ")));
        if (static_cast<qsizetype>(badges->size()) > summaryCount)
        {
            messageText += QStringLiteral("; +%1 more")
                               .arg(static_cast<qsizetype>(badges->size()) -
                                    summaryCount);
        }
        messageText += QStringLiteral(" ") +
                       QString::fromLatin1(STREAMDATABASE_SITE);
    }

    auto message =
        std::const_pointer_cast<Message>(makeSystemMessage(messageText));
    message->flags.unset(MessageFlag::DoNotTriggerNotification);
    message->flags.set(MessageFlag::Highlighted);
    message->flags.set(MessageFlag::ShowInMentions);
    message->highlightColor =
        ColorProvider::instance().color(ColorType::SelfHighlight);
    message->channelName = channel->getName();

    MessageBuilder::triggerHighlights(
        channel.get(),
        HighlightAlert{
            QUrl(getSettings()->selfHighlightSoundUrl.getValue()),
            getSettings()->enableSelfHighlightSound.getValue(),
            getSettings()->enableSelfHighlightTaskbar.getValue(),
        });

    getApp()->getTwitch()->getMentionsChannel()->addMessage(
        message, MessageContext::Original);
    channel->addMessage(message, MessageContext::Original);
}

void StreamDatabaseBadgeBar::requestBadgeImages()
{
    for (auto &badge : this->badges_)
    {
        if (badge.badgeImageUrl.isEmpty())
        {
            continue;
        }

        if (const auto cached = this->badgePixmaps_.constFind(badge.badgeImageUrl);
            cached != this->badgePixmaps_.constEnd())
        {
            badge.accent = accentFromBadgePixmap(cached.value());
            continue;
        }

        QNetworkRequest request(QUrl(badge.badgeImageUrl));
        auto *reply = this->network_.get(request);
        this->pendingBadgeRequests_.insert(reply, badge.badgeImageUrl);

        QObject::connect(reply, &QNetworkReply::finished, this,
                         [this, reply] {
                             this->handleBadgeImageFinished(reply);
                         });
    }
}

void StreamDatabaseBadgeBar::handleBadgeImageFinished(QNetworkReply *reply)
{
    const QString badgeImageUrl = this->pendingBadgeRequests_.take(reply);
    if (reply->error() == QNetworkReply::NoError)
    {
        QPixmap pixmap;
        pixmap.loadFromData(reply->readAll());
        if (!pixmap.isNull())
        {
            this->badgePixmaps_.insert(badgeImageUrl, pixmap);
            const QString accent = accentFromBadgePixmap(pixmap);
            for (auto &badge : this->badges_)
            {
                if (badge.badgeImageUrl == badgeImageUrl)
                {
                    badge.accent = accent;
                }
            }
            this->update();
        }
    }

    reply->deleteLater();
}

void StreamDatabaseBadgeBar::requestStreamDatabaseLogo()
{
    if (this->pendingLogoRequest_ != nullptr ||
        !this->streamDatabaseLogo_.isNull())
    {
        return;
    }

    QNetworkRequest request(
        QUrl(QString::fromLatin1(STREAMDATABASE_LOGO_URL)));
    auto *reply = this->network_.get(request);
    this->pendingLogoRequest_ = reply;

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply] {
        this->handleStreamDatabaseLogoFinished(reply);
    });
}

void StreamDatabaseBadgeBar::handleStreamDatabaseLogoFinished(
    QNetworkReply *reply)
{
    if (this->pendingLogoRequest_ == reply)
    {
        this->pendingLogoRequest_ = nullptr;
    }

    if (reply->error() == QNetworkReply::NoError)
    {
        QPixmap pixmap;
        pixmap.loadFromData(reply->readAll());
        if (!pixmap.isNull())
        {
            this->streamDatabaseLogo_ = pixmap;
            this->update();
        }
    }

    reply->deleteLater();
}

void StreamDatabaseBadgeBar::reconcileOwnedBadgeVisibility()
{
    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    this->tickerSlideAnimation_.stop();
    this->slideSourceTickerIndex_ = -1;
    this->pendingTickerIndex_ = -1;
    this->tickerSlideProgress_ = 0.0;
    this->tickerSlideDirection_ = 1;
    this->tickerIndex_ = this->normalizedTickerIndex(visibleBadgeIndices);

    if (this->expandedIndex_ >= 0 &&
        this->badgeHiddenByOwnedMode(this->expandedIndex_))
    {
        this->detailAnimation_.stop();
        this->expandedIndex_ = -1;
        this->detailVisibleHeight_ = 0.0;
    }

    this->hoverBadgeIndex_ = -1;
    this->hoverChannelLink_ = false;
    this->badgeRects_.clear();
    this->channelLinkRects_.clear();
    this->restartTickerIfNeeded();
    this->updateFixedHeight();
    this->update();
}

int StreamDatabaseBadgeBar::collapsedHeight() const
{
    const auto scale = this->scale();
    auto *fonts = getApp()->getFonts();
    const QFontMetrics titleMetrics(
        fonts->getFont(FontStyle::UiMediumBold, scale));
    const QFontMetrics detailMetrics(fonts->getFont(FontStyle::UiMedium, scale));

    return std::max(scaledInt(scale, 42),
                    titleMetrics.height() + detailMetrics.height() +
                        scaledInt(scale, 11));
}

int StreamDatabaseBadgeBar::expandedDetailHeight() const
{
    const auto scale = this->scale();
    auto *fonts = getApp()->getFonts();
    const QFontMetrics titleMetrics(
        fonts->getFont(FontStyle::UiMediumBold, scale));
    const QFontMetrics detailMetrics(fonts->getFont(FontStyle::UiMedium, scale));

    int requirementHeight = detailMetrics.height();
    if (this->expandedIndex_ >= 0 &&
        this->expandedIndex_ < static_cast<int>(this->badges_.size()))
    {
        const int outerInset = std::max(1, scaledInt(scale, 1));
        const int paddingX = scaledInt(scale, 8);
        const int railWidth = std::max(2, scaledInt(scale, 3));
        const int contentLeft = paddingX + railWidth + scaledInt(scale, 5);
        const int contentRight = this->width() - outerInset - paddingX - 1;
        const int largeBadgeSize = std::max(22, scaledInt(scale, 26));
        const int textLeft = contentLeft + largeBadgeSize + scaledInt(scale, 9);
        const int textWidth = std::max(0, contentRight - textLeft);

        if (textWidth > 0)
        {
            requirementHeight = std::max(
                requirementHeight,
                detailMetrics
                    .boundingRect(QRect{0, 0, textWidth, 10000},
                                  Qt::TextWordWrap | Qt::AlignLeft |
                                      Qt::AlignTop,
                                  this->badges_[this->expandedIndex_]
                                      .requirementText)
                    .height());
        }
    }

    const int textHeight = titleMetrics.height() + detailMetrics.height() +
                           requirementHeight + detailMetrics.height() +
                           scaledInt(scale, 4);
    const int detailPaddingY = scaledInt(scale, 7);
    const int largeBadgeSize = std::max(22, scaledInt(scale, 26));

    return std::max(scaledInt(scale, 72),
                    std::max(largeBadgeSize, textHeight) +
                        detailPaddingY * 2);
}

int StreamDatabaseBadgeBar::barHeight() const
{
    if (!this->visibleForChannel_ || this->badges_.empty())
    {
        return 0;
    }

    const int outerInset = std::max(1, scaledInt(this->scale(), 1));
    int height = this->collapsedHeight() + outerInset * 2;
    if (this->expandedIndex_ >= 0 && this->detailVisibleHeight_ > 0.0)
    {
        height += int(std::round(this->detailVisibleHeight_));
    }
    return height;
}

bool StreamDatabaseBadgeBar::badgeHiddenByOwnedMode(int index) const
{
    if (index < 0 || index >= static_cast<int>(this->badges_.size()))
    {
        return true;
    }

    if (streamDatabaseOwnedBadgeFeedMode() != OwnedBadgeFeedMode::HideOwned)
    {
        return false;
    }

    const auto &badge = this->badges_[index];
    return twitch::currentUserHasBadge(badge.setID, badge.versionID);
}

std::vector<int> StreamDatabaseBadgeBar::visibleBadgeIndexes() const
{
    std::vector<int> indexes;
    indexes.reserve(this->badges_.size());
    for (int i = 0; i < static_cast<int>(this->badges_.size()); ++i)
    {
        if (!this->badgeHiddenByOwnedMode(i))
        {
            indexes.push_back(i);
        }
    }
    return indexes;
}

int StreamDatabaseBadgeBar::normalizedTickerIndex(
    const std::vector<int> &visibleBadgeIndexes) const
{
    if (visibleBadgeIndexes.empty())
    {
        return -1;
    }

    for (const int index : visibleBadgeIndexes)
    {
        if (index == this->tickerIndex_)
        {
            return index;
        }
    }

    for (const int index : visibleBadgeIndexes)
    {
        if (index > this->tickerIndex_)
        {
            return index;
        }
    }

    return visibleBadgeIndexes.front();
}

int StreamDatabaseBadgeBar::badgeIndexAt(const QPoint &position) const
{
    for (size_t i = 0; i < this->badgeRects_.size(); ++i)
    {
        if (this->badgeRects_[i].contains(position))
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

int StreamDatabaseBadgeBar::channelLinkIndexAt(const QPoint &position) const
{
    for (size_t i = 0; i < this->channelLinkRects_.size(); ++i)
    {
        if (this->channelLinkRects_[i].rect.contains(position))
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

bool StreamDatabaseBadgeBar::linkContains(const QPoint &position) const
{
    return this->siteLinkRect_.contains(position);
}

bool StreamDatabaseBadgeBar::settingsButtonContains(
    const QPoint &position) const
{
    return this->settingsButtonRect_.contains(position);
}

bool StreamDatabaseBadgeBar::sourceLinkContains(const QPoint &position) const
{
    return this->sourceLinkRect_.contains(position);
}

bool StreamDatabaseBadgeBar::navigationHoverContains(
    const QPoint &position) const
{
    if (this->navigationHoverProgress_ > 0.001 &&
        (this->previousArrowRect_.contains(position) ||
         this->nextArrowRect_.contains(position)))
    {
        return true;
    }

    QRect badgeArea;
    for (const auto &rect : this->badgeRects_)
    {
        if (!rect.isValid())
        {
            continue;
        }

        badgeArea = badgeArea.isValid() ? badgeArea.united(rect) : rect;
    }

    if (badgeArea.isValid())
    {
        const auto scale = this->scale();
        const QRect triggerRect =
            badgeArea.adjusted(-scaledInt(scale, 24), -scaledInt(scale, 5),
                               scaledInt(scale, 3), scaledInt(scale, 5));
        return triggerRect.contains(position);
    }

    return false;
}

bool StreamDatabaseBadgeBar::previousArrowContains(
    const QPoint &position) const
{
    return this->previousArrowRect_.contains(position);
}

bool StreamDatabaseBadgeBar::nextArrowContains(const QPoint &position) const
{
    return this->nextArrowRect_.contains(position);
}

bool StreamDatabaseBadgeBar::collapsedBadgeInfoContains(
    const QPoint &position) const
{
    if (!this->visibleForChannel_ || this->badges_.empty() ||
        this->siteLinkRect_.contains(position) ||
        this->previousArrowRect_.contains(position) ||
        this->nextArrowRect_.contains(position))
    {
        return false;
    }

    const auto scale = this->scale();
    const int outerInset = std::max(1, scaledInt(scale, 1));
    const int rowHeight = this->collapsedHeight();
    const QRect itemRect =
        this->rect().adjusted(0, outerInset, -1, -outerInset).normalized();
    const QRect collapsedRect{itemRect.left(), itemRect.top(),
                              itemRect.width(), rowHeight};
    if (!collapsedRect.contains(position))
    {
        return false;
    }

    int badgesRight = -1;
    for (const auto &rect : this->badgeRects_)
    {
        if (!rect.isValid())
        {
            continue;
        }

        if (rect.contains(position))
        {
            return false;
        }
        badgesRight = std::max(badgesRight, rect.right());
    }

    const int left =
        badgesRight >= 0 ? badgesRight + scaledInt(scale, 6)
                         : itemRect.left() + scaledInt(scale, 24);
    const int right = this->siteLinkRect_.isValid()
                          ? this->siteLinkRect_.left() - scaledInt(scale, 4)
                          : itemRect.right();
    if (right <= left)
    {
        return false;
    }

    return QRect{left, collapsedRect.top(), right - left,
                 collapsedRect.height()}
        .contains(position);
}

int StreamDatabaseBadgeBar::linkedBadgeIndex() const
{
    if (this->expandedIndex_ >= 0 && this->expandedIndex_ <
                                         static_cast<int>(this->badges_.size()) &&
        !this->badgeHiddenByOwnedMode(this->expandedIndex_) &&
        this->detailVisibleHeight_ > 0.001)
    {
        return this->expandedIndex_;
    }

    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    if (visibleBadgeIndices.empty())
    {
        return -1;
    }

    return this->normalizedTickerIndex(visibleBadgeIndices);
}

bool StreamDatabaseBadgeBar::canRunTicker() const
{
    const auto visibleBadgeIndices = this->visibleBadgeIndexes();
    if (!this->visibleForChannel_ || this->hoverPaused_ || this->underMouse() ||
        this->expandedIndex_ >= 0 ||
        visibleBadgeIndices.size() <= VISIBLE_BADGE_COUNT || !this->isVisible())
    {
        return false;
    }

    auto *topLevelWindow = this->window();
    if (topLevelWindow == nullptr ||
        !this->isVisibleTo(topLevelWindow))
    {
        return false;
    }

    return true;
}

void StreamDatabaseBadgeBar::drawRequirementText(
    QPainter &painter, const QRect &rect, const BadgeItem &badge,
    const QFont &font, const QColor &textColor, const QColor &linkColor)
{
    if (rect.width() <= 0 || rect.height() <= 0)
    {
        return;
    }

    const auto ranges = channelLinkRangesForText(
        badge.requirementText, badge.channelLinks, badge.requirementLinks);

    QTextLayout layout(badge.requirementText, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(option);

    QList<QTextLayout::FormatRange> formats;
    formats.reserve(static_cast<qsizetype>(ranges.size()));
    for (const auto &range : ranges)
    {
        QTextCharFormat format;
        format.setForeground(linkColor);
        format.setFontUnderline(false);

        QTextLayout::FormatRange formatRange;
        formatRange.start = range.start;
        formatRange.length = range.length;
        formatRange.format = format;
        formats.push_back(formatRange);
    }
    layout.setFormats(formats);

    layout.beginLayout();
    qreal y = 0.0;
    while (true)
    {
        QTextLine line = layout.createLine();
        if (!line.isValid())
        {
            break;
        }

        line.setLineWidth(rect.width());
        line.setPosition(QPointF(0.0, y));
        y += line.height();
        if (y > rect.height() + line.height())
        {
            break;
        }
    }
    layout.endLayout();

    painter.save();
    painter.setFont(font);
    painter.setPen(textColor);
    layout.draw(&painter, rect.topLeft());
    painter.restore();

    for (const auto &range : ranges)
    {
        const int rangeEnd = range.start + range.length;
        for (int i = 0; i < layout.lineCount(); ++i)
        {
            const QTextLine line = layout.lineAt(i);
            const int lineStart = line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const int start = std::max(range.start, lineStart);
            const int end = std::min(rangeEnd, lineEnd);
            if (start >= end)
            {
                continue;
            }

            const qreal startX = line.cursorToX(start);
            const qreal endX = line.cursorToX(end);
            const QPointF linePosition = line.position();
            QRect linkRect{
                rect.left() +
                    static_cast<int>(std::floor(std::min(startX, endX))),
                rect.top() + static_cast<int>(std::floor(linePosition.y())),
                static_cast<int>(std::ceil(std::abs(endX - startX))),
                static_cast<int>(std::ceil(line.height())),
            };
            this->channelLinkRects_.push_back(
                {linkRect.adjusted(-1, 0, 1, 0), range.login, range.url});
        }
    }
}

void StreamDatabaseBadgeBar::drawBadgeIcon(QPainter &painter,
                                           const QRect &rect,
                                           const BadgeItem &badge,
                                           bool active) const
{
    const bool light = this->theme->isLightTheme();
    const QColor accent(badge.accent);
    const bool owned = twitch::currentUserHasBadge(badge.setID,
                                                   badge.versionID);
    const QColor shadow = withAlpha(accent, light ? 42 : 58);

    painter.save();
    fillRoundedRect(painter, QRectF(rect.adjusted(-1, -1, 1, 1)), shadow,
                    std::max(2, rect.height() / 5));

    fillRoundedRect(
        painter, QRectF(rect),
        withAlpha(this->theme->messages.backgrounds.regular, light ? 180 : 210),
        std::max(2, rect.height() / 5));

    const QPixmap pixmap = this->badgePixmaps_.value(badge.badgeImageUrl);
    if (!pixmap.isNull())
    {
        const QRect imageRect = rect.adjusted(1, 1, -1, -1);
        const bool wasSmooth =
            painter.testRenderHint(QPainter::SmoothPixmapTransform);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawPixmap(imageRect, pixmap);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, wasSmooth);
    }
    else
    {
        QString initials;
        for (const auto ch : badge.badgeName)
        {
            if (ch.isLetterOrNumber())
            {
                initials.append(ch.toUpper());
                if (initials.size() == 2)
                {
                    break;
                }
            }
        }

        if (initials.isEmpty())
        {
            initials = QStringLiteral("?");
        }

        painter.setPen(withAlpha(accent, light ? 255 : 230));
        painter.setFont(getApp()->getFonts()->getFont(FontStyle::UiMediumBold,
                                                      this->scale()));
        painter.drawText(rect, Qt::AlignCenter, initials);
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(accent, active ? 2 : 1));
    painter.drawRoundedRect(QRectF(rect.adjusted(0, 0, -1, -1)),
                            std::max(2, rect.height() / 5),
                            std::max(2, rect.height() / 5));
    if (owned &&
        streamDatabaseOwnedBadgeFeedMode() ==
            OwnedBadgeFeedMode::CheckmarkOwned)
    {
        this->drawOwnedBadgeMark(painter, rect);
    }
    painter.restore();
}

void StreamDatabaseBadgeBar::drawOwnedBadgeMark(QPainter &painter,
                                                const QRect &rect) const
{
    const QColor green("#22C55E");

    painter.save();
    const qreal markSize = std::max<qreal>(8.0, rect.height() * 0.48);
    QRectF markRect{
        qreal(rect.right()) - markSize + 1.0,
        qreal(rect.top()),
        markSize,
        markSize,
    };

    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(Qt::black, 85));
    painter.drawEllipse(markRect.adjusted(-1.0, 1.0, 1.0, 2.0));
    painter.setBrush(green);
    painter.drawEllipse(markRect);

    QPainterPath check;
    check.moveTo(markRect.left() + markRect.width() * 0.26,
                 markRect.top() + markRect.height() * 0.53);
    check.lineTo(markRect.left() + markRect.width() * 0.43,
                 markRect.top() + markRect.height() * 0.69);
    check.lineTo(markRect.left() + markRect.width() * 0.75,
                 markRect.top() + markRect.height() * 0.33);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white,
                        std::max<qreal>(1.1, markRect.width() * 0.16),
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(check);
    painter.restore();
}

void StreamDatabaseBadgeBar::drawNavigationArrow(QPainter &painter,
                                                 const QRect &rect,
                                                 bool previous,
                                                 qreal progress, bool hovered,
                                                 const QColor &color) const
{
    if (!rect.isValid() || progress <= 0.001)
    {
        return;
    }

    progress = std::clamp(progress, 0.0, 1.0);
    QColor arrowColor = color;
    arrowColor.setAlphaF(
        std::clamp(progress * (hovered ? 1.0 : 0.9), 0.0, 1.0));

    const auto scale = this->scale();
    const qreal halfHeight = rect.height() * 0.31;
    const qreal halfWidth = rect.width() * 0.24;
    const QPointF center = QRectF(rect).center();
    const qreal entranceOffset =
        (previous ? -1.0 : 1.0) * scaledInt(scale, 2) * (1.0 - progress);
    const QPointF offset{entranceOffset, 0.0};

    QPointF top;
    QPointF mid;
    QPointF bottom;
    if (previous)
    {
        top = {center.x() + halfWidth, center.y() - halfHeight};
        mid = {center.x() - halfWidth, center.y()};
        bottom = {center.x() + halfWidth, center.y() + halfHeight};
    }
    else
    {
        top = {center.x() - halfWidth, center.y() - halfHeight};
        mid = {center.x() + halfWidth, center.y()};
        bottom = {center.x() - halfWidth, center.y() + halfHeight};
    }

    QPen pen(arrowColor, hovered ? std::max<qreal>(1.55, scale * 1.65)
                                 : std::max<qreal>(1.35, scale * 1.45));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPolyline(QPolygonF{top + offset, mid + offset,
                                   bottom + offset});
    painter.restore();
}

void StreamDatabaseBadgeBar::drawStreamDatabaseLink(QPainter &painter,
                                                    const QRect &rect,
                                                    const QFont &font,
                                                    bool hovered) const
{
    const auto scale = this->scale();
    const int paddingX = scaledInt(scale, 4);
    const int logoSize =
        std::min(rect.height() - scaledInt(scale, 4), scaledInt(scale, 24));
    const int gap = scaledInt(scale, 5);

    painter.save();

    const QRect logoRect{
        rect.left() + paddingX,
        rect.center().y() - logoSize / 2,
        logoSize,
        logoSize,
    };
    this->drawStreamDatabaseLogoMark(painter, logoRect);

    painter.setFont(font);
    painter.setPen(withAlpha(Qt::white, hovered ? 255 : 238));

    const QFontMetrics metrics(font);
    const int lineHeight =
        std::max(1, metrics.height() - scaledInt(scale, 2));
    const int textLeft = logoRect.right() + gap;
    const int textWidth = std::max(0, rect.right() - textLeft + 1);
    const int textTop = rect.center().y() - lineHeight;

    painter.drawText(QRect{textLeft, textTop, textWidth, lineHeight},
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("STREAM"));
    painter.drawText(QRect{textLeft, textTop + lineHeight, textWidth,
                           lineHeight},
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("DATABASE"));
    painter.restore();
}

void StreamDatabaseBadgeBar::drawSettingsButton(QPainter &painter,
                                                const QRect &rect,
                                                bool hovered) const
{
    if (!rect.isValid())
    {
        return;
    }

    const auto scale = this->scale();
    const QColor dotColor = withAlpha(Qt::white, hovered ? 255 : 205);
    const int radius = std::max(2, rect.height() / 4);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (hovered)
    {
        fillRoundedRect(painter, QRectF(rect),
                        withAlpha(Qt::white, this->theme->isLightTheme() ? 45
                                                                         : 32),
                        radius);
    }

    const qreal dotRadius = std::max<qreal>(1.4, scale * 1.55);
    const qreal gap = std::max<qreal>(4.0, scale * 4.0);
    const QPointF center = QRectF(rect).center();

    painter.setPen(Qt::NoPen);
    painter.setBrush(dotColor);
    for (int i = -1; i <= 1; ++i)
    {
        painter.drawEllipse(QPointF(center.x(), center.y() + gap * i),
                            dotRadius, dotRadius);
    }
    painter.restore();
}

void StreamDatabaseBadgeBar::drawStreamDatabaseLogoMark(
    QPainter &painter, const QRect &rect) const
{
    const QColor purple("#9146FF");

    painter.save();
    QPainterPath clip;
    clip.addEllipse(QRectF(rect));
    painter.setClipPath(clip);

    if (!this->streamDatabaseLogo_.isNull())
    {
        const bool wasSmooth =
            painter.testRenderHint(QPainter::SmoothPixmapTransform);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawPixmap(rect, this->streamDatabaseLogo_);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, wasSmooth);
        painter.restore();
        return;
    }

    painter.setPen(Qt::NoPen);
    painter.setBrush(purple);
    painter.drawEllipse(rect);
    painter.setClipping(false);

    const qreal stroke = std::max<qreal>(1.5, rect.height() * 0.12);
    const qreal x = rect.left() + rect.width() * 0.24;
    const qreal y = rect.top() + rect.height() * 0.25;
    const qreal width = rect.width() * 0.52;
    const qreal ovalHeight = rect.height() * 0.22;
    const qreal bottom = rect.bottom() - rect.height() * 0.24;
    const QRectF topOval{x, y, width, ovalHeight};
    const QRectF midOval{x, y + rect.height() * 0.25, width, ovalHeight};
    const QRectF bottomOval{x, bottom - ovalHeight, width, ovalHeight};

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, stroke, Qt::SolidLine, Qt::RoundCap,
                        Qt::RoundJoin));
    painter.drawEllipse(topOval);
    painter.drawLine(QPointF(topOval.left(), topOval.center().y()),
                     QPointF(topOval.left(), bottomOval.center().y()));
    painter.drawLine(QPointF(topOval.right(), topOval.center().y()),
                     QPointF(topOval.right(), bottomOval.center().y()));
    painter.drawArc(midOval, 0, -180 * 16);
    painter.drawArc(bottomOval, 0, -180 * 16);
    painter.restore();
}

}  // namespace chatterino
