// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "controllers/commands/builtin/MassModeration.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandContext.hpp"
#include "controllers/moderation/MassModerationQueue.hpp"
#include "MergedChannelTestAccess.hpp"
#include "messages/Message.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Helix.hpp"
#include "mocks/Logging.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/twitch/IrcMessageHandler.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "Test.hpp"
#include "widgets/dialogs/MassModerationDialog.hpp"

#include <IrcMessage>
#include <QApplication>
#include <QCloseEvent>
#include <QEventLoop>
#include <QGraphicsOpacityEffect>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <memory>
#include <vector>

using namespace chatterino;
using Queue = MassModerationQueue;

namespace chatterino {
class MassModerationDialogTestAccess
{
public:
    static MassModerationQueue &start(MassModerationDialog &dialog,
                                      MassModerationQueue::Action action,
                                      const QStringList &names)
    {
        dialog.startQueue(std::move(action), names);
        return *dialog.queue_;
    }
    static bool close(MassModerationDialog &dialog)
    {
        QCloseEvent event;
        dialog.closeEvent(&event);
        return event.isAccepted();
    }
    static void reset(MassModerationDialog &dialog)
    {
        dialog.reset();
    }
    static QTimer &retentionTimer(MassModerationDialog &dialog)
    {
        return dialog.previewExpiry_;
    }
};
}  // namespace chatterino

namespace {
void runFor(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
}

MessagePtr moderationNotice(const QString &name, bool ban)
{
    auto message = std::make_shared<Message>();
    message->flags.set(MessageFlag::System,
                       ban ? MessageFlag::Timeout : MessageFlag::Untimeout);
    message->timeoutUser = name;
    return message;
}

struct Pending {
    QString name;
    Queue::Guard canAct;
    Queue::Completion done;
};

class MassModerationQueueTest : public ::testing::Test
{
protected:
    std::vector<Pending> requests;
    std::vector<QString> reports;
    Queue queue{[this](const QString &name, Queue::Guard guard,
                       Queue::Completion done) {
        requests.push_back({name, std::move(guard), std::move(done)});
    }};

    void SetUp() override
    {
        queue.reported = [this](const QString &name, const Queue::Result &) {
            reports.push_back(name);
        };
    }
    void start(int delay = 10)
    {
        ASSERT_TRUE(queue.start({"first", "second", "third"}, delay));
        runFor(5);
        ASSERT_EQ(requests.size(), 1);
    }
};

class MassModerationApplication : public mock::BaseApplication
{
public:
    ITwitchIrcServer *getTwitch() override
    {
        return &this->twitch;
    }
    AccountController *getAccounts() override
    {
        return &this->accounts;
    }
    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }
    ILogging *getChatLogger() override
    {
        return &this->logging;
    }

    AccountController accounts;
    mock::MockTwitchIrcServer twitch;
    mock::EmoteController emotes;
    mock::EmptyLogging logging;
};

class MassModerationTwitch : public ::testing::Test
{
protected:
    MassModerationApplication app;
    ::testing::StrictMock<mock::Helix> helix;
    QObject receiver;
    std::shared_ptr<TwitchChannel> channel;
    Queue::Result result{Queue::Outcome::Cancelled, {}};

    void SetUp() override
    {
        using ::testing::_;
        initializeHelix(&helix);
        // Do not validate a test token against Twitch or load account data.
        app.accounts.twitch.currentUserChanged.disconnect_all_slots();
        EXPECT_CALL(helix, update(_, _)).Times(::testing::AnyNumber());
        app.accounts.twitch.accounts.append(std::make_shared<TwitchAccount>(
            "mass_test_account", "fake_test_token", "fake_client", "123"));
        app.accounts.twitch.currentUsername = "mass_test_account";
        app.accounts.twitch.load();
        channel = std::make_shared<TwitchChannel>("mass_test_channel");
        app.twitch.mockChannels.emplace("mass_test_channel", channel);
        std::unique_ptr<Communi::IrcMessage> roomState(
            Communi::IrcMessage::fromData(
                "@room-id=456 :tmi.twitch.tv ROOMSTATE #mass_test_channel",
                nullptr));
        IrcMessageHandler::instance().handleRoomStateMessage(roomState.get());
        this->setMod(true);
    }

    void TearDown() override
    {
        initializeHelix(nullptr);
    }

    void setMod(bool mod)
    {
        std::unique_ptr<Communi::IrcMessage> state(
            Communi::IrcMessage::fromData(
                mod ? "@mod=1 :tmi.twitch.tv USERSTATE #mass_test_channel"
                    : "@mod=0 :tmi.twitch.tv USERSTATE #mass_test_channel",
                nullptr));
        IrcMessageHandler::instance().handleUserStateMessage(state.get());
    }

    HelixUser target()
    {
        return HelixUser(QJsonObject{
            {"id", "789"}, {"login", "target"}, {"display_name", "Target"}});
    }
};
}  // namespace

TEST(MassModerationList, FormatsQuotedCommaListAndMentions)
{
    auto list = parseMassModerationList("\"Name1, @NAME2, name3\"", false);
    EXPECT_EQ(list.names, (QStringList{"name1", "name2", "name3"}));
    EXPECT_EQ(list.formatted, list.names);
    EXPECT_TRUE(list.invalid.isEmpty());
}

TEST(MassModerationList, MixedSeparatorsAndCaseInsensitiveDuplicates)
{
    auto list = parseMassModerationList(
        "first\r\nFIRST; 'second'\t@third,second", false);
    EXPECT_EQ(list.names, (QStringList{"first", "second", "third"}));
    EXPECT_EQ(list.duplicates, 2);
}

TEST(MassModerationList, PreservesInvalidEntriesInsteadOfChangingTargets)
{
    auto list = parseMassModerationList(
        "good,Name 1,https://twitch.tv/someone,/ban innocent", false);
    EXPECT_EQ(list.names, (QStringList{"good"}));
    EXPECT_EQ(list.invalid.size(), 3);
    EXPECT_TRUE(list.formatted.contains("Name 1"));
    EXPECT_EQ(parseMassModerationList(list.formatted.join('\n'), false).invalid,
              list.invalid);
}

