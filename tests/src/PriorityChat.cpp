// SPDX-License-Identifier: MIT
#include "widgets/splits/PriorityChat.hpp"

#include "common/WindowDescriptors.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/priority/PriorityChatModel.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/dialogs/SplitSettingsDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitInput.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QTabBar>
#include <QToolButton>

using namespace chatterino;
using namespace chatterino::priority;

namespace {
const auto NOW = QDateTime::fromString("2026-11-15T12:00:00Z", Qt::ISODate);
std::shared_ptr<Message> message(
    QString id = "1", MessagePlatform platform = MessagePlatform::AnyOrTwitch)
{
    auto m = std::make_shared<Message>();
    m->id = "message-" + id;
    m->userID = id;
    m->platform = platform;
    m->loginName = "viewer" + id;
    m->displayName = m->loginName;
    m->messageText = "Hello";
    m->serverReceivedTime = NOW;
    return m;
}
class PriorityRoleChannel : public Channel
{
public:
    explicit PriorityRoleChannel(const QString &name)
        : Channel(name, Channel::Type::Twitch)
    {
    }
    bool isMod() const override
    {
        return moderator;
    }
    bool isBroadcaster() const override
    {
        return broadcaster;
    }
    bool moderator = true;
    bool broadcaster = false;
};
class PriorityTwitchServer : public mock::MockTwitchIrcServer
{
public:
    ChannelPtr getOrAddChannel(const QString &name) override
    {
        auto &channel = channels[name.toLower()];
        if (!channel)
            channel = std::make_shared<PriorityRoleChannel>(name);
        return channel;
    }
    QHash<QString, std::shared_ptr<PriorityRoleChannel>> channels;
};
class PriorityApplication : public mock::BaseApplication
{
public:
    PriorityApplication()
        : mock::BaseApplication(QStringLiteral("{}"))
        , windows(args, paths_, settings, theme, fonts)
        , commands(paths_)
    {
    }
    WindowManager *getWindows() override
    {
        return &windows;
    }
    HotkeyController *getHotkeys() override
    {
        return &hotkeys;
    }
    AccountController *getAccounts() override
    {
        return &accounts;
    }
    CommandController *getCommands() override
    {
        return &commands;
    }
    EmoteController *getEmotes() override
    {
        return &emotes;
    }
    ITwitchIrcServer *getTwitch() override
    {
        return &twitch;
    }
    WindowManager windows;
    HotkeyController hotkeys;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
    PriorityTwitchServer twitch;
};
}  // namespace
namespace chatterino {
class PriorityChatTestAccess
{
public:
    static void refreshAccess(Split &split)
    {
        split.refreshPriorityChatAccess();
    }
    static MessagePtr parseYouTube(const QJsonObject &renderer)
    {
        return YouTubeLiveChat::parseRendererMessage(
            renderer, "liveChatTextMessageRenderer", "source");
    }
    static void accountChanged(PriorityChat &pane)
    {
        pane.accountsChanged();
    }
    static void save(PriorityChat &pane)
    {
        pane.savePreferences();
    }
    static Model &model(PriorityChat &pane)
    {
        return pane.model_;
    }
    static ChannelView &view(PriorityChat &pane)
    {
        return *pane.view_;
    }
    static void flush(PriorityChat &pane)
    {
        pane.flush();
        pane.refreshFeed();
    }
    static void refresh(PriorityChat &pane)
    {
        pane.refreshFeed();
    }
    static quint64 generation(PriorityChat &pane)
    {
        return pane.generation_;
    }
    static void finish(PriorityChat &pane, quint64 generation)
    {
        pane.lookupFinished(generation, true);
    }
    static BasePopup *popup(PriorityChat &pane)
    {
        return pane.settingsPopup_;
    }
    static quint64 settingsGeneration(PriorityChat &pane)
    {
        return pane.settingsGeneration_;
    }
    static void resolvePerson(PriorityChat &pane, quint64 generation,
                              quint64 settingsGeneration, const Person &person)
    {
        pane.addResolvedPerson(generation, settingsGeneration, person);
    }
    static PriorityChat *pane(Split &split)
    {
        return split.priorityChat_;
    }
};
}  // namespace chatterino

TEST(PriorityChatModel, DefaultsDoNotImposeThresholdsOrIncludeUnknownUsers)
{
    Model model;
    auto m = message();
    model.observe(*m);
    EXPECT_FALSE(model.matches(*m, NOW));
    for (const auto &r : model.rules)
    {
        EXPECT_FALSE(r.configured());
        EXPECT_EQ(r.subscriptionMonths, 0);
        EXPECT_EQ(r.followMonths, 0);
    }
    model.rules[0].subscriptions = true;
    model.rules[0].subscriptionMonths = 1;
    EXPECT_FALSE(model.matches(*m, NOW));
}
TEST(PriorityChatModel, ModeratorsAndVipsFollowEachPlatformsCheckboxes)
{
    Model model;
    auto twitchMod = message("1");
    twitchMod->twitchBadges.emplace_back("moderator", "1");
    auto twitchVip = message("2");
    twitchVip->twitchBadges.emplace_back("vip", "1");
    auto kickMod = message("3", MessagePlatform::Kick);
    kickMod->externalBadges.append("kick:moderator");
    auto kickVip = message("4", MessagePlatform::Kick);
    kickVip->externalBadges.append("kick:vip");
    auto youtubeMod = message("5", MessagePlatform::YouTube);
    youtubeMod->externalBadges.append("youtube:moderator");
    auto viewer = message("6");
    for (const auto &m : {twitchMod, twitchVip, kickMod, kickVip, youtubeMod})
        EXPECT_FALSE(model.matches(*m, NOW));

    model.rules[0].moderators = true;
    EXPECT_TRUE(model.matches(*twitchMod, NOW));
    EXPECT_FALSE(model.matches(*twitchVip, NOW));
    EXPECT_FALSE(model.matches(*kickMod, NOW));
    model.rules[0].vips = true;
    EXPECT_TRUE(model.matches(*twitchVip, NOW));
    EXPECT_FALSE(model.matches(*viewer, NOW));

    model.rules[1].vips = true;
    EXPECT_FALSE(model.matches(*kickMod, NOW));
    EXPECT_TRUE(model.matches(*kickVip, NOW));
    model.rules[1].moderators = true;
    EXPECT_TRUE(model.matches(*kickMod, NOW));

    model.rules[2].moderators = true;
    EXPECT_TRUE(model.matches(*youtubeMod, NOW));

    // Removing someone still wins over their role.
    Person person;
    person.platform = MessagePlatform::AnyOrTwitch;
    person.id = twitchMod->userID;
    person.login = twitchMod->loginName;
    model.setChoice(person, Choice::Exclude);
    EXPECT_FALSE(model.matches(*twitchMod, NOW));

    // The checkboxes are saved.
    Model loaded;
    loaded.load(model.toJson());
    EXPECT_TRUE(loaded.rules[0].moderators && loaded.rules[0].vips);
    EXPECT_TRUE(loaded.rules[1].moderators && loaded.rules[1].vips);
    EXPECT_TRUE(loaded.rules[2].moderators);
    EXPECT_FALSE(loaded.rules[3].moderators || loaded.rules[3].vips);
}
TEST(PriorityChatModel,
     BotsStayOutUnlessEnabledOrManuallyIncludedOnEveryPlatform)
{
    for (int i = 0; i < 4; ++i)
    {
        SCOPED_TRACE(i);
        Model model;
        auto &rule = model.rules[i];
        auto m = message("bot", static_cast<MessagePlatform>(i));
        m->loginName = "Nightbot";
        if (i == 2)
            m->userID = "UCjerlCIbLPQwSnYlClkjDXg";
        if (i == 3)
        {
            rule.botAccounts = {"@custom_bot"};
            m->userID = "custom_bot";
        }
        m->twitchBadges.emplace_back("moderator", "1");
        m->twitchBadges.emplace_back("vip", "1");
        m->externalBadges = {"kick:moderator", "youtube:moderator"};
        m->membershipMonths = 24;
        m->activeMember = true;
        model.observe(*m);
        ASSERT_TRUE(model.isBot(*m));
        EXPECT_FALSE(rule.bots);
        rule.moderators = rule.vips = rule.subscriptions = true;
        rule.subscriptionMonths = 1;
        EXPECT_FALSE(model.matches(*m, NOW));
        const auto person =
            model.people.value(personKey(m->platform, m->userID));
        model.setChoice(person, Choice::Include);
        EXPECT_TRUE(model.matches(*m, NOW));
        EXPECT_TRUE(model.qualifies(person, NOW));
        Model loaded;
        loaded.load(model.toJson());
        EXPECT_FALSE(loaded.rules[i].bots);
        EXPECT_TRUE(loaded.matches(*m, NOW));
        model.setChoice(person, Choice::Automatic);
        EXPECT_FALSE(model.matches(*m, NOW));
        EXPECT_FALSE(model.qualifies(person, NOW));
        rule.bots = true;
        // Bots is itself an inclusion rule; other rules are not required.
        rule.moderators = rule.vips = rule.subscriptions = false;
        model.setChoice(person, Choice::Automatic);
        EXPECT_TRUE(model.matches(*m, NOW));
        EXPECT_TRUE(model.qualifies(person, NOW));
        model.setChoice(person, Choice::Exclude);
        EXPECT_FALSE(model.matches(*m, NOW));
        model.setChoice(person, Choice::Include);
        m->flags.set(MessageFlag::Disabled);
        EXPECT_FALSE(model.matches(*m, NOW));
    }
}

