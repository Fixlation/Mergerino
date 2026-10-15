#include "providers/kick/KickApi.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "controllers/highlights/HighlightController.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageElement.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickBadges.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/kick/KickEmotes.hpp"
#include "providers/kick/KickMessageBuilder.hpp"
#include "providers/seventv/SeventvBadges.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/seventv/SeventvPersonalEmotes.hpp"
#include "singletons/Settings.hpp"
#include "util/BoostJsonWrap.hpp"

#include <boost/json.hpp>
#include <gtest/gtest.h>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>

using namespace chatterino;

TEST(KickApi, DecodesManualWebsiteCookieWithoutRequiringXsrf)
{
    EXPECT_EQ(parseKickIdentityToken(" 123%7Cabc_DEF-123 "), "123|abc_DEF-123");
    EXPECT_EQ(parseKickIdentityToken("123|abc_DEF-123"), "123|abc_DEF-123");
    EXPECT_EQ(parseKickIdentityToken(R"({"sessionToken":"123|abc"})"),
              "123|abc");
    EXPECT_EQ(parseKickIdentityToken(
                  R"({"sessionToken":"123%7Cabc","xsrfToken":""})"),
              "123|abc");
}

TEST(KickApi, PreservesOptionalSecurityCookie)
{
    const auto token = parseKickIdentityToken(
        R"({"sessionToken":"123%7Cabc","xsrfToken":"abc%2Bdef%3D"})");
    const auto object = QJsonDocument::fromJson(token.toUtf8()).object();
    EXPECT_EQ(object.value("sessionToken").toString(), "123|abc");
    EXPECT_EQ(object.value("xsrfToken").toString(), "abc+def=");
}

TEST(KickApi, RejectsMalformedCredentialsAndHeaderInjection)
{
    for (const auto *input :
         {"", "  ", "{}", "[]", "null", "{broken", "session_token=abc; Path=/",
          "abc%0D%0AX-Test:1", "abc\r\ndef", "abc%ZZ", "document.cookie",
          R"({"sessionToken":123})", R"({"sessionToken":"abc;cookie=value"})"})
    {
        SCOPED_TRACE(input);
        // A bare word can be a valid opaque token. JSON null is not one.
        EXPECT_TRUE(parseKickIdentityToken(input).isEmpty());
    }
}

TEST(KickApi, RequiresServerValidatedMatchingAccount)
{
    for (const auto *response :
         {R"({"id":123})", R"({"id":"123"})", R"({"user":{"id":123}})",
          R"({"data":{"user":{"user_id":123}}})"})
    {
        SCOPED_TRACE(response);
        EXPECT_TRUE(validateKickIdentityResponse(response, 123).has_value());
        EXPECT_FALSE(validateKickIdentityResponse(response, 456).has_value());
        EXPECT_FALSE(validateKickIdentityResponse(response, 0).has_value());
    }
}

TEST(KickApi, RejectsUnauthenticatedAndNonAccountResponses)
{
    for (const auto *response :
         {"{}", "null", "[]", "", "<html>Verify</html>",
          R"({"message":"Unauthenticated"})", R"({"id":0})", R"({"id":true})",
          R"({"id":123.5})"})
    {
        SCOPED_TRACE(response);
        EXPECT_FALSE(validateKickIdentityResponse(response, 123).has_value());
    }
    EXPECT_TRUE(validateKickIdentityResponse("{}", 123).error().contains(
        "fresh token"));
    EXPECT_TRUE(validateKickIdentityResponse("<html>", 123)
                    .error()
                    .contains("verification"));
}

TEST(KickApi, PreservesChannelSlugSeparators)
{
    EXPECT_EQ(KickApi::slugify(QStringLiteral("channel_with_underscores")),
              QStringLiteral("channel_with_underscores"));
    EXPECT_EQ(KickApi::slugify(QStringLiteral("hyphen-channel")),
              QStringLiteral("hyphen-channel"));
}

