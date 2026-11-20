// SPDX-License-Identifier: MIT
#include "controllers/priority/PriorityChatModel.hpp"

#include "providers/twitch/TwitchBadge.hpp"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>
#include <limits>

namespace chatterino::priority {
namespace {
int months(const QJsonValue &value)
{
    return std::clamp(value.toInt(), 0, 1200);
}
int index(MessagePlatform platform)
{
    return std::clamp(static_cast<int>(platform), 0, 3);
}
// Platform identities, never display-name guesses. Twitch also provides its
// bot badge and FFZ bot metadata; Kick exposes kick:bot.
bool hasBotBadge(const Message &message)
{
    if (message.platform == MessagePlatform::AnyOrTwitch)
    {
        for (const auto &badge : message.twitchBadges)
            if (badge.key_ == "bot-badge")
                return true;
        return message.externalBadges.contains(
            QStringLiteral("frankerfacez:bot"));
    }
    return message.platform == MessagePlatform::Kick &&
           message.externalBadges.contains(QStringLiteral("kick:bot"));
}

bool botAccount(MessagePlatform platform, const QString &id,
                const QString &login, const Rules &rule)
{
    QString identity;
    const QStringList *known = nullptr;
    static const QStringList twitch = {"nightbot", "streamelements",
                                       "streamlabs", "botrix"};
    static const QStringList kick = {"botrix",         "kicklet",    "nightbot",
                                     "streamelements", "streamlabs", "kickbot"};
    // Provider setup guides publish these exact moderator account IDs:
    // support.streamelements.com/hc/en-us/articles/18486326016402
    // support.streamlabs.com/hc/en-us/articles/50635570817435
    // nightbot.tv: YOUTUBE_NIGHTBOT_CHANNEL in the public setup app
    static const QStringList youtube = {"UCjerlCIbLPQwSnYlClkjDXg",
                                        "UCNL8jaJ9hId96P13QmQXNtA",
                                        "UCSvjQBDgYDB5TGVmCZObcwA"};
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch:
            identity = login;
            known = &twitch;
            break;
        case MessagePlatform::Kick:
            identity = login;
            known = &kick;
            break;
        case MessagePlatform::YouTube:
            identity = id;
            known = &youtube;
            break;
        case MessagePlatform::TikTok:
            // The parser stores the unique handle here; login can fall back
            // to a non-unique display name when no handle was received.
            identity = id;
            break;
    }
    if (identity.isEmpty())
        return false;
    const auto sensitivity = platform == MessagePlatform::YouTube
                                 ? Qt::CaseSensitive
                                 : Qt::CaseInsensitive;
    if (known && known->contains(identity, sensitivity))
        return true;
    for (auto account : rule.botAccounts)
    {
        if (platform != MessagePlatform::YouTube && account.startsWith('@'))
            account.remove(0, 1);
        if (identity.compare(account, sensitivity) == 0)
            return true;
    }
    return false;
}
}  // namespace

QStringList parseBotAccounts(const QString &text)
{
    static const QRegularExpression separator(QStringLiteral("[,;\\s]+"));
    QStringList result;
    for (const auto &account :
         text.left(4096).split(separator, Qt::SkipEmptyParts))
    {
        if (account.size() <= 256 && !result.contains(account))
            result.append(account);
        if (result.size() == 256)
            break;
    }
    return result;
}

bool Rules::configured() const
{
    return (subscriptions && subscriptionMonths > 0) ||
           (follows && followMonths > 0);
}
QJsonObject Rules::toJson() const
{
    return {{"subscriptions", subscriptions},
            {"subscriptionMonths", subscriptionMonths},
            {"activeOnly", activeOnly},
            {"follows", follows},
            {"followMonths", followMonths},
            {"matchAll", matchAll},
            {"moderators", moderators},
            {"vips", vips},
            {"bots", bots},
            {"botAccounts", QJsonArray::fromStringList(botAccounts)}};
}
Rules Rules::fromJson(const QJsonObject &json)
{
    Rules result{
        json["subscriptions"].toBool(), months(json["subscriptionMonths"]),
        json["activeOnly"].toBool(),    json["follows"].toBool(),
        months(json["followMonths"]),   json["matchAll"].toBool(),
        json["moderators"].toBool(),    json["vips"].toBool()};
    result.bots = json["bots"].toBool();
    QStringList accounts;
    for (const auto &value : json["botAccounts"].toArray())
    {
        if (accounts.size() == 256)
            break;
        accounts.append(value.toString().left(256));
    }
    result.botAccounts = parseBotAccounts(accounts.join(','));
    return result;
}

