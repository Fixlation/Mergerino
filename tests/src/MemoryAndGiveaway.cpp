#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "MergedChannelTestAccess.hpp"
#include "messages/Emote.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/LimitedQueue.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/WeakMessageIndex.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/BoundedUniqueSet.hpp"
#include "widgets/dialogs/GiveawayDraws.hpp"
#include "widgets/dialogs/GiveawayPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>

using namespace chatterino;

TEST(MemoryRetention, OldMessagesExpireButOpenCardReferencesRemainUsable)
{
    WeakMessageIndex index;
    LimitedQueue<MessagePtr> history(1000);
    MessagePtr openCard;
    std::weak_ptr<const Message> first;
    for (int i = 0; i < 100000; ++i)
    {
        auto message = std::make_shared<Message>();
        message->id = QString::number(i);
        if (i == 0)
            first = message;
        if (i == 1)
            openCard = message;
        index.insert(message->id, message);
        history.pushBack(message);
    }
    EXPECT_TRUE(first.expired());
    EXPECT_EQ(history.size(), 1000);
    EXPECT_LE(index.size(), 1257);
    EXPECT_EQ(index.find("1"), openCard);
    history.clear();
    index.pruneExpired();
    EXPECT_EQ(index.size(), 1);
    openCard.reset();
    index.pruneExpired();
    EXPECT_EQ(index.size(), 0);
}

TEST(MemoryRetention, ReplacementDoesNotOwnPreviousMessage)
{
    WeakMessageIndex index;
    auto old = std::make_shared<Message>();
    auto next = std::make_shared<Message>();
    std::weak_ptr<const Message> weak = old;
    index.insert("id", old);
    index.insert("id", next);
    old.reset();
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(index.find("id"), next);
    index.erase("id");
    EXPECT_FALSE(index.contains("id"));
}

TEST(MemoryRetention, DuplicateWindowIsBoundedAndResets)
{
    BoundedUniqueSet<QString> ids(32768);
    for (int i = 0; i < 100000; ++i)
        EXPECT_TRUE(ids.insert(QString::number(i)));
    EXPECT_EQ(ids.size(), 32768);
    EXPECT_FALSE(ids.contains("0"));
    EXPECT_TRUE(ids.contains("99999"));
    for (int i = 0; i < 10000; ++i)
        EXPECT_FALSE(ids.insert("99999"));
    EXPECT_EQ(ids.size(), 32768);
    EXPECT_TRUE(ids.contains("67232"));
    ids.clear();
    EXPECT_EQ(ids.size(), 0);
    EXPECT_TRUE(ids.insert("99999"));
}

namespace {
class GiveawayApplication : public mock::BaseApplication
{
public:
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }
    BttvEmotes *getBttvEmotes() override
    {
        return &this->bttvEmotes;
    }
    FfzEmotes *getFfzEmotes() override
    {
        return &this->ffzEmotes;
    }
    SeventvEmotes *getSeventvEmotes() override
    {
        return &this->seventvEmotes;
    }

    AccountController accounts;
    BttvEmotes bttvEmotes;
    FfzEmotes ffzEmotes;
    SeventvEmotes seventvEmotes;
};

class GiveawayTest : public ::testing::Test
{
public:
    GiveawayApplication app;
    GiveawayPopup popup;
    std::shared_ptr<Channel> channel =
        std::make_shared<Channel>("test", Channel::Type::None);
    void SetUp() override
    {
#ifdef Q_OS_WIN
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/segoeui.ttf");
#endif
        popup.setContext(channel,
                         {MessagePlatform::AnyOrTwitch, MessagePlatform::Kick,
                          MessagePlatform::YouTube, MessagePlatform::TikTok});
        popup.show();
        QCoreApplication::processEvents();
        for (auto *input : popup.findChildren<QLineEdit *>())
            if (input->placeholderText() == "Entry keyword")
                input->setText("entry");
        button("GiveawayStartButton")->click();
        ASSERT_EQ(button("GiveawayStartButton")->text(), "Stop");
    }
    void TearDown() override
    {
        popup.hide();
    }
    QPushButton *button(const char *name)
    {
        return popup.findChild<QPushButton *>(name);
    }
    QLabel *label(const char *name)
    {
        return popup.findChild<QLabel *>(name);
    }
    // Rolls are animated now; skip straight to the result
    void roll()
    {
        button("GiveawayRollButton")->click();
        if (button("GiveawayRollButton")->text() == "Skip")
            button("GiveawayRollButton")->click();
    }
    void send(QString name, QString body = "entry",
              MessagePlatform platform = MessagePlatform::AnyOrTwitch)
    {
        auto msg = std::make_shared<Message>();
        msg->loginName = name;
        msg->displayName = name;
        msg->messageText = body;
        msg->platform = platform;
        MessagePtr ptr = msg;
        channel->messageAppended.invoke(ptr, std::nullopt);
    }
    void settle()
    {
        QEventLoop loop;
        QTimer::singleShot(120, &loop, &QEventLoop::quit);
        loop.exec();
    }
};
}  // namespace