TEST(PriorityChatModel, BotBadgesAndExactAccountsDoNotGuessFromDisplayNames)
{
    Model model;
    auto twitch = message("twitch");
    twitch->twitchBadges.emplace_back("bot-badge", "1");
    EXPECT_TRUE(model.isBot(*twitch));
    model.observe(*twitch);
    twitch->twitchBadges.clear();
    EXPECT_TRUE(model.isBot(*twitch));  // Badge evidence survives missing tags.
    auto ffz = message("ffz");
    ffz->externalBadges = {"frankerfacez:bot"};
    EXPECT_TRUE(model.isBot(*ffz));
    auto kick = message("kick", MessagePlatform::Kick);
    kick->externalBadges = {"kick:bot"};
    EXPECT_TRUE(model.isBot(*kick));
    kick->externalBadges.clear();
    for (const auto *login : {"botrix", "Kicklet", "Nightbot", "StreamElements",
                              "streamlabs", "KickBot"})
    {
        kick->loginName = login;
        EXPECT_TRUE(model.isBot(*kick));
    }
    kick->loginName = "nightbotfan";
    kick->displayName = "Nightbot";
    EXPECT_FALSE(model.isBot(*kick));
    auto youtube = message("not-the-bot", MessagePlatform::YouTube);
    youtube->loginName = youtube->displayName = "StreamElements";
    EXPECT_FALSE(model.isBot(*youtube));
    for (const auto *id :
         {"UCjerlCIbLPQwSnYlClkjDXg", "UCNL8jaJ9hId96P13QmQXNtA",
          "UCSvjQBDgYDB5TGVmCZObcwA"})
    {
        youtube->userID = id;
        EXPECT_TRUE(model.isBot(*youtube));
    }
    youtube->userID = youtube->userID.toLower();
    EXPECT_FALSE(model.isBot(*youtube));
    model.rules[3].botAccounts =
        parseBotAccounts("@my_bot, other_bot; @my_bot");
    auto tiktok = message("MY_BOT", MessagePlatform::TikTok);
    EXPECT_TRUE(model.isBot(*tiktok));
    tiktok->userID.clear();
    tiktok->loginName = "my_bot";
    EXPECT_FALSE(
        model.isBot(*tiktok));  // Display-name fallback is not identity.
    tiktok->userID = "my_bot_fan";
    EXPECT_FALSE(model.isBot(*tiktok));
    tiktok->userID = "my_bot";
    model.rules[3].botAccounts.clear();
    EXPECT_FALSE(model.isBot(*tiktok));
}

TEST(PriorityChatModel, BotSettingsMigrateOffAndRoundTripAllPlatforms)
{
    Model model;
    model.load({{"rules", QJsonArray{QJsonObject{{"moderators", true}}}}});
    for (const auto &rule : model.rules)
    {
        EXPECT_FALSE(rule.bots);
        EXPECT_TRUE(rule.botAccounts.isEmpty());
    }
    for (auto &rule : model.rules)
    {
        rule.bots = true;
        rule.botAccounts = {"custom_bot", "second_bot"};
        EXPECT_FALSE(rule.configured());  // Bot-only rules need no age lookup.
    }
    Model loaded;
    loaded.load(model.toJson());
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(loaded.rules[i].toJson(), model.rules[i].toJson());
    EXPECT_EQ(parseBotAccounts(" first,second; first\nthird "),
              (QStringList{"first", "second", "third"}));
    EXPECT_TRUE(parseBotAccounts(QString(257, 'a')).isEmpty());
}

TEST(PriorityChatModel, ArbitraryThresholdUsesTwitchMetadataAndFounderMonths)
{
    Model model;
    model.rules[0] = {true, 17, false, false, 0, false};
    auto m = message();
    m->twitchBadgeInfos["subscriber"] = "16";
    model.observe(*m);
    EXPECT_FALSE(model.matches(*m, NOW));
    m = message();
    m->twitchBadgeInfos["founder"] = "17";
    model.observe(*m);
    EXPECT_TRUE(model.matches(*m, NOW));
}
TEST(PriorityChatModel,
     YouTubeMilestonesAreConservativeAndNeverGuessedFromTierNames)
{
    EXPECT_EQ(youtubeBadgeMonths("Member (6 months)"), 6);
    EXPECT_EQ(youtubeBadgeMonths("Member (2 years)"), 24);
    EXPECT_EQ(youtubeBadgeMonths("Member (1 month)"), 1);
    for (const auto &s :
         {"Member", "Gold member", "Gifted 12 memberships", "6 months",
          "Member (99999999999 years)", "Member (6 months) VIP"})
        EXPECT_FALSE(youtubeBadgeMonths(s));
    Model model;
    model.rules[2] = {true, 9, false, false, 0, false};
    auto m = message("1", MessagePlatform::YouTube);
    m->membershipMonths = 6;
    m->activeMember = true;
    model.observe(*m);
    EXPECT_FALSE(model.matches(*m, NOW));
    model.rules[2].subscriptionMonths = 6;
    EXPECT_TRUE(model.matches(*m, NOW));
}
TEST(PriorityChatModel, FollowRulesUseCalendarMonthsAndKnownDates)
{
    Model model;
    auto m = message();
    model.observe(*m);
    auto &p = model.people[personKey(m->platform, m->userID)];
    model.rules[0] = {false, 0, false, true, 3, false};
    p.followedAt = NOW.addMonths(-3);
    EXPECT_FALSE(model.matches(*m, NOW));
    p.followingKnown = true;
    EXPECT_TRUE(model.matches(*m, NOW));
    p.followedAt = NOW.addMonths(-3).addSecs(1);
    EXPECT_FALSE(model.matches(*m, NOW));
    p.followedAt = NOW.addYears(1);
    EXPECT_FALSE(model.matches(*m, NOW));
}
TEST(PriorityChatModel, AnyAllAndIncompleteRulesAreExplicit)
{
    Model model;
    auto m = message();
    m->twitchBadgeInfos["subscriber"] = "17";
    model.observe(*m);
    auto &r = model.rules[0];
    r = {true, 17, false, true, 24, false};
    EXPECT_TRUE(model.matches(*m, NOW));
    r.matchAll = true;
    EXPECT_FALSE(model.matches(*m, NOW));
    auto &p = model.people[personKey(m->platform, m->userID)];
    p.followingKnown = true;
    p.followedAt = NOW.addMonths(-24);
    EXPECT_TRUE(model.matches(*m, NOW));
    r.followMonths = 0;
    EXPECT_FALSE(model.matches(*m, NOW));
}
TEST(PriorityChatModel,
     LapsedSubscriptionsRetainVerifiedHistoryButActiveOnlyRemainsOptional)
{
    Model model;
    model.rules[0] = {true, 12, false, false, 0, false};
    auto m = message();
    m->twitchBadgeInfos["subscriber"] = "22";
    model.observe(*m);
    auto lapsed = message();
    lapsed->serverReceivedTime = NOW.addSecs(1);
    model.observe(*lapsed);
    EXPECT_TRUE(model.matches(*lapsed, NOW));
    model.rules[0].activeOnly = true;
    EXPECT_FALSE(model.matches(*lapsed, NOW));
    Model restored;
    restored.load(model.toJson());
    EXPECT_EQ(restored.people.value(personKey(m->platform, m->userID)).months,
              22);
    EXPECT_FALSE(restored.people.value(personKey(m->platform, m->userID))
                     .active.has_value());
    EXPECT_FALSE(restored.matches(*m, NOW));
    restored.rules[0].activeOnly = false;
    EXPECT_TRUE(restored.matches(*lapsed, NOW));
}
TEST(PriorityChatModel, ManualChoicesUsePlatformAndIdentityNotTheDisplayName)
{
    Model model;
    auto a = message("1"), b = message("2"),
         kick = message("1", MessagePlatform::Kick);
    b->loginName = a->loginName;
    model.observe(*a);
    model.observe(*b);
    model.observe(*kick);
    const auto p = model.people.value(personKey(a->platform, a->userID));
    model.setChoice(p, Choice::Include);
    EXPECT_TRUE(model.matches(*a, NOW));
    EXPECT_FALSE(model.matches(*b, NOW));
    EXPECT_FALSE(model.matches(*kick, NOW));
    a->loginName = "renamed";
    model.observe(*a);
    EXPECT_TRUE(model.matches(*a, NOW));
    model.setChoice(p, Choice::Exclude);
    EXPECT_FALSE(model.matches(*a, NOW));
    Model restored;
    restored.load(model.toJson());
    EXPECT_EQ(restored.choice(personKey(a->platform, a->userID)),
              Choice::Exclude);
}
TEST(PriorityChatModel, ManualInclusionDoesNotResurrectDeletedOrSystemMessages)
{
    Model model;
    auto m = message();
    model.observe(*m);
    model.setChoice(model.people.begin().value(), Choice::Include);
    for (const auto flag :
         {MessageFlag::Disabled, MessageFlag::InvalidReplyTarget,
          MessageFlag::System, MessageFlag::SharedMessage})
    {
        m->flags.set(flag);
        EXPECT_FALSE(model.matches(*m, NOW));
        m->flags.unset(flag);
    }
}
TEST(PriorityChatModel, OutOfOrderHistoryCannotOverwriteNewerMembershipStatus)
{
    Model model;
    auto current = message();
    model.observe(*current);
    auto old = message();
    old->serverReceivedTime = NOW.addDays(-10);
    old->twitchBadgeInfos["subscriber"] = "22";
    model.observe(*old);
    const auto p =
        model.people.value(personKey(current->platform, current->userID));
    EXPECT_EQ(p.months, 22);
    ASSERT_TRUE(p.active.has_value());
    EXPECT_FALSE(*p.active);
}
TEST(PriorityChatModel, CacheIsBoundedAndPreferencesClampInvalidInputs)
{
    Model model;
    for (int i = 0; i < 2300; ++i)
    {
        auto m = message(QString::number(i));
        m->serverReceivedTime = NOW.addSecs(i);
        model.observe(*m);
    }
    EXPECT_EQ(model.people.size(), Model::MAX_PEOPLE);
    model.load({{"width", 99999},
                {"rules", QJsonArray{QJsonObject{{"subscriptions", true},
                                                 {"subscriptionMonths", 99999},
                                                 {"followMonths", -10}}}}});
    EXPECT_EQ(model.width, 800);
    EXPECT_EQ(model.rules[0].subscriptionMonths, 1200);
    EXPECT_EQ(model.rules[0].followMonths, 0);
}
TEST(PriorityChatModel, MembershipMetadataSurvivesMessageCloning)
{
    auto m = message("1", MessagePlatform::YouTube);
    m->membershipMonths = 12;
    m->activeMember = true;
    auto cloned = m->clone();
    EXPECT_EQ(cloned->membershipMonths, 12);
    EXPECT_EQ(cloned->activeMember, true);
}