TEST(MassModerationList, KickHyphensAndTwitchUnderscores)
{
    EXPECT_EQ(parseMassModerationList("a-b,_a123", true).names.size(), 2);
    EXPECT_EQ(parseMassModerationList("a-b,_a123", false).invalid,
              (QStringList{"a-b"}));
}

TEST(MassModerationList, EmptyList)
{
    EXPECT_TRUE(parseMassModerationList(" \r\n,,,;\t", true).names.isEmpty());
}

TEST_F(MassModerationQueueTest, DoesNothingUntilStartAndRejectsReentry)
{
    runFor(5);
    EXPECT_TRUE(requests.empty());
    EXPECT_FALSE(queue.start({}, 10));
    EXPECT_FALSE(queue.start({"first"}, 0));
    start();
    EXPECT_FALSE(queue.start({"different"}, 10));
}

TEST_F(MassModerationQueueTest, SerialRequestsRespectInterval)
{
    start(60);
    runFor(70);
    EXPECT_EQ(requests.size(), 1);
    requests[0].done({Queue::Outcome::Success, {}});
    runFor(15);
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(queue.succeeded(), 1);
    runFor(70);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].name, "second");
}

TEST_F(MassModerationQueueTest, PauseDuringLookupPreventsActionAndRetainsTarget)
{
    start();
    queue.pause();
    EXPECT_FALSE(requests[0].canAct());
    requests[0].done({Queue::Outcome::Cancelled, {}});
    runFor(25);
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(queue.completed(), 0);
    queue.resume();
    runFor(25);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].name, "first");
    EXPECT_TRUE(requests[1].canAct());
}

TEST_F(MassModerationQueueTest, StopDuringLookupPreventsActionAndResume)
{
    start();
    queue.stop();
    EXPECT_FALSE(requests[0].canAct());
    requests[0].done({Queue::Outcome::Cancelled, {}});
    queue.resume();
    runFor(30);
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(queue.state(), Queue::State::Stopped);
    EXPECT_EQ(queue.completed(), 0);
}

TEST_F(MassModerationQueueTest, PauseAllowsSentRequestToFinishButHoldsRemaining)
{
    start();
    queue.pause();
    requests[0].done({Queue::Outcome::Success, {}});
    runFor(30);
    EXPECT_EQ(queue.completed(), 1);
    EXPECT_EQ(queue.state(), Queue::State::Paused);
    EXPECT_EQ(requests.size(), 1);
    queue.resume();
    runFor(25);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].name, "second");
}

TEST_F(MassModerationQueueTest, StopReportsSentRequestWithoutSendingNext)
{
    start();
    queue.stop();
    requests[0].done({Queue::Outcome::Success, {}});
    runFor(30);
    EXPECT_EQ(queue.succeeded(), 1);
    EXPECT_EQ(queue.state(), Queue::State::Stopped);
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(reports.size(), 1);
}

TEST_F(MassModerationQueueTest, ResumeDuringInFlightRequestCannotOverlap)
{
    start();
    queue.pause();
    queue.resume();
    runFor(25);
    EXPECT_EQ(requests.size(), 1);
    requests[0].done({Queue::Outcome::Success, {}});
    runFor(25);
    EXPECT_EQ(requests.size(), 2);
}

TEST_F(MassModerationQueueTest, RateLimitPausesAndRetriesOnlyAfterResume)
{
    start();
    requests[0].done({Queue::Outcome::RateLimited, "rate limited"});
    runFor(25);
    EXPECT_EQ(queue.state(), Queue::State::Paused);
    EXPECT_EQ(queue.completed(), 0);
    EXPECT_EQ(requests.size(), 1);
    queue.setInterval(60);
    queue.resume();
    runFor(15);
    EXPECT_EQ(requests.size(), 1);
    runFor(70);
    ASSERT_EQ(requests.size(), 2);
    EXPECT_EQ(requests[1].name, "first");
}

TEST_F(MassModerationQueueTest, PermissionOrAccountFailureStopsQueue)
{
    start();
    requests[0].done({Queue::Outcome::Fatal, "account changed"});
    runFor(25);
    EXPECT_EQ(queue.state(), Queue::State::Stopped);
    EXPECT_EQ(requests.size(), 1);
    EXPECT_EQ(reports.size(), 1);
}

TEST_F(MassModerationQueueTest, FailureContinuesAndLastResponseFinishes)
{
    start();
    requests[0].done({Queue::Outcome::Failed, "not found"});
    runFor(25);
    ASSERT_EQ(requests.size(), 2);
    requests[1].done({Queue::Outcome::Success, {}});
    runFor(25);
    ASSERT_EQ(requests.size(), 3);
    queue.pause();
    requests[2].done({Queue::Outcome::Success, {}});
    runFor(5);
    EXPECT_EQ(queue.state(), Queue::State::Finished);
    EXPECT_EQ(queue.completed(), 3);
    EXPECT_EQ(queue.succeeded(), 2);
    EXPECT_EQ(queue.failed(), 1);
}

TEST_F(MassModerationQueueTest, ChangingIntervalReschedulesPendingWait)
{
    start(10);
    requests[0].done({Queue::Outcome::Success, {}});
    runFor(5);
    queue.setInterval(70);
    runFor(25);
    EXPECT_EQ(requests.size(), 1);
    runFor(80);
    EXPECT_EQ(requests.size(), 2);
}

TEST_F(MassModerationQueueTest, StaleDuplicateCallbackCannotCompleteAnotherUser)
{
    start();
    auto stale = requests[0].done;
    stale({Queue::Outcome::Success, {}});
    runFor(25);
    ASSERT_EQ(requests.size(), 2);
    stale({Queue::Outcome::Success, {}});
    runFor(5);
    EXPECT_EQ(queue.completed(), 1);
    EXPECT_TRUE(queue.inFlight());
}

TEST(MassModerationQueue, DestroyedQueueCancelsLookupAndIgnoresLateResponse)
{
    Pending pending;
    auto queue = std::make_unique<Queue>([&pending](const QString &name,
                                                    Queue::Guard guard,
                                                    Queue::Completion done) {
        pending = {name, std::move(guard), std::move(done)};
    });
    ASSERT_TRUE(queue->start({"first"}, 10));
    runFor(5);
    ASSERT_TRUE(pending.canAct());
    queue.reset();
    EXPECT_FALSE(pending.canAct());
    pending.done({Queue::Outcome::Success, {}});
    runFor(5);
}