std::optional<int> youtubeBadgeMonths(const QString &label)
{
    // Only explicit membership duration labels are evidence. Custom images,
    // tier names, account age and gift counts must not become tenure.
    static const QRegularExpression duration(
        QStringLiteral(
            R"(^\s*(?:member|membership)\s*\(\s*(\d+)\s*(month|year)s?\s*\)\s*$)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = duration.match(label);
    if (!match.hasMatch())
        return std::nullopt;
    bool ok = false;
    const auto count = match.captured(1).toLongLong(&ok);
    const auto factor =
        match.captured(2).compare("year", Qt::CaseInsensitive) == 0 ? 12 : 1;
    if (!ok || count < 0 || count > 1200 / factor)
        return std::nullopt;
    return int(count * factor);
}

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("Kick");
        case MessagePlatform::YouTube:
            return QStringLiteral("YouTube");
        case MessagePlatform::TikTok:
            return QStringLiteral("TikTok");
        default:
            return QStringLiteral("Twitch");
    }
}
QString personKey(MessagePlatform platform, const QString &id,
                  const QString &login)
{
    const auto identity =
        id.isEmpty()
            ? QStringLiteral("login:") + login.toCaseFolded()
            : QStringLiteral("id:") + (platform == MessagePlatform::TikTok
                                           ? id.toCaseFolded()
                                           : id);
    return QString::number(index(platform)) + ':' + identity;
}
bool isChatMessage(const Message &message)
{
    return !message.loginName.isEmpty() && !message.messageText.isEmpty() &&
           !message.flags.hasAny({MessageFlag::System, MessageFlag::Disabled,
                                  MessageFlag::InvalidReplyTarget,
                                  MessageFlag::ModerationAction,
                                  MessageFlag::SharedMessage});
}

// Twitch keeps role badges by key. Kick and YouTube messages carry
// "kick:moderator", "kick:vip" and "youtube:moderator" tags instead.
bool isModerator(const Message &message)
{
    for (const auto &badge : message.twitchBadges)
        if (badge.key_ == "moderator" || badge.key_ == "lead_moderator")
            return true;
    return message.externalBadges.contains(QStringLiteral("kick:moderator")) ||
           message.externalBadges.contains(QStringLiteral("youtube:moderator"));
}
bool isVip(const Message &message)
{
    for (const auto &badge : message.twitchBadges)
        if (badge.key_ == "vip")
            return true;
    return message.externalBadges.contains(QStringLiteral("kick:vip"));
}

bool Model::observe(const Message &message)
{
    if (!isChatMessage(message))
        return false;
    const auto key =
        personKey(message.platform, message.userID, message.loginName);
    const bool inserted = !people.contains(key);
    auto &person = people[key];
    auto oldMonths = person.months;
    auto oldActive = person.active;
    const bool oldBotBadge = person.botBadge;
    person.botBadge |= hasBotBadge(message);
    person.platform = message.platform;
    person.id = message.userID;
    person.login = message.loginName;
    person.name =
        message.displayName.isEmpty() ? message.loginName : message.displayName;
    const auto at = message.serverReceivedTime.isValid()
                        ? message.serverReceivedTime
                        : QDateTime::currentDateTimeUtc();
    if (!person.lastSeen.isValid() || at > person.lastSeen)
        person.lastSeen = at;
    auto reported = message.membershipMonths;
    auto active = message.activeMember;
    if (message.platform == MessagePlatform::AnyOrTwitch)
    {
        active = false;
        for (const auto *type : {u"subscriber", u"founder"})
        {
            auto it = message.twitchBadgeInfos.find(QString::fromUtf16(type));
            if (it == message.twitchBadgeInfos.end())
                continue;
            bool ok = false;
            const auto value = it->second.toInt(&ok);
            if (ok && value >= 0 && value <= 1200)
                reported = value;
            active = true;
        }
    }
    if (reported && *reported <= 1200)
        person.months = std::max(person.months.value_or(0), int(*reported));
    if (active && (!person.activeAt.isValid() || at >= person.activeAt))
    {
        person.active = active;
        person.activeAt = at;
    }
    const bool changed = inserted || person.months != oldMonths ||
                         person.active != oldActive ||
                         person.botBadge != oldBotBadge;
    boundPeople();
    return changed;
}

bool Model::isBot(const Message &message) const
{
    const auto person = people.constFind(
        personKey(message.platform, message.userID, message.loginName));
    return hasBotBadge(message) ||
           (person != people.cend() && isBot(*person)) ||
           botAccount(message.platform, message.userID, message.loginName,
                      rules[index(message.platform)]);
}

bool Model::isBot(const Person &person) const
{
    return person.botBadge ||
           botAccount(person.platform, person.id, person.login,
                      rules[index(person.platform)]);
}

