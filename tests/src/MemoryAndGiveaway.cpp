#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/filters/FilterRecord.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "MergedChannelTestAccess.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/layouts/MessageLayout.hpp"
#include "messages/layouts/MessageLayoutContext.hpp"
#include "messages/LimitedQueue.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/WeakMessageIndex.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickUserBlock.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "util/BoundedUniqueSet.hpp"
#include "widgets/dialogs/GiveawayDraws.hpp"
#include "widgets/dialogs/GiveawayPopup.hpp"
#include "widgets/dialogs/SplitSettingsDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Scrollbar.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/settingspages/IgnoresPage.hpp"

#include <QCheckBox>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFontDatabase>
#include <QLabel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QListView>
#include <QItemSelectionModel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <iostream>
#include <thread>

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
class ScopedKickBlockList
{
public:
    ScopedKickBlockList()
        : previous_(pajlada::Settings::Setting<std::vector<QString>>::get(
              KICK_LOCAL_BLOCKED_USERS))
    {
        pajlada::Settings::Setting<std::vector<QString>>::set(
            KICK_LOCAL_BLOCKED_USERS, {});
    }

    ~ScopedKickBlockList()
    {
        pajlada::Settings::Setting<std::vector<QString>>::set(
            KICK_LOCAL_BLOCKED_USERS, this->previous_);
    }

private:
    std::vector<QString> previous_;
};

class SlowChatApplication : public mock::BaseApplication
{
public:
    // A settings folder of its own: the default one is the real profile next
    // to the test binary, and the window manager saves its layout there
    SlowChatApplication()
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
    mock::MockTwitchIrcServer twitch;
};
}  // namespace

namespace {
class ActivityPaneState : public ::testing::Test
{
public:
    SlowChatApplication app;
    SplitContainer container{nullptr};

    void TearDown() override
    {
        // releaseSplit reparents closed panes until deleteLater runs. Destroy
        // them while this fixture's app, theme and image services still live.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    static std::shared_ptr<MergedChannel> channel(QString name, bool joins)
    {
        MergedChannelConfig config;
        config.tabName = name;
        config.twitchEnabled = config.kickEnabled = false;
        config.tiktokShowJoinMessages = joins;
        return std::make_shared<MergedChannel>(config);
    }
    Split *addChat(QString name = "activity-test", bool joins = false)
    {
        auto *split = new Split(&container);
        split->setChannel(IndirectChannel(channel(name, joins)));
        container.insertSplit(split);
        return split;
    }
    Split *addActivity(Split *owner, bool joins = true)
    {
        auto *split = new Split(&container);
        split->setChannel(
            IndirectChannel(channel(owner->getChannel()->getName(), joins)));
        split->setFilters(owner->getFilters());
        split->setInputEnabled(false);
        container.insertSplit(split);
        owner->setFilterActivity(true);
        return split;
    }
    TabDescriptor savedTab()
    {
        QJsonArray items;
        for (auto *split : container.getSplits())
        {
            QJsonObject data;
            QJsonArray filters;
            WindowManager::encodeChannel(split->getIndirectChannel(), data);
            WindowManager::encodeFilters(split, filters);
            items.append(QJsonObject{
                {"type", "split"},
                {"data", data},
                {"activityPane", split->isActivityPane()},
                {"inputEnabled", split->inputEnabled()},
                {"filterActivity", split->filterActivity()},
                {"filterActivityExplicit", split->filterActivityExplicit()},
                {"activityMessageScale", split->activityMessageScale()},
                {"showRaids", split->showRaids()},
                {"showWatchStreaks", split->showWatchStreaks()},
                {"filters", filters}});
        }
        return TabDescriptor::loadFromJSON(
            QJsonObject{{"splits2", QJsonObject{{"type", "horizontal"},
                                                {"items", items}}}});
    }
};
}  // namespace

TEST_F(ActivityPaneState,
       DisplayPreferencesCannotDisconnectTheIconOrCreateAnotherPane)
{
    auto *owner = addChat();
    addActivity(owner, true);
    ASSERT_EQ(container.getSplits().size(), 2);
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_FALSE(owner->hasLinkedActivityPane());
    EXPECT_FALSE(owner->filterActivity());
    for (int i = 0; i < 3; ++i)
    {
        owner->openAlertsPane();
        EXPECT_EQ(container.getSplits().size(), 2);
        EXPECT_TRUE(owner->hasLinkedActivityPane());
        owner->openAlertsPane();
        EXPECT_EQ(container.getSplits().size(), 1);
        EXPECT_FALSE(owner->hasLinkedActivityPane());
    }
}

TEST_F(ActivityPaneState, ToggleOffRemovesEveryDuplicateAndPreservesOtherChats)
{
    auto *owner = addChat();
    addActivity(owner, false);
    addActivity(owner, true);
    auto *other = addChat("another-chat");
    auto *otherActivity = addActivity(other, false);
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 3);
    EXPECT_FALSE(owner->hasLinkedActivityPane());
    EXPECT_TRUE(other->hasLinkedActivityPane());
    const auto splits = container.getSplits();
    EXPECT_NE(std::find(splits.begin(), splits.end(), otherActivity),
              splits.end());
}