TEST(PriorityChatModel,
     ActualYouTubeChatBadgesReachEligibilityWithoutGuessingCustomArtwork)
{
    PriorityApplication app;
    QJsonObject renderer{
        {"id", "yt-message"},
        {"authorExternalChannelId", "UC0123456789012345678901"},
        {"authorName", QJsonObject{{"simpleText", "viewer"}}},
        {"message", QJsonObject{{"simpleText", "Hello"}}}};
    renderer.insert(
        "authorBadges",
        QJsonArray{QJsonObject{
            {"liveChatAuthorBadgeRenderer",
             QJsonObject{
                 {"tooltip", "Member (6 months)"},
                 {"customThumbnail",
                  QJsonObject{
                      {"thumbnails",
                       QJsonArray{QJsonObject{
                           {"url",
                            "https://example.invalid/badge.png"}}}}}}}}}});
    auto parsed = PriorityChatTestAccess::parseYouTube(renderer);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->membershipMonths, 6);
    EXPECT_EQ(parsed->activeMember, true);
    Model model;
    model.rules[2] = {true, 6, false, false, 0, false};
    model.observe(*parsed);
    EXPECT_TRUE(model.matches(*parsed, NOW));
    renderer.insert(
        "authorBadges",
        QJsonArray{QJsonObject{
            {"liveChatAuthorBadgeRenderer",
             QJsonObject{
                 {"tooltip", "Member"},
                 {"customThumbnail",
                  QJsonObject{
                      {"thumbnails",
                       QJsonArray{QJsonObject{
                           {"url",
                            "https://example.invalid/badge.png"}}}}}}}}}});
    parsed = PriorityChatTestAccess::parseYouTube(renderer);
    ASSERT_NE(parsed, nullptr);
    EXPECT_FALSE(parsed->membershipMonths.has_value());
    EXPECT_EQ(parsed->activeMember, true);
}

TEST(PriorityChatModel, StaleActivityDoesNotProveAnActiveSubscription)
{
    Model model;
    model.rules[0] = {true, 12, true, false, 0, false};
    auto m = message();
    m->twitchBadgeInfos["subscriber"] = "22";
    model.observe(*m);
    EXPECT_TRUE(model.matches(*m, NOW));
    EXPECT_FALSE(model.matches(*m, NOW.addSecs(1801)));
}

TEST(PriorityChatWidget, BackfillsBufferedMessagesAndLeavesTheSourceUntouched)
{
    PriorityApplication app;
    Split split(nullptr);
    auto channel =
        std::make_shared<Channel>("priority-test", Channel::Type::None);
    split.setChannel(channel);
    auto a = message("a"), b = message("b");
    channel->addMessage(a, MessageContext::Repost);
    channel->addMessage(b, MessageContext::Repost);
    PriorityChat pane(&split, &split);
    pane.setChannel(channel);
    pane.setMonitoring(true);
    auto &view = PriorityChatTestAccess::view(pane);
    EXPECT_EQ(view.channel()->countMessages(), 0);
    pane.setUserChoice(a, Choice::Include);
    PriorityChatTestAccess::refresh(pane);
    ASSERT_EQ(view.channel()->countMessages(), 1);
    EXPECT_EQ(view.channel()->getLastMessage(), a);
    EXPECT_EQ(channel->countMessages(), 2);
    EXPECT_EQ(view.underlyingChannel(), channel);
    channel->disableMessage(a->id);
    PriorityChatTestAccess::flush(pane);
    EXPECT_EQ(view.channel()->countMessages(), 0);
    EXPECT_EQ(channel->countMessages(), 2);
}
TEST(PriorityChatWidget, ChannelSettingsAreIsolatedAndClosedPanesDisconnect)
{
    PriorityApplication app;
    Split split(nullptr);
    auto a = std::make_shared<Channel>("priority-a", Channel::Type::Twitch);
    auto b = std::make_shared<Channel>("priority-b", Channel::Type::Twitch);
    PriorityChat pane(&split, &split);
    pane.setChannel(a);
    pane.setMonitoring(true);
    auto m = message();
    a->addMessage(m, MessageContext::Repost);
    pane.setUserChoice(m, Choice::Include);
    PriorityChatTestAccess::refresh(pane);
    EXPECT_EQ(PriorityChatTestAccess::view(pane).channel()->countMessages(), 1);
    pane.setChannel(b);
    b->addMessage(m, MessageContext::Repost);
    PriorityChatTestAccess::flush(pane);
    EXPECT_EQ(PriorityChatTestAccess::view(pane).channel()->countMessages(), 0);
    pane.setChannel(a);
    PriorityChatTestAccess::refresh(pane);
    EXPECT_EQ(PriorityChatTestAccess::view(pane).channel()->countMessages(), 1);
    const auto stale = PriorityChatTestAccess::generation(pane);
    pane.setMonitoring(false);
    PriorityChatTestAccess::finish(pane, stale);
    a->addMessage(message("late"), MessageContext::Repost);
    PriorityChatTestAccess::flush(pane);
    EXPECT_EQ(PriorityChatTestAccess::view(pane).underlyingChannel(),
              Channel::getEmpty());
}
TEST(PriorityChatWidget, SplitToggleAndActivityStateRemainIndependent)
{
    PriorityApplication app;
    Split split(nullptr);
    split.setChannel(
        IndirectChannel(std::make_shared<PriorityRoleChannel>("toggle-role")));
    EXPECT_FALSE(split.priorityChatEnabled());
    split.setPriorityChatEnabled(true);
    EXPECT_TRUE(split.priorityChatEnabled());
    EXPECT_FALSE(split.isActivityPane());
    EXPECT_TRUE(split.inputEnabled());
    split.setPriorityChatEnabled(false);
    EXPECT_FALSE(split.priorityChatEnabled());
    EXPECT_TRUE(split.inputEnabled());
    split.setInputEnabled(false);
    split.setPriorityChatEnabled(true);
    EXPECT_FALSE(split.priorityChatEnabled());
}
TEST(PriorityChatWidget, LayoutDescriptorRestoresOnlyExplicitPriorityActivation)
{
    SplitDescriptor descriptor;
    SplitDescriptor::loadFromJSON(descriptor, {{"priorityChatEnabled", true}},
                                  {});
    EXPECT_TRUE(descriptor.priorityChatEnabled_);
    SplitDescriptor::loadFromJSON(descriptor, {}, {});
    EXPECT_FALSE(descriptor.priorityChatEnabled_);
}

TEST(PriorityChatWidget,
     AccountChangesInvalidateCachedFollowEligibilityAndStaleResults)
{
    PriorityApplication app;
    Split split(nullptr);
    auto channel =
        std::make_shared<Channel>("follow-cache-test", Channel::Type::None);
    auto m = message();
    channel->addMessage(m, MessageContext::Repost);
    PriorityChat pane(&split, &split);
    pane.setChannel(channel);
    auto &model = PriorityChatTestAccess::model(pane);
    model.rules[0] = {false, 0, false, true, 1, false};
    model.observe(*m);
    auto &p = model.people[personKey(m->platform, m->userID)];
    p.followingKnown = true;
    p.followedAt = QDateTime::currentDateTimeUtc().addMonths(-2);
    pane.setMonitoring(true);
    ASSERT_EQ(PriorityChatTestAccess::view(pane).channel()->countMessages(), 1);
    const auto generation = PriorityChatTestAccess::generation(pane);
    PriorityChatTestAccess::accountChanged(pane);
    PriorityChatTestAccess::finish(pane, generation);
    PriorityChatTestAccess::refresh(pane);
    EXPECT_EQ(PriorityChatTestAccess::view(pane).channel()->countMessages(), 0);
}

TEST(PriorityChatWidget,
     RefilteringPreservesTheScrolledMessageAndItsFractionalOffset)
{
    PriorityApplication app;
    auto channel =
        std::make_shared<Channel>("scroll-test", Channel::Type::None);
    for (int i = 0; i < 100; ++i)
    {
        auto m = message(QString::number(i));
        channel->addMessage(m, MessageContext::Repost);
    }
    bool showAll = false;
    ChannelView view(nullptr);
    view.setMessagePredicate([&](const auto &m) {
        return showAll || m->userID.toInt() >= 20;
    });
    view.setChannel(channel);
    auto &scroll = view.getScrollBar();
    scroll.setDesiredValue(17.25, false);
    ASSERT_FALSE(scroll.isAtBottom());
    const auto before = view.channel()->getMessageSnapshot()[17];
    showAll = true;
    view.refreshMessagePredicate();
    EXPECT_EQ(view.channel()->getMessageSnapshot()[37], before);
    EXPECT_NEAR(scroll.getDesiredValue(), 37.25, 0.01);
    EXPECT_EQ(channel->countMessages(), 100);
    EXPECT_TRUE(QFile::exists(":/settings/filters.svg"));
}

namespace {
// The app sets AA_DontCreateNativeWidgetSiblings on Windows (RunGui.cpp).
// Without it, closing an open settings window while its split is destroyed
// makes Qt recreate native child windows until the stack overflows.
class NativeWidgetsLikeTheApp
{
public:
    NativeWidgetsLikeTheApp()
        : previous_(QCoreApplication::testAttribute(
              Qt::AA_DontCreateNativeWidgetSiblings))
    {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    }
    ~NativeWidgetsLikeTheApp()
    {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings,
                                       previous_);
    }

private:
    bool previous_;
};

class PriorityPaneState : public ::testing::Test
{
public:
    // Declared first so it outlives the widgets below.
    NativeWidgetsLikeTheApp nativeWidgets;
    PriorityApplication app;
    SplitContainer container{nullptr};
    void TearDown() override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    Split *addChat(QString name = "priority-native")
    {
        MergedChannelConfig config;
        config.tabName = name;
        config.twitchEnabled = true;
        config.kickEnabled = false;
        auto *split = new Split(&container);
        split->setChannel(
            IndirectChannel(std::make_shared<MergedChannel>(config)));
        container.insertSplit(split);
        return split;
    }
    Split *pane(Split *owner)
    {
        for (auto *split : container.getSplits())
            if (split->isPriorityChatPane() &&
                split->priorityChatOwnerId() == owner->priorityChatId())
                return split;
        return nullptr;
    }
    /// Types a name into the Users tab and clicks Add.
    static void addOnUsersTab(PriorityChat *settings, const QString &name,
                              int platform = 0)
    {
        settings->findChild<QComboBox *>("PriorityChatAddPlatform")
            ->setCurrentIndex(platform);
        settings->findChild<QLineEdit *>("PriorityChatAddName")->setText(name);
        settings->findChild<QPushButton *>("PriorityChatAddUser")->click();
    }
    static QListWidget *usersList(PriorityChat *settings)
    {
        return settings->findChild<QListWidget *>("PriorityChatUsers");
    }
    static QStringList listed(PriorityChat *settings)
    {
        QStringList names;
        auto *list = usersList(settings);
        for (int i = 0; i < list->count(); ++i)
            if (auto *row = list->itemWidget(list->item(i)))
                names << row->findChild<QLabel *>("PriorityChatUserName")->text();
        return names;
    }
};
}  // namespace