TEST(KickApi, ParsesAuthoritativeChannelRolesWithoutSelectedBadges)
{
    const boost::json::object payload{
        {"id", 123},
        {"username", "Fixlation"},
        {"is_moderator", true},
        {"is_channel_owner", false},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_EQ(info.userID, 123);
    EXPECT_EQ(info.username, QStringLiteral("Fixlation"));
    EXPECT_TRUE(info.isModerator);
    EXPECT_FALSE(info.isChannelOwner);
}

TEST(KickApi, ParsesChannelOwnerSeparatelyFromModerator)
{
    const boost::json::object payload{
        {"id", 456},
        {"username", "Broadcaster"},
        {"is_moderator", false},
        {"is_channel_owner", true},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_FALSE(info.isModerator);
    EXPECT_TRUE(info.isChannelOwner);
}

TEST(KickAssets, ParsesSelectedGlobalBadgeNamesAndImages)
{
    const auto data = boost::json::parse(R"({
        "id":123,"username":"Viewer","badges_v2":[
          {"name":"GOAT","badge_type":"global",
           "image_url":"https://ext.cdn.kick.com/chat/badges/GOAT.png",
           "selected":true,"sort_order":4},
          {"name":"rs_ags_blade","badge_type":"global",
           "image_url":"https://ext.cdn.kick.com/chat/badges/rs_ags_blade.png",
           "selected":false,"sort_order":5}]})").as_object();
    const KickPrivateUserInChannelInfo info{BoostJsonObject(data)};
    ASSERT_EQ(info.badges.size(), 2);
    EXPECT_EQ(info.badges[0].type, "GOAT");
    EXPECT_EQ(info.badges[0].text, "GOAT");
    EXPECT_TRUE(info.badges[0].selected);
    EXPECT_EQ(info.badges[0].imageUrl,
              "https://ext.cdn.kick.com/chat/badges/GOAT.png");
    EXPECT_FALSE(info.badges[1].selected);
}

TEST(KickAssets, ParsesNestedBadgeImagesAndStringNumbers)
{
    const auto data = boost::json::parse(R"({"name":"subscriber",
      "count":"12","sort_order":"9","badge_image":{
      "src":"//files.kick.com/subscriber.png"}})").as_object();
    const KickPrivateUserBadgeInfo badge{BoostJsonObject(data)};
    EXPECT_EQ(badge.count, 12);
    EXPECT_EQ(badge.sortOrder, 9);
    EXPECT_EQ(badge.imageUrl, "https://files.kick.com/subscriber.png");
    const auto subData = boost::json::parse(R"({"months":"12",
      "badge_image":{"src":"https://files.kick.com/subscriber.png"}})")
                             .as_object();
    const KickPrivateSubscriberBadgeInfo sub{BoostJsonObject(subData)};
    EXPECT_EQ(sub.months, 12);
    EXPECT_EQ(sub.imageUrl, badge.imageUrl);
}

TEST(KickAssets, RendersUnlistedBadgesUsingSuppliedImages)
{
    auto [badge, flag] = KickBadges::lookup(
        "GOAT", "https://ext.cdn.kick.com/chat/badges/GOAT.png", "GOAT");
    ASSERT_NE(badge, nullptr);
    EXPECT_EQ(badge->name.string, "GOAT");
    EXPECT_EQ(badge->images.getImage1()->url().string,
              "https://ext.cdn.kick.com/chat/badges/GOAT.png");
    EXPECT_EQ(flag, MessageElementFlag::BadgeVanity);
    EXPECT_EQ(KickBadges::lookup("missing-without-image").first, nullptr);
}

TEST(KickAssets, PreservesRoleFlagsAndRecognizesBadgeAliases)
{
    const auto moderator = KickBadges::lookup("moderator");
    const auto leadModerator = KickBadges::lookup("LEAD-MODERATOR");
    ASSERT_NE(moderator.first, nullptr);
    EXPECT_EQ(leadModerator.first, moderator.first);
    EXPECT_EQ(leadModerator.second, MessageElementFlag::BadgeChannelAuthority);
    EXPECT_NE(KickBadges::lookup("sub-gifter").first, nullptr);
    auto [badge, flag] = KickBadges::lookup(
        "moderator", "https://files.kick.com/moderator.png", "Lead Moderator");
    EXPECT_EQ(flag, MessageElementFlag::BadgeChannelAuthority);
    EXPECT_EQ(badge->name.string, "Lead Moderator");
}