TEST_F(ActivityPaneState, RestoreReconcilesDuplicatesAndKeepsPanePreferences)
{
    auto *owner = addChat();
    auto *first = addActivity(owner, false);
    first->setActivityMessageScale(0.85);
    addActivity(owner, true);
    const auto tab = savedTab();
    ASSERT_TRUE(tab.rootNode_);
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(*tab.rootNode_);
    const auto splits = restored.getSplits();
    ASSERT_EQ(splits.size(), 2);
    EXPECT_FALSE(splits[0]->isActivityPane());
    EXPECT_TRUE(splits[0]->hasLinkedActivityPane());
    EXPECT_TRUE(splits[1]->isActivityPane());
    EXPECT_DOUBLE_EQ(splits[1]->activityMessageScale(), 0.85);
    splits[0]->openAlertsPane();
    EXPECT_EQ(restored.getSplits().size(), 1);
    EXPECT_FALSE(splits[0]->hasLinkedActivityPane());
}

TEST_F(ActivityPaneState, IndependentEventChoicesStayLinkedAfterRestoreAndReopen)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner);
    pane->setShowRaids(false);
    pane->setShowWatchStreaks(true);
    const auto saved = savedTab();
    ASSERT_TRUE(saved.rootNode_);
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(*saved.rootNode_);
    const auto splits = restored.getSplits();
    ASSERT_EQ(splits.size(), 2);
    for (auto *split : splits)
    {
        EXPECT_FALSE(split->showRaids());
        EXPECT_TRUE(split->showWatchStreaks());
    }
    auto *restoredOwner = splits[0];
    restoredOwner->openAlertsPane();
    ASSERT_EQ(restored.getSplits().size(), 1);
    restoredOwner->openAlertsPane();
    ASSERT_EQ(restored.getSplits().size(), 2);
    for (auto *split : restored.getSplits())
    {
        EXPECT_FALSE(split->showRaids());
        EXPECT_TRUE(split->showWatchStreaks());
    }
}

TEST_F(ActivityPaneState, ClosingThePaneDirectlyImmediatelyDeactivatesItsOwner)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner, false);
    ASSERT_TRUE(owner->hasLinkedActivityPane());
    container.deleteSplit(pane);
    EXPECT_FALSE(owner->hasLinkedActivityPane());
    EXPECT_FALSE(owner->filterActivity());
}

TEST_F(ActivityPaneState, SourceAndFilterChangesKeepThePaneAttached)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner, false);
    ASSERT_TRUE(owner->hasLinkedActivityPane());
    owner->setChannel(IndirectChannel(channel("new-activity-chat", false)));
    EXPECT_EQ(pane->getChannel(), owner->getChannel());
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    auto filter = std::make_shared<FilterRecord>("activity-test-filter",
                                                 "flags.sub_message");
    ASSERT_TRUE(filter->valid());
    getSettings()->filterRecords.append(filter);
    owner->setFilters({filter->getId()});
    EXPECT_TRUE(pane->getFilters().contains(filter->getId()));
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 1);
}

TEST_F(ActivityPaneState, ProviderResolutionCannotDetachAnAlreadyLinkedPane)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner, false);
    auto *sourceChannel =
        dynamic_cast<MergedChannel *>(owner->getChannel().get());
    auto *paneChannel = dynamic_cast<MergedChannel *>(pane->getChannel().get());
    MergedChannelTestAccess::resolveTikTokSource(
        *sourceChannel, "https://www.tiktok.com/@activity_test/live");
    MergedChannelTestAccess::resolveTikTokSource(
        *paneChannel, "https://www.tiktok.com/@activity_test/live");
    ASSERT_TRUE(owner->hasLinkedActivityPane());
    MergedChannelTestAccess::resolveTikTokSource(*sourceChannel,
                                                 "@resolved_activity_test");
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 1);
}

TEST_F(ActivityPaneState, AnEmptySourceCanStillSwitchItsExistingPaneOff)
{
    auto *owner = addChat();
    addActivity(owner, false);
    ASSERT_TRUE(owner->hasLinkedActivityPane());
    owner->setChannel(IndirectChannel(Channel::getEmpty()));
    EXPECT_TRUE(owner->hasLinkedActivityPane());
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_FALSE(owner->hasLinkedActivityPane());
}

TEST_F(ActivityPaneState, RemovingTheOwnerCannotLeaveAnOrphanActivityPane)
{
    auto *owner = addChat();
    addActivity(owner, false);
    addActivity(owner, true);
    auto *other = addChat("unrelated-chat");
    container.deleteSplit(owner);
    const auto splits = container.getSplits();
    ASSERT_EQ(splits.size(), 1);
    EXPECT_EQ(splits.front(), other);
}

TEST_F(ActivityPaneState, ThePaneCloseToggleUsesTheSameOwnerState)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner, true);
    addActivity(owner, false);
    pane->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_FALSE(owner->hasLinkedActivityPane());
    EXPECT_FALSE(owner->filterActivity());
}