TEST_F(MassModerationTwitch, BanUsesPinnedChannelModeratorAndReason)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, true, "spam", &receiver);
    ASSERT_TRUE(action.has_value());
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([this](auto, auto success, auto) {
            success(target());
        });
    EXPECT_CALL(helix, banUser(QString("456"), QString("123"), QString("789"),
                               std::optional<int>{}, QString("spam"), _, _))
        .WillOnce([](auto, auto, auto, auto, auto, auto success, auto) {
            success();
        });
    (*action)(
        "target",
        [] {
            return true;
        },
        [this](auto r) {
            result = std::move(r);
        });
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::Success);
}

TEST_F(MassModerationTwitch, UnbanUsesUnbanApi)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, false, {}, &receiver);
    ASSERT_TRUE(action.has_value());
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([this](auto, auto success, auto) {
            success(target());
        });
    EXPECT_CALL(helix,
                unbanUser(QString("456"), QString("123"), QString("789"), _, _))
        .WillOnce([](auto, auto, auto, auto success, auto) {
            success();
        });
    (*action)(
        "target",
        [] {
            return true;
        },
        [this](auto r) {
            result = std::move(r);
        });
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::Success);
}

TEST_F(MassModerationTwitch, StopDuringLookupPreventsBanApiCall)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, true, {}, &receiver);
    ASSERT_TRUE(action.has_value());
    ResultCallback<HelixUser> resolved;
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([&resolved](auto, auto success, auto) {
            resolved = success;
        });
    EXPECT_CALL(helix, banUser(_, _, _, _, _, _, _)).Times(0);
    bool allowed = true;
    (*action)(
        "target",
        [&allowed] {
            return allowed;
        },
        [this](auto r) {
            result = std::move(r);
        });
    allowed = false;
    resolved(target());
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::Cancelled);
}

TEST_F(MassModerationTwitch, AccountSwitchDuringLookupStopsBeforeBan)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, true, {}, &receiver);
    ASSERT_TRUE(action.has_value());
    ResultCallback<HelixUser> resolved;
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([&resolved](auto, auto success, auto) {
            resolved = success;
        });
    EXPECT_CALL(helix, banUser(_, _, _, _, _, _, _)).Times(0);
    (*action)(
        "target",
        [] {
            return true;
        },
        [this](auto r) {
            result = std::move(r);
        });
    app.accounts.twitch.currentUsername = "";
    resolved(target());
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::Fatal);
}

TEST_F(MassModerationTwitch, RateLimitIsReportedAsPause)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, true, {}, &receiver);
    ASSERT_TRUE(action.has_value());
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([this](auto, auto success, auto) {
            success(target());
        });
    EXPECT_CALL(helix, banUser(_, _, _, _, _, _, _))
        .WillOnce([](auto, auto, auto, auto, auto, auto, auto failure) {
            failure(HelixBanUserError::Ratelimited, {});
        });
    (*action)(
        "target",
        [] {
            return true;
        },
        [this](auto r) {
            result = std::move(r);
        });
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::RateLimited);
}