TEST(KickAssets, ParsesEmoteMetadataWithoutLosingImagesOrStringIds)
{
    const auto data = boost::json::parse(R"({"id":"1082364",
      "name":"xqcAM","subscribers_only":true,
      "image":{"src":"https://files.kick.com/custom-emote.gif"}})")
                          .as_object();
    const KickPrivateEmoteInfo info{BoostJsonObject(data)};
    EXPECT_EQ(info.emoteID, 1082364);
    EXPECT_EQ(info.name, "xqcAM");
    EXPECT_TRUE(info.subscribersOnly);
    EXPECT_EQ(info.imageUrl, "https://files.kick.com/custom-emote.gif");
}

TEST(KickAssets, RefreshesEmoteImagesAndKeepsMetadataForInlineTags)
{
    auto first = KickEmotes::emoteForID(
        u"987650001", u"Native", "https://files.kick.com/native-old.gif");
    auto next = KickEmotes::emoteForID(
        u"987650001", u"Native", "https://files.kick.com/native-new.gif");
    EXPECT_NE(first, next);
    EXPECT_EQ(next->images.getImage1()->url().string,
              "https://files.kick.com/native-new.gif");
    EXPECT_EQ(KickEmotes::emoteForID(u"987650001", u"Native"), next);
    auto renamed = KickEmotes::emoteForID(u"987650001", u"Renamed");
    EXPECT_EQ(renamed->name.string, "Renamed");
    EXPECT_EQ(renamed->images.getImage1()->url().string,
              "https://files.kick.com/native-new.gif");
}

TEST(KickAssets, CacheDoesNotKeepUnusedEmotesAlive)
{
    std::weak_ptr<const Emote> weak;
    {
        auto emote = KickEmotes::emoteForID(u"987650002", u"Temporary");
        weak = emote;
    }
    EXPECT_TRUE(weak.expired());
    for (int i = 0; i < 1024; ++i)
    {
        KickEmotes::emoteForID(QString::number(990000000 + i), u"Transient");
    }
    EXPECT_TRUE(weak.expired());
}

namespace chatterino {
class KickAssetTestAccess
{
public:
    static void cacheIdentity(KickChannel &channel,
                              const KickPrivateUserInChannelInfo &info)
    {
        channel.cacheOwnIdentityFromUserInfo(info);
    }

    static void applySets(KickChannel &channel,
                          const std::vector<KickPrivateEmoteSetInfo> &sets,
                          uint64_t generation, bool personal)
    {
        channel.applyKickEmoteSets(sets, generation, personal);
    }

    static uint64_t generation(KickChannel &channel)
    {
        return channel.kickEmoteGeneration_;
    }

    static void changeAccount(KickChannel &channel)
    {
        channel.updateKickEmoteAccount();
    }

    static QString prepare(KickChannel &channel, const QString &message)
    {
        return channel.prepareMessage(message);
    }

    static void setEmotes(KickChannel &channel, const EmotePtr &emote)
    {
        auto map = std::make_shared<EmoteMap>();
        map->emplace(emote->name, emote);
        channel.kickChannelEmotes_.set(std::move(map));
    }
};
}  // namespace chatterino