TEST_F(GiveawayTest, ThousandsOfEntrantsKeepOnlyViewportRowsAndCorrectCount)
{
    for (int i = 9999; i >= 0; --i)
        send(QString("user%1").arg(i, 5, 10, QLatin1Char('0')));
    settle();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "10000 entrants");
    EXPECT_LE(popup.findChildren<QWidget *>("GiveawayEntrantRow").size(), 8);
    auto *scroll = popup.findChild<QScrollArea *>("GiveawayEntrantScrollArea");
    scroll->verticalScrollBar()->setValue(
        scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    bool lastVisible = false;
    for (auto *name : popup.findChildren<QLabel *>("GiveawayEntrantName"))
        if (name->isVisible() && name->text() == "user09999")
            lastVisible = true;
    EXPECT_TRUE(lastVisible);
    EXPECT_LE(popup.findChildren<QWidget *>("GiveawayEntrantRow").size(), 8);
    popup.grab().save("giveaway-10000.png");
}

TEST_F(GiveawayTest, DuplicatesAndRemovedEntrantsCannotReenter)
{
    send("Alice");
    send("ALICE", "entry", MessagePlatform::Kick);
    send("Bob", "entry", MessagePlatform::YouTube);
    send("Carol", "entry", MessagePlatform::TikTok);
    settle();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "3 entrants");
    for (auto *remove :
         popup.findChildren<QToolButton *>("GiveawayRemoveEntrant"))
        if (remove->property("participantKey").toString() == "bob")
            remove->click();
    QCoreApplication::processEvents();
    send("Bob");
    settle();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "2 entrants");
    button("GiveawayStartButton")->click();
    send("NewEntry");
    settle();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "2 entrants");
}

TEST_F(GiveawayTest, PendingEntriesParticipateInImmediateRollAndExclusions)
{
    send("OnlyWinner");
    // The display timer has not fired; closing entries must settle it now.
    button("GiveawayStartButton")->click();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "1 entrant");
    auto *exclude =
        popup.findChild<QCheckBox *>("GiveawayExcludePreviousWinners");
    exclude->setChecked(true);
    roll();
    EXPECT_TRUE(label("GiveawayWinnerLabel")->text().startsWith("OnlyWinner"));
    EXPECT_FALSE(button("GiveawayRollButton")->isEnabled());
    exclude->setChecked(false);
    EXPECT_TRUE(button("GiveawayRollButton")->isEnabled());
    for (int i = 0; i < 1000; ++i)
        send("OnlyWinner", QString("followup%1").arg(i));
    EXPECT_LE(popup.findChild<QTextBrowser *>("GiveawayWinnerMessages")
                  ->document()
                  ->blockCount(),
              40);
}