TEST_F(PriorityPaneState, RegularViewersCanOpenAndUsePriorityChat)
{
    auto *owner = addChat();
    const auto merged =
        std::dynamic_pointer_cast<MergedChannel>(owner->getChannel());
    const auto channel =
        std::dynamic_pointer_cast<PriorityRoleChannel>(merged->twitchChannel());
    ASSERT_NE(channel, nullptr);
    channel->moderator = false;
    ASSERT_FALSE(owner->getChannel()->hasModRights());
    PriorityChatTestAccess::refreshAccess(*owner);
    QCoreApplication::processEvents();
    EXPECT_TRUE(owner->canUsePriorityChat());
    auto *icon = owner->findChild<QWidget *>("PriorityChatToggle");
    ASSERT_NE(icon, nullptr);
    EXPECT_FALSE(icon->isHidden());
    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    EXPECT_TRUE(owner->priorityChatRequested());
    auto selected = message("regular-viewer");
    owner->getChannel()->addMessage(selected, MessageContext::Repost);
    owner->setPriorityChatUserChoice(selected, Choice::Include);
    EXPECT_EQ(owner->priorityChatUserChoice(*selected), Choice::Include);
    PriorityChatTestAccess::refresh(*PriorityChatTestAccess::pane(*feed));
    EXPECT_EQ(feed->getChannelView().channel()->getLastMessage(), selected);

    channel->moderator = true;
    PriorityChatTestAccess::refreshAccess(*owner);
    QCoreApplication::processEvents();
    EXPECT_EQ(pane(owner), feed);
    channel->moderator = false;
    channel->broadcaster = true;
    EXPECT_TRUE(owner->canUsePriorityChat());
    channel->broadcaster = false;
    PriorityChatTestAccess::refreshAccess(*owner);
    QCoreApplication::processEvents();
    EXPECT_EQ(pane(owner), feed);
    EXPECT_TRUE(owner->priorityChatEnabled());
    EXPECT_FALSE(icon->isHidden());
    owner->setPriorityChatEnabled(false);
    EXPECT_EQ(pane(owner), nullptr);
    EXPECT_FALSE(owner->priorityChatRequested());
}

TEST_F(PriorityPaneState, RestoredPriorityDoesNotRequireModeratorRights)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    const auto descriptor = container.buildDescriptor();
    const auto merged =
        std::dynamic_pointer_cast<MergedChannel>(owner->getChannel());
    const auto channel =
        std::dynamic_pointer_cast<PriorityRoleChannel>(merged->twitchChannel());
    ASSERT_NE(channel, nullptr);
    channel->moderator = false;
    ASSERT_FALSE(owner->getChannel()->hasModRights());
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(descriptor);
    const auto splits = restored.getSplits();
    ASSERT_EQ(splits.size(), 2);
    auto *restoredOwner = splits[0];
    auto *restoredPane = splits[1];
    EXPECT_TRUE(restoredOwner->canUsePriorityChat());
    EXPECT_TRUE(restoredOwner->priorityChatEnabled());
    EXPECT_TRUE(restoredOwner->priorityChatRequested());
    EXPECT_TRUE(restoredPane->isPriorityChatPane());
    EXPECT_EQ(restoredPane->priorityChatOwner(), restoredOwner);
    PriorityChatTestAccess::refreshAccess(*restoredOwner);
    QCoreApplication::processEvents();
    EXPECT_EQ(restored.getSplits().size(), 2);
    EXPECT_TRUE(restoredOwner->priorityChatEnabled());
    EXPECT_TRUE(QFile::exists(":/buttons/priorityChat.svg"));
}

TEST_F(PriorityPaneState, FeedIsAFullSiblingSplitWithNoEmbeddedSettings)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    EXPECT_EQ(container.getSplits().size(), 2);
    EXPECT_EQ(feed->parentWidget(), owner->parentWidget());
    EXPECT_EQ(feed->getChannelView().parentWidget(), feed);
    EXPECT_FALSE(feed->inputEnabled());
    EXPECT_FALSE(feed->isActivityPane());
    feed->setInputEnabled(true);
    EXPECT_FALSE(feed->inputEnabled());
    EXPECT_EQ(feed->getFilters(), owner->getFilters());
    auto *settings = PriorityChatTestAccess::pane(*feed);
    ASSERT_NE(settings, nullptr);
    EXPECT_EQ(&PriorityChatTestAccess::view(*settings),
              &feed->getChannelView());
    EXPECT_TRUE(settings->isHidden());
    EXPECT_FALSE(feed->findChild<QTabBar *>()->isVisibleTo(feed));
    EXPECT_TRUE(owner->inputEnabled());
    EXPECT_TRUE(owner->priorityChatEnabled());
}

TEST_F(PriorityPaneState, ToggleAndDirectRemovalUseTheActualPaneState)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    owner->setPriorityChatEnabled(true);
    ASSERT_EQ(container.getSplits().size(), 2);
    container.deleteSplit(pane(owner));
    EXPECT_FALSE(owner->priorityChatEnabled());
    owner->normalizePriorityChatPanes();
    EXPECT_EQ(container.getSplits().size(), 1);
    owner->setPriorityChatEnabled(true);
    ASSERT_NE(pane(owner), nullptr);
    owner->setPriorityChatEnabled(false);
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_FALSE(owner->priorityChatEnabled());
}

TEST_F(PriorityPaneState, ActivityAndPriorityHaveIndependentOwnersAndToggles)
{
    auto *owner = addChat();
    owner->openAlertsPane();
    owner->setPriorityChatEnabled(true);
    ASSERT_EQ(container.getSplits().size(), 3);
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    EXPECT_TRUE(owner->priorityChatEnabled());
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 2);
    EXPECT_TRUE(owner->priorityChatEnabled());
    EXPECT_FALSE(owner->hasLinkedActivityPane());
    owner->setPriorityChatEnabled(false);
    EXPECT_EQ(container.getSplits().size(), 1);
}

TEST_F(PriorityPaneState, ClosingSettingsLeavesTheNativeFeedInPlace)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    auto *settings = PriorityChatTestAccess::pane(*feed);
    feed->showSettingsDialog();
    auto *popup = PriorityChatTestAccess::popup(*settings);
    ASSERT_NE(popup, nullptr);
    EXPECT_EQ(feed->getChannelView().parentWidget(), feed);
    EXPECT_EQ(settings->findChild<QTabBar *>()->count(), 2);
    EXPECT_EQ(settings->findChild<QTabBar *>()->tabText(0), "Rules");
    EXPECT_EQ(settings->findChild<QTabBar *>()->tabText(1), "Users");
    popup->close();
    EXPECT_EQ(PriorityChatTestAccess::popup(*settings), nullptr);
    EXPECT_EQ(settings->parentWidget(), feed);
    EXPECT_TRUE(settings->isHidden());
    EXPECT_TRUE(owner->priorityChatEnabled());
    EXPECT_EQ(feed->getChannelView().underlyingChannel(), owner->getChannel());
}

TEST_F(PriorityPaneState, ClosingTheSourceRemovesItsLinkedPriorityPane)
{
    auto *owner = addChat();
    auto *other = addChat("another-chat");
    owner->setPriorityChatEnabled(true);
    other->setPriorityChatEnabled(true);
    container.deleteSplit(owner);
    EXPECT_EQ(container.getSplits().size(), 2);
    EXPECT_TRUE(other->priorityChatEnabled());
    EXPECT_EQ(pane(other)->priorityChatOwner(), other);
}

TEST_F(PriorityPaneState, ChannelFiltersAndTextInsertionRemainLinked)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    auto channel = std::make_shared<PriorityRoleChannel>("updated-source");
    owner->setChannel(IndirectChannel(channel));
    // This test exercises forwarding into an enabled input.
    owner->getInput().setAnonymousState({});
    EXPECT_EQ(feed->getChannel(), channel);
    EXPECT_EQ(feed->getChannelView().underlyingChannel(), channel);
    auto filter =
        std::make_shared<FilterRecord>("priority-filter", "flags.sub_message");
    ASSERT_TRUE(filter->valid());
    getSettings()->filterRecords.append(filter);
    const QList<QUuid> filters{filter->getId()};
    owner->setFilters(filters);
    EXPECT_EQ(feed->getFilters(), filters);
    EXPECT_TRUE(owner->priorityChatEnabled());
    feed->insertTextToInput("hello ");
    EXPECT_EQ(owner->getInput().getInputText(), "hello ");
    EXPECT_EQ(&feed->getInput(), &owner->getInput());
    auto reply = message("reply");
    feed->setInputReply(reply);
    EXPECT_TRUE(owner->getInput().getInputText().startsWith("@viewerreply"));
}

TEST_F(PriorityPaneState, SavedNativeLayoutRestoresExactlyOnePaneAndItsOwner)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    const auto descriptor = container.buildDescriptor();
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(descriptor);
    const auto splits = restored.getSplits();
    ASSERT_EQ(splits.size(), 2);
    auto *restoredOwner = splits[0];
    auto *restoredPane = splits[1];
    EXPECT_TRUE(restoredPane->isPriorityChatPane());
    EXPECT_FALSE(restoredPane->isActivityPane());
    EXPECT_EQ(restoredPane->priorityChatOwner(), restoredOwner);
    EXPECT_TRUE(restoredOwner->priorityChatEnabled());
    restoredOwner->normalizePriorityChatPanes();
    EXPECT_EQ(restored.getSplits().size(), 2);
    restored.deleteSplit(restoredPane);
    EXPECT_FALSE(restoredOwner->priorityChatEnabled());
}

TEST_F(PriorityPaneState,
       OldInlineActivationMigratesToASiblingWithoutDuplicates)
{
    SplitNodeDescriptor old;
    old.type_ = "merged";
    old.mergedTwitchEnabled = true;
    old.channelName_ = "old-priority";
    old.priorityChatEnabled_ = true;
    old.activityPane_ = false;
    container.applyFromDescriptor(old);
    auto splits = container.getSplits();
    ASSERT_EQ(splits.size(), 2);
    EXPECT_TRUE(splits[0]->priorityChatEnabled());
    EXPECT_TRUE(splits[1]->isPriorityChatPane());
    EXPECT_EQ(splits[1]->parentWidget(), splits[0]->parentWidget());
    splits[0]->normalizePriorityChatPanes();
    EXPECT_EQ(container.getSplits().size(), 2);
}