namespace {
class KickAssetApplication : public mock::BaseApplication
{
public:
    KickAssetApplication()
        : highlights(this->settings, &this->accounts)
    {
    }
    AccountController *getAccounts() override { return &accounts; }
    EmoteController *getEmotes() override { return &emotes; }
    HighlightController *getHighlights() override { return &highlights; }
    KickChatServer *getKickChatServer() override { return &server; }
    SeventvBadges *getSeventvBadges() override { return &badges; }
    SeventvPersonalEmotes *getSeventvPersonalEmotes() override
    {
        return &personal;
    }
    BttvEmotes *getBttvEmotes() override { return &bttv; }
    FfzEmotes *getFfzEmotes() override { return &ffz; }
    SeventvEmotes *getSeventvEmotes() override { return &seventv; }
    SeventvAPI *getSeventvAPI() override { return nullptr; }
    AccountController accounts;
    mock::EmoteController emotes;
    HighlightController highlights;
    KickChatServer server;
    SeventvBadges badges;
    SeventvPersonalEmotes personal;
    BttvEmotes bttv;
    FfzEmotes ffz;
    SeventvEmotes seventv;
};

MessagePtr buildKickAssetMessage(KickChannel &channel, const char *payload)
{
    const auto data = boost::json::parse(payload).as_object();
    return KickMessageBuilder::makeChatMessage(&channel, BoostJsonObject(data))
        .first;
}

std::vector<EmotePtr> messageBadges(const Message &message)
{
    std::vector<EmotePtr> badges;
    for (const auto &element : message.elements)
    {
        if (const auto *badge = dynamic_cast<const BadgeElement *>(element.get()))
        {
            badges.push_back(badge->getEmote());
        }
    }
    return badges;
}
}  // namespace

TEST(KickAssets, ChatKeepsSelectedV2BadgesAndHidesUnselectedOnes)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto message = buildKickAssetMessage(*channel, R"({"id":"1",
      "content":"","created_at":"2026-09-30T00:00:00Z",
      "sender":{"id":123,"username":"Viewer","identity":{
       "badges_v2":[{"name":"GOAT","badge_type":"global",
        "image_url":"https://ext.cdn.kick.com/chat/badges/GOAT.png",
        "selected":true},{"name":"rs_ags_blade","badge_type":"global",
        "image_url":"https://ext.cdn.kick.com/chat/badges/rs_ags_blade.png",
        "selected":false}],"badges":[]}}})");
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 1);
    EXPECT_EQ(badges[0]->name.string, "GOAT");
}

TEST(KickAssets, LegacyAndV2RoleBadgesDoNotDuplicate)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto message = buildKickAssetMessage(*channel, R"({"id":"2",
      "content":"","created_at":"2026-09-30T00:00:00Z",
      "sender":{"id":123,"username":"Viewer","identity":{
       "badges_v2":[{"name":"moderator","badge_type":"channel",
        "image_url":"https://files.kick.com/moderator.png","selected":true}],
       "badges":[{"type":"moderator","text":"Moderator"}]}}})");
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 1);
    EXPECT_EQ(badges[0]->images.getImage1()->url().string,
              "https://files.kick.com/moderator.png");
}

TEST(KickAssets, PlainNativeNamesAndInlineTagsBothRenderEmotes)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto native = KickEmotes::emoteForID(
        u"987650003", u"Native", "https://files.kick.com/native.gif");
    KickAssetTestAccess::setEmotes(*channel, native);
    for (const auto *payload : {
        R"({"id":"3","content":"Native","sender":{"id":123,
             "username":"Viewer","identity":{}}})",
        R"({"id":"4","content":"[emote:987650003:Native]",
             "sender":{"id":123,"username":"Viewer","identity":{}}})"})
    {
        const auto message = buildKickAssetMessage(*channel, payload);
        int emoteCount = 0;
        for (const auto &element : message->elements)
        {
            if (const auto *emote =
                    dynamic_cast<const EmoteElement *>(element.get());
                emote && element->getFlags().has(MessageElementFlag::Emote))
            {
                ++emoteCount;
                EXPECT_EQ(emote->getEmote(), native);
            }
        }
        EXPECT_EQ(emoteCount, 1);
        EXPECT_EQ(message->messageText, "Native");
    }
}