TEST_F(GiveawayTest, NewRoundRefreshesReusedRowsAndSubscriberWeights)
{
    send("Alice");
    settle();
    button("GiveawayStartButton")->click();
    popup.findChild<QSlider *>("GiveawaySubscriberLuck")->setValue(7);
    button("GiveawayStartButton")->click();
    auto msg = std::make_shared<Message>();
    msg->loginName = "alice";
    msg->displayName = "ALICE";
    msg->messageText = "entry";
    msg->platform = MessagePlatform::Kick;
    msg->twitchBadges.push_back(TwitchBadge("subscriber", "1"));
    MessagePtr ptr = msg;
    channel->messageAppended.invoke(ptr, std::nullopt);
    settle();
    EXPECT_EQ(label("GiveawayEntrantName")->text(), "ALICE");
    EXPECT_EQ(label("GiveawayEntrantEntries")->text(), "7 entries");
    EXPECT_EQ(label("GiveawayEntrantPlatformIcon")->toolTip(), "Kick");
}

TEST(MemoryRetention,
     RealMergedChannelEvictsMessagesAndStillUpdatesRetainedHistory)
{
    mock::BaseApplication app;
    MergedChannelConfig config;
    config.twitchEnabled = false;
    config.kickEnabled = false;
    MergedChannel channel(config);
    const auto source = [](int id) {
        auto msg = std::make_shared<Message>();
        msg->id = QString::number(id);
        msg->loginName = "test";
        msg->messageText = "test";
        return msg;
    };
    auto firstSource = source(0);
    MergedChannelTestAccess::append(channel, firstSource,
                                    MessagePlatform::AnyOrTwitch);
    std::weak_ptr<const Message> firstMirror = channel.getLastMessage();
    for (int i = 1; i < 3000; ++i)
        MergedChannelTestAccess::append(channel, source(i),
                                        MessagePlatform::AnyOrTwitch);
    EXPECT_TRUE(firstMirror.expired());
    EXPECT_EQ(channel.countMessages(), 1000);
    MergedChannelTestAccess::append(channel, firstSource,
                                    MessagePlatform::AnyOrTwitch);
    EXPECT_EQ(channel.getLastMessage()->id, "2999");
    auto old = source(2999);
    auto replacement = source(2999);
    replacement->messageText = "updated";
    MergedChannelTestAccess::replace(channel, old, replacement,
                                     MessagePlatform::AnyOrTwitch);
    EXPECT_EQ(channel.getLastMessage()->messageText, "updated");
    std::weak_ptr<const Message> last = channel.getLastMessage();
    channel.clearMessages();
    EXPECT_TRUE(last.expired());
    for (auto platform : {MessagePlatform::Kick, MessagePlatform::YouTube,
                          MessagePlatform::TikTok})
    {
        MergedChannelTestAccess::append(channel, source(42), platform);
        EXPECT_EQ(channel.getLastMessage()->platform, platform);
        std::weak_ptr<const Message> message = channel.getLastMessage();
        channel.removeMessage(channel.getLastMessage());
        EXPECT_TRUE(message.expired());
    }
}

namespace {
class SlowChatApplication : public mock::BaseApplication
{
public:
    SlowChatApplication()
        : windows(args, paths_, settings, theme, fonts)
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
    mock::MockTwitchIrcServer twitch;
};
}  // namespace

TEST(ActivityRouting, FreshMergedSubscriptionsReachAnInitiallyEmptyView)
{
    SlowChatApplication app;
    auto filter = std::make_shared<FilterRecord>(
        "Mergerino donations + subs",
        "flags.sub_message || flags.elevated_message || flags.cheer_message");
    ASSERT_TRUE(filter->valid());
    app.settings.filterRecords.append(filter);

    MergedChannelConfig config;
    config.twitchEnabled = false;
    config.kickEnabled = false;
    auto merged = std::make_shared<MergedChannel>(config);
    auto source = std::make_shared<Channel>("source", Channel::Type::None);
    MergedChannelTestAccess::connect(*merged, source,
                                     MessagePlatform::AnyOrTwitch);
    ChannelView activity(nullptr);
    activity.setFilters({filter->getId()});
    activity.setChannel(merged);
    ASSERT_EQ(activity.channel()->countMessages(), 0);

    auto ordinary = std::make_shared<Message>();
    ordinary->id = "ordinary";
    ordinary->messageText = "A regular chat message";
    source->addMessage(ordinary, MessageContext::Repost);
    EXPECT_EQ(activity.channel()->countMessages(), 0);

    auto subscription = std::make_shared<Message>();
    subscription->id = "fresh-subscription";
    subscription->flags.set(MessageFlag::System, MessageFlag::Subscription);
    subscription->messageText = "A viewer subscribed with Prime.";
    source->addMessage(subscription, MessageContext::Repost);
    ASSERT_EQ(activity.channel()->countMessages(), 1);
    EXPECT_EQ(activity.channel()->getLastMessage()->id, subscription->id);
    EXPECT_TRUE(activity.channel()->getLastMessage()->flags.has(
        MessageFlag::Subscription));
}