TEST(MassModerationAppearance, UsesNativeDefaultDarkControlsAndUpdatesWithTheme)
{
    MassModerationApplication app;
    app.theme.themeName = "Dark";
    app.theme.appearanceMode = "Dark";
    const auto original = QApplication::palette();
    auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
    for (const bool ban : {true, false})
    {
        MassModerationDialog dialog(channel, ban);
        const auto check = [&](const QPalette &expected) {
            const auto palette = dialog.getLayoutContainer()->palette();
            EXPECT_EQ(palette.color(QPalette::Window),
                      app.theme.window.background);
            EXPECT_EQ(palette.color(QPalette::WindowText),
                      app.theme.window.text);
            for (const auto role :
                 {QPalette::Base, QPalette::Text, QPalette::Button,
                  QPalette::ButtonText, QPalette::Highlight,
                  QPalette::HighlightedText, QPalette::PlaceholderText})
            {
                for (const auto group :
                     {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
                {
                    EXPECT_EQ(palette.color(group, role),
                              expected.color(group, role));
                }
            }
            const auto fields = dialog.findChildren<QPlainTextEdit *>();
            ASSERT_EQ(fields.size(), 2);
            for (const auto *field : fields)
            {
                EXPECT_EQ(field->palette().color(QPalette::Base),
                          expected.color(QPalette::Base));
                EXPECT_EQ(field->viewport()->palette().color(QPalette::Base),
                          expected.color(QPalette::Base));
            }
            for (const auto *button : dialog.findChildren<QPushButton *>())
            {
                EXPECT_EQ(button->palette().color(QPalette::Button),
                          expected.color(QPalette::Button));
            }
            for (const auto *edit : dialog.findChildren<QLineEdit *>())
            {
                EXPECT_EQ(edit->palette().color(QPalette::Base),
                          expected.color(QPalette::Base));
            }
            const auto *interval = dialog.findChild<QSpinBox *>();
            ASSERT_NE(interval, nullptr);
            EXPECT_EQ(interval->palette().color(QPalette::Base),
                      expected.color(QPalette::Base));
        };
        check(original);
        app.theme.themeName = "preset.crimson.dark";
        check(app.theme.palette);
        app.theme.appearanceMode = "Light";
        check(app.theme.palette);
        app.theme.themeName = "Dark";
        app.theme.appearanceMode = "Dark";
        check(original);
    }
}

TEST_F(MassModerationTwitch, ViewerCannotStartMassModeration)
{
    using ::testing::_;
    this->setMod(false);
    EXPECT_CALL(helix, getUserByName(_, _, _)).Times(0);
    auto action = makeMassModerationAction(channel, true, {}, &receiver);
    ASSERT_FALSE(action.has_value());
    EXPECT_TRUE(action.error().contains("moderator or broadcaster"));
}

TEST_F(MassModerationTwitch, LosingModRightsDuringLookupPreventsBan)
{
    using ::testing::_;
    auto action = makeMassModerationAction(channel, true, {}, &receiver);
    ASSERT_TRUE(action.has_value());
    ResultCallback<HelixUser> resolved;
    EXPECT_CALL(helix, getUserByName(QString("target"), _, _))
        .WillOnce([&resolved](auto, auto success, auto) {
            resolved = success;
        });
    EXPECT_CALL(helix, banUser(_, _, _, _, _, _, _)).Times(0);
    (*action)(
        "target",
        [] {
            return true;
        },
        [this](auto r) {
            result = r;
        });
    this->setMod(false);
    resolved(target());
    runFor(5);
    EXPECT_EQ(result.outcome, Queue::Outcome::Fatal);
    EXPECT_TRUE(result.message.contains("moderation rights lost"));
}

TEST(MassModerationPermissions, AnonymousKickAccountIsNotAnUnloadedBroadcaster)
{
    class KickModerationApplication : public MassModerationApplication
    {
    public:
        KickChatServer *getKickChatServer() override
        {
            return &server;
        }
        KickChatServer server;
    } app;
    auto channel = std::make_shared<KickChannel>("test");
    EXPECT_FALSE(channel->isBroadcaster());
    EXPECT_FALSE(channel->hasModRights());
    QObject receiver;
    EXPECT_FALSE(
        makeMassModerationAction(channel, true, {}, &receiver).has_value());
}

TEST(MassModerationSession, HidingKeepsQueueAndPreviewWithProportionalFill)
{
    MassModerationApplication app;
    for (const bool ban : {true, false})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
        MassModerationPreview preview;
        preview.setChannel(channel);
        EXPECT_EQ(preview.preferredHeight(), 0);
        EXPECT_TRUE(preview.isHidden());
        MassModerationDialog dialog(channel, ban);
        std::vector<Pending> requests;
        QStringList names;
        for (int i = 0; i < 184; ++i)
            names.append(QString("name%1").arg(i));
        auto &queue = MassModerationDialogTestAccess::start(
            dialog,
            [&requests](auto name, auto guard, auto done) {
                requests.push_back({name, guard, done});
            },
            names);
        queue.setInterval(1);
        runFor(5);
        ASSERT_EQ(requests.size(), 1);
        EXPECT_FALSE(MassModerationDialogTestAccess::close(dialog));
        EXPECT_TRUE(dialog.isHidden());
        EXPECT_EQ(queue.state(), Queue::State::Running);
        EXPECT_EQ(MassModerationDialog::find(channel, ban), &dialog);
        EXPECT_EQ(MassModerationDialog::find(channel, !ban), nullptr);
        EXPECT_EQ(MassModerationDialog::forChannel(channel).size(), 1);
        EXPECT_FALSE(preview.isHidden());
        EXPECT_GT(preview.preferredHeight(), 0);
        for (int i = 0; i < 4; ++i)
        {
            ASSERT_GT(requests.size(), i);
            channel->addMessage(moderationNotice(requests[i].name, ban),
                                MessageContext::Original);
            auto done = requests[i].done;
            done({Queue::Outcome::Success, {}});
            runFor(5);
        }
        EXPECT_EQ(queue.completed(), 4);
        EXPECT_EQ(dialog.previewText(),
                  ban ? "Mass bans: 4/184" : "Mass unbans: 4/184");
        auto *label = preview.findChild<QLabel *>();
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label->text(), dialog.previewText());
        auto *bar =
            preview.findChild<QProgressBar *>("MassModerationPreviewBar");
        ASSERT_NE(bar, nullptr);
        runFor(260);
        EXPECT_EQ(bar->value(), int(10000LL * 4 / 184));
        EXPECT_EQ(bar->maximum(), 10000);
        queue.pause();
        EXPECT_TRUE(label->text().contains("paused"));
        EXPECT_FALSE(MassModerationDialogTestAccess::close(dialog));
        EXPECT_EQ(queue.state(), Queue::State::Paused);
        queue.stop();
        EXPECT_TRUE(label->text().contains("stopped"));
        EXPECT_FALSE(MassModerationDialogTestAccess::close(dialog));
        ASSERT_TRUE(queue.inFlight());
        auto last = requests.back().done;
        last({Queue::Outcome::Cancelled, {}});
        runFor(5);
        EXPECT_TRUE(MassModerationDialogTestAccess::close(dialog));
    }
}

TEST(MassModerationSession,
     DestroyingDialogRemovesPreviewAndCancelsPendingLookup)
{
    MassModerationApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
    MassModerationPreview preview;
    preview.setChannel(channel);
    Pending pending;
    auto dialog = std::make_unique<MassModerationDialog>(channel, true);
    MassModerationDialogTestAccess::start(
        *dialog,
        [&pending](auto name, auto guard, auto done) {
            pending = {name, guard, done};
        },
        {"name"});
    runFor(5);
    ASSERT_TRUE(pending.canAct());
    dialog.reset();
    EXPECT_FALSE(pending.canAct());
    EXPECT_EQ(MassModerationDialog::find(channel, true), nullptr);
    EXPECT_TRUE(MassModerationDialog::forChannel(channel).empty());
    EXPECT_EQ(preview.preferredHeight(), 0);
    EXPECT_TRUE(preview.isHidden());
    pending.done({Queue::Outcome::Success, {}});
    runFor(5);
}

TEST(MassModerationSession,
     FinishedQueueCanResetToANewListWithoutDuplicateSession)
{
    MassModerationApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
    MassModerationDialog dialog(channel, false);
    MassModerationPreview preview;
    preview.setChannel(channel);
    auto &queue = MassModerationDialogTestAccess::start(
        dialog,
        [](auto, auto, auto done) {
            done({Queue::Outcome::Success, {}});
        },
        {"name"});
    runFor(300);
    EXPECT_EQ(queue.state(), Queue::State::Finished);
    EXPECT_EQ(dialog.previewText(), "Mass unbans: 1/1 (finished)");
    EXPECT_TRUE(MassModerationDialogTestAccess::close(dialog));
    auto buttons = dialog.findChildren<QPushButton *>();
    auto reset = std::ranges::find_if(buttons, [](auto *button) {
        return button->text() == "New list";
    });
    ASSERT_NE(reset, buttons.end());
    EXPECT_TRUE((*reset)->isEnabled());
    MassModerationDialogTestAccess::reset(dialog);
    EXPECT_EQ(dialog.total(), 0);
    EXPECT_EQ(MassModerationDialog::find(channel, false), &dialog);
    EXPECT_TRUE(preview.isHidden());
    for (auto *edit : dialog.findChildren<QPlainTextEdit *>())
        EXPECT_TRUE(edit->toPlainText().isEmpty());
}