namespace {
constexpr auto FOUNDER_BADGE_IDENTITY = R"({
 "badges":[{"type":"vip","text":"VIP","sort_order":5}],
 "badges_v2":[
  {"name":"kick_founder22","badge_type":"global","selected":true,
   "sort_order":2,"image_url":"https://ext.cdn.kick.com/chat/badges/kick_founder22_2a2d3875-276e-4d87-ab63-d7578d2e10c0.png"},
  {"name":"level","badge_type":"global","selected":true,"sort_order":1,
   "metadata":{"level":37},"image_url":"https://ext.cdn.kick.com/chat/badges/37_019f35ce-472c-749c-8e50-5f53afb1de7b.png"},
  {"name":"WorldofWarcraftForeverBeta","badge_type":"global","selected":true,
   "sort_order":4,"image_url":"https://ext.cdn.kick.com/chat/badges/WorldofWarcraftForeverBeta_c4ae6085-8ec5-4905-bca5-23c56b9787d8.png"}
 ]})";

boost::json::object founderBadgePayload()
{
    auto data = boost::json::parse(R"({"id":"founder22","content":"",
      "sender":{"id":123,"username":"Viewer"}})").as_object();
    data["sender"].as_object()["identity"] =
        boost::json::parse(FOUNDER_BADGE_IDENTITY);
    return data;
}
}  // namespace

TEST(KickAssets, ChannelRolesLeadFounderAndLevelBadges)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto data = founderBadgePayload();
    const auto message =
        KickMessageBuilder::makeChatMessage(channel.get(), BoostJsonObject(data))
            .first;
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 4);
    EXPECT_EQ(badges[0]->name.string, "VIP");
    EXPECT_EQ(badges[1]->name.string, "kick_founder22");
    EXPECT_EQ(badges[2]->name.string, "WorldofWarcraftForeverBeta");
    EXPECT_EQ(badges[3]->name.string, "Level 37");
}

TEST(KickAssets, CustomChannelBadgesAndModeratorAliasesLeadGlobalBadges)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto message = buildKickAssetMessage(*channel, R"({"id":"roles",
      "content":"","sender":{"id":123,"username":"Viewer","identity":{
       "badges_v2":[{"name":"GOAT","badge_type":"global","sort_order":1,
         "image_url":"https://ext.cdn.kick.com/GOAT.png"},
        {"name":"channel_helper","badge_type":"channel","sort_order":2,
         "image_url":"https://ext.cdn.kick.com/helper.png"}],
       "badges":[{"type":"lead-moderator","text":"Moderator","sort_order":0},
        {"type":"vip","text":"VIP","sort_order":5}]}}})");
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 4);
    EXPECT_EQ(badges[0]->name.string, "Moderator");
    EXPECT_EQ(badges[1]->name.string, "channel_helper");
    EXPECT_EQ(badges[2]->name.string, "VIP");
    EXPECT_EQ(badges[3]->name.string, "GOAT");
}

TEST(KickAssets, CachedFounderBadgeIsNotClassifiedAsLevel)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    // Modify only this fixture's anonymous account; no real login or network.
    app.accounts.kick.current()->update(KickAccountData{.userID = 123});
    auto data = boost::json::parse(FOUNDER_BADGE_IDENTITY).as_object();
    data["id"] = 123;
    data["username"] = "Viewer";
    const KickPrivateUserInChannelInfo info{BoostJsonObject(data)};
    KickAssetTestAccess::cacheIdentity(*channel, info);
    const auto message = KickMessageBuilder::makeSentMessage(
        channel.get(), "", "local-founder22");
    ASSERT_NE(message, nullptr);
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 4);
    EXPECT_EQ(badges[0]->name.string, "VIP");
    EXPECT_EQ(badges[1]->name.string, "kick_founder22");
    EXPECT_EQ(badges[2]->name.string, "WorldofWarcraftForeverBeta");
    EXPECT_EQ(badges[3]->name.string, "Level 37");
    int levelBadges = 0;
    for (const auto &element : message->elements)
    {
        if (element->getFlags().has(MessageElementFlag::BadgeKickLevel))
        {
            ++levelBadges;
            const auto *badge = dynamic_cast<const BadgeElement *>(element.get());
            ASSERT_NE(badge, nullptr);
            EXPECT_EQ(badge->getEmote()->name.string, "Level 37");
        }
    }
    EXPECT_EQ(levelBadges, 1);
}