TEST(MemoryRetention, SlowChatOverflowCatchesUpWithoutDroppingEntries)
{
    SlowChatApplication app;
    Split split(nullptr);
    auto channel = std::make_shared<Channel>("test", Channel::Type::None);
    auto &view = split.getChannelView();
    view.setChannel(channel);
    split.setSlowerChatEnabled(true);
    split.setSlowerChatMessagesPerSecond(0.01);
    int delivered = 0;
    QObject::connect(&view, &ChannelView::messageAddedToChannel, &view,
                     [&delivered](const MessagePtr &) {
                         ++delivered;
                     });
    for (int i = 0; i < 3000; ++i)
    {
        auto message = std::make_shared<Message>();
        message->id = QString::number(i);
        message->loginName = "test";
        message->messageText = "test";
        message->serverReceivedTime = QDateTime::currentDateTime();
        MessagePtr ptr = message;
        channel->messageAppended.invoke(ptr, std::nullopt);
    }
    EXPECT_LE(view.pendingSlowChatMessageCount(), 1000);
    EXPECT_EQ(delivered + view.pendingSlowChatMessageCount(), 3000);
    split.setSlowerChatEnabled(false);
    EXPECT_EQ(view.pendingSlowChatMessageCount(), 0);
    EXPECT_EQ(delivered, 3000);
}

TEST_F(GiveawayTest, RepeatedSpamDoesNotAddEntriesOrGrowRowCount)
{
    for (int i = 0; i < 10000; ++i)
        send(QString("spammer%1").arg(i));
    for (int i = 0; i < 500000; ++i)
    {
        send(QString("spammer%1").arg(i % 10000),
             i % 3 == 0 ? "ordinary chat Kappa Kappa" : "entry",
             static_cast<MessagePlatform>(i % 4));
        if (i % 1000 == 999)
            QCoreApplication::processEvents();
    }
    button("GiveawayStartButton")->click();
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "10000 entrants");
    EXPECT_LE(popup.findChildren<QWidget *>("GiveawayEntrantRow").size(), 8);
    roll();
    EXPECT_TRUE(label("GiveawayWinnerLabel")->text().startsWith("spammer"));
    EXPECT_LE(popup.findChild<QTextBrowser *>("GiveawayWinnerMessages")
                  ->document()
                  ->blockCount(),
              40);
}

TEST_F(GiveawayTest, NumberHuntStillChoosesFirstCorrectMessageDuringSpam)
{
    button("GiveawayStartButton")->click();
    button("GiveawayModeRight")->click();
    QLineEdit *target = nullptr;
    for (auto *input : popup.findChildren<QLineEdit *>())
        if (input->isVisible() &&
            input->parentWidget()->inherits("QSpinBox") == false)
            target = input;
    ASSERT_NE(target, nullptr);
    target->setText("42");
    button("GiveawayStartButton")->click();
    ASSERT_EQ(button("GiveawayStartButton")->text(), "Stop");
    for (int i = 0; i < 10000; ++i)
        send(QString("wrong%1").arg(i), "41");
    send("FirstCorrect", "42");
    send("SecondCorrect", "42");
    EXPECT_TRUE(
        label("GiveawayWinnerLabel")->text().startsWith("FirstCorrect"));
    EXPECT_EQ(button("GiveawayStartButton")->text(), "Start number hunt");
}