TEST(MassModerationSession,
     MergedPreviewShowsEachPlatformAndTracksChannelChanges)
{
    MassModerationApplication app;
    auto twitch = std::make_shared<Channel>("same", Channel::Type::Twitch);
    auto kick = std::make_shared<Channel>("same", Channel::Type::Kick);
    MergedChannelConfig config;
    config.twitchEnabled = false;
    config.kickEnabled = false;
    auto merged = std::make_shared<MergedChannel>(config);
    MergedChannelTestAccess::setSources(*merged, twitch, kick);
    MassModerationDialog twitchDialog(twitch, true);
    MassModerationDialog kickDialog(kick, false);
    auto action = [](auto, auto, auto) {};
    MassModerationDialogTestAccess::start(twitchDialog, action, {"one"});
    MassModerationDialogTestAccess::start(kickDialog, action, {"two", "three"});
    MassModerationPreview preview;
    preview.setChannel(merged);
    EXPECT_EQ(preview.findChildren<QProgressBar *>().size(), 2);
    QStringList texts;
    for (auto *label : preview.findChildren<QLabel *>())
        texts.append(label->text());
    EXPECT_TRUE(texts.contains("Mass bans: 0/1 · Twitch"));
    EXPECT_TRUE(texts.contains("Mass unbans: 0/2 · Kick"));
    preview.setChannel(twitch);
    EXPECT_EQ(preview.findChildren<QProgressBar *>().size(), 1);
    preview.setChannel(std::make_shared<Channel>("other", Channel::Type::Kick));
    EXPECT_TRUE(preview.isHidden());
    preview.setChannel(merged);
    EXPECT_EQ(preview.findChildren<QProgressBar *>().size(), 2);
}

TEST(MassModerationSession, PreviewClickReopensSameDialogAndClosingKeepsItAlive)
{
    MassModerationApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
    MassModerationPreview preview;
    preview.setChannel(channel);
    auto dialog = std::make_unique<MassModerationDialog>(channel, true);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto *original = dialog.get();
    MassModerationDialogTestAccess::start(*dialog, [](auto, auto, auto) {},
                                          {"name"});
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ASSERT_EQ(MassModerationDialog::find(channel, true), original);
    EXPECT_TRUE(dialog->isHidden());
    auto *button =
        preview.findChild<QPushButton *>("MassModerationPreviewButton");
    ASSERT_NE(button, nullptr);
    button->click();
    EXPECT_TRUE(dialog->isVisible());
    EXPECT_EQ(MassModerationDialog::find(channel, true), original);
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_TRUE(dialog->isHidden());
    EXPECT_EQ(MassModerationDialog::find(channel, true), original);
}

TEST_F(MassModerationTwitch, ViewerCannotOpenOrReopenUsingMassCommands)
{
    MassModerationDialog dialog(channel, true);
    this->setMod(false);
    CommandContext context{};
    context.channel = channel;
    context.twitchChannel = channel.get();
    commands::massBan(context);
    EXPECT_TRUE(dialog.isHidden());
    EXPECT_TRUE(channel->getLastMessage()->messageText.contains(
        "moderator or broadcaster"));
    commands::massUnban(context);
    EXPECT_TRUE(channel->getLastMessage()->messageText.contains(
        "moderator or broadcaster"));
}

TEST(MassModerationPreview, PauseResumeSlidesControlsWithoutChangingRowHeight)
{
    MassModerationApplication app;
    for (const bool ban : {true, false})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
        MassModerationDialog dialog(channel, ban);
        MassModerationPreview preview;
        MassModerationPreview otherSplit;
        preview.setChannel(channel);
        otherSplit.setChannel(channel);
        std::vector<Pending> requests;
        auto &queue = MassModerationDialogTestAccess::start(
            dialog,
            [&requests](auto name, auto guard, auto done) {
                requests.push_back({name, guard, done});
            },
            {"first", "second"});
        queue.setInterval(1);
        preview.resize(360, preview.preferredHeight());
        preview.show();
        runFor(5);
        ASSERT_EQ(requests.size(), 1);
        auto *pause =
            preview.findChild<QToolButton *>("MassModerationPreviewPause");
        auto *otherPause =
            otherSplit.findChild<QToolButton *>("MassModerationPreviewPause");
        auto *bar =
            preview.findChild<QProgressBar *>("MassModerationPreviewBar");
        ASSERT_NE(pause, nullptr);
        ASSERT_NE(otherPause, nullptr);
        ASSERT_NE(bar, nullptr);
        const auto position = [&preview](QWidget *widget) {
            return widget->mapTo(&preview, QPoint{}).x();
        };
        EXPECT_LT(position(bar) + bar->width(), position(pause));
        auto *close =
            preview.findChild<QToolButton *>("MassModerationPreviewClose");
        ASSERT_NE(close, nullptr);
        EXPECT_TRUE(close->isHidden());
        const auto pauseX = position(pause);
        const auto bounds = bar->geometry();
        const auto height = preview.preferredHeight();
        pause->click();
        EXPECT_EQ(queue.state(), Queue::State::Paused);
        EXPECT_TRUE(pause->property("paused").toBool());
        EXPECT_TRUE(otherPause->property("paused").toBool());
        EXPECT_TRUE(dialog.isHidden());
        runFor(70);
        EXPECT_EQ(requests.size(), 1);
        EXPECT_LT(position(pause), pauseX);
        EXPECT_GT(position(pause), pauseX - pause->width() - 4);
        EXPECT_FALSE(close->isHidden());
        EXPECT_GT(close->property("opacity").toDouble(), 0);
        EXPECT_LT(close->property("opacity").toDouble(), 1);
        EXPECT_EQ(preview.preferredHeight(), height);
        // Refreshing unchanged state must not restart the slide.
        channel->addSystemMessage("unrelated message");
        MassModerationDialog::progressChanged().invoke();
        runFor(140);
        EXPECT_EQ(position(pause), pauseX - pause->width() - 4);
        EXPECT_LT(position(pause) + pause->width(), position(close));
        EXPECT_TRUE(close->isEnabled());
        EXPECT_DOUBLE_EQ(close->property("opacity").toDouble(), 1);
        EXPECT_EQ(preview.preferredHeight(), height);
        otherPause->click();
        EXPECT_EQ(queue.state(), Queue::State::Running);
        EXPECT_FALSE(pause->property("paused").toBool());
        EXPECT_TRUE(dialog.isHidden());
        channel->addMessage(moderationNotice("first", ban),
                            MessageContext::Original);
        requests.front().done({Queue::Outcome::Success, {}});
        runFor(220);
        EXPECT_EQ(requests.size(), 2);
        EXPECT_EQ(bar->value(), 5000);
        EXPECT_EQ(bar->geometry(), bounds);
        EXPECT_EQ(position(pause), pauseX);
        EXPECT_TRUE(close->isHidden());
        EXPECT_EQ(preview.preferredHeight(), height);
        queue.stop();
        EXPECT_FALSE(pause->isEnabled());
        EXPECT_FALSE(otherPause->isEnabled());
    }
}