TEST(KickAssets, HidingLevelsKeepsThe2022FounderBadge)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    const auto data = founderBadgePayload();
    const auto previous = app.settings.showKickLevelBadges.getValue();
    app.settings.showKickLevelBadges.setValue(false);
    const auto message =
        KickMessageBuilder::makeChatMessage(channel.get(), BoostJsonObject(data))
            .first;
    app.settings.showKickLevelBadges.setValue(previous);
    const auto badges = messageBadges(*message);
    ASSERT_EQ(badges.size(), 3);
    EXPECT_EQ(badges[0]->name.string, "VIP");
    EXPECT_EQ(badges[1]->name.string, "kick_founder22");
    EXPECT_EQ(badges[2]->name.string, "WorldofWarcraftForeverBeta");
}

TEST(KickAssets, LargeFounderSizedImageDecodesWithinBadgeBounds)
{
    // The actual Kick 2022 founder image is a static 3087 x 3087 PNG.
    QImage source(3087, 3087, QImage::Format_ARGB32);
    source.fill(Qt::yellow);
    QByteArray data;
    QBuffer output(&data);
    ASSERT_TRUE(output.open(QIODevice::WriteOnly));
    ASSERT_TRUE(source.save(&output, "PNG"));
    output.close();
    source = {};

    QBuffer unscaledBuffer(&data);
    ASSERT_TRUE(unscaledBuffer.open(QIODevice::ReadOnly));
    QImageReader unscaled(&unscaledBuffer);
    EXPECT_TRUE(detail::readFrames(unscaled, {"https://example.test/founder.png"})
                    .empty());

    QBuffer badgeBuffer(&data);
    ASSERT_TRUE(badgeBuffer.open(QIODevice::ReadOnly));
    QImageReader badgeReader(&badgeBuffer);
    const auto frames = detail::readFrames(
        badgeReader, {"https://example.test/founder.png"}, {72, 72});
    ASSERT_EQ(frames.size(), 1);
    EXPECT_EQ(frames[0].image.size(), QSize(72, 72));
    EXPECT_EQ(frames[0].image.toImage().pixelColor(36, 36), QColor(Qt::yellow));
}

namespace {
std::vector<KickPrivateEmoteSetInfo> collectibleCatalog()
{
    const auto data = boost::json::parse(R"([
      {"id":"Global","name":"Global","emotes":[{"id":1,"name":"Global"}]},
      {"id":"Collectibles","name":"Collectibles","emotes":[
        {"id":"6163976","name":"collectiblesMurlocJam"}]},
      {"name":"Channel","user_id":"123","emotes":[
        {"id":2,"name":"ChannelEmote"}]}
    ])")
                          .as_array();
    std::vector<KickPrivateEmoteSetInfo> sets;
    for (const auto &set : data)
    {
        sets.emplace_back(BoostJsonObject(set.as_object()));
    }
    return sets;
}

QByteArray largeCollectibleGif(int frameCount)
{
    // A 500 x 500 logical canvas with 1-pixel updates. GIF decoders composite
    // every animation frame at canvas size, just like Kick's MurlocJam asset.
    auto data = QByteArray::fromHex("474946383961f401f401800000000000ffffff");
    for (int i = 0; i < frameCount; ++i)
    {
        data += QByteArray::fromHex("21f90404040000002c0000000001000100000202");
        data += QByteArray::fromHex(i % 2 == 0 ? "440100" : "4c0100");
    }
    data += char(0x3b);
    return data;
}
}  // namespace

TEST(KickApi, CollectibleSetKeepsAccountOwnedIdentityWithoutUserId)
{
    const auto sets = collectibleCatalog();
    ASSERT_EQ(sets.size(), 3);
    EXPECT_EQ(sets[1].name, "Collectibles");
    EXPECT_FALSE(sets[1].userID);
    ASSERT_EQ(sets[1].emotes.size(), 1);
    EXPECT_EQ(sets[1].emotes[0].emoteID, 6163976);
}