TEST_F(GiveawayTest, KeywordRollSpinsTheReelAndLandsOnTheDrawnWinner)
{
    send("Alice");
    send("Bob", "entry", MessagePlatform::Kick);
    button("GiveawayStartButton")->click();
    auto *reel = dynamic_cast<GiveawayReel *>(
        popup.findChild<QWidget *>("GiveawayReel"));
    ASSERT_NE(reel, nullptr);
    button("GiveawayRollButton")->click();
    EXPECT_TRUE(reel->spinning());
    EXPECT_TRUE(reel->isVisible());
    EXPECT_EQ(button("GiveawayRollButton")->text(), "Skip");
    EXPECT_FALSE(button("GiveawayStartButton")->isEnabled());
    EXPECT_EQ(label("GiveawayWinnerLabel")->text(), "No winner yet");
    button("GiveawayRollButton")->click();
    EXPECT_FALSE(reel->spinning());
    EXPECT_TRUE(reel->landed());
    const auto winner = label("GiveawayWinnerLabel")->text();
    EXPECT_TRUE(winner.startsWith("Alice") || winner.startsWith("Bob"));
    EXPECT_EQ(button("GiveawayRollButton")->text(), "Roll winner");
}

TEST_F(GiveawayTest, WheelTakesKeywordEntriesAndDropsWinnersAfterTheSpin)
{
    button("GiveawayStartButton")->click();
    button("GiveawayModeWheel")->click();
    button("GiveawayStartButton")->click();
    ASSERT_EQ(button("GiveawayStartButton")->text(), "Stop");
    send("Alice");
    send("Bob", "entry", MessagePlatform::YouTube);
    send("Carol", "entry", MessagePlatform::TikTok);
    send("Carol", "entry");
    settle();
    auto *wheel = dynamic_cast<GiveawayWheel *>(
        popup.findChild<QWidget *>("GiveawayWheel"));
    ASSERT_NE(wheel, nullptr);
    EXPECT_TRUE(wheel->isVisible());
    EXPECT_EQ(wheel->entries().size(), 3);
    EXPECT_EQ(button("GiveawayRollButton")->text(), "Spin wheel");
    button("GiveawayRollButton")->click();
    EXPECT_TRUE(wheel->spinning());
    button("GiveawayRollButton")->click();
    EXPECT_FALSE(wheel->spinning());
    const auto winner = label("GiveawayWinnerLabel")->text();
    EXPECT_FALSE(winner.startsWith("No winner"));
    // The result stays on the wheel until it is clicked away
    EXPECT_EQ(wheel->entries().size(), 3);
    wheel->stop();
    EXPECT_EQ(wheel->entries().size(), 2);
}

TEST_F(GiveawayTest, EmoteGuessNeedsTheExactEmoteOnItsOwn)
{
    auto makeEmote = [](const QString &name) {
        return std::make_shared<const Emote>(Emote{.name = EmoteName{name}});
    };
    auto bttv = std::make_shared<EmoteMap>();
    bttv->emplace(EmoteName{"catJAM"}, makeEmote("catJAM"));
    bttv->emplace(EmoteName{"FeelsBadMan"}, makeEmote("FeelsBadMan"));
    app.bttvEmotes.setEmotes(std::move(bttv));

    button("GiveawayStartButton")->click();
    button("GiveawayModeEmote")->click();
    EXPECT_EQ(label("GiveawayEmoteHintText")->text(), "2 emotes in the pool");
    auto *target = popup.findChild<QLineEdit *>("GiveawayEmoteInput");
    ASSERT_NE(target, nullptr);
    target->setText("catjam");
    EXPECT_FALSE(button("GiveawayStartButton")->isEnabled());
    target->setText("catJAM");
    button("GiveawayEmoteHint")->click();
    button("GiveawayEmoteHint")->click();
    EXPECT_EQ(label("GiveawayEmoteHintText")->text(),
              "Hint: BTTV emote • 6 letters");
    EXPECT_EQ(button("GiveawayEmoteHint")->text(), "Hint 2/3");

    // Hide the target: the name turns into scrambled text and the tile spins
    for (auto *eye : popup.findChildren<QPushButton *>("GiveawayVisibilityButton"))
        if (eye->isVisible())
            eye->click();
    QEventLoop fade;
    QTimer::singleShot(300, &fade, &QEventLoop::quit);
    fade.exec();
    EXPECT_EQ(target->echoMode(), QLineEdit::NoEcho);
    auto *tile = dynamic_cast<GiveawayEmoteTile *>(
        popup.findChild<QWidget *>("GiveawayEmoteTile"));
    ASSERT_NE(tile, nullptr);
    EXPECT_FALSE(tile->revealed());

    button("GiveawayStartButton")->click();
    ASSERT_EQ(button("GiveawayStartButton")->text(), "Stop");
    EXPECT_TRUE(label("GiveawayStatus")->text().endsWith("target: hidden"));

    send("NotAnEmote", "hello");
    send("Shotgun", "FeelsBadMan catJAM");
    send("WrongCase", "catjam");
    send("Wrong", "FeelsBadMan");
    EXPECT_EQ(label("GiveawayWinnerLabel")->text(), "No winner yet");
    send("Winner", QString("catJAM catJAM ") + QString::fromUcs4(U"\U000E0000"));
    send("TooLate", "catJAM");
    EXPECT_TRUE(label("GiveawayWinnerLabel")->text().startsWith("Winner"));
    EXPECT_EQ(button("GiveawayStartButton")->text(), "Start emote hunt");
    EXPECT_EQ(label("GiveawayEntrantCount")->text(), "2 guesses");
    // The hidden reel lands on the guessed emote instead of just appearing
    EXPECT_TRUE(tile->landing());
    EXPECT_FALSE(tile->revealed());
}