bool Model::qualifies(const Person &person, const QDateTime &now) const
{
    const auto key = personKey(person.platform, person.id, person.login);
    if (choice(key) == Choice::Exclude)
        return false;
    if (choice(key) == Choice::Include)
        return true;
    const auto &rule = rules[index(person.platform)];
    if (isBot(person))
        return rule.bots;
    if (!rule.configured())
        return false;
    bool any = false, all = true;
    if (rule.subscriptions)
    {
        // An enabled but incomplete condition never qualifies anyone.
        const bool match =
            rule.subscriptionMonths > 0 && person.months &&
            *person.months >= rule.subscriptionMonths &&
            (!rule.activeOnly ||
             (person.active.value_or(false) && person.activeAt.isValid() &&
              person.activeAt.addSecs(1800) >= now));
        any |= match;
        all &= match;
    }
    if (rule.follows)
    {
        const bool match =
            rule.followMonths > 0 && person.followingKnown &&
            person.followedAt.isValid() && person.followedAt <= now &&
            person.followedAt <= now.addMonths(-rule.followMonths);
        any |= match;
        all &= match;
    }
    return rule.matchAll ? all : any;
}
bool Model::matches(const Message &message, const QDateTime &now) const
{
    if (!isChatMessage(message))
        return false;
    const auto key =
        personKey(message.platform, message.userID, message.loginName);
    if (choice(key) == Choice::Exclude)
        return false;
    if (choice(key) == Choice::Include)
        return true;
    const auto &rule = rules[index(message.platform)];
    if (isBot(message))
        return rule.bots;
    if ((rule.moderators && isModerator(message)) ||
        (rule.vips && isVip(message)))
        return true;
    const auto it = people.constFind(key);
    return it != people.cend() && qualifies(*it, now);
}
Choice Model::choice(const QString &key) const
{
    const auto it = manual.constFind(key);
    return it == manual.cend() ? Choice::Automatic : it->choice;
}
void Model::setChoice(const Person &person, Choice value)
{
    const auto key = personKey(person.platform, person.id, person.login);
    if (value == Choice::Automatic)
        manual.remove(key);
    else
        manual.insert(key, {value, person.name, person.login, person.platform,
                            person.id});
}
void Model::boundPeople()
{
    while (people.size() > MAX_PEOPLE)
    {
        auto oldest = people.begin();
        for (auto it = people.begin(); it != people.end(); ++it)
            if (it->lastSeen < oldest->lastSeen)
                oldest = it;
        people.erase(oldest);
    }
}
QJsonObject Model::toJson() const
{
    QJsonArray ruleJson, userJson, history;
    for (const auto &rule : rules)
        ruleJson.append(rule.toJson());
    for (auto it = manual.cbegin(); it != manual.cend(); ++it)
    {
        const auto &p = *it;
        userJson.append(QJsonObject{
            {"id", p.id},
            {"login", p.login},
            {"name", p.name},
            {"platform", index(p.platform)},
            {"choice", p.choice == Choice::Include ? "include" : "exclude"}});
    }
    for (const auto &p : people)
        if (p.months)
            history.append(
                QJsonObject{{"id", p.id},
                            {"login", p.login},
                            {"name", p.name},
                            {"platform", index(p.platform)},
                            {"months", *p.months},
                            {"seen", p.lastSeen.toString(Qt::ISODate)}});
    return {{"title", title},
            {"width", width},
            {"rules", ruleJson},
            {"people", userJson},
            {"history", history}};
}
void Model::load(const QJsonObject &json)
{
    *this = Model();
    title = json["title"].toString(title).trimmed().left(64);
    if (title.isEmpty())
        title = QStringLiteral("Priority Chat");
    width = std::clamp(json["width"].toInt(350), 240, 800);
    const auto ruleJson = json["rules"].toArray();
    for (int i = 0; i < std::min(4, int(ruleJson.size())); ++i)
        rules[i] = Rules::fromJson(ruleJson[i].toObject());
    for (const auto &value : json["people"].toArray())
    {
        if (manual.size() >= 5000)
            break;
        const auto p = value.toObject();
        const auto platform = p["platform"].toInt(-1);
        const auto id = p["id"].toString().left(256);
        const auto login = p["login"].toString().left(256);
        const auto mode = p["choice"].toString();
        if (platform < 0 || platform > 3 || (id.isEmpty() && login.isEmpty()) ||
            (mode != "include" && mode != "exclude"))
            continue;
        const auto type = static_cast<MessagePlatform>(platform);
        manual.insert(personKey(type, id, login),
                      {mode == "include" ? Choice::Include : Choice::Exclude,
                       p["name"].toString().left(256), login, type, id});
    }
    for (const auto &value : json["history"].toArray())
    {
        if (people.size() >= MAX_PEOPLE)
            break;
        const auto p = value.toObject();
        const int platform = p["platform"].toInt(-1),
                  count = p["months"].toInt(-1);
        if (platform < 0 || platform > 3 || count < 0 || count > 1200)
            continue;
        Person person;
        person.platform = static_cast<MessagePlatform>(platform);
        person.id = p["id"].toString().left(256);
        person.login = p["login"].toString().left(256);
        if (person.id.isEmpty() && person.login.isEmpty())
            continue;
        person.name = p["name"].toString().left(256);
        person.months = count;
        person.lastSeen =
            QDateTime::fromString(p["seen"].toString(), Qt::ISODate);
        // Current member/follow status is deliberately not restored from history.
        people.insert(personKey(person.platform, person.id, person.login),
                      person);
    }
}
}  // namespace chatterino::priority