TEST(KickAssets, OwnedCollectiblesRenderAndSendAsNativeEmotes)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    KickAssetTestAccess::applySets(*channel, collectibleCatalog(), 0, true);
    ASSERT_EQ(channel->kickChannelEmotes()->size(), 2);
    const auto prepared = KickAssetTestAccess::prepare(
        *channel, "collectiblesMurlocJam ChannelEmote");
    EXPECT_EQ(prepared,
              "[emote:6163976:collectiblesMurlocJam] [emote:2:ChannelEmote]");
    for (
        const auto *payload : {
            R"({"id":"collectible-1","content":"collectiblesMurlocJam",
          "sender":{"id":123,"username":"Viewer","identity":{}}})",
            R"({"id":"collectible-2","content":"[emote:6163976:collectiblesMurlocJam]",
          "sender":{"id":123,"username":"Viewer","identity":{}}})"})
    {
        const auto message = buildKickAssetMessage(*channel, payload);
        int count = 0;
        for (const auto &element : message->elements)
        {
            const auto *emote =
                dynamic_cast<const EmoteElement *>(element.get());
            if (emote && element->getFlags().has(MessageElementFlag::Emote))
            {
                ++count;
                EXPECT_EQ(emote->getEmote()->id.string, "6163976");
                EXPECT_EQ(emote->getEmote()->images.getImage1()->url().string,
                          "https://files.kick.com/emotes/6163976/fullsize");
            }
        }
        EXPECT_EQ(count, 1);
    }
}

TEST(KickAssets, PublicCatalogDoesNotGrantCollectibles)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    KickAssetTestAccess::applySets(*channel, collectibleCatalog(), 0, false);
    ASSERT_EQ(channel->kickChannelEmotes()->size(), 1);
    EXPECT_EQ(KickAssetTestAccess::prepare(*channel, "collectiblesMurlocJam"),
              "collectiblesMurlocJam");
}

TEST(KickAssets, AccountChangesClearCollectiblesAndIgnoreOldResponses)
{
    KickAssetApplication app;
    auto channel = std::make_shared<KickChannel>("test");
    KickAssetTestAccess::applySets(*channel, collectibleCatalog(), 0, true);
    ASSERT_EQ(channel->kickChannelEmotes()->size(), 2);
    KickAssetTestAccess::changeAccount(*channel);
    EXPECT_TRUE(channel->kickChannelEmotes()->empty());
    KickAssetTestAccess::applySets(*channel, collectibleCatalog(), 0, true);
    EXPECT_TRUE(channel->kickChannelEmotes()->empty());
    KickAssetTestAccess::applySets(*channel, collectibleCatalog(),
                                   KickAssetTestAccess::generation(*channel),
                                   false);
    EXPECT_EQ(channel->kickChannelEmotes()->size(), 1);
}

TEST(KickAssets, LargeCollectibleAnimationKeepsAllFramesWithinDisplayBounds)
{
    auto data = largeCollectibleGif(45);
    QBuffer originalBuffer(&data);
    ASSERT_TRUE(originalBuffer.open(QIODevice::ReadOnly));
    QImageReader original(&originalBuffer);
    ASSERT_EQ(original.size(), QSize(500, 500));
    ASSERT_EQ(original.imageCount(), 45);
    EXPECT_TRUE(
        detail::readFrames(original, {"https://example.test/murloc.gif"})
            .empty());

    QBuffer boundedBuffer(&data);
    ASSERT_TRUE(boundedBuffer.open(QIODevice::ReadOnly));
    QImageReader bounded(&boundedBuffer);
    const auto frames = detail::readFrames(
        bounded, {"https://example.test/murloc.gif"}, {224, 112});
    ASSERT_EQ(frames.size(), 45);
    qint64 retainedBytes = 0;
    for (const auto &frame : frames)
    {
        EXPECT_EQ(frame.image.size(), QSize(112, 112));
        EXPECT_EQ(frame.duration, 40);
        retainedBytes += frame.image.width() * frame.image.height() * 4;
    }
    EXPECT_LT(retainedBytes, Image::maxBytesRam);
}