namespace {
MessagePtr moderationChatMessage(const QString &id, const QString &login)
{
    MessageBuilder builder;
    builder.emplace<TextElement>("chat message", MessageElementFlag::Text);
    builder->id = id;
    builder->loginName = login;
    builder->messageText = "chat message";
    builder->serverReceivedTime = QDateTime::currentDateTime();
    return builder.release();
}

MessagePtr moderationTimeout(const QString &login, bool eventSub = true)
{
    auto message = std::make_shared<Message>();
    message->loginName = "moderator";
    message->timeoutUser = login;
    message->serverReceivedTime = QDateTime::currentDateTime();
    message->flags.set({MessageFlag::System, MessageFlag::Timeout,
                        MessageFlag::ModerationAction, MessageFlag::DoNotLog});
    message->flags.set(MessageFlag::PubSub, eventSub);
    return message;
}
}  // namespace

TEST(MessageModeration, TimeoutNotifiesCopiesAndSkipsUnrelatedMessages)
{
    mock::BaseApplication app;
    Channel channel("test", Channel::Type::None);
    auto target = moderationChatMessage("target", "target");
    auto unrelated = moderationChatMessage("other", "other");
    auto whisper = moderationChatMessage("whisper", "target");
    whisper->flags.set(MessageFlag::Whisper);
    auto action = moderationChatMessage("action", "target");
    action->flags.set(MessageFlag::ModerationAction);
    for (const auto &message : {target, unrelated, whisper, action})
    {
        channel.addMessage(message, MessageContext::Repost);
    }
    int notifications = 0;
    pajlada::Signals::SignalHolder connections;
    connections.managedConnect(Channel::messageFlagsChanged,
                               [&](Channel *source, const MessagePtr &message) {
                                   if (source == &channel)
                                   {
                                       EXPECT_EQ(message, target);
                                       ++notifications;
                                   }
                               });
    channel.addOrReplaceTimeout(moderationTimeout("target"),
                                QDateTime::currentDateTime());
    EXPECT_TRUE(target->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(target->flags.has(MessageFlag::InvalidReplyTarget));
    EXPECT_FALSE(unrelated->flags.has(MessageFlag::Disabled));
    EXPECT_FALSE(whisper->flags.has(MessageFlag::Disabled));
    EXPECT_FALSE(action->flags.has(MessageFlag::Disabled));
    EXPECT_EQ(notifications, 1);

    // Receiving the corresponding IRC event must not publish redundant updates.
    channel.addOrReplaceTimeout(moderationTimeout("target", false),
                                QDateTime::currentDateTime());
    EXPECT_EQ(notifications, 1);
}

TEST(MessageModeration, ClearChatNotifiesOnlyNewlyDisabledChatMessages)
{
    mock::BaseApplication app;
    Channel channel("test", Channel::Type::None);
    auto target = moderationChatMessage("target", "target");
    auto system = moderationTimeout("target");
    auto whisper = moderationChatMessage("whisper", "target");
    whisper->flags.set(MessageFlag::Whisper);
    for (const auto &message : {target, system, whisper})
    {
        channel.addMessage(message, MessageContext::Repost);
    }
    int notifications = 0;
    pajlada::Signals::SignalHolder connections;
    connections.managedConnect(Channel::messageFlagsChanged,
                               [&](Channel *source, const MessagePtr &message) {
                                   if (source == &channel)
                                   {
                                       EXPECT_EQ(message, target);
                                       ++notifications;
                                   }
                               });
    channel.disableAllMessages();
    EXPECT_TRUE(target->flags.has(MessageFlag::Disabled));
    EXPECT_FALSE(system->flags.has(MessageFlag::Disabled));
    EXPECT_FALSE(whisper->flags.has(MessageFlag::Disabled));
    EXPECT_EQ(notifications, 1);
    channel.disableAllMessages();
    EXPECT_EQ(notifications, 1);
}

TEST(MessageModeration, MergedTimeoutStillWorksAfterHistoryRollover)
{
    mock::BaseApplication app;
    MergedChannelConfig config;
    config.twitchEnabled = false;
    config.kickEnabled = false;
    MergedChannel merged(config);
    auto twitch = std::make_shared<Channel>("twitch", Channel::Type::None);
    auto kick = std::make_shared<Channel>("kick", Channel::Type::None);
    MergedChannelTestAccess::connect(merged, twitch,
                                     MessagePlatform::AnyOrTwitch);
    MergedChannelTestAccess::connect(merged, kick, MessagePlatform::Kick);
    for (int i = 0; i < 10000; ++i)
    {
        twitch->addMessage(moderationChatMessage(QString::number(i), "other"),
                           MessageContext::Repost);
    }
    auto target = moderationChatMessage("same-id", "target");
    twitch->addMessage(target, MessageContext::Repost);
    auto twitchMirror = merged.getLastMessage();
    kick->addMessage(moderationChatMessage("same-id", "target"),
                     MessageContext::Repost);
    auto kickMirror = merged.getLastMessage();
    ASSERT_NE(twitchMirror, target);
    int notifications = 0;
    pajlada::Signals::SignalHolder connections;
    connections.managedConnect(Channel::messageFlagsChanged,
                               [&](Channel *source, const MessagePtr &message) {
                                   if (source == &merged)
                                   {
                                       EXPECT_EQ(message, twitchMirror);
                                       ++notifications;
                                   }
                               });
    twitch->addOrReplaceTimeout(moderationTimeout("target"),
                                QDateTime::currentDateTime());
    EXPECT_TRUE(twitchMirror->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(twitchMirror->flags.has(MessageFlag::InvalidReplyTarget));
    EXPECT_FALSE(kickMirror->flags.has(MessageFlag::Disabled));
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(merged.countMessages(), 1000);
}

TEST(MessageModeration, MergedSingleDeletionPublishesItsOwnFlagUpdate)
{
    mock::BaseApplication app;
    MergedChannelConfig config;
    config.twitchEnabled = false;
    config.kickEnabled = false;
    MergedChannel merged(config);
    auto source = std::make_shared<Channel>("source", Channel::Type::None);
    MergedChannelTestAccess::connect(merged, source, MessagePlatform::Kick);
    source->addMessage(moderationChatMessage("id", "target"),
                       MessageContext::Repost);
    auto mirror = merged.getLastMessage();
    int notifications = 0;
    pajlada::Signals::SignalHolder connections;
    connections.managedConnect(
        Channel::messageFlagsChanged,
        [&](Channel *channel, const MessagePtr &message) {
            if (channel == &merged)
            {
                EXPECT_EQ(message, mirror);
                ++notifications;
            }
        });
    source->disableMessage("id", MessageFlag::InvalidReplyTarget);
    EXPECT_TRUE(mirror->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(mirror->flags.has(MessageFlag::InvalidReplyTarget));
    EXPECT_EQ(notifications, 1);
    source->disableMessage("id", MessageFlag::InvalidReplyTarget);
    EXPECT_EQ(notifications, 1);
}

TEST(MessageModeration, DecoratedViewReceivesDeletionAndTimeoutUpdates)
{
    SlowChatApplication app;
    ChannelView view(nullptr);
    auto source =
        std::make_shared<Channel>("streamdatabase", Channel::Type::Twitch);
    view.setChannel(source);
    auto deleted = moderationChatMessage("deleted", "target");
    source->addMessage(deleted, MessageContext::Repost);
    auto decoratedDeleted =
        view.channel()->findMessageByID(QStringLiteral("deleted"));
    ASSERT_NE(decoratedDeleted, nullptr);
    ASSERT_NE(decoratedDeleted, deleted);
    source->disableMessage("deleted", MessageFlag::InvalidReplyTarget);
    EXPECT_TRUE(decoratedDeleted->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(decoratedDeleted->flags.has(MessageFlag::InvalidReplyTarget));

    source->addMessage(moderationChatMessage("timed-out", "target"),
                       MessageContext::Repost);
    auto decoratedTimeout =
        view.channel()->findMessageByID(QStringLiteral("timed-out"));
    ASSERT_NE(decoratedTimeout, nullptr);
    source->addOrReplaceTimeout(moderationTimeout("target"),
                                QDateTime::currentDateTime());
    EXPECT_TRUE(decoratedTimeout->flags.has(MessageFlag::Disabled));
    EXPECT_TRUE(decoratedTimeout->flags.has(MessageFlag::InvalidReplyTarget));

    // Changing the displayed channel must disconnect its old moderation updates.
    auto next = std::make_shared<Channel>("next", Channel::Type::None);
    view.setChannel(next);
    source->disableAllMessages();
    EXPECT_EQ(view.channel()->countMessages(), 0);
}

TEST(MessageModeration, VisibleViewRefreshesDeletionWithoutNewChatTraffic)
{
    SlowChatApplication app;
    app.settings.hideModerated = true;
    ChannelView view(nullptr);
    auto source = std::make_shared<Channel>("test", Channel::Type::None);
    view.setOverrideFlags(MessageElementFlag::Text);
    view.setChannel(source);
    source->addMessage(moderationChatMessage("deleted", "target"),
                       MessageContext::Repost);
    view.resize(400, 300);
    view.show();
    QCoreApplication::processEvents();
    auto &layouts = view.getMessagesSnapshot();
    auto target =
        std::find_if(layouts.begin(), layouts.end(), [](const auto &row) {
            return row->getMessagePtr()->id == "deleted";
        });
    ASSERT_NE(target, layouts.end());
    auto layout = *target;
    ASSERT_GT(layout->getHeight(), 0);
    const auto generation = app.windows.getGeneration();
    source->disableMessage("deleted", MessageFlag::InvalidReplyTarget);
    QCoreApplication::processEvents();
    EXPECT_EQ(layout->getHeight(), 0);
    EXPECT_EQ(app.windows.getGeneration(), generation);
}

TEST(MessageModeration, HiddenViewAppliesTimeoutWhenShownAgain)
{
    SlowChatApplication app;
    app.settings.hideModerated = true;
    ChannelView view(nullptr);
    auto source = std::make_shared<Channel>("test", Channel::Type::None);
    view.setOverrideFlags(MessageElementFlag::Text);
    view.setChannel(source);
    source->addMessage(moderationChatMessage("hidden", "target"),
                       MessageContext::Repost);
    view.resize(400, 300);
    view.show();
    QCoreApplication::processEvents();
    auto &layouts = view.getMessagesSnapshot();
    auto target =
        std::find_if(layouts.begin(), layouts.end(), [](const auto &row) {
            return row->getMessagePtr()->id == "hidden";
        });
    ASSERT_NE(target, layouts.end());
    auto layout = *target;
    ASSERT_GT(layout->getHeight(), 0);
    view.hide();
    source->addOrReplaceTimeout(moderationTimeout("target"),
                                QDateTime::currentDateTime());
    QCoreApplication::processEvents();
    view.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(layout->getHeight(), 0);
}