TEST(MassModerationPreview, RetentionStartsOnlyAfterTerminalRequestSettles)
{
    MassModerationApplication app;
    for (const bool stop : {false, true})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
        MassModerationDialog dialog(channel, true);
        MassModerationPreview preview;
        MassModerationPreview otherSplit;
        preview.setChannel(channel);
        otherSplit.setChannel(channel);
        auto &retention =
            MassModerationDialogTestAccess::retentionTimer(dialog);
        EXPECT_EQ(retention.interval(), 90'000);
        retention.setInterval(60);
        Pending pending;
        auto &queue = MassModerationDialogTestAccess::start(
            dialog,
            [&pending](auto name, auto guard, auto done) {
                pending = {name, guard, done};
            },
            {"target"});
        runFor(80);
        EXPECT_FALSE(retention.isActive());
        EXPECT_FALSE(dialog.previewExpired());
        queue.pause();
        runFor(80);
        EXPECT_FALSE(retention.isActive());
        EXPECT_FALSE(dialog.previewExpired());
        if (stop)
        {
            queue.stop();
            runFor(80);
            EXPECT_FALSE(retention.isActive());
            EXPECT_FALSE(dialog.previewExpired());
            EXPECT_TRUE(queue.inFlight());
            pending.done({Queue::Outcome::Cancelled, {}});
        }
        else
        {
            channel->addMessage(moderationNotice("target", true),
                                MessageContext::Original);
            pending.done({Queue::Outcome::Success, {}});
        }
        runFor(5);
        ASSERT_TRUE(retention.isActive());
        const auto height = preview.preferredHeight();
        EXPECT_GT(height, 0);
        EXPECT_FALSE(dialog.previewExpired());
        runFor(80);
        EXPECT_TRUE(dialog.previewExpired());
        EXPECT_FALSE(preview.isHidden());
        EXPECT_GT(preview.preferredHeight(), 0);
        runFor(80);
        EXPECT_GT(preview.preferredHeight(), 0);
        EXPECT_LT(preview.preferredHeight(), height);
        auto *opacity = preview.findChild<QGraphicsOpacityEffect *>();
        ASSERT_NE(opacity, nullptr);
        EXPECT_GT(opacity->opacity(), 0);
        EXPECT_LT(opacity->opacity(), 1);
        auto *row = preview.findChild<QWidget *>("MassModerationPreviewRow");
        auto *content =
            preview.findChild<QPushButton *>("MassModerationPreviewButton");
        ASSERT_NE(row, nullptr);
        ASSERT_NE(content, nullptr);
        EXPECT_EQ(content->height(), height);
        EXPECT_LT(row->height(), content->height());
        runFor(160);
        EXPECT_TRUE(preview.isHidden());
        EXPECT_TRUE(otherSplit.isHidden());
        EXPECT_EQ(preview.preferredHeight(), 0);
        EXPECT_EQ(MassModerationDialog::find(channel, true), &dialog);
        preview.setChannel(
            std::make_shared<Channel>("other", Channel::Type::Kick));
        preview.setChannel(channel);
        EXPECT_TRUE(preview.isHidden());
        MassModerationDialogTestAccess::reset(dialog);
        MassModerationDialogTestAccess::start(dialog, [](auto, auto, auto) {},
                                              {"new"});
        EXPECT_FALSE(preview.isHidden());
        EXPECT_FALSE(dialog.previewExpired());
        EXPECT_FALSE(retention.isActive());
    }
}

TEST(MassModerationPreview, ProgressWaitsForMatchingNoticeThenAnimatesOnce)
{
    MassModerationApplication app;
    for (const auto platform : {Channel::Type::Twitch, Channel::Type::Kick})
    {
        for (const bool ban : {true, false})
        {
            auto channel = std::make_shared<Channel>("test", platform);
            MassModerationDialog dialog(channel, ban);
            MassModerationPreview preview;
            preview.setChannel(channel);
            Pending pending;
            MassModerationDialogTestAccess::start(
                dialog,
                [&pending](auto name, auto guard, auto done) {
                    pending = {name, guard, done};
                },
                {"target", "other"});
            runFor(5);
            pending.done({Queue::Outcome::Success, {}});
            runFor(5);
            EXPECT_EQ(dialog.completed(), 1);
            EXPECT_EQ(dialog.previewCompleted(), 0);
            channel->addMessage(moderationNotice("someoneelse", ban),
                                MessageContext::Original);
            channel->addMessage(moderationNotice("target", !ban),
                                MessageContext::Original);
            auto history = std::make_shared<Message>();
            history->flags.set(
                MessageFlag::RecentMessage,
                ban ? MessageFlag::Timeout : MessageFlag::Untimeout);
            history->timeoutUser = "target";
            channel->addMessage(history, MessageContext::Original);
            runFor(5);
            EXPECT_EQ(dialog.previewCompleted(), 0);
            auto *bar =
                preview.findChild<QProgressBar *>("MassModerationPreviewBar");
            ASSERT_NE(bar, nullptr);
            EXPECT_EQ(bar->value(), 0);
            channel->addMessage(moderationNotice("TaRgEt", ban),
                                MessageContext::Original);
            EXPECT_EQ(dialog.previewCompleted(), 0);
            runFor(70);
            EXPECT_EQ(dialog.previewCompleted(), 1);
            EXPECT_GT(bar->value(), 0);
            EXPECT_LT(bar->value(), 5000);
            channel->addMessage(moderationNotice("target", ban),
                                MessageContext::Original);
            runFor(140);
            EXPECT_EQ(bar->value(), 5000);
            EXPECT_EQ(preview.findChild<QLabel *>()->text(),
                      ban ? "Mass bans: 1/2" : "Mass unbans: 1/2");
        }
    }
}

