#include "providers/kick/KickMessageBuilder.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "messages/Image.hpp"
#include "messages/MessageThread.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/emoji/Emojis.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickBadges.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickEmotes.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Resources.hpp"
#include "singletons/Settings.hpp"
#include "util/BoostJsonWrap.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/Variant.hpp"

#include <QRegularExpression>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace {

using namespace chatterino;
using namespace Qt::Literals;

constexpr uint16_t KICK_LEVEL_BADGE_IMAGE_SIZE = 17;

using KickLevelBadge = std::pair<BoostJsonObject, std::string>;

QString qStringFromView(std::string_view text)
{
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

EmotePtr lookupEmote(const KickChannel &channel, uint64_t senderID,
                     QStringView word)
{
    EmoteName wordStr(word.toString());  // FIXME: don't do this...
    const auto *globalFfzEmotes = getApp()->getFfzEmotes();
    const auto *globalBttvEmotes = getApp()->getBttvEmotes();
    const auto *globalSeventvEmotes = getApp()->getSeventvEmotes();

    EmotePtr emote;

    emote = getApp()->getSeventvPersonalEmotes()->getEmoteForKickUser(senderID,
                                                                      wordStr);
    if (emote)
    {
        return emote;
    }

    emote = channel.seventvEmote(wordStr);
    if (emote)
    {
        return emote;
    }

    emote = globalFfzEmotes->emote(wordStr).value_or(std::move(emote));
    if (emote)
    {
        return emote;
    }

    emote = globalBttvEmotes->emote(wordStr).value_or(std::move(emote));
    if (emote)
    {
        return emote;
    }

    emote =
        globalSeventvEmotes->globalEmote(wordStr).value_or(std::move(emote));
    if (emote)
    {
        return emote;
    }

    return emote;
}

void appendWord(KickMessageBuilder &builder, QStringView word)
{
    auto emote = lookupEmote(*builder.channel(), builder.senderID, word);
    if (emote)
    {
        builder.appendEmote(emote);
        return;
    }

    builder.addWordFromUserMessage(word, builder.channel());
}

bool isEmoteID(QStringView v)
{
    for (QChar c : v)
    {
        if (c < '0' || c > '9')
        {
            return false;
        }
    }
    return !v.empty();
}

void appendNonKickEmoteText(KickMessageBuilder &builder, QStringView text)
{
    for (const auto &variant : getApp()->getEmotes()->getEmojis()->parse(text))
    {
        std::visit(variant::Overloaded{
                       [&](const EmotePtr &emote) {
                           builder.emplace<EmoteElement>(
                               emote, MessageElementFlag::EmojiAll);
                       },
                       [&](QStringView text) {
                           appendWord(builder, text);
                       },
                   },
                   variant);
    }
}

/// Try to find the next emote in `text`
///
/// Kick emotes are present as `[emote:{id}:{name}]` where `{id}` is numeric.
/// They can be right next to each other or to text. For example, we could find
/// the following message: `foo [emote:1234:name]foo[emote:1234:name]`.
bool tryAppendKickEmoteText(KickMessageBuilder &builder, QString &messageText,
                            QStringView &text)
{
    static constexpr QStringView emotePrefix = u"[emote:";

    auto nextEmote = text.indexOf(emotePrefix);
    if (nextEmote < 0)
    {
        return false;
    }
    auto secondColon = text.indexOf(u':', nextEmote + emotePrefix.size());
    if (secondColon < 0)
    {
        return false;
    }
    auto endBrace = text.indexOf(u']', secondColon + 1);
    if (endBrace < 0)
    {
        return false;
    }

    auto emoteID = text.sliced(nextEmote + emotePrefix.size(),
                               secondColon - nextEmote - emotePrefix.size());
    if (!isEmoteID(emoteID))
    {
        return false;
    }

    if (nextEmote > 0)
    {
        auto prefix = text.sliced(0, nextEmote);
        messageText.append(prefix);
        messageText.append(' ');
        appendNonKickEmoteText(builder, prefix);
    }

    auto emoteName = text.sliced(secondColon + 1, endBrace - secondColon - 1);
    builder.emplace<EmoteElement>(KickEmotes::emoteForID(emoteID, emoteName),
                                  MessageElementFlag::Emote,
                                  builder.textColor());
    messageText.append(emoteName);

    text = text.sliced(endBrace + 1);
    if (!text.empty())
    {
        messageText.append(' ');
    }

    return true;
}

void parseContent(KickMessageBuilder &builder, QString &messageText,
                  QStringView content)
{
    for (auto word : content.tokenize(u' ', Qt::SkipEmptyParts))
    {
        if (!messageText.isEmpty())
        {
            messageText.append(' ');
        }

        while (!word.empty())
        {
            if (!tryAppendKickEmoteText(builder, messageText, word))
            {
                messageText.append(word);
                appendNonKickEmoteText(builder, word);
                break;
            }
        }
    }
}

QString firstString(BoostJsonObject obj,
                    std::initializer_list<std::string_view> keys)
{
    for (auto key : keys)
    {
        auto value = obj[key].toQString();
        if (!value.isEmpty())
        {
            return value;
        }
    }
    return {};
}

std::string firstStdString(BoostJsonObject obj,
                           std::initializer_list<std::string_view> keys)
{
    for (auto key : keys)
    {
        auto value = obj[key].toStdString();
        if (!value.empty())
        {
            return value;
        }
    }
    return {};
}

std::string badgeTypeFromObject(BoostJsonObject obj)
{
    return firstStdString(obj, {"type", "badge_type", "badgeType", "kind",
                                "id", "slug", "key", "name", "title", "alt",
                                "tooltip", "display_name", "displayName",
                                "badge_name", "badgeName"});
}

QString imageUrlFromObject(BoostJsonObject obj);

QString imageUrlFromValue(BoostJsonValue value)
{
    if (value.isString())
    {
        return value.toQString();
    }
    if (value.isObject())
    {
        return imageUrlFromObject(value.toObject());
    }
    if (value.isArray())
    {
        for (auto entry : value.toArray())
        {
            auto url = imageUrlFromValue(entry);
            if (!url.isEmpty())
            {
                return url;
            }
        }
    }
    return {};
}

QString imageUrlFromObject(BoostJsonObject obj)
{
    auto direct = firstString(
        obj, {"url", "src", "image", "image_url", "imageUrl",
              "badge_image_url", "badgeImageUrl", "icon", "icon_url",
              "iconUrl", "small_image_url", "smallImageUrl", "asset",
              "asset_url", "assetUrl"});
    if (!direct.isEmpty())
    {
        return direct;
    }

    for (auto key : {"badge_image", "badgeImage", "image", "images", "icon",
                     "icons", "badge", "level", "level_badge", "levelBadge",
                     "thumbnail", "thumbnails"})
    {
        auto url = imageUrlFromValue(obj[key]);
        if (!url.isEmpty())
        {
            return url;
        }
    }

    return {};
}

bool isKickLevelBadge(std::string_view type)
{
    auto normalized = qStringFromView(type).toLower();
    return normalized == u"level"_s || normalized == u"kick_level"_s ||
           normalized == u"kick-level"_s ||
           normalized == u"level_badge"_s ||
           normalized == u"level-badge"_s ||
           normalized.contains(u"level"_s);
}

QString levelTextFromImageUrl(const QString &imageUrl)
{
    static const QRegularExpression badgePathPattern(
        R"(/chat/badges/(\d+)_)",
        QRegularExpression::CaseInsensitiveOption);
    auto match = badgePathPattern.match(imageUrl);
    if (!match.hasMatch())
    {
        return {};
    }
    return match.captured(1);
}

QString levelTextFromValue(BoostJsonValue value);

QString levelTextFromString(QString text)
{
    text = text.trimmed();
    if (text.isEmpty())
    {
        return {};
    }

    static const QRegularExpression numberPattern(R"((\d+))");
    auto match = numberPattern.match(text);
    if (!match.hasMatch())
    {
        return {};
    }

    bool ok = false;
    auto number = match.captured(1).toULongLong(&ok);
    if (!ok || number == 0)
    {
        return {};
    }

    return QString::number(number);
}

QString levelTextFromObject(BoostJsonObject obj)
{
    for (auto key : {"level", "chat_level", "chatLevel", "channel_level",
                     "channelLevel", "user_level", "userLevel",
                     "badge_level", "badgeLevel", "current_level",
                     "currentLevel", "current", "value", "count", "tier",
                     "rank", "amount", "number", "metadata"})
    {
        auto levelText = levelTextFromValue(obj[key]);
        if (!levelText.isEmpty())
        {
            return levelText;
        }
    }

    for (auto key : {"text", "label", "name", "title", "display_name",
                     "displayName", "badge_name", "badgeName", "alt",
                     "tooltip"})
    {
        auto levelText = levelTextFromValue(obj[key]);
        if (!levelText.isEmpty())
        {
            return levelText;
        }
    }

    return {};
}

QString levelTextFromValue(BoostJsonValue value)
{
    if (value.isInt64())
    {
        auto level = value.toUint64();
        return level > 0 ? QString::number(level) : QString{};
    }
    if (value.isString())
    {
        return levelTextFromString(value.toQString());
    }
    if (value.isObject())
    {
        return levelTextFromObject(value.toObject());
    }
    if (value.isArray())
    {
        for (auto entry : value.toArray())
        {
            auto levelText = levelTextFromValue(entry);
            if (!levelText.isEmpty())
            {
                return levelText;
            }
        }
    }
    return {};
}

bool isKickLevelBadgeObject(BoostJsonObject badgeObj, std::string_view type)
{
    if (isKickLevelBadge(type))
    {
        return true;
    }

    auto imageUrl = imageUrlFromObject(badgeObj);
    if (!levelTextFromImageUrl(imageUrl).isEmpty())
    {
        return true;
    }

    auto descriptor = firstStdString(
        badgeObj, {"text", "label", "description", "alt", "tooltip",
                   "display_name", "displayName", "badge_name", "badgeName"});
    return isKickLevelBadge(descriptor);
}

bool isVisibleKickBadgeObject(BoostJsonObject badgeObj)
{
    return badgeObj["active"].toBool(true) &&
           badgeObj["selected"].toBool(true);
}

bool appendKickLevelBadge(KickMessageBuilder &builder, BoostJsonObject badgeObj,
                          std::string_view type)
{
    if (!getSettings()->showKickLevelBadges ||
        !isVisibleKickBadgeObject(badgeObj) ||
        !isKickLevelBadgeObject(badgeObj, type))
    {
        return false;
    }

    auto imageUrl = imageUrlFromObject(badgeObj);
    if (imageUrl.isEmpty())
    {
        return false;
    }

    auto levelText = levelTextFromObject(badgeObj);
    if (levelText.isEmpty())
    {
        levelText = levelTextFromImageUrl(imageUrl);
    }

    auto name = firstString(badgeObj, {"name", "title", "label", "text",
                                       "alt", "badge_name", "badgeName"});
    if (name.isEmpty() || name.compare(u"level"_s, Qt::CaseInsensitive) == 0)
    {
        name = levelText.isEmpty() ? u"Kick Level"_s
                                   : u"Level "_s % levelText;
    }

    auto tooltip = firstString(badgeObj, {"tooltip", "description", "alt",
                                          "display_name", "displayName"});
    if (tooltip.isEmpty() ||
        tooltip.compare(u"level"_s, Qt::CaseInsensitive) == 0)
    {
        tooltip = name;
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {name},
        .images = ImageSet(Image::fromAutoscaledUrl(
            {imageUrl}, KICK_LEVEL_BADGE_IMAGE_SIZE)),
        .tooltip = Tooltip{tooltip},
        .homePage = {u"https://kick.com/" % builder.channel()->slug()},
    });
    builder.emplace<BadgeElement>(emote, MessageElementFlag::BadgeKickLevel);
    return true;
}