TEST_F(ActivityPaneState, RestoreKeepsALinkWhenThePaneHasAdditionalFilters)
{
    auto *owner = addChat();
    auto *pane = addActivity(owner, true);
    auto filter = std::make_shared<FilterRecord>("extra-activity-filter",
                                                 "flags.sub_message");
    ASSERT_TRUE(filter->valid());
    getSettings()->filterRecords.append(filter);
    auto filters = pane->getFilters();
    filters.append(filter->getId());
    pane->setFilters(filters);
    const auto tab = savedTab();
    ASSERT_TRUE(tab.rootNode_);
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(*tab.rootNode_);
    const auto splits = restored.getSplits();
    ASSERT_EQ(splits.size(), 2);
    EXPECT_TRUE(splits[0]->hasLinkedActivityPane());
    EXPECT_TRUE(splits[1]->getFilters().contains(filter->getId()));
    splits[0]->openAlertsPane();
    EXPECT_EQ(restored.getSplits().size(), 1);
}

TEST_F(ActivityPaneState, ClosingDuringAQueuedWatchingChangeCancelsTheRemovedPaneCallback)
{
    auto *owner = addChat();
    owner->setChannel(app.twitch.getWatchingChannel());
    owner->openAlertsPane();
    const auto splits = container.getSplits();
    ASSERT_EQ(splits.size(), 2);
    auto *pane = splits.back();
    QPointer<Split> weakPane(pane);
    app.twitch.setWatchingChannel(
        std::make_shared<Channel>("watching-next", Channel::Type::Twitch));
    container.deleteSplit(pane);
    QCoreApplication::sendPostedEvents(pane, QEvent::DeferredDelete);
    ASSERT_TRUE(weakPane.isNull());
    QCoreApplication::processEvents();
    EXPECT_EQ(owner->getChannel()->getName(), "watching-next");
    EXPECT_FALSE(owner->hasLinkedActivityPane());
}

namespace chatterino {
class ChannelViewTestAccess
{
public:
    static void shift(ChannelView &view, const MessageLayoutPtr &layout)
    {
        view.startOrUpdateMessageLayoutShiftAnimation(layout, 10, 100);
    }
    static std::optional<qreal> position(ChannelView &view,
                                         const MessageLayoutPtr &layout)
    {
        return view.messageLayoutShiftAnimationY(layout);
    }
    static void scroll(ChannelView &view)
    {
        view.performLayout(true);
    }
    static void reflow(ChannelView &view)
    {
        view.performLayout();
    }
    static size_t shifts(ChannelView &view)
    {
        return view.messageLayoutShiftAnimations_.size();
    }
    static void arrival(ChannelView &view, const MessageLayoutPtr &layout)
    {
        view.addMessageArrivalAnimation(layout);
    }
    static bool hasArrival(ChannelView &view, const MessageLayoutPtr &layout)
    {
        return view.messageArrivalAnimationProgress(layout).has_value();
    }
    static void cacheAtWidth(ChannelView &view,
                             const std::vector<MessageLayoutPtr> &messages,
                             int width)
    {
        for (const auto &message : messages)
            message->layout({.messageColors = view.effectiveMessageColors(),
                             .flags = view.getFlags(),
                             .width = width,
                             .scale = view.activityMessageScale(),
                             .imageScale = view.activityMessageImageScale()},
                            false);
    }
    static void layoutVisible(ChannelView &view,
                              const std::vector<MessageLayoutPtr> &messages)
    {
        view.layoutVisibleMessages(messages);
    }
    static void hide(ChannelView &view)
    {
        view.hideEvent(nullptr);
    }
    static bool includes(ChannelView &view, const MessagePtr &message)
    {
        return view.shouldIncludeMessage(message);
    }
    static bool activityIncludes(ChannelView &view, const MessagePtr &message)
    {
        int recipients = 0;
        bool announcement = false;
        return view.transformActivityMessage(message, recipients, announcement)
            .has_value();
    }
};
}  // namespace chatterino

TEST(ActivitySettings, SeparateControlsDefaultToRaidsOnAndWatchStreaksOff)
{
    SlowChatApplication app;
    Split split(nullptr);
    EXPECT_TRUE(split.showRaids());
    EXPECT_FALSE(split.showWatchStreaks());
    SplitSettingsDialog dialog(true, false, false, false, false);
    EXPECT_TRUE(dialog.showRaids());
    EXPECT_FALSE(dialog.showWatchStreaks());
    for (const bool raids : {false, true})
    {
        for (const bool streaks : {false, true})
        {
            dialog.setShowRaids(raids);
            dialog.setShowWatchStreaks(streaks);
            EXPECT_EQ(dialog.showRaids(), raids);
            EXPECT_EQ(dialog.showWatchStreaks(), streaks);
        }
    }
    EXPECT_FALSE(dialog.isVisible());
}