TEST_F(PriorityPaneState, NativeFeedStillBackfillsManualSelections)
{
    auto *owner = addChat();
    auto m = message("native");
    owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatUserChoice(m, Choice::Include);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    auto *settings = PriorityChatTestAccess::pane(*feed);
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(feed->getChannelView().channel()->countMessages(), 1);
    EXPECT_EQ(feed->getChannelView().channel()->getLastMessage(), m);
    EXPECT_EQ(owner->getChannel()->countMessages(), 1);
    owner->setPriorityChatUserChoice(m, Choice::Exclude);
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(feed->getChannelView().channel()->countMessages(), 0);
}

TEST_F(PriorityPaneState, ConvertingASourceToActivityCannotStrandPriority)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    ASSERT_EQ(container.getSplits().size(), 2);
    owner->setInputEnabled(false);
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_EQ(pane(owner), nullptr);
}

TEST_F(PriorityPaneState, SiblingPanesShareTheSameFullHeight)
{
    container.setAttribute(Qt::WA_DontShowOnScreen);
    container.resize(1100, 700);
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    container.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(feed->geometry().top(), owner->geometry().top());
    EXPECT_EQ(feed->height(), owner->height());
    EXPECT_GT(feed->width(), 200);
    EXPECT_GT(owner->width(), feed->width());
    EXPECT_GT(feed->getChannelView().height(),
              owner->getChannelView().height());
    EXPECT_GE(feed->getChannelView().geometry().bottom(), feed->height() - 3);
}

TEST_F(PriorityPaneState, SettingsUseActivityPaletteAndNativeSizedControls)
{
    if (qEnvironmentVariableIsSet("MERGERINO_RENDER_PRIORITY_SETTINGS"))
    {
        // Render with the app's Fusion style and default dark palette.
        QApplication::setStyle(QStyleFactory::create("Fusion"));
        auto dark = QApplication::palette();
        dark.setColor(QPalette::Window, QColor(22, 22, 22));
        dark.setColor(QPalette::WindowText, Qt::white);
        dark.setColor(QPalette::Text, Qt::white);
        dark.setColor(QPalette::Base, QColor("#333"));
        dark.setColor(QPalette::AlternateBase, QColor("#444"));
        dark.setColor(QPalette::Button, QColor(70, 70, 70));
        dark.setColor(QPalette::ButtonText, Qt::white);
        dark.setColor(QPalette::Highlight, QColor(42, 130, 218));
        dark.setColor(QPalette::HighlightedText, Qt::white);
        dark.setColor(QPalette::PlaceholderText, QColor(127, 127, 127));
        for (const auto role :
             {QPalette::ButtonText, QPalette::Text, QPalette::WindowText})
            dark.setColor(QPalette::Disabled, role, QColor(127, 127, 127));
        QApplication::setPalette(dark);
        app.theme.palette = dark;
    }
    // The production palette has explicit roles. Resolve the minimal platform
    // palette too, so a native window caption cannot supply content colors.
    auto palette = app.theme.palette;
    for (int group = 0; group < QPalette::NColorGroups; ++group)
        for (int role = 0; role < QPalette::NColorRoles; ++role)
            if (role != QPalette::NoRole)
                palette.setBrush(static_cast<QPalette::ColorGroup>(group),
                                 static_cast<QPalette::ColorRole>(role),
                                 app.theme.palette.brush(
                                     static_cast<QPalette::ColorGroup>(group),
                                     static_cast<QPalette::ColorRole>(role)));
    app.theme.palette = palette;
    auto *owner = addChat();
    const std::array<std::pair<QString, MessagePlatform>, 4> sample = {{
        {"250scrap", MessagePlatform::AnyOrTwitch},
        {"3binz", MessagePlatform::Kick},
        {"2high4eso", MessagePlatform::YouTube},
        {"angel_is_washed", MessagePlatform::TikTok},
    }};
    for (const auto &[name, platform] : sample)
    {
        auto m = message(name, platform);
        m->loginName = m->displayName = name;
        owner->getChannel()->addMessage(m, MessageContext::Repost);
    }
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    owner->setPriorityChatUserChoice(
        owner->getChannel()->getMessageSnapshot().front(), Choice::Include);
    settings->showSettings();
    auto *popup = PriorityChatTestAccess::popup(*settings);
    SplitSettingsDialog activity(true, true, false, false, false, owner);
    activity.setAttribute(Qt::WA_DontShowOnScreen);
    activity.adjustSize();
    activity.show();
    auto *checkbox =
        settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions");
    auto *activityCheckbox = activity.findChild<QCheckBox *>();
    ASSERT_NE(checkbox, nullptr);
    ASSERT_NE(activityCheckbox, nullptr);
    EXPECT_TRUE(settings->styleSheet().isEmpty());
    EXPECT_TRUE(checkbox->styleSheet().isEmpty());
    for (const auto role : {QPalette::Window, QPalette::WindowText,
                            QPalette::Base, QPalette::Text, QPalette::Button,
                            QPalette::ButtonText, QPalette::Highlight})
        EXPECT_EQ(checkbox->palette().brush(QPalette::Active, role),
                  activityCheckbox->palette().brush(QPalette::Active, role));
    EXPECT_EQ(checkbox->style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr,
                                             checkbox),
              activityCheckbox->style()->pixelMetric(
                  QStyle::PM_IndicatorWidth, nullptr, activityCheckbox));
    QStyleOptionButton option;
    option.initFrom(checkbox);
    EXPECT_GT(
        checkbox->style()
            ->subElementRect(QStyle::SE_CheckBoxIndicator, &option, checkbox)
            .width(),
        8);
    EXPECT_GE(settings->findChildren<QGroupBox *>().size(), 2);
    ASSERT_NE(settings->findChild<QDialogButtonBox *>(), nullptr);
    // Both tabs share one size, so this covers the Users list as well.
    EXPECT_LT(popup->height(), qRound(520 * settings->scale()));
    // Minimal has no system fonts. Allow two native form columns while
    // keeping the dialog narrower than two separate Activity dialogs.
    EXPECT_LT(popup->width(), qRound(activity.width() * 1.9));
    EXPECT_EQ(settings->findChild<QLabel *>("PriorityChatStatus"), nullptr);
    const bool render =
        qEnvironmentVariableIsSet("MERGERINO_RENDER_PRIORITY_SETTINGS");
    if (render)
    {
        EXPECT_TRUE(popup->grab().save("priority-settings.png"));
        EXPECT_TRUE(activity.grab().save("activity-settings.png"));
    }
    // Switching tabs must not resize the window.
    const auto rulesSize = popup->size();
    settings->findChild<QTabBar *>()->setCurrentIndex(1);
    QCoreApplication::processEvents();
    EXPECT_EQ(popup->size(), rulesSize);
    if (render)
        EXPECT_TRUE(popup->grab().save("priority-settings-people.png"));
    settings->findChild<QTabBar *>()->setCurrentIndex(0);
    QCoreApplication::processEvents();
    EXPECT_EQ(popup->size(), rulesSize);
    if (render)
    {
        settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
            ->setChecked(true);
        settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")
            ->setValue(6);
        settings->findChild<QCheckBox *>("PriorityChatTwitchFollows")
            ->setChecked(true);
        settings->findChild<QSpinBox *>("PriorityChatTwitchFollowMonths")
            ->setValue(12);
        settings->findChild<QCheckBox *>("PriorityChatYouTubeSubscriptions")
            ->setChecked(true);
        settings->findChild<QCheckBox *>("PriorityChatTwitchModerators")
            ->setChecked(true);
        settings->findChild<QCheckBox *>("PriorityChatTwitchVips")
            ->setChecked(true);
        QCoreApplication::processEvents();
        EXPECT_TRUE(popup->grab().save("priority-settings-on.png"));
        settings->findChild<QTabBar *>()->setCurrentIndex(1);
        addOnUsersTab(settings, "angel_is_washed", 3);
        addOnUsersTab(settings, "3binz", 1);
        QCoreApplication::processEvents();
        EXPECT_TRUE(popup->grab().save("priority-settings-people-on.png"));
        EXPECT_EQ(popup->size(), rulesSize);
    }
}

TEST_F(PriorityPaneState, AddUserHintFollowsTheSelectedPlatform)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    auto *platform =
        settings->findChild<QComboBox *>("PriorityChatAddPlatform");
    auto *name = settings->findChild<QLineEdit *>("PriorityChatAddName");
    ASSERT_NE(platform, nullptr);
    ASSERT_NE(name, nullptr);
    EXPECT_EQ(platform->parentWidget(), name->parentWidget());
    EXPECT_EQ(name->parentWidget()->objectName(), "PriorityChatUserEntry");
    platform->setCurrentIndex(0);
    EXPECT_EQ(name->placeholderText(), "Twitch username");
    EXPECT_TRUE(platform->toolTip().contains("Twitch"));
    platform->setCurrentIndex(1);
    EXPECT_EQ(name->placeholderText(), "Kick username");
    platform->setCurrentIndex(2);
    EXPECT_TRUE(name->placeholderText().contains("channel ID"));
    platform->setCurrentIndex(3);
    EXPECT_EQ(name->placeholderText(), "TikTok username");
    EXPECT_TRUE(platform->toolTip().contains("TikTok"));
    for (int i = 0; i < platform->count(); ++i)
        if (i != 2)
        {
            platform->setCurrentIndex(i);
            EXPECT_FALSE(
                name->placeholderText().contains("YouTube", Qt::CaseInsensitive));
        }
}

TEST_F(PriorityPaneState, EitherOrBothOnlyAppliesWhenBothRulesAreOn)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    auto *subs =
        settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions");
    auto *follows = settings->findChild<QCheckBox *>("PriorityChatTwitchFollows");
    auto *matching = settings->findChild<QCheckBox *>("PriorityChatTwitchMatching");
    auto *months = settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths");
    ASSERT_NE(matching, nullptr);
    EXPECT_FALSE(matching->isEnabled());
    subs->setChecked(true);
    EXPECT_FALSE(matching->isEnabled());
    // A checked rule always has a usable threshold.
    EXPECT_GE(months->value(), 1);
    follows->setChecked(true);
    EXPECT_TRUE(matching->isEnabled());
    subs->setChecked(false);
    EXPECT_FALSE(matching->isEnabled());
}