QString displayedUsername(const Message &message)
{
    QString usernameText;
    switch (getSettings()->usernameDisplayMode.getValue())
    {
        case UsernameDisplayMode::Username:
            usernameText = message.loginName;
            break;

        case UsernameDisplayMode::LocalizedName:
        case UsernameDisplayMode::UsernameAndLocalizedName:
        default:
            usernameText = message.displayName;
            break;
    }

    if (auto nicknameText = getSettings()->matchNickname(usernameText))
    {
        usernameText = *nicknameText;
    }

    return usernameText;
}

void checkThreadSubscription(const QString &senderLogin,
                             const QString &originalSender,
                             const std::shared_ptr<MessageThread> &thread)
{
    if (thread->subscribed() || thread->unsubscribed())
    {
        return;
    }

    if (getSettings()->autoSubToParticipatedThreads)
    {
        const auto &current = getApp()->getAccounts()->kick.current();
        if (current->isAnonymous())
        {
            return;
        }
        auto user = current->username();
        if (senderLogin.compare(user, Qt::CaseInsensitive) == 0 ||
            originalSender.compare(user, Qt::CaseInsensitive) == 0)
        {
            thread->markSubscribed();
        }
    }
}

// FIXME: this is 🍝
void appendReply(KickMessageBuilder &builder, BoostJsonObject metadata)
{
    auto originalMessage = metadata["original_message"].toObject();
    auto originalSender = metadata["original_sender"]["username"].toQString();
    auto originalMessageID = originalMessage["id"].toQString();
    auto [thread, replyParent] =
        builder.channel()->getOrCreateThread(originalMessageID);
    if (thread)
    {
        builder->replyThread = thread;
        builder->replyParent = replyParent;
        thread->addToThread(std::weak_ptr{builder.weakOf()});
        checkThreadSubscription(builder->loginName, originalSender, thread);
        if (thread->subscribed())
        {
            builder->flags.set(MessageFlag::SubscribedThread);
        }
    }

    builder->flags.set(MessageFlag::ReplyMessage);

    QString usernameText = originalSender;
    if (replyParent)
    {
        usernameText = displayedUsername(*replyParent);
    }

    builder.emplace<ReplyCurveElement>();

    auto *replyingTo = builder.emplace<TextElement>(
        "Replying to", MessageElementFlag::RepliedMessage, MessageColor::System,
        FontStyle::ChatMediumSmall);
    if (thread)
    {
        replyingTo->setLink({Link::ViewThread, thread->rootId()});
    }

    builder
        .emplace<TextElement>(
            '@' % usernameText % ':', MessageElementFlag::RepliedMessage,
            replyParent ? replyParent->usernameColor : QColor{},
            FontStyle::ChatMediumSmall)
        ->setLink({Link::UserInfo,
                   replyParent ? replyParent->displayName : usernameText});

    MessageColor color = MessageColor::Text;
    if (replyParent && replyParent->flags.has(MessageFlag::Action))
    {
        color = replyParent->usernameColor;
    }

    QString messageText = [&] {
        if (replyParent)
        {
            return replyParent->messageText;
        }
        return originalMessage["content"].toQString();
    }();

    auto *textEl = builder.emplace<SingleLineTextElement>(
        messageText,
        MessageElementFlags(
            {MessageElementFlag::RepliedMessage, MessageElementFlag::Text}),
        color, FontStyle::ChatMediumSmall);
    if (thread)
    {
        textEl->setLink({Link::ViewThread, thread->rootId()});
    }
}