TEST(ActivitySettings, MainChatAndActivityRouteEachEventIndependently)
{
    SlowChatApplication app;
    Split main(nullptr);
    Split activity(nullptr);
    main.setFilterActivity(true);
    activity.setInputEnabled(false);
    for (const bool raids : {false, true})
    {
        for (const bool streaks : {false, true})
        {
            main.setShowRaids(raids);
            main.setShowWatchStreaks(streaks);
            activity.setShowRaids(raids);
            activity.setShowWatchStreaks(streaks);
            for (const auto flag : {MessageFlag::Raid, MessageFlag::WatchStreak})
            {
                auto message = std::make_shared<Message>();
                message->id = "event";
                message->flags.set(MessageFlag::System, flag);
                message->serverReceivedTime = QDateTime::currentDateTime();
                message->messageText = "An activity event";
                const bool inActivity =
                    flag == MessageFlag::Raid ? raids : streaks;
                EXPECT_EQ(ChannelViewTestAccess::includes(main.getChannelView(),
                                                          message),
                          !inActivity);
                EXPECT_EQ(ChannelViewTestAccess::activityIncludes(
                              activity.getChannelView(), message),
                          inActivity);
            }
        }
    }
}

TEST(ActivitySettings, EventChoicesSurviveRealLayoutSavingAndCloning)
{
    SlowChatApplication app;
    Notebook notebook(nullptr);
    auto *container = new SplitContainer(&notebook);
    container->setTab(notebook.addPage(container));
    auto *source = new Split(container);
    source->setChannel(
        IndirectChannel(ActivityPaneState::channel("events", false)));
    container->insertSplit(source);

    const auto saveAndRestore = [&] {
        QJsonObject saved;
        WindowManager::encodeTab(container, false, saved);
        const auto tab = TabDescriptor::loadFromJSON(saved);
        ASSERT_TRUE(tab.rootNode_);
        SplitContainer restored(nullptr);
        restored.applyFromDescriptor(*tab.rootNode_);
        const auto originals = container->getSplits();
        const auto copies = restored.getSplits();
        ASSERT_EQ(copies.size(), originals.size());
        for (size_t i = 0; i < originals.size(); ++i)
        {
            EXPECT_EQ(copies[i]->showRaids(), originals[i]->showRaids());
            EXPECT_EQ(copies[i]->showWatchStreaks(),
                      originals[i]->showWatchStreaks());
        }
        EXPECT_FALSE(QJsonDocument(saved).toJson().contains(
            "showRaidsAndWatchStreaks"));
    };
    for (const bool raids : {false, true})
    {
        for (const bool streaks : {false, true})
        {
            source->setShowRaids(raids);
            source->setShowWatchStreaks(streaks);
            saveAndRestore();
            auto *clone = container->cloneSplit(source, source->getFilters());
            ASSERT_TRUE(clone);
            EXPECT_EQ(clone->showRaids(), raids);
            EXPECT_EQ(clone->showWatchStreaks(), streaks);
            clone->setShowRaids(!raids);
            clone->setShowWatchStreaks(!streaks);
            saveAndRestore();
            container->deleteSplit(clone);
            QCoreApplication::sendPostedEvents(clone, QEvent::DeferredDelete);
        }
    }
}

TEST(ChatRendering, UnchangedSnapshotsReuseStorageAndMutationsRefreshIt)
{
    SlowChatApplication app;
    ChannelView view(nullptr);
    auto channel = std::make_shared<Channel>("render-test", Channel::Type::None);
    view.setChannel(channel);
    auto first = std::make_shared<Message>();
    first->id = "first";
    channel->addMessage(first, MessageContext::Repost);
    auto &snapshot = view.getMessagesSnapshot();
    ASSERT_FALSE(snapshot.empty());
    const auto initialSize = snapshot.size();
    const auto *storage = snapshot.data();
    for (int i = 0; i < 1000; ++i)
    {
        EXPECT_EQ(view.getMessagesSnapshot().data(), storage);
    }
    auto second = std::make_shared<Message>();
    second->id = "second";
    channel->addMessage(second, MessageContext::Repost);
    EXPECT_EQ(view.getMessagesSnapshot().size(), initialSize + 1);
    EXPECT_EQ(view.getMessagesSnapshot().back()->getMessagePtr(), second);
    channel->removeMessage(second);
    EXPECT_EQ(view.getMessagesSnapshot().size(), initialSize);
    view.clearMessages();
    EXPECT_TRUE(view.getMessagesSnapshot().empty());
}

TEST(ChatRendering, ScrollingAndHidingDiscardOldViewportAnimationCoordinates)
{
    SlowChatApplication app;
    ChannelView view(nullptr);
    auto layout = std::make_shared<MessageLayout>(std::make_shared<Message>());
    ChannelViewTestAccess::shift(view, layout);
    ASSERT_TRUE(ChannelViewTestAccess::position(view, layout).has_value());
    ChannelViewTestAccess::scroll(view);
    EXPECT_FALSE(ChannelViewTestAccess::position(view, layout).has_value());
    ChannelViewTestAccess::shift(view, layout);
    ChannelViewTestAccess::hide(view);
    EXPECT_FALSE(ChannelViewTestAccess::position(view, layout).has_value());
}