TEST_F(PriorityPaneState, CancelDiscardsUnappliedRulesAndManualChoices)
{
    auto *owner = addChat();
    auto m = message("draft");
    owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    const auto preferences = getSettings()->priorityChatPreferences.getValue();
    settings->showSettings();
    EXPECT_EQ(settings->findChild<QLineEdit *>("PriorityChatName"), nullptr);
    settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
        ->setChecked(true);
    settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")->setValue(7);
    addOnUsersTab(settings, "viewerdraft");
    EXPECT_EQ(listed(settings), QStringList{"viewerdraft"});
    EXPECT_EQ(settings->title(), "Priority Chat");
    EXPECT_FALSE(
        PriorityChatTestAccess::model(*settings).rules[0].subscriptions);
    EXPECT_EQ(settings->userChoice(*m), Choice::Automatic);
    EXPECT_EQ(getSettings()->priorityChatPreferences.getValue(), preferences);
    settings->findChild<QDialogButtonBox *>()->rejected();
    EXPECT_EQ(PriorityChatTestAccess::popup(*settings), nullptr);
    EXPECT_EQ(settings->userChoice(*m), Choice::Automatic);
    EXPECT_EQ(getSettings()->priorityChatPreferences.getValue(), preferences);
    settings->showSettings();
    EXPECT_EQ(settings->findChild<QLineEdit *>("PriorityChatName"), nullptr);
    EXPECT_FALSE(
        settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
            ->isChecked());
}

TEST_F(PriorityPaneState, OkAppliesAllPlatformsAndManualChoicesTogether)
{
    auto *owner = addChat();
    auto m = message("commit");
    owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    EXPECT_EQ(settings->findChild<QLineEdit *>("PriorityChatName"), nullptr);
    settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
        ->setChecked(true);
    settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")->setValue(7);
    settings->findChild<QCheckBox *>("PriorityChatKickFollows")
        ->setChecked(true);
    settings->findChild<QSpinBox *>("PriorityChatKickFollowMonths")
        ->setValue(15);
    addOnUsersTab(settings, "viewercommit");
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
    settings->findChild<QDialogButtonBox *>()->accepted();
    EXPECT_EQ(settings->title(), "Priority Chat");
    EXPECT_TRUE(
        PriorityChatTestAccess::model(*settings).rules[0].subscriptions);
    EXPECT_EQ(
        PriorityChatTestAccess::model(*settings).rules[0].subscriptionMonths,
        7);
    EXPECT_TRUE(PriorityChatTestAccess::model(*settings).rules[1].follows);
    EXPECT_EQ(PriorityChatTestAccess::model(*settings).rules[1].followMonths,
              15);
    EXPECT_EQ(settings->userChoice(*m), Choice::Include);
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
}

TEST_F(PriorityPaneState, BotsControlsDefaultOffAndRespectOkAndCancel)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    const QStringList platforms = {"Twitch", "Kick", "YouTube", "TikTok"};
    settings->showSettings();
    for (const auto &platform : platforms)
    {
        auto *box = settings->findChild<QCheckBox *>("PriorityChat" + platform +
                                                     "Bots");
        ASSERT_NE(box, nullptr);
        EXPECT_FALSE(box->isChecked());
        box->setChecked(true);
        auto *accounts = settings->findChild<QLineEdit *>(
            "PriorityChat" + platform + "BotAccounts");
        ASSERT_NE(accounts, nullptr);
        accounts->setText("@custom_bot, another_bot");
    }
    settings->findChild<QDialogButtonBox *>()->rejected();
    settings->showSettings();
    for (const auto &platform : platforms)
    {
        auto *box = settings->findChild<QCheckBox *>("PriorityChat" + platform +
                                                     "Bots");
        EXPECT_FALSE(box->isChecked());
        box->setChecked(true);
        settings
            ->findChild<QLineEdit *>("PriorityChat" + platform + "BotAccounts")
            ->setText("@custom_bot, another_bot");
    }
    settings->findChild<QDialogButtonBox *>()->accepted();
    for (const auto &rule : PriorityChatTestAccess::model(*settings).rules)
    {
        EXPECT_TRUE(rule.bots);
        EXPECT_EQ(rule.botAccounts,
                  (QStringList{"@custom_bot", "another_bot"}));
    }
    settings->showSettings();
    for (const auto &platform : platforms)
        EXPECT_TRUE(
            settings->findChild<QCheckBox *>("PriorityChat" + platform + "Bots")
                ->isChecked());
    settings->findChild<QDialogButtonBox *>()->rejected();
}

TEST_F(PriorityPaneState,
       ManualBotOverrideSurvivesUserCacheRotationAndCanBeRemoved)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto unbadged = message("custom-bot");
    auto badged = message("custom-bot");
    badged->id = "later-bot-message";
    badged->externalBadges = {"frankerfacez:bot"};
    owner->getChannel()->addMessage(unbadged, MessageContext::Repost);
    owner->getChannel()->addMessage(badged, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    auto &model = PriorityChatTestAccess::model(*settings);
    model.rules[0].bots = true;
    PriorityChatTestAccess::save(*settings);
    owner->setPriorityChatUserChoice(unbadged, Choice::Include);
    PriorityChatTestAccess::flush(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 2);
    for (int i = 0; i < 5; ++i)
        owner->getChannel()->addMessage(message("rotate-" + QString::number(i)),
                                        MessageContext::Repost);
    PriorityChatTestAccess::flush(*settings);
    const auto key = personKey(unbadged->platform, unbadged->userID);
    const auto person = model.people.take(key);
    model.rules[0].bots = false;
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 2);
    EXPECT_FALSE(model.people.contains(key));
    model.setChoice(person, Choice::Automatic);
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
}

TEST_F(PriorityPaneState, ManuallyAddingBotOnUsersTabOverridesBotsOff)
{
    auto *owner = addChat();
    auto bot = message("known-bot");
    bot->loginName = bot->displayName = "nightbot";
    owner->getChannel()->addMessage(bot, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    EXPECT_FALSE(PriorityChatTestAccess::model(*settings).rules[0].bots);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
    settings->showSettings();
    addOnUsersTab(settings, "nightbot");
    settings->findChild<QDialogButtonBox *>()->accepted();
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_FALSE(PriorityChatTestAccess::model(*settings).rules[0].bots);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->getLastMessage(),
        bot);
    owner->setPriorityChatEnabled(false);
    owner->setPriorityChatEnabled(true);
    settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_FALSE(PriorityChatTestAccess::model(*settings).rules[0].bots);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->getLastMessage(),
        bot);
}