void appendReplyButtons(KickMessageBuilder &builder)
{
    if (builder->replyThread)
    {
        auto &img = getResources().buttons.replyThreadDark;
        builder
            .emplace<CircularImageElement>(Image::fromResourcePixmap(img, 0.15),
                                           2, Qt::gray,
                                           MessageElementFlag::ReplyButton)
            ->setLink({Link::ViewThread, builder->replyThread->rootId()});
    }
    else
    {
        auto &img = getResources().buttons.replyDark;
        builder
            .emplace<CircularImageElement>(Image::fromResourcePixmap(img, 0.15),
                                           2, Qt::gray,
                                           MessageElementFlag::ReplyButton)
            ->setLink({Link::ReplyToMessage, builder->id});
    }
}

std::optional<KickLevelBadge> appendKickBadges(KickMessageBuilder &builder,
                                               BoostJsonArray badges,
                                               bool updateSelfState = true)
{
    bool hasMod = false;
    bool hasVip = false;
    std::optional<KickLevelBadge> levelBadge;
    for (auto badgeObj : badges)
    {
        auto obj = badgeObj.toObject();
        if (!isVisibleKickBadgeObject(obj))
        {
            continue;
        }

        auto ty = badgeTypeFromObject(obj);
        if (ty == "bot" &&
            !builder->externalBadges.contains(QStringLiteral("kick:bot")))
        {
            builder->externalBadges.append(QStringLiteral("kick:bot"));
        }
        if (isKickLevelBadgeObject(obj, ty))
        {
            if (!levelBadge)
            {
                levelBadge.emplace(std::move(obj), std::move(ty));
            }
            continue;
        }

        auto [emote, flag] = [&] {
            if (ty == "subscriber")
            {
                auto subscriberBadge =
                    builder.channel()->subscriberBadgeForMonths(
                        obj["count"].toUint64());
                if (subscriberBadge)
                {
                    return std::pair{subscriberBadge,
                                     MessageElementFlag::BadgeSubscription};
                }
            }

            return KickBadges::lookup(ty);
        }();
        if (!emote)
        {
            continue;
        }

        if (ty == "moderator")
        {
            hasMod = true;
        }
        else if (ty == "vip")
        {
            hasVip = true;
        }

        builder.emplace<BadgeElement>(emote, flag);
    }

    bool isSelf = builder->loginName ==
                  getApp()->getAccounts()->kick.current()->username();
    if (updateSelfState && isSelf)
    {
        builder.channel()->setMod(hasMod);
        builder.channel()->setVip(hasVip);
    }
    return levelBadge;
}