TEST(MassModerationPreview, NoticeBeforeResponseAndReplacementAreSynchronized)
{
    MassModerationApplication app;
    for (const bool beforeResponse : {true, false})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Twitch);
        MassModerationDialog dialog(channel, true);
        Pending pending;
        MassModerationDialogTestAccess::start(
            dialog,
            [&pending](auto name, auto guard, auto done) {
                pending = {name, guard, done};
            },
            {"target"});
        runFor(5);
        const auto notice = moderationNotice("target", true);
        if (beforeResponse)
        {
            channel->addMessage(notice, MessageContext::Original);
            EXPECT_EQ(dialog.previewCompleted(), 0);
            pending.done({Queue::Outcome::Success, {}});
        }
        else
        {
            auto original = std::make_shared<Message>();
            channel->addMessage(original, MessageContext::Original);
            pending.done({Queue::Outcome::Success, {}});
            runFor(5);
            EXPECT_EQ(dialog.previewCompleted(), 0);
            channel->replaceMessage(original, notice);
        }
        runFor(5);
        EXPECT_EQ(dialog.previewCompleted(), 1);
        EXPECT_EQ(dialog.previewText(), "Mass bans: 1/1 (finished)");
    }
}

TEST(MassModerationPreview, MissingNoticesFallBackAndFailuresAdvanceImmediately)
{
    MassModerationApplication app;
    for (const auto outcome : {Queue::Outcome::Success, Queue::Outcome::Failed})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Twitch);
        MassModerationDialog dialog(channel, false);
        MassModerationDialogTestAccess::start(dialog,
                                              [outcome](auto, auto, auto done) {
                                                  done({outcome, "error"});
                                              },
                                              {"target"});
        runFor(10);
        EXPECT_EQ(dialog.completed(), 1);
        EXPECT_EQ(dialog.previewCompleted(),
                  outcome == Queue::Outcome::Success ? 0 : 1);
        runFor(300);
        EXPECT_EQ(dialog.previewCompleted(), 1);
        EXPECT_EQ(dialog.previewText(), "Mass unbans: 1/1 (finished)");
    }
}

TEST(MassModerationPreview, PausedXStopsRemainingActionsAndClosesEveryPreview)
{
    MassModerationApplication app;
    for (const auto platform : {Channel::Type::Twitch, Channel::Type::Kick})
    {
        for (const bool ban : {true, false})
        {
            auto channel = std::make_shared<Channel>("test", platform);
            MassModerationDialog dialog(channel, ban);
            MassModerationPreview preview;
            MassModerationPreview otherSplit;
            preview.setChannel(channel);
            otherSplit.setChannel(channel);
            std::vector<Pending> requests;
            auto &queue = MassModerationDialogTestAccess::start(
                dialog,
                [&requests](auto name, auto guard, auto done) {
                    requests.push_back({name, guard, done});
                },
                {"first", "second"});
            queue.setInterval(1);
            runFor(5);
            ASSERT_EQ(requests.size(), 1);
            auto *close =
                preview.findChild<QToolButton *>("MassModerationPreviewClose");
            ASSERT_NE(close, nullptr);
            EXPECT_TRUE(close->isHidden());
            dialog.cancelAndDismissPreview();
            EXPECT_EQ(queue.state(), Queue::State::Running);
            EXPECT_FALSE(dialog.previewExpired());
            queue.pause();
            runFor(210);
            EXPECT_TRUE(close->isEnabled());
            EXPECT_FALSE(close->isHidden());
            close->click();
            EXPECT_EQ(queue.state(), Queue::State::Stopped);
            EXPECT_TRUE(dialog.previewExpired());
            EXPECT_TRUE(dialog.isHidden());
            EXPECT_FALSE(requests.front().canAct());
            EXPECT_FALSE(preview.isHidden());
            EXPECT_GT(preview.preferredHeight(), 0);
            // A request already sent can still report success; it must never
            // dispatch the next name or bring back a dismissed preview.
            channel->addMessage(moderationNotice("first", ban),
                                MessageContext::Original);
            requests.front().done({Queue::Outcome::Success, {}});
            runFor(260);
            EXPECT_EQ(requests.size(), 1);
            EXPECT_EQ(queue.completed(), 1);
            EXPECT_TRUE(preview.isHidden());
            EXPECT_TRUE(otherSplit.isHidden());
            EXPECT_FALSE(MassModerationDialogTestAccess::retentionTimer(dialog)
                             .isActive());
            EXPECT_EQ(MassModerationDialog::find(channel, ban), nullptr);
        }
    }
}