TEST(ChatRendering, HeightChangesCannotAnimateFollowingRowsThroughEachOther)
{
    for (const bool atBottom : {false, true})
    {
        for (const bool highlighted : {false, true})
        {
            SCOPED_TRACE(atBottom);
            SCOPED_TRACE(highlighted);
            SlowChatApplication app;
            app.settings.messageAnimations = true;
            app.settings.enableSmoothScrollingNewMessages = false;
            Split split(nullptr);
            split.setAttribute(Qt::WA_DontShowOnScreen);
            split.resize(400, 400);
            auto channel =
                std::make_shared<Channel>("reflow", Channel::Type::None);
            split.setChannel(IndirectChannel(channel));
            const auto text =
                QString("A message that wraps across lines. ").repeated(5);
            for (int i = 0; i < 40; ++i)
            {
                MessageBuilder builder;
                builder->id = QString::number(i);
                builder->messageText = text;
                builder->platformAccentColor = QColor(120, 70, 180, 70);
                if (highlighted)
                {
                    builder->flags.set(MessageFlag::Highlighted);
                    builder->highlightColor =
                        std::make_shared<QColor>(120, 70, 180, 70);
                }
                builder.emplace<TextElement>(text, MessageElementFlag::Text);
                channel->addMessage(builder.release(), MessageContext::Repost);
            }
            split.show();  // WA_DontShowOnScreen: no desktop window or focus.
            auto &view = split.getChannelView();
            ASSERT_TRUE(view.isVisible());
            view.resize(350, 250);
            ChannelViewTestAccess::scroll(view);
            auto *scroll = view.scrollbar();
            scroll->setDesiredValue(atBottom ? scroll->getBottom() : 0, false);
            const auto &messages = view.getMessagesSnapshot();
            ChannelViewTestAccess::cacheAtWidth(view, messages, 4000);
            const auto before = messages[static_cast<size_t>(
                scroll->getRelativeCurrentValue())]->getHeight();
            ChannelViewTestAccess::reflow(view);
            const auto start =
                static_cast<size_t>(scroll->getRelativeCurrentValue());
            ASSERT_LT(start, messages.size());
            ASSERT_GT(messages[start]->getHeight(), before);
            EXPECT_EQ(ChannelViewTestAccess::shifts(view), 0);
        }
    }
}

TEST(ChatRendering, FractionalScrollingLaysOutEveryRowThatWillBePainted)
{
    SlowChatApplication app;
    ChannelView view(nullptr);
    view.resize(300, 200);
    std::vector<MessageLayoutPtr> messages;
    for (int i = 0; i < 30; ++i)
    {
        MessageBuilder builder;
        const auto text = i == 0
                              ? QString("A wrapping first row. ").repeated(8)
                              : QString("A following row.");
        builder.emplace<TextElement>(text, MessageElementFlag::Text);
        messages.push_back(std::make_shared<MessageLayout>(builder.release()));
    }
    view.scrollbar()->setMaximum(messages.size());
    view.scrollbar()->setPageSize(1);
    view.scrollbar()->setCurrentValue(0.9);
    ChannelViewTestAccess::cacheAtWidth(view, {messages.front()}, 4000);
    ChannelViewTestAccess::layoutVisible(view, messages);
    qreal y = -messages.front()->getHeight() * 0.9;
    for (const auto &message : messages)
    {
        if (y > view.height())
            break;
        EXPECT_GT(message->getHeight(), 0);
        y += message->getHeight();
    }
    EXPECT_GT(y, view.height());
}

TEST(ChatRendering, ScrollingIntoHistoryCancelsPendingArrivalOffsets)
{
    SlowChatApplication app;
    Split split(nullptr);
    auto &view = split.getChannelView();
    view.scrollbar()->setMaximum(100);
    view.scrollbar()->setPageSize(10);
    view.scrollbar()->setDesiredValue(5, false);
    auto layout = std::make_shared<MessageLayout>(std::make_shared<Message>());
    ChannelViewTestAccess::arrival(view, layout);
    ASSERT_TRUE(ChannelViewTestAccess::hasArrival(view, layout));
    ChannelViewTestAccess::scroll(view);
    EXPECT_FALSE(ChannelViewTestAccess::hasArrival(view, layout));
}

TEST(ImageCleanup, ExpiredRegistryEntriesAreSafeToClear)
{
    SlowChatApplication app;
    auto &pool = ImageExpirationPool::instance();
    auto image = Image::fromUrl(Url{"https://example.invalid/expired-image"});
    const auto raw = image.get();
    std::weak_ptr<Image> weak = image;
    image.reset();
    ASSERT_TRUE(weak.expired());
    {
        std::lock_guard lock(pool.mutex_);
        pool.allImages_.emplace(raw, weak);
    }
    pool.freeAll();
    std::lock_guard lock(pool.mutex_);
    EXPECT_FALSE(pool.allImages_.contains(raw));
}

TEST(ImageCleanup, ConcurrentFinalOwnersCanReleaseDuringCollection)
{
    SlowChatApplication app;
    auto &pool = ImageExpirationPool::instance();
    for (int round = 0; round < 8; ++round)
    {
        std::vector<ImagePtr> images;
        std::vector<std::weak_ptr<Image>> weak;
        for (int i = 0; i < 2000; ++i)
        {
            auto image = Image::fromUrl(Url{
                QString("https://example.invalid/cleanup-%1").arg(i)});
            pool.addImagePtr(image);
            weak.push_back(image);
            images.push_back(std::move(image));
        }
        std::thread worker([images = std::move(images)]() mutable {
            images.clear();
        });
        for (int i = 0; i < 8; ++i)
        {
            pool.freeOld();
        }
        worker.join();
        for (const auto &image : weak)
        {
            EXPECT_TRUE(image.expired());
        }
        QCoreApplication::processEvents();
    }
}