bool appendDeferredKickLevelBadge(
    KickMessageBuilder &builder,
    const std::optional<KickLevelBadge> &levelBadge)
{
    if (!levelBadge)
    {
        return false;
    }
    return appendKickLevelBadge(builder, levelBadge->first, levelBadge->second);
}

void appendSeventvBadge(KickMessageBuilder &builder)
{
    auto badge = getApp()->getSeventvBadges()->getKickBadge(builder.senderID);
    if (badge)
    {
        builder.emplace<BadgeElement>(*badge, MessageElementFlag::BadgeSevenTV);
    }
}

bool cachedIdentityHasBadgeFlag(const KickChannel::CachedOwnIdentity *identity,
                                MessageElementFlag flag)
{
    if (!identity)
    {
        return false;
    }

    for (const auto &badge : identity->badges)
    {
        if (badge && badge->getFlags().has(flag))
        {
            return true;
        }
    }
    return false;
}

HighlightAlert processHighlights(KickMessageBuilder &builder,
                                 const MessageParseArgs &args)
{
    if (getSettings()->isBlacklistedUser(builder->loginName))
    {
        // Do nothing. We ignore highlights from this user.
        return {};
    }

    auto [highlighted, highlightResult] = getApp()->getHighlights()->check(
        args, {}, builder->loginName, builder->messageText, builder->flags,
        builder->platform);

    if (!highlighted)
    {
        return {};
    }

    // This message triggered one or more highlights, act upon the highlight result

    builder->flags.set(MessageFlag::Highlighted);
    builder->highlightColor = highlightResult.color;

    if (highlightResult.showInMentions)
    {
        builder->flags.set(MessageFlag::ShowInMentions);
    }

    return {
        .customSound = highlightResult.customSoundUrl.value_or(QUrl{}),
        .playSound = highlightResult.playSound,
        .windowAlert = highlightResult.alert,
    };
}

QStringView plural(QStringView pluralText, uint64_t n)
{
    assert(pluralText.endsWith('s'));
    if (n == 1)
    {
        return pluralText.sliced(0, pluralText.length() - 1);
    }
    return pluralText;
}

}  // namespace