TEST_F(PriorityPaneState, SettingsPreserveChoicesMadeInChatWhileTheDialogIsOpen)
{
    auto *owner = addChat();
    auto a = message("draft-a"), b = message("external-b");
    owner->getChannel()->addMessage(a, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    addOnUsersTab(settings, "viewerdraft-a");
    owner->setPriorityChatUserChoice(b, Choice::Include);
    settings->findChild<QDialogButtonBox *>()->accepted();
    EXPECT_EQ(settings->userChoice(*a), Choice::Include);
    EXPECT_EQ(settings->userChoice(*b), Choice::Include);
}

TEST_F(PriorityPaneState, UsersTabIsOneListWithRemoveButtons)
{
    auto *owner = addChat();
    auto a = message("alice"), b = message("bob"), c = message("carol");
    for (const auto &m : {a, b, c})
        owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    auto *name = settings->findChild<QLineEdit *>("PriorityChatAddName");
    auto *add = settings->findChild<QPushButton *>("PriorityChatAddUser");
    ASSERT_NE(add, nullptr);
    // Nothing to add until a name is typed.
    EXPECT_FALSE(add->isEnabled());
    EXPECT_TRUE(listed(settings).isEmpty());

    // Names from chat need no lookup, with or without @ and in any case.
    addOnUsersTab(settings, "@VIEWERALICE");
    addOnUsersTab(settings, "viewerbob");
    EXPECT_EQ(listed(settings), (QStringList{"vieweralice", "viewerbob"}));
    EXPECT_TRUE(name->text().isEmpty());

    // Explicit exclusions stay in the same list.
    owner->setPriorityChatUserChoice(c, Choice::Exclude);
    EXPECT_EQ(listed(settings),
              (QStringList{"vieweralice", "viewerbob", "viewercarol"}));

    // The x on a row removes that user.
    auto *list = usersList(settings);
    list->itemWidget(list->item(0))
        ->findChild<QToolButton *>("PriorityChatRemoveUser")
        ->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(listed(settings),
              (QStringList{"viewerbob", "viewercarol"}));
    list->itemWidget(list->item(1))
        ->findChild<QToolButton *>("PriorityChatRemoveUser")
        ->click();
    QCoreApplication::processEvents();
    EXPECT_EQ(listed(settings), QStringList{"viewerbob"});

    // Unknown Twitch names can't be looked up anonymously.
    addOnUsersTab(settings, "nobodyhere");
    EXPECT_EQ(listed(settings), QStringList{"viewerbob"});
    EXPECT_TRUE(settings->findChild<QLabel *>("PriorityChatAddHint")
                    ->text()
                    .contains("nobodyhere"));

    settings->findChild<QDialogButtonBox *>()->accepted();
    EXPECT_EQ(settings->userChoice(*a), Choice::Automatic);
    EXPECT_EQ(settings->userChoice(*b), Choice::Include);
    // Removing the excluded user in the window applies on OK.
    EXPECT_EQ(settings->userChoice(*c), Choice::Automatic);
}

TEST_F(PriorityPaneState, ContextMenuEligibilityUsesRulesAndVisibleUsers)
{
    auto *owner = addChat();
    auto moderator = message("moderator");
    moderator->twitchBadges.emplace_back("moderator", "1");
    auto plain = message("moderator");
    plain->id = "message-without-badge";
    auto other = message("other");
    const auto channel = owner->getChannel();
    for (const auto &m : {moderator, plain, other})
        channel->addMessage(m, MessageContext::Repost);

    owner->setPriorityChatEnabled(true);
    auto *feed = pane(owner);
    ASSERT_NE(feed, nullptr);
    auto *settings = PriorityChatTestAccess::pane(*feed);
    settings->showSettings();
    settings->findChild<QCheckBox *>("PriorityChatTwitchModerators")
        ->setChecked(true);
    settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
        ->setChecked(true);
    settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")->setValue(6);
    settings->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
    PriorityChatTestAccess::refresh(*settings);
    ASSERT_EQ(feed->getChannelView().channel()->countMessages(), 1);

    EXPECT_TRUE(owner->isPriorityChatUserIncluded(*moderator));
    EXPECT_TRUE(feed->isPriorityChatUserIncluded(*moderator));
    EXPECT_TRUE(owner->isPriorityChatUserIncluded(*plain));
    EXPECT_FALSE(owner->isPriorityChatUserIncluded(*other));
    EXPECT_FALSE(owner->isPriorityChatUserIncluded(
        *message("moderator", MessagePlatform::Kick)));

    feed->setPriorityChatUserChoice(moderator, Choice::Exclude);
    EXPECT_FALSE(feed->isPriorityChatUserIncluded(*moderator));
    EXPECT_FALSE(owner->isPriorityChatUserIncluded(*plain));
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(feed->getChannelView().channel()->countMessages(), 0);

    owner->setPriorityChatEnabled(false);
    auto subscriber = message("subscriber");
    subscriber->twitchBadgeInfos["subscriber"] = "12";
    EXPECT_TRUE(owner->isPriorityChatUserIncluded(*subscriber));
}

TEST_F(PriorityPaneState, UserVisibilityOverridesRulesAfterConfirmation)
{
    auto *owner = addChat();
    auto user = message("alice");
    user->twitchBadges.emplace_back("moderator", "1");
    const auto channel = owner->getChannel();
    channel->addMessage(user, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    const auto feedCount = [&settings] {
        return PriorityChatTestAccess::view(*settings).channel()->countMessages();
    };
    const auto toggle = [&settings] {
        auto *list = usersList(settings);
        auto *row = list->itemWidget(list->item(0));
        return row ? row->findChild<QToolButton *>("PriorityChatUserVisibility")
                   : nullptr;
    };
    const auto accept = [&settings] {
        auto *save = settings->findChild<QDialogButtonBox *>()
                         ->button(QDialogButtonBox::Save);
        ASSERT_NE(save, nullptr);
        save->click();
        PriorityChatTestAccess::refresh(*settings);
    };

    settings->showSettings();
    settings->findChild<QCheckBox *>("PriorityChatTwitchModerators")
        ->setChecked(true);
    addOnUsersTab(settings, "vieweralice");
    ASSERT_NE(toggle(), nullptr);
    EXPECT_TRUE(toggle()->isChecked());
    accept();
    EXPECT_EQ(settings->userChoice(*user), Choice::Include);
    ASSERT_EQ(feedCount(), 1);

    settings->showSettings();
    ASSERT_NE(toggle(), nullptr);
    toggle()->click();
    EXPECT_FALSE(toggle()->isChecked());
    EXPECT_EQ(listed(settings), QStringList{"vieweralice"});
    EXPECT_EQ(settings->userChoice(*user), Choice::Include);
    settings->findChild<QDialogButtonBox *>()->rejected();

    settings->showSettings();
    ASSERT_NE(toggle(), nullptr);
    EXPECT_TRUE(toggle()->isChecked());
    toggle()->click();
    accept();
    EXPECT_EQ(settings->userChoice(*user), Choice::Exclude);
    EXPECT_TRUE(PriorityChatTestAccess::model(*settings).rules[0].moderators);
    EXPECT_EQ(feedCount(), 0);
    EXPECT_EQ(channel->countMessages(), 1);

    QFile savedSettings(app.settingsDir.filePath("settings.json"));
    ASSERT_TRUE(savedSettings.open(QIODevice::ReadOnly));
    const auto saved = QJsonDocument::fromJson(savedSettings.readAll()).object();
    savedSettings.close();
    const auto savedPreferences =
        saved.value("priorityChat").toObject().value("preferences").toString();
    EXPECT_EQ(savedPreferences,
              getSettings()->priorityChatPreferences.getValue());
    const auto savedChats =
        QJsonDocument::fromJson(savedPreferences.toUtf8()).object();
    ASSERT_EQ(savedChats.size(), 1);
    const auto savedPeople =
        savedChats.constBegin()->toObject().value("people").toArray();
    ASSERT_EQ(savedPeople.size(), 1);
    EXPECT_EQ(savedPeople.at(0).toObject().value("choice").toString(), "exclude");

    owner->setPriorityChatEnabled(false);
    owner->setPriorityChatEnabled(true);
    ASSERT_NE(pane(owner), nullptr);
    settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    ASSERT_NE(toggle(), nullptr);
    EXPECT_FALSE(toggle()->isChecked());
    toggle()->click();
    accept();
    EXPECT_EQ(settings->userChoice(*user), Choice::Include);
    EXPECT_EQ(feedCount(), 1);
}

TEST_F(PriorityPaneState, CancelledUserLookupCannotModifyAReopenedDialog)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    const auto generation = PriorityChatTestAccess::generation(*settings);
    const auto session = PriorityChatTestAccess::settingsGeneration(*settings);
    settings->findChild<QDialogButtonBox *>()->rejected();
    settings->showSettings();
    Person person;
    person.platform = MessagePlatform::TikTok;
    person.id = person.login = person.name = "lookup-viewer";
    PriorityChatTestAccess::resolvePerson(*settings, generation, session,
                                          person);
    settings->findChild<QDialogButtonBox *>()->accepted();
    EXPECT_EQ(PriorityChatTestAccess::model(*settings).choice(
                  personKey(person.platform, person.id)),
              Choice::Automatic);
}

TEST_F(PriorityPaneState, ChangingTheSourceDiscardsTheOpenSettingsDraft)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    settings->findChild<QCheckBox *>("PriorityChatTwitchSubscriptions")
        ->setChecked(true);
    owner->setChannel(
        std::make_shared<Channel>("different-source", Channel::Type::None));
    EXPECT_EQ(PriorityChatTestAccess::popup(*settings), nullptr);
    EXPECT_EQ(settings->title(), "Priority Chat");
}

TEST_F(PriorityPaneState,
       EveryPlatformIsVisibleWithoutAPlatformSelectorOrTabLine)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    settings->showSettings();
    EXPECT_EQ(settings->findChild<QComboBox *>("PriorityChatPlatform"),
              nullptr);
    EXPECT_FALSE(settings->findChild<QTabBar *>()->drawBase());
    for (const auto &platform : {"Twitch", "Kick", "YouTube", "TikTok"})
    {
        auto *group = settings->findChild<QGroupBox *>(QString("PriorityChat") +
                                                       platform + "Rules");
        ASSERT_NE(group, nullptr);
        EXPECT_TRUE(group->isVisibleTo(settings));
    }
    EXPECT_EQ(settings->findChild<QCheckBox *>("PriorityChatYouTubeFollows"),
              nullptr);
    EXPECT_EQ(
        settings->findChild<QCheckBox *>("PriorityChatTikTokSubscriptions"),
        nullptr);
}

TEST_F(PriorityPaneState, SavedPlatformRulesLoadTogetherAndCancelKeepsThem)
{
    auto *owner = addChat();
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    auto &model = PriorityChatTestAccess::model(*settings);
    model.rules[0] = {true, 6, true, true, 24, true};
    model.rules[1] = {true, 3, false, true, 12, false};
    model.rules[2] = {true, 9, true, false, 0, true};
    PriorityChatTestAccess::save(*settings);
    const auto saved = getSettings()->priorityChatPreferences.getValue();
    settings->showSettings();
    EXPECT_EQ(
        settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")->value(),
        6);
    EXPECT_EQ(
        settings->findChild<QSpinBox *>("PriorityChatKickSubMonths")->value(),
        3);
    EXPECT_EQ(settings->findChild<QSpinBox *>("PriorityChatYouTubeSubMonths")
                  ->value(),
              9);
    EXPECT_EQ(settings->findChild<QSpinBox *>("PriorityChatTwitchFollowMonths")
                  ->value(),
              24);
    EXPECT_EQ(settings->findChild<QSpinBox *>("PriorityChatKickFollowMonths")
                  ->value(),
              12);
    EXPECT_TRUE(
        settings->findChild<QCheckBox *>("PriorityChatYouTubeActiveOnly")
            ->isChecked());
    settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")
        ->setValue(30);
    settings->findChild<QSpinBox *>("PriorityChatKickSubMonths")->setValue(40);
    settings->findChild<QSpinBox *>("PriorityChatYouTubeSubMonths")
        ->setValue(50);
    settings->findChild<QDialogButtonBox *>()->rejected();
    EXPECT_EQ(getSettings()->priorityChatPreferences.getValue(), saved);
    settings->showSettings();
    EXPECT_EQ(
        settings->findChild<QSpinBox *>("PriorityChatTwitchSubMonths")->value(),
        6);
    EXPECT_EQ(
        settings->findChild<QSpinBox *>("PriorityChatKickSubMonths")->value(),
        3);
    EXPECT_EQ(settings->findChild<QSpinBox *>("PriorityChatYouTubeSubMonths")
                  ->value(),
              9);
}

TEST_F(PriorityPaneState,
       ConcurrentPlatformRulesFeedOneChatAndRemainIndependent)
{
    auto *owner = addChat();
    auto twitch = message("twitch", MessagePlatform::AnyOrTwitch);
    twitch->twitchBadgeInfos["subscriber"] = "6";
    auto kick = message("kick", MessagePlatform::Kick);
    kick->membershipMonths = 3;
    auto youtube = message("youtube", MessagePlatform::YouTube);
    youtube->membershipMonths = 12;
    auto tiktok = message("tiktok", MessagePlatform::TikTok);
    for (const auto &m : {twitch, kick, youtube, tiktok})
        owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    owner->setPriorityChatUserChoice(tiktok, Choice::Include);
    settings->showSettings();
    const std::array<QString, 3> platforms = {"Twitch", "Kick", "YouTube"};
    const std::array<int, 3> months = {6, 3, 12};
    for (int i = 0; i < 3; ++i)
    {
        settings
            ->findChild<QCheckBox *>("PriorityChat" + platforms[i] +
                                     "Subscriptions")
            ->setChecked(true);
        settings
            ->findChild<QSpinBox *>("PriorityChat" + platforms[i] + "SubMonths")
            ->setValue(months[i]);
    }
    settings->findChild<QDialogButtonBox *>()->accepted();
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 4);
    settings->showSettings();
    settings->findChild<QCheckBox *>("PriorityChatKickSubscriptions")
        ->setChecked(false);
    settings->findChild<QDialogButtonBox *>()->accepted();
    PriorityChatTestAccess::refresh(*settings);
    const auto visible =
        PriorityChatTestAccess::view(*settings).channel()->getMessageSnapshot();
    ASSERT_EQ(visible.size(), 3);
    EXPECT_EQ(visible[0], twitch);
    EXPECT_EQ(visible[1], youtube);
    EXPECT_EQ(visible[2], tiktok);
    EXPECT_EQ(owner->getChannel()->countMessages(), 4);
}