TEST(KickBlockedAccountsSettings, ShowsLocalBlocksAndUnblocksSelectedAccounts)
{
    SlowChatApplication app;
    ScopedKickBlockList blocks;
    setKickUserLocallyBlocked("alpha_viewer", true);
    setKickUserLocallyBlocked("beta_viewer", true);
    setKickUserLocallyBlocked("gamma_viewer", true);
    IgnoresPage page;
    auto *list = page.findChild<QListView *>("kickBlockedAccountsList");
    auto *unblock = page.findChild<QPushButton *>("unblockKickAccountsButton");
    ASSERT_NE(list, nullptr);
    ASSERT_NE(unblock, nullptr);
    ASSERT_EQ(list->model()->rowCount(), 3);
    EXPECT_FALSE(unblock->isEnabled());
    EXPECT_EQ(list->editTriggers(), QAbstractItemView::NoEditTriggers);
    list->selectionModel()->select(
        list->model()->index(0, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    list->selectionModel()->select(
        list->model()->index(1, 0),
        QItemSelectionModel::Select | QItemSelectionModel::Rows);
    EXPECT_TRUE(unblock->isEnabled());
    unblock->click();
    EXPECT_FALSE(isKickUserLocallyBlocked("alpha_viewer"));
    EXPECT_FALSE(isKickUserLocallyBlocked("beta_viewer"));
    EXPECT_TRUE(isKickUserLocallyBlocked("gamma_viewer"));
    ASSERT_EQ(list->model()->rowCount(), 1);
    EXPECT_EQ(list->model()->index(0, 0).data().toString(), "gamma_viewer");
    EXPECT_FALSE(unblock->isEnabled());
}

TEST(KickBlockedAccountsSettings, UpdatesWhenUsercardChangesLocalBlocks)
{
    SlowChatApplication app;
    ScopedKickBlockList blocks;
    IgnoresPage page;
    auto *list = page.findChild<QListView *>("kickBlockedAccountsList");
    auto *unblock = page.findChild<QPushButton *>("unblockKickAccountsButton");
    ASSERT_NE(list, nullptr);
    ASSERT_NE(unblock, nullptr);
    EXPECT_EQ(list->model()->rowCount(), 0);
    EXPECT_FALSE(unblock->isEnabled());
    setKickUserLocallyBlocked("new_viewer", true);
    ASSERT_EQ(list->model()->rowCount(), 1);
    EXPECT_EQ(list->model()->index(0, 0).data().toString(), "new_viewer");
    setKickUserLocallyBlocked("NEW_VIEWER", false);
    EXPECT_EQ(list->model()->rowCount(), 0);
    EXPECT_FALSE(unblock->isEnabled());
}

TEST(SplitViewerCount, TabCanEnableCountWhenGlobalDefaultIsOff)
{
    SlowChatApplication app;
    const bool previous = getSettings()->headerViewerCount.getValue();
    getSettings()->headerViewerCount = false;
    Split split(nullptr);
    split.setViewerCountEnabled(true);
    EXPECT_TRUE(split.viewerCountEnabled());
    EXPECT_EQ(split.viewerCountEnabledOverride(), std::optional<bool>(true));
    Split restored(nullptr);
    restored.setViewerCountEnabledOverride(split.viewerCountEnabledOverride());
    EXPECT_TRUE(restored.viewerCountEnabled());
    getSettings()->headerViewerCount = previous;
}

TEST(SplitViewerCount, TabCanDisableCountWhenGlobalDefaultIsOn)
{
    SlowChatApplication app;
    const bool previous = getSettings()->headerViewerCount.getValue();
    getSettings()->headerViewerCount = true;
    Split split(nullptr);
    split.setViewerCountEnabled(false);
    EXPECT_FALSE(split.viewerCountEnabled());
    EXPECT_EQ(split.viewerCountEnabledOverride(), std::optional<bool>(false));
    getSettings()->headerViewerCount = previous;
}

TEST(SplitViewerCount, TabWithoutOverrideFollowsGlobalDefault)
{
    SlowChatApplication app;
    const bool previous = getSettings()->headerViewerCount.getValue();
    Split split(nullptr);
    getSettings()->headerViewerCount = false;
    EXPECT_FALSE(split.viewerCountEnabled());
    getSettings()->headerViewerCount = true;
    EXPECT_TRUE(split.viewerCountEnabled());
    getSettings()->headerViewerCount = previous;
}

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

namespace {
class ScopedUiScale
{
public:
    explicit ScopedUiScale(float scale)
        : original_(getSettings()->getClampedUiScale())
    {
        getSettings()->setClampedUiScale(scale);
    }
    ~ScopedUiScale()
    {
        getSettings()->setClampedUiScale(this->original_);
    }

private:
    float original_;
};

class GiveawayLayoutTest : public ::testing::Test
{
public:
    static constexpr float SCALE = 1.7F;
    GiveawayApplication app;
    ScopedUiScale uiScale{SCALE};
    GiveawayPopup popup;
    std::shared_ptr<Channel> channel =
        std::make_shared<Channel>("coconutb", Channel::Type::None);

    void SetUp() override
    {
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        popup.setContext(channel, {MessagePlatform::AnyOrTwitch,
                                   MessagePlatform::Kick,
                                   MessagePlatform::YouTube});
        popup.show();
        settle();
    }
    void TearDown() override
    {
        popup.hide();
    }
    QPushButton *button(const char *name)
    {
        return popup.findChild<QPushButton *>(name);
    }
    QScrollArea *scrollArea(const char *name)
    {
        return popup.findChild<QScrollArea *>(name);
    }
    void addEntrants(int count)
    {
        for (auto *input : popup.findChildren<QLineEdit *>())
            if (input->placeholderText() == "Entry keyword")
                input->setText("PogU");
        button("GiveawayStartButton")->click();
        for (int i = 0; i < count; ++i)
        {
            auto entry = std::make_shared<Message>();
            entry->loginName = QString("viewer%1").arg(i, 2, 10, QChar('0'));
            entry->displayName = entry->loginName;
            entry->messageText = "PogU";
            entry->platform = MessagePlatform::Kick;
            MessagePtr ptr = entry;
            channel->messageAppended.invoke(ptr, std::nullopt);
        }
        settle();
    }
    void settle(int milliseconds = 200)
    {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    }
};
}  // namespace

TEST_F(GiveawayLayoutTest, WindowIsResizableAndFitsTheScreen)
{
    EXPECT_LT(popup.minimumHeight(), popup.maximumHeight());
    EXPECT_LT(popup.minimumWidth(), popup.maximumWidth());
    EXPECT_FALSE(popup.windowFlags().testFlag(Qt::FramelessWindowHint));
    const auto available = popup.screen()->availableGeometry().size();
    EXPECT_LE(popup.height(), available.height());
    EXPECT_LE(popup.width(), available.width());
}

TEST_F(GiveawayLayoutTest, EverythingFollowsTheUiScale)
{
    addEntrants(10);
    const auto expected = getApp()
                              ->getFonts()
                              ->getFont(FontStyle::UiMedium, SCALE)
                              .pointSizeF();
    ASSERT_GT(expected, 12);
    for (const auto *name : {"GiveawayStatus", "GiveawayEntrantCount"})
        EXPECT_NEAR(popup.findChild<QLabel *>(name)->font().pointSizeF(),
                    expected, 0.1)
            << name;
    const auto rows = popup.findChildren<QWidget *>("GiveawayEntrantRow");
    ASSERT_FALSE(rows.isEmpty());
    EXPECT_EQ(rows.front()->height(), qRound(24 * SCALE));
    for (auto *name : popup.findChildren<QLabel *>("GiveawayEntrantName"))
        EXPECT_NEAR(name->font().pointSizeF(), expected, 0.1);
}

TEST_F(GiveawayLayoutTest, ActionsStayReachableInAShortWindow)
{
    popup.resize(popup.width(), qRound(260 * SCALE));
    settle();
    for (auto *action : {button("GiveawayStartButton"),
                         button("GiveawayRollButton")})
    {
        const QRect area(action->mapTo(&popup, QPoint()), action->size());
        EXPECT_TRUE(popup.rect().contains(area));
        EXPECT_GT(area.height(), 0);
    }
    EXPECT_GT(scrollArea("GiveawayBodyScroll")->verticalScrollBar()->maximum(),
              0);
}

TEST_F(GiveawayLayoutTest, ExtraHeightGoesToTheEntrantsNotTheGaps)
{
    const auto *entrants = scrollArea("GiveawayEntrantScrollArea");
    auto *section = popup.findChild<QLabel *>("GiveawaySectionLabel");
    const auto listHeight = entrants->height();
    const auto sectionHeight = section->height();
    const auto scrollMaximum =
        scrollArea("GiveawayBodyScroll")->verticalScrollBar()->maximum();

    popup.resize(popup.width(), popup.height() + 200);
    settle();
    if (scrollMaximum == 0)
    {
        EXPECT_GE(entrants->height(), listHeight + 120);
    }
    EXPECT_EQ(section->height(), sectionHeight);
}

TEST_F(GiveawayLayoutTest, UserSizeSurvivesSwitchingModesAndStartingARound)
{
    const auto available = popup.screen()->availableGeometry().size();
    popup.resize(std::min(popup.width() + 40, available.width()),
                 popup.height() - 100);
    settle();
    const auto shrunk = popup.size();
    addEntrants(1);
    EXPECT_EQ(popup.size(), shrunk);
    button("GiveawayStartButton")->click();
    settle();
    EXPECT_EQ(popup.size(), shrunk);

    button("GiveawayModeRight")->click();
    settle();
    EXPECT_EQ(popup.width(), shrunk.width());
    const auto requestedHeight = popup.height() + 60;
    popup.resize(popup.width(), requestedHeight);
    button("GiveawayModeEmote")->click();
    button("GiveawayModeRight")->click();
    settle();
    EXPECT_EQ(popup.height(), std::min(requestedHeight, available.height()));
}

TEST_F(GiveawayLayoutTest, OpensReadyOnTheAvailablePlatforms)
{
    EXPECT_EQ(popup.findChild<QLabel *>("GiveawayStatus")->text(),
              "Ready on Twitch + Kick + YouTube.");
}

TEST(GiveawayPopupLayout, DefaultSizeIsUnchangedAtNormalScale)
{
    GiveawayApplication app;
    ScopedUiScale uiScale(1.F);
    GiveawayPopup popup;
    popup.setAttribute(Qt::WA_DontShowOnScreen);
    popup.setContext(std::make_shared<Channel>("coconutb", Channel::Type::None),
                     {MessagePlatform::AnyOrTwitch, MessagePlatform::Kick,
                      MessagePlatform::YouTube});
    popup.show();
    QCoreApplication::processEvents();
    EXPECT_EQ(popup.width(), 520);
    EXPECT_NEAR(popup.height(), 625, 40);
    popup.hide();
}

TEST(GiveawayPopupLayout, RenderAtUiScales)
{
    const auto render = qEnvironmentVariable("MERGERINO_GIVEAWAY_RENDER");
    if (render.isEmpty())
        GTEST_SKIP();
    GiveawayApplication app;
    constexpr int SCREEN_HEIGHT = 1040;  // 1080p above the taskbar
    const auto pump = [](int milliseconds = 200) {
        QEventLoop loop;
        QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
        loop.exec();
    };
    for (const float scale : {1.F, 1.7F})
    {
        ScopedUiScale uiScale(scale);
        const auto name = [&](const char *state) {
            return render + QString("-%1-%2.png")
                                .arg(scale == 1.F ? "1x" : "1.7x", state);
        };
        auto channel =
            std::make_shared<Channel>("coconutb", Channel::Type::None);
        GiveawayPopup popup;
        popup.setAttribute(Qt::WA_DontShowOnScreen);
        popup.setContext(channel, {MessagePlatform::AnyOrTwitch,
                                   MessagePlatform::Kick,
                                   MessagePlatform::YouTube});
        popup.show();
        pump();
        const auto button = [&](const char *objectName) {
            return popup.findChild<QPushButton *>(objectName);
        };
        const auto report = [&](const char *state) {
            std::cout << "scale " << scale << " " << state << " "
                      << popup.width() << "x" << popup.height() << std::endl;
        };
        const auto fitScreen = [&] {
            popup.resize(popup.width(),
                         std::min(popup.height(), SCREEN_HEIGHT));
            pump();
        };
        const auto addEntrants = [&](int count) {
            for (auto *input : popup.findChildren<QLineEdit *>())
                if (input->isVisible() &&
                    input->placeholderText() == "Entry keyword")
                    input->setText("PogU");
            button("GiveawayStartButton")->click();
            for (int i = 0; i < count; ++i)
            {
                auto entry = std::make_shared<Message>();
                entry->loginName =
                    QString("viewer%1").arg(i, 2, 10, QChar('0'));
                entry->displayName = entry->loginName;
                entry->messageText = "PogU";
                entry->platform = MessagePlatform::Kick;
                MessagePtr ptr = entry;
                channel->messageAppended.invoke(ptr, std::nullopt);
            }
            pump();
        };
        fitScreen();
        report("idle");
        ASSERT_TRUE(popup.grab().save(name("idle")));

        addEntrants(40);
        ASSERT_TRUE(popup.grab().save(name("default")));
        button("GiveawayRollButton")->click();
        pump(900);
        ASSERT_TRUE(popup.grab().save(name("reel")));
        button("GiveawayRollButton")->click();
        pump(100);
        button("GiveawayStartButton")->click();
        pump();

        button("GiveawayModeWheel")->click();
        pump();
        fitScreen();
        report("wheel");
        addEntrants(12);
        ASSERT_TRUE(popup.grab().save(name("wheel")));
        button("GiveawayStartButton")->click();
        pump();

        button("GiveawayModeEmote")->click();
        pump();
        report("emote");
        ASSERT_TRUE(popup.grab().save(name("emote")));
        button("GiveawayModeRight")->click();
        pump();
        report("number");
        ASSERT_TRUE(popup.grab().save(name("number")));

        button("GiveawayModeLeft")->click();
        pump();
        popup.resize(popup.width(), qRound(360 * scale));
        pump();
        ASSERT_TRUE(popup.grab().save(name("short")));
        popup.resize(popup.width() + qRound(160 * scale),
                     qRound(760 * scale));
        pump();
        fitScreen();
        ASSERT_TRUE(popup.grab().save(name("tall")));
        popup.resize(1, popup.height());
        pump();
        report("narrow");
        ASSERT_TRUE(popup.grab().save(name("narrow")));
        button("GiveawayModeEmote")->click();
        pump();
        ASSERT_TRUE(popup.grab().save(name("narrow-emote")));
        popup.hide();
    }
}