namespace chatterino {

KickMessageBuilder::KickMessageBuilder(SystemMessageTag /* tag */,
                                       KickChannel *channel,
                                       const QDateTime &time)
    : channel_(channel)
{
    this->emplace<TimestampElement>(time.time());
    this->message().flags.set(MessageFlag::System);
    this->message().platform = MessagePlatform::Kick;
    this->message().serverReceivedTime = time;
}

KickMessageBuilder::KickMessageBuilder(KickChannel *channel,
                                       const QDateTime &time)
    : channel_(channel)
{
    this->emplace<TimestampElement>(time.time());
    this->message().platform = MessagePlatform::Kick;
    this->message().serverReceivedTime = time;
}

KickMessageBuilder::KickMessageBuilder(KickChannel *channel)
    : channel_(channel)
{
    this->message().platform = MessagePlatform::Kick;
}

std::pair<MessagePtrMut, HighlightAlert> KickMessageBuilder::makeChatMessage(
    KickChannel *kickChannel, BoostJsonObject data)
{
    auto id = data["id"].toQString();
    auto content = data["content"].toQString().simplified();
    auto createdAt = data["created_at"].toQString();

    auto sender = data["sender"].toObject();
    auto identity = sender["identity"].toObject();

    KickMessageBuilder builder(kickChannel);
    builder->channelName = kickChannel->getName();
    builder->kickMessageJson = data.toJson();
    builder->id = id;
    builder->serverReceivedTime =
        QDateTime::fromString(createdAt, Qt::DateFormat::ISODate).toLocalTime();
    builder->parseTime = QTime::currentTime();
    builder->displayName = sender["username"].toQString();
    builder->loginName = builder->displayName.toLower();
    builder.senderID = sender["id"].toUint64();
    builder->userID = QString::number(builder.senderID);

    if (data["type"].toStringView() == "reply")
    {
        appendReply(builder, data["metadata"].toObject());
    }

    builder.appendChannelName();

    builder.emplace<TimestampElement>(builder->serverReceivedTime.time());
    builder.emplace<TwitchModerationElement>();

    const auto levelBadgeV2 =
        appendKickBadges(builder, identity["badges_v2"].toArray(), false);
    const auto levelBadge =
        appendKickBadges(builder, identity["badges"].toArray(), true);
    auto hasLevelBadge = appendDeferredKickLevelBadge(builder, levelBadgeV2);
    if (!hasLevelBadge)
    {
        hasLevelBadge = appendDeferredKickLevelBadge(builder, levelBadge);
    }
    appendSeventvBadge(builder);

    builder.appendUsername(identity);
    kickChannel->setUserColor(builder->displayName, builder->usernameColor);
    kickChannel->addRecentChatter(builder->displayName);

    QString messageText;
    parseContent(builder, messageText, content);
    appendReplyButtons(builder);

    builder->searchText =
        builder->loginName % ' ' % builder->displayName % u": " % messageText;
    builder->messageText = messageText;

    MessageParseArgs args;
    args.isStaffOrBroadcaster = kickChannel->isBroadcaster();

    auto highlightAlert = processHighlights(builder, args);

    return {builder.release(), highlightAlert};
}

MessagePtrMut KickMessageBuilder::makeSentMessage(KickChannel *kickChannel,
                                                  const QString &content,
                                                  const QString &localID)
{
    auto account = getApp()->getAccounts()->kick.current();
    if (!account || account->isAnonymous())
    {
        return nullptr;
    }

    const auto now = QDateTime::currentDateTime();
    const auto *identity = kickChannel->ownIdentity();
    const auto displayName =
        identity && !identity->displayName.isEmpty() ? identity->displayName
                                                     : account->username();
    KickMessageBuilder builder(kickChannel);
    builder->channelName = kickChannel->getName();
    builder->id = localID;
    builder->serverReceivedTime = now;
    builder->parseTime = QTime::currentTime();
    builder->displayName = displayName;
    builder->loginName =
        identity && !identity->loginName.isEmpty() ? identity->loginName
                                                   : displayName.toLower();
    builder.senderID = account->userID();
    builder->userID = QString::number(builder.senderID);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);

    builder.appendChannelName();
    builder.emplace<TimestampElement>(now.time());
    const auto hasCachedSevenTVBadge =
        cachedIdentityHasBadgeFlag(identity, MessageElementFlag::BadgeSevenTV);
    if (identity)
    {
        for (const auto &badge : identity->badges)
        {
            if (badge)
            {
                builder->elements.emplace_back(badge->clone());
            }
        }
    }
    if (!hasCachedSevenTVBadge)
    {
        appendSeventvBadge(builder);
    }

    if (identity)
    {
        builder.appendUsernameAsSender(displayName, identity->usernameColor);
    }
    else
    {
        builder.appendUsernameAsSender(displayName);
    }

    QString messageText;
    parseContent(builder, messageText, content.simplified());