TEST_F(PriorityPaneState, MainChatRotationDoesNotEvictPriorityHistory)
{
    getSettings()->scrollbackSplitLimit = 5;
    auto *owner = addChat();
    auto a = message("retained-a"), b = message("retained-b");
    owner->getChannel()->addMessage(a, MessageContext::Repost);
    owner->getChannel()->addMessage(b, MessageContext::Repost);
    owner->setPriorityChatUserChoice(a, Choice::Include);
    owner->setPriorityChatUserChoice(b, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 2);
    for (int i = 0; i < 20; ++i)
        owner->getChannel()->addMessage(
            message("ordinary-" + QString::number(i)), MessageContext::Repost);
    ASSERT_EQ(owner->getChannel()->countMessages(), 5);
    ASSERT_EQ(owner->getChannel()->findMessageByID(a->id), nullptr);
    PriorityChatTestAccess::flush(*settings);
    auto &view = PriorityChatTestAccess::view(*settings);
    auto snapshot = view.channel()->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0], a);
    EXPECT_EQ(snapshot[1], b);
    view.refreshMessages();
    EXPECT_EQ(view.channel()->getMessageSnapshot(), snapshot);
    EXPECT_EQ(view.underlyingChannel(), owner->getChannel());
}

TEST_F(PriorityPaneState, OnlyPriorityMessagesConsumeThePriorityHistoryLimit)
{
    getSettings()->scrollbackSplitLimit = 3;
    auto *owner = addChat();
    auto first = message("priority-author");
    owner->getChannel()->addMessage(first, MessageContext::Repost);
    owner->setPriorityChatUserChoice(first, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    for (int i = 0; i < 5; ++i)
    {
        for (int j = 0; j < 8; ++j)
            owner->getChannel()->addMessage(
                message(QString("ordinary-%1-%2").arg(i).arg(j)),
                MessageContext::Repost);
        auto next = message("priority-author");
        next->id = "priority-" + QString::number(i);
        owner->getChannel()->addMessage(next, MessageContext::Repost);
        PriorityChatTestAccess::flush(*settings);
    }
    const auto snapshot =
        PriorityChatTestAccess::view(*settings).channel()->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 3);
    EXPECT_EQ(snapshot[0]->id, "priority-2");
    EXPECT_EQ(snapshot[1]->id, "priority-3");
    EXPECT_EQ(snapshot[2]->id, "priority-4");
}

TEST_F(PriorityPaneState,
       RetainedMessagesRemainSubjectToUserChoicesAndModeration)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto a = message("old-a"), b = message("old-b");
    owner->getChannel()->addMessage(a, MessageContext::Repost);
    owner->getChannel()->addMessage(b, MessageContext::Repost);
    owner->setPriorityChatUserChoice(a, Choice::Include);
    owner->setPriorityChatUserChoice(b, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    for (int i = 0; i < 12; ++i)
        owner->getChannel()->addMessage(message("flood-" + QString::number(i)),
                                        MessageContext::Repost);
    PriorityChatTestAccess::flush(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 2);
    owner->setPriorityChatUserChoice(a, Choice::Exclude);
    PriorityChatTestAccess::refresh(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->getLastMessage(), b);
    b->flags.set(MessageFlag::Disabled);
    Channel::messageFlagsChanged.invoke(owner->getChannel().get(), b);
    PriorityChatTestAccess::flush(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
}

TEST_F(PriorityPaneState,
       RefreshDoesNotDuplicateRetainedMessagesOrMixPlatformIds)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto twitch = message("same-id", MessagePlatform::AnyOrTwitch);
    auto kick = message("same-id", MessagePlatform::Kick);
    owner->getChannel()->addMessage(twitch, MessageContext::Repost);
    owner->setPriorityChatUserChoice(twitch, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    for (int i = 0; i < 4; ++i)
        owner->getChannel()->addMessage(message("rotate-" + QString::number(i)),
                                        MessageContext::Repost);
    owner->getChannel()->addMessage(kick, MessageContext::Repost);
    owner->setPriorityChatUserChoice(kick, Choice::Include);
    for (int i = 0; i < 3; ++i)
        PriorityChatTestAccess::flush(*settings);
    const auto snapshot =
        PriorityChatTestAccess::view(*settings).channel()->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0], twitch);
    EXPECT_EQ(snapshot[1], kick);
    owner->getChannel()->removeMessage(kick);
    PriorityChatTestAccess::flush(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->getLastMessage(),
        twitch);
}

TEST_F(PriorityPaneState,
       MainChatUserCacheRotationCannotRevokeRetainedEligibility)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto subscriber = message("retained-subscriber");
    subscriber->twitchBadgeInfos["subscriber"] = "9";
    owner->getChannel()->addMessage(subscriber, MessageContext::Repost);
    owner->setPriorityChatEnabled(true);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    auto &model = PriorityChatTestAccess::model(*settings);
    model.rules[0] = {true, 6, false, false, 0, false};
    PriorityChatTestAccess::refresh(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    for (int i = 0; i < Model::MAX_PEOPLE + 128; ++i)
    {
        owner->getChannel()->addMessage(
            message("new-viewer-" + QString::number(i)),
            MessageContext::Repost);
        if (i % 64 == 63)
            PriorityChatTestAccess::flush(*settings);
    }
    PriorityChatTestAccess::flush(*settings);
    EXPECT_LE(model.people.size(), Model::MAX_PEOPLE);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->getLastMessage(),
        subscriber);
    owner->setPriorityChatUserChoice(subscriber, Choice::Exclude);
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
}

TEST_F(PriorityPaneState, AReplacementThatNoLongerQualifiesIsNotRetained)
{
    auto *owner = addChat();
    auto original = message("edited-user");
    owner->getChannel()->addMessage(original, MessageContext::Repost);
    owner->setPriorityChatUserChoice(original, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    ASSERT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    auto replacement = message("different-user");
    owner->getChannel()->replaceMessage(original, replacement);
    PriorityChatTestAccess::flush(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
}

TEST_F(PriorityPaneState, RetainedHistoryRefreshKeepsTheScrolledMessageInPlace)
{
    getSettings()->scrollbackSplitLimit = 8;
    auto *owner = addChat();
    auto author = message("scroll-author");
    owner->setPriorityChatUserChoice(author, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    for (int i = 0; i < 8; ++i)
    {
        auto m = message("scroll-author");
        m->id = "scroll-priority-" + QString::number(i);
        owner->getChannel()->addMessage(m, MessageContext::Repost);
    }
    PriorityChatTestAccess::flush(*settings);
    auto &view = PriorityChatTestAccess::view(*settings);
    auto &scroll = view.getScrollBar();
    scroll.setDesiredValue(2.25, false);
    ASSERT_FALSE(scroll.isAtBottom());
    const auto anchor = view.channel()->getMessageSnapshot()[2];
    for (int i = 0; i < 32; ++i)
        owner->getChannel()->addMessage(
            message("scroll-flood-" + QString::number(i)),
            MessageContext::Repost);
    PriorityChatTestAccess::flush(*settings);
    ASSERT_EQ(view.channel()->countMessages(), 8);
    EXPECT_EQ(view.channel()->getMessageSnapshot()[2], anchor);
    EXPECT_NEAR(scroll.getDesiredValue(), 2.25, 0.01);
}

TEST_F(PriorityPaneState, SourceClearAndChannelChangesCannotRestoreOldHistory)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto m = message("clear-author");
    owner->getChannel()->addMessage(m, MessageContext::Repost);
    owner->setPriorityChatUserChoice(m, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    owner->clear();
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 1);
    owner->getChannel()->clearMessages();
    PriorityChatTestAccess::flush(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
    auto next =
        std::make_shared<PriorityRoleChannel>("different-retained-source");
    owner->setChannel(IndirectChannel(next));
    PriorityChatTestAccess::refresh(*settings);
    EXPECT_EQ(
        PriorityChatTestAccess::view(*settings).channel()->countMessages(), 0);
    EXPECT_EQ(PriorityChatTestAccess::view(*settings).underlyingChannel(),
              next);
}

TEST_F(PriorityPaneState, LoadingOlderMessagesPreservesRetainedHistoryOrder)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto retained = message("retained-after-history");
    owner->getChannel()->addMessage(retained, MessageContext::Repost);
    owner->setPriorityChatUserChoice(retained, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    PriorityChatTestAccess::refresh(*settings);
    for (int i = 0; i < 4; ++i)
        owner->getChannel()->addMessage(
            message("history-flood-" + QString::number(i)),
            MessageContext::Repost);
    owner->getChannel()->removeMessage(
        owner->getChannel()->getMessageSnapshot()[0]);
    auto older = message("older-history");
    older->serverReceivedTime = NOW.addSecs(-5);
    owner->setPriorityChatUserChoice(older, Choice::Include);
    owner->getChannel()->addMessagesAtStart({older});
    PriorityChatTestAccess::flush(*settings);
    const auto snapshot =
        PriorityChatTestAccess::view(*settings).channel()->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0], older);
    EXPECT_EQ(snapshot[1], retained);
}

TEST_F(PriorityPaneState, NewlyQualifiedBackfillIsPlacedBeforeRetainedMessages)
{
    getSettings()->scrollbackSplitLimit = 4;
    auto *owner = addChat();
    auto retained = message("existing-retained");
    owner->getChannel()->addMessage(retained, MessageContext::Repost);
    owner->setPriorityChatUserChoice(retained, Choice::Include);
    auto *settings = PriorityChatTestAccess::pane(*pane(owner));
    auto &model = PriorityChatTestAccess::model(*settings);
    model.rules[0] = {true, 6, false, false, 0, false};
    PriorityChatTestAccess::refresh(*settings);
    for (int i = 0; i < 4; ++i)
        owner->getChannel()->addMessage(
            message("unseen-history-flood-" + QString::number(i)),
            MessageContext::Repost);
    owner->getChannel()->removeMessage(
        owner->getChannel()->getMessageSnapshot()[0]);
    auto older = message("newly-qualified-history");
    older->twitchBadgeInfos["subscriber"] = "9";
    older->serverReceivedTime = NOW.addSecs(-5);
    owner->getChannel()->addMessagesAtStart({older});
    PriorityChatTestAccess::flush(*settings);
    const auto snapshot =
        PriorityChatTestAccess::view(*settings).channel()->getMessageSnapshot();
    ASSERT_EQ(snapshot.size(), 2);
    EXPECT_EQ(snapshot[0], older);
    EXPECT_EQ(snapshot[1], retained);
}