TEST(MassModerationPreview, TerminalPreviewShowsOnlyXAndDismissesSmoothly)
{
    MassModerationApplication app;
    for (const bool finish : {true, false})
    {
        auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
        MassModerationDialog dialog(channel, false);
        MassModerationPreview preview;
        preview.setChannel(channel);
        auto &queue = MassModerationDialogTestAccess::start(
            dialog,
            [](auto, auto, auto done) {
                done({Queue::Outcome::Success, {}});
            },
            {"target"});
        if (!finish)
        {
            queue.stop();
        }
        preview.resize(360, preview.preferredHeight());
        preview.show();
        runFor(500);
        EXPECT_EQ(queue.state(),
                  finish ? Queue::State::Finished : Queue::State::Stopped);
        auto *pause =
            preview.findChild<QToolButton *>("MassModerationPreviewPause");
        auto *close =
            preview.findChild<QToolButton *>("MassModerationPreviewClose");
        auto *controls =
            preview.findChild<QWidget *>("MassModerationPreviewControls");
        auto *opacity = preview.findChild<QGraphicsOpacityEffect *>();
        ASSERT_NE(pause, nullptr);
        ASSERT_NE(close, nullptr);
        ASSERT_NE(controls, nullptr);
        ASSERT_NE(opacity, nullptr);
        EXPECT_TRUE(pause->isHidden());
        EXPECT_FALSE(close->isHidden());
        EXPECT_TRUE(close->isEnabled());
        EXPECT_EQ(controls->width(), close->width());
        EXPECT_FALSE(opacity->isEnabled());
        const auto height = preview.preferredHeight();
        close->click();
        EXPECT_TRUE(dialog.previewExpired());
        EXPECT_FALSE(preview.isHidden());
        EXPECT_EQ(preview.preferredHeight(), height);
        EXPECT_TRUE(opacity->isEnabled());
        runFor(90);
        EXPECT_GT(preview.preferredHeight(), 0);
        EXPECT_LT(preview.preferredHeight(), height);
        runFor(150);
        EXPECT_TRUE(preview.isHidden());
    }
}

TEST(MassModerationSession, XAllowsFreshEmptySessionBeforeDismissalFinishes)
{
    MassModerationApplication app;
    for (const auto platform : {Channel::Type::Twitch, Channel::Type::Kick})
    {
        for (const bool ban : {true, false})
        {
            for (const bool finished : {true, false})
            {
                auto channel = std::make_shared<Channel>("test", platform);
                MassModerationPreview preview;
                preview.setChannel(channel);
                MassModerationDialog oldDialog(channel, ban);
                oldDialog.findChild<QSpinBox *>()->setValue(7);
                if (ban)
                {
                    auto *reason = oldDialog.findChild<QLineEdit *>();
                    ASSERT_NE(reason, nullptr);
                    reason->setText("old reason");
                }
                Pending oldRequest;
                auto &oldQueue = MassModerationDialogTestAccess::start(
                    oldDialog,
                    [&oldRequest](auto name, auto guard, auto done) {
                        oldRequest = {name, guard, done};
                    },
                    {"oldname"});
                runFor(5);
                if (finished)
                {
                    channel->addMessage(moderationNotice("oldname", ban),
                                        MessageContext::Original);
                    oldRequest.done({Queue::Outcome::Success, {}});
                    runFor(5);
                }
                else
                {
                    oldQueue.pause();
                }
                oldDialog.cancelAndDismissPreview();
                EXPECT_EQ(MassModerationDialog::find(channel, ban), nullptr);
                EXPECT_FALSE(preview.isHidden());
                EXPECT_GT(preview.preferredHeight(), 0);
                oldDialog.reopen();
                EXPECT_TRUE(oldDialog.isHidden());
                MassModerationDialog fresh(channel, ban);
                EXPECT_EQ(MassModerationDialog::find(channel, ban), &fresh);
                EXPECT_EQ(fresh.state(), Queue::State::Idle);
                EXPECT_EQ(fresh.total(), 0);
                EXPECT_EQ(fresh.completed(), 0);
                EXPECT_EQ(fresh.findChild<QSpinBox *>()->value(), 1);
                if (ban)
                {
                    auto *reason = fresh.findChild<QLineEdit *>();
                    ASSERT_NE(reason, nullptr);
                    EXPECT_TRUE(reason->text().isEmpty());
                }
                for (auto *field : fresh.findChildren<QPlainTextEdit *>())
                    EXPECT_TRUE(field->toPlainText().isEmpty());
                for (auto *button : fresh.findChildren<QPushButton *>())
                    EXPECT_NE(button->text(), "New list");
                Pending newRequest;
                auto &newQueue = MassModerationDialogTestAccess::start(
                    fresh,
                    [&newRequest](auto name, auto guard, auto done) {
                        newRequest = {name, guard, done};
                    },
                    {"newname"});
                runFor(5);
                ASSERT_EQ(newRequest.name, "newname");
                if (!finished)
                    oldRequest.done({Queue::Outcome::Success, {}});
                runFor(260);
                EXPECT_EQ(MassModerationDialog::find(channel, ban), &fresh);
                EXPECT_EQ(newQueue.state(), Queue::State::Running);
                EXPECT_EQ(fresh.completed(), 0);
                EXPECT_FALSE(preview.isHidden());
                ASSERT_EQ(preview.findChildren<QLabel *>().size(), 1);
                EXPECT_EQ(preview.findChild<QLabel *>()->text(),
                          ban ? "Mass bans: 0/1" : "Mass unbans: 0/1");
            }
        }
    }
}

TEST(MassModerationSession, XReleasesOldDialogAfterFadeAndIgnoresLateResponse)
{
    MassModerationApplication app;
    auto channel = std::make_shared<Channel>("test", Channel::Type::Kick);
    MassModerationPreview preview;
    preview.setChannel(channel);
    QPointer<MassModerationDialog> oldDialog =
        new MassModerationDialog(channel, true);
    oldDialog->setAttribute(Qt::WA_DeleteOnClose);
    Pending pending;
    auto &queue = MassModerationDialogTestAccess::start(
        *oldDialog,
        [&pending](auto name, auto guard, auto done) {
            pending = {name, guard, done};
        },
        {"oldname", "remaining"});
    runFor(5);
    queue.pause();
    oldDialog->cancelAndDismissPreview();
    EXPECT_EQ(MassModerationDialog::find(channel, true), nullptr);
    EXPECT_FALSE(pending.canAct());
    runFor(90);
    EXPECT_FALSE(oldDialog.isNull());
    EXPECT_FALSE(preview.isHidden());
    runFor(180);
    EXPECT_TRUE(oldDialog.isNull());
    EXPECT_TRUE(preview.isHidden());
    EXPECT_TRUE(MassModerationDialog::forChannel(channel).empty());
    MassModerationDialog fresh(channel, true);
    pending.done({Queue::Outcome::Success, {}});
    runFor(5);
    EXPECT_EQ(MassModerationDialog::find(channel, true), &fresh);
    EXPECT_EQ(fresh.state(), Queue::State::Idle);
    EXPECT_EQ(fresh.completed(), 0);
}