    builder->searchText =
        builder->loginName % ' ' % builder->displayName % u": " % messageText;
    builder->messageText = messageText;

    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeTimeoutMessage(KickChannel *channel,
                                                     const QDateTime &now,
                                                     BoostJsonObject data)
{
    bool isPermanent = data["permanent"].toBool();
    auto duration = [&] {
        if (isPermanent)
        {
            return std::chrono::seconds{};
        }
        auto dur = data["duration"].toInt64();
        if (dur != 0)
        {
            return std::chrono::seconds{dur * 60};
        }
        auto expiresAt =
            QDateTime::fromString(data["expires_at"].toQString(), Qt::ISODate);
        if (!expiresAt.isValid())
        {
            return std::chrono::seconds{0};
        }
        return std::chrono::round<std::chrono::seconds>(expiresAt - now);
    }();

    KickMessageBuilder builder(systemMessage, channel, now);
    const auto timedOutUsername = data["user"]["username"].toQString();
    const auto moderatorUsername = data["banned_by"]["username"].toQString();

    bool hasBannedBy = !moderatorUsername.isEmpty();

    builder->flags.set(MessageFlag::PubSub, MessageFlag::Timeout,
                       MessageFlag::ModerationAction);

    QString text;

    if (hasBannedBy)
    {
        builder->loginName = moderatorUsername;
        builder.emplaceSystemTextAndUpdate(moderatorUsername, text)
            ->setLink({Link::UserInfo, moderatorUsername});
        if (isPermanent)
        {
            builder.emplaceSystemTextAndUpdate("permanently banned", text);
        }
        else
        {
            builder.emplaceSystemTextAndUpdate("timed out", text);
        }
        builder.emplaceSystemTextAndUpdate(timedOutUsername, text)
            ->setLink({Link::UserInfo, timedOutUsername});
    }
    else
    {
        builder.emplaceSystemTextAndUpdate(timedOutUsername, text)
            ->setLink({Link::UserInfo, timedOutUsername});
        if (isPermanent)
        {
            builder.emplaceSystemTextAndUpdate("has been permanently banned",
                                               text);
        }
        else
        {
            builder.emplaceSystemTextAndUpdate("has been timed out", text);
        }
    }

    if (!isPermanent)
    {
        builder.emplaceSystemTextAndUpdate("for", text);
        builder.emplaceSystemTextAndUpdate(
            formatTime(static_cast<int>(duration.count())), text);
    }

    builder->elements.back()->setTrailingSpace(false);
    text.removeLast();  // trailing space

    builder.emplaceSystemTextAndUpdate(".", text);
    builder->messageText = text;
    builder->searchText = text;
    builder->timeoutUser = timedOutUsername.toLower();

    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeUntimeoutMessage(KickChannel *channel,
                                                       BoostJsonObject data)
{
    bool isPermanent = data["permanent"].toBool();

    KickMessageBuilder builder(systemMessage, channel,
                               QDateTime::currentDateTime());
    const auto timedOutUsername = data["user"]["username"].toQString();
    const auto moderatorUsername = data["unbanned_by"]["username"].toQString();

    bool hasBannedBy = !moderatorUsername.isEmpty();

    builder->flags.set(MessageFlag::PubSub, MessageFlag::Untimeout,
                       MessageFlag::ModerationAction);

    QString text;

    if (hasBannedBy)
    {
        builder->loginName = moderatorUsername;
        builder.appendMentionedUser(moderatorUsername, text);
        if (isPermanent)
        {
            builder.emplaceSystemTextAndUpdate("unbanned", text);
        }
        else
        {
            builder.emplaceSystemTextAndUpdate("untimed out", text);
        }
        builder.appendMentionedUser(timedOutUsername, text);
    }
    else
    {
        builder.appendMentionedUser(timedOutUsername, text);
        if (isPermanent)
        {
            builder.emplaceSystemTextAndUpdate("was unbanned", text);
        }
        else
        {
            builder.emplaceSystemTextAndUpdate("was untimed out", text);
        }
    }

    builder->elements.back()->setTrailingSpace(false);
    text.removeLast();  // trailing space

    builder.emplaceSystemTextAndUpdate(".", text);
    builder->messageText = text;
    builder->searchText = text;
    builder->timeoutUser = timedOutUsername;

    return builder.release();
}

MessagePtrMut KickMessageBuilder::makePinnedMessage(KickChannel *channel,
                                                    BoostJsonObject data)
{
    const auto creator = data["pinnedBy"]["username"].toQString();
    auto jsonMessage = data["message"];
    auto messageID = jsonMessage["id"].toQString();

    auto existing = channel->findMessageByID(messageID);
    auto messageText = [&] {
        if (existing)
        {
            return existing->messageText;
        }
        return jsonMessage["text"].toQString().simplified();
    }();
    auto limit = getSettings()->deletedMessageLengthLimit.getValue();
    if (limit > 0 && messageText.length() > limit)
    {
        messageText = QStringView(messageText).left(limit) % u'…';
    }

    KickMessageBuilder builder(systemMessage, channel,
                               QDateTime::currentDateTime());
    QString text;
    builder.appendMentionedUser(creator, text);
    builder.appendOrEmplaceSystemTextAndUpdate(u"pinned"_s, text);
    builder
        .emplace<TextElement>(messageText, MessageElementFlag::Text,
                              MessageColor::Text)
        ->setLink({Link::JumpToMessage, messageID});

    builder->searchText = text;
    builder->messageText = text;
    builder->loginName = creator.toLower();
    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeHostMessage(KickChannel *channel,
                                                  BoostJsonObject data)
{
    const auto user = data["host_username"].toQString();
    const auto viewers = data["number_viewers"].toUint64();
    KickMessageBuilder builder(systemMessage, channel,
                               QDateTime::currentDateTime());
    QString text;
    builder.appendMentionedUser(user, text);
    builder.appendOrEmplaceSystemTextAndUpdate(
        u"hosted the stream with " % QString::number(viewers) % ' ' %
            plural(u"viewers", viewers) % '.',
        text);

    builder->searchText = text;
    builder->messageText = text;
    builder->loginName = user.toLower();
    return builder.release();
}

std::tuple<MessagePtrMut, MessagePtrMut, HighlightAlert>
    KickMessageBuilder::makeSubscriptionMessage(KickChannel *channel,
                                                BoostJsonObject data)
{
    MessagePtrMut customMessage;
    HighlightAlert alert;
    auto now = QDateTime::currentDateTime();

    auto months = data["months"].toUint64();
    auto username = data["username"].toQString();
    auto customMessageText = data["custom_message"].toQString().simplified();
    if (!customMessageText.isEmpty())
    {
        KickMessageBuilder builder(channel);
        builder->serverReceivedTime = now;
        builder->flags.set(MessageFlag::Subscription);
        builder->loginName = username;

        builder.emplace<TimestampElement>(now.time());
        builder.appendUsernameAsSender(username);
        QString text;
        parseContent(builder, text, customMessageText);

        builder->messageText = text;
        builder->searchText = username % ": " % text;

        MessageParseArgs args;
        args.isSubscriptionMessage = true;
        args.isStaffOrBroadcaster = channel->isBroadcaster();
        alert = processHighlights(builder, args);
        customMessage = builder.release();
    }

    KickMessageBuilder builder(systemMessage, channel, now);
    builder->flags.set(MessageFlag::Subscription);

    QString text;
    builder.appendMentionedUser(username, text);
    builder.appendOrEmplaceSystemTextAndUpdate(
        u"subscribed for " % QString::number(months) %
            plural(u" months", months) % '.',
        text);
    builder->searchText = text;
    builder->messageText = text;
    builder->loginName = username;

    return {customMessage, builder.release(), alert};
}

MessagePtrMut KickMessageBuilder::makeGiftedSubscriptionMessage(
    KickChannel *channel, BoostJsonObject data)
{
    const auto gifter = data["gifter_username"].toQString();
    const auto gifted = data["gifted_usernames"].toArray();
    auto total = data["gifter_total"].toUint64();

    if (gifted.empty())
    {
        return nullptr;
    }

    KickMessageBuilder builder(systemMessage, channel,
                               QDateTime::currentDateTime());
    builder->flags.set(MessageFlag::Subscription);
    builder->giftedSubscriptionRecipientCount =
        gifted.size() > std::numeric_limits<uint32_t>::max()
            ? std::numeric_limits<uint32_t>::max()
            : static_cast<uint32_t>(gifted.size());
    if (gifted.size() > 1)
    {
        builder->flags.set(MessageFlag::Collapsed);
    }

    QString text;
    builder.appendMentionedUser(gifter, text);
    builder.emplaceSystemTextAndUpdate(
        u"gifted " % QString::number(gifted.size()) %
            plural(u" subscriptions", gifted.size()) % u" to",
        text);

    for (size_t i = 0; i < gifted.size(); i++)
    {
        if (i != 0)
        {
            if (i == gifted.size() - 1)
            {
                QString toAdd = u", and"_s;
                if (gifted.size() == 2)
                {
                    toAdd = u"and"_s;
                    builder->elements.back()->setTrailingSpace(true);
                    text.append(' ');
                }
                builder.appendOrEmplaceSystemTextAndUpdate(toAdd, text);
            }
            else
            {
                builder.appendOrEmplaceSystemTextAndUpdate(u","_s, text);
            }
        }
        builder.appendMentionedUser(gifted[i].toQString(), text, false);
    }
    builder.appendOrEmplaceSystemTextAndUpdate(
        u". They gifted "_s % QString::number(total) % plural(u" subs", total) %
            u" in total.",
        text);

    builder->messageText = text;
    builder->searchText = text;
    builder->loginName = gifter;

    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeRewardRedeemedMessage(
    KickChannel *channel, BoostJsonObject data)
{
    const auto reward = data["reward_title"].toQString();
    const auto username = data["username"].toQString();
    const auto userInput = data["user_input"].toQString().simplified();

    KickMessageBuilder builder(channel, QDateTime::currentDateTime());
    builder->flags.set(MessageFlag::RedeemedChannelPointReward);
    builder->flags.set(MessageFlag::CheerMessage);

    QString text;
    if (userInput.isEmpty())
    {
        builder.appendMentionedUser(username, text);
        builder.appendOrEmplaceText(u"redeemed"_s, MessageColor::Text);
        text += u" redeemed ";
    }
    else
    {
        builder.appendOrEmplaceText(u"Redeemed"_s, MessageColor::Text);
        text += u"Redeemed ";
    }
    builder.emplace<TextElement>(reward, MessageElementFlag::Text,
                                 MessageColor::Text, FontStyle::ChatMediumBold);
    text += reward;

    if (userInput.isEmpty())
    {
        builder->messageText = text;
        builder->searchText = text;
    }
    else
    {
        builder.emplace<LinebreakElement>(
            MessageElementFlag::ChannelPointReward);
        builder.appendUsernameAsSender(username);
        QString redeemedText;
        parseContent(builder, redeemedText, userInput);
        builder->messageText = text;
        builder->searchText = text % ' ' % username % u": " % redeemedText;
    }
    builder->loginName = username;
    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeKicksGiftedMessage(KickChannel *channel,
                                                         BoostJsonObject data)
{
    const auto username = data["sender"]["username"].toQString();
    const auto gift = data["gift"].toObject();
    const auto giftName = gift["name"].toQString();
    const auto giftAmount = gift["amount"].toUint64();
    const auto userInput = data["message"].toQString().simplified();

    KickMessageBuilder builder(channel, QDateTime::currentDateTime());
    builder->flags.set(MessageFlag::RedeemedChannelPointReward);
    builder->flags.set(MessageFlag::CheerMessage);
    builder->kickGiftKicks = giftAmount;

    QString text;
    if (userInput.isEmpty())
    {
        builder.appendMentionedUser(username, text);
        builder.appendOrEmplaceText(u"gifted"_s, MessageColor::Text);
        text += u" gifted ";
    }
    else
    {
        builder.appendOrEmplaceText(u"Gifted"_s, MessageColor::Text);
        text += u"Gifted ";
    }
    builder.emplace<TextElement>(giftName, MessageElementFlag::Text,
                                 MessageColor::Text, FontStyle::ChatMediumBold);
    QString kickInfo =
        '(' % localizeNumbers(giftAmount) % plural(u" Kicks", giftAmount) % ')';
    builder.appendOrEmplaceText(kickInfo, MessageColor::Text);
    text += kickInfo;

    if (userInput.isEmpty())
    {
        builder->messageText = text;
        builder->searchText = text;
    }
    else
    {
        builder.emplace<LinebreakElement>(
            MessageElementFlag::ChannelPointReward);
        builder.appendUsernameAsSender(username);
        QString redeemedText;
        parseContent(builder, redeemedText, userInput);
        builder->messageText = text;
        builder->searchText = text % ' ' % username % u": " % redeemedText;
    }
    builder->loginName = username;
    return builder.release();
}

MessagePtrMut KickMessageBuilder::makeRoomModeMessage(
    KickChannel *channel, const QString &mode, bool enabled,
    std::optional<std::chrono::seconds> duration)
{
    KickMessageBuilder builder(systemMessage, channel,
                               QDateTime::currentDateTime());
    builder->flags.set(MessageFlag::ModerationAction);
    QString text;

    QString enabledText = enabled ? u"enabled"_s : u"disabled"_s;

    builder.emplaceSystemTextAndUpdate(mode, text);
    builder.emplaceSystemTextAndUpdate(u"mode"_s, text);

    if (duration && duration->count() > 0)
    {
        builder.emplaceSystemTextAndUpdate(u'(' % formatTime(*duration) % u')',
                                           text);
    }

    builder.emplaceSystemTextAndUpdate(u"has been"_s, text);
    builder.emplaceSystemTextAndUpdate(enabledText, text);

    text.removeLast();
    builder->elements.back()->setTrailingSpace(false);
    builder.emplaceSystemTextAndUpdate(u"."_s, text);

    builder->messageText = text;
    builder->searchText = text;
    return builder.release();
}

void KickMessageBuilder::appendChannelName()
{
    QString channelName('#' + this->channel()->getName());
    Link link(Link::JumpToChannel, u":kick:" % this->channel()->getName());

    this->emplace<TextElement>(channelName, MessageElementFlag::ChannelName,
                               MessageColor::System)
        ->setLink(link);
}

void KickMessageBuilder::appendUsername(BoostJsonObject identityObj)
{
    QString usernameText = displayedUsername(this->message()) + ':';

    auto userColor = QColor::fromString(identityObj["color"].toStringView());
    this->message().usernameColor = userColor;
    this->emplace<TextElement>(usernameText,
                               MessageElementFlags{
                                   MessageElementFlag::Username,
                                   MessageElementFlag::KickUsername,
                               },
                               userColor, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, this->message().displayName});
}

void KickMessageBuilder::appendUsernameAsSender(const QString &username)
{
    this->appendUsernameAsSender(username,
                                 this->channel()->getUserColor(username));
}

void KickMessageBuilder::appendUsernameAsSender(const QString &username,
                                                const QColor &userColor)
{
    MessageColor userMessageColor(userColor);
    if (!userColor.isValid())
    {
        userMessageColor = MessageColor::Text;
    }
    this->emplace<TextElement>(username + ':',
                               MessageElementFlags{
                                   MessageElementFlag::Username,
                                   MessageElementFlag::KickUsername,
                               },
                               userMessageColor, FontStyle::ChatMediumBold)
        ->setLink({Link::UserInfo, username});
}

void KickMessageBuilder::appendMentionedUser(const QString &username,
                                             QString &text, bool trailingSpace)
{
    auto *el =
        this->emplace<MentionElement>(username, username, MessageColor::System,
                                      this->channel()->getUserColor(username));
    el->addFlags(MessageElementFlag::KickUsername);
    text.append(username);

    if (trailingSpace)
    {
        text.append(u' ');
    }
    else
    {
        el->setTrailingSpace(false);
    }
}

}  // namespace chatterino
