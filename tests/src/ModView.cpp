#include "widgets/splits/ModView.hpp"

#include "common/WindowDescriptors.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/moderation/ModerationProfiles.hpp"
#include "controllers/moderation/ModerationReview.hpp"
#include "MergedChannelTestAccess.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "mocks/Helix.hpp"
#include "mocks/TwitchIrcServer.hpp"
#include "mocks/UserData.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/buttons/Button.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/dialogs/UserInfoPopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/ModViewRules.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QFontDatabase>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabBar>
#include <QThread>

using namespace chatterino;
using namespace chatterino::moderation;

namespace {
ReviewInput input(QString id, QString text = "test message",
                  MessagePlatform platform = MessagePlatform::AnyOrTwitch)
{
    ReviewInput result;
    result.platform = platform;
    result.userID = id;
    result.login = "viewer" + id;
    result.displayName = result.login;
    result.text = text;
    return result;
}

ReviewRules termsOnly()
{
    ReviewRules rules;
    rules.bursts.enabled = rules.repeats.enabled = rules.links.enabled =
        rules.platformAlerts.enabled = false;
    WordList list;
    list.name = "Terms";
    list.enabled = true;
    list.terms = {"review term"};
    rules.lists.push_back(list);
    return rules;
}

ReviewRules nothingButCoordinated()
{
    auto rules = termsOnly();
    rules.lists.clear();
    return rules;
}

class ModViewApplication : public mock::BaseApplication
{
public:
    // A settings folder of its own: the default one is the real profile next
    // to the test binary, and the window manager saves its layout there
    ModViewApplication()
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

class UsercardApplication : public ModViewApplication
{
public:
    IUserDataController *getUserData() override
    {
        return &users;
    }
    mock::UserDataController users;
};

MessagePtr message(QString id, QString text = "test message",
                   MessagePlatform platform = MessagePlatform::AnyOrTwitch)
{
    auto result = std::make_shared<Message>();
    result->platform = platform;
    result->id = "message-" + id;
    result->userID = id;
    result->loginName = "viewer" + id;
    result->displayName = result->loginName;
    result->messageText = text;
    result->serverReceivedTime = QDateTime::currentDateTimeUtc();
    return result;
}

void click(QWidget &view, const QString &name)
{
    auto *button = view.findChild<Button *>(name);
    ASSERT_TRUE(button) << name.toStdString();
    Q_EMIT button->leftClicked();
}

void selectTab(QWidget &view, int index)
{
    view.findChild<QTabBar *>("ModViewTabs")->setCurrentIndex(index);
}
}  // namespace

namespace chatterino {
class UserInfoPopupTestAccess
{
public:
    static void target(UserInfoPopup &popup, MessagePlatform platform,
                       const QString &name, uint64_t id)
    {
        popup.platform_ = platform;
        popup.isKick_ = platform == MessagePlatform::Kick;
        popup.userName_ = name;
        popup.updateKickUserID(id);
    }
    static QString command(UserInfoPopup &popup, const QString &action,
                           int seconds = 0)
    {
        return popup.moderationCommand(action, seconds);
    }
    static QString visibleID(UserInfoPopup &popup)
    {
        return popup.ui_.userIDLabel->property("copy-text").toString();
    }
    static QString notesKey(UserInfoPopup &popup)
    {
        return popup.userId_;
    }
    static void requestFollowDate(UserInfoPopup &popup, IHelix &helix,
                                  const QString &broadcasterID,
                                  const QString &userID)
    {
        popup.requestTwitchFollowDate(helix, broadcasterID, userID);
    }
    static QString followDate(const UserInfoPopup &popup)
    {
        return popup.ui_.followageLabel->getText();
    }
    static QString followDateTooltip(const UserInfoPopup &popup)
    {
        return popup.ui_.followageLabel->toolTip();
    }
    static const Label &followDateInfo(const UserInfoPopup &popup)
    {
        return *popup.ui_.followageInfo;
    }
};

class ModViewTestAccess
{
public:
    static QList<QLabel *> emptyLabels(ModView &view)
    {
        return {view.emptyQueueTitle_, view.emptyQueueHint_};
    }
    static void flush(ModView &view)
    {
        view.flushMessages();
    }
    static ModerationReview &model(ModView &view)
    {
        return view.review_;
    }
    static void saveRules(ModView &view)
    {
        view.saveRulesEditor();
    }
    static QString scope(ModView &view)
    {
        return view.scope_;
    }
    static ChannelPtr source(ModView &view, MessagePlatform platform)
    {
        return view.platformChannel(platform);
    }
    static void popOut(Split &split)
    {
        for (auto *tool : split.findChildren<ModView *>())
        {
            tool->popOutRequested.invoke();
            return;
        }
    }
    static QString selected(ModView &view)
    {
        return view.selectedKey_;
    }
    static void select(ModView &view, const QString &key)
    {
        view.selectUser(key);
    }
    static QWidget *details(ModView &view)
    {
        return view.details_;
    }
    static QWidget *timeouts(ModView &view)
    {
        return view.timeouts_;
    }
    static void refresh(ModView &view)
    {
        view.refreshLists(true);
    }
    static void feedback(ModView &view, const QString &text)
    {
        view.feedback(text);
    }
    // Moderation access needs a signed-in moderator, which tests don't have.
    static void showActions(ModView &view)
    {
        view.timeouts_->show();
        view.findChild<QWidget *>("ModViewBan")->show();
        view.more_->show();
        view.accessLabel_->hide();
    }
    static ReviewRules rules(ModView &view)
    {
        return view.profileRules_;
    }
    static void editRules(ModView &view, const ReviewRules &rules)
    {
        view.rulesEditor_->setRules(rules);
        view.saveRulesEditor();
    }
    static void setPlatform(ModView &view, MessagePlatform platform,
                            bool active)
    {
        view.setPlatformActive(platform, active);
    }
    static bool muted(ModView &view, MessagePlatform platform)
    {
        return view.mutedPlatforms_.contains(platform);
    }
    static size_t logSize(ModView &view)
    {
        return view.log_.size();
    }
    static QString logStatus(ModView &view, size_t index)
    {
        return view.log_.at(index).status;
    }
    static QString logSource(ModView &view, size_t index)
    {
        return view.log_.at(index).request.source;
    }
    static bool logFailed(ModView &view, size_t index)
    {
        return view.log_.at(index).failed;
    }
    static void addLog(ModView &view, const QString &name,
                       MessagePlatform platform, int seconds,
                       const QString &source, const QString &status,
                       bool failed)
    {
        ModView::ActionRequest request;
        request.name = name;
        request.platform = platform;
        request.userID = "1234";
        request.action = seconds > 0 ? ModView::Action::Timeout
                                     : ModView::Action::Ban;
        request.seconds = seconds;
        request.source = source;
        const auto id = view.addLog(request, status, failed);
        if (!failed)
            view.updateLog(id, status, false, true);
    }
    static void showLockdown(ModView &view)
    {
        view.lockdown_.active = true;
        view.lockdown_.followers = true;
        view.lockdown_.slow = true;
        view.updateLockdownBar();
    }
    static void hideLockdown(ModView &view)
    {
        view.lockdown_ = {};
        view.updateLockdownBar();
    }
    static void showBanConfirmation(ModView &view, const QString &key)
    {
        view.confirmationKey_ = key;
        view.confirmationLabel_->setText(
            "Ban " + view.review_.find(key)->displayName + " on Twitch?");
        static_cast<LabelButton *>(
            view.findChild<Button *>("ModViewConfirmAction"))
            ->setText("Ban");
        view.confirmation_->show();
        view.primaryActions_->hide();
        view.timeouts_->hide();
        view.accessLabel_->hide();
    }
};
}  // namespace chatterino

TEST(TwitchUsercard, ShowsChannelFollowDateAndIgnoresLateResponses)
{
    UsercardApplication app;
    Split split(nullptr);
    UserInfoPopup popup(false, &split);
    mock::Helix helix;
    ResultCallback<QDateTime> callback;
    IHelix::FailureCallback<QString> failureCallback;
    EXPECT_CALL(helix, getChannelFollowDate(QString("101"), QString("202"),
                                           &popup, testing::_, testing::_))
        .WillOnce([&](const auto &, const auto &, const auto *, auto showDate,
                      auto showUnavailable) {
            callback = std::move(showDate);
            failureCallback = std::move(showUnavailable);
        });
    UserInfoPopupTestAccess::requestFollowDate(popup, helix, "101", "202");
    ASSERT_TRUE(callback);
    ASSERT_TRUE(failureCallback);
    const auto &info = UserInfoPopupTestAccess::followDateInfo(popup);
    EXPECT_TRUE(info.isHidden());
    failureCallback({});
    EXPECT_EQ(UserInfoPopupTestAccess::followDate(popup),
              QStringLiteral("Follow date unavailable"));
    EXPECT_FALSE(info.isHidden());
    EXPECT_EQ(info.getText(), QStringLiteral("ⓘ"));
    const auto tooltip = info.toolTip();
    EXPECT_EQ(tooltip, UserInfoPopupTestAccess::followDateTooltip(popup));
    EXPECT_TRUE(tooltip.startsWith("Viewing another user's follow date"));
    EXPECT_TRUE(tooltip.contains("moderator or\nbroadcaster access"));
    EXPECT_TRUE(tooltip.contains("sign in with Twitch again"));
    callback({});
    EXPECT_EQ(UserInfoPopupTestAccess::followDate(popup),
              QStringLiteral("Not following"));
    EXPECT_TRUE(UserInfoPopupTestAccess::followDateTooltip(popup).isEmpty());
    EXPECT_TRUE(info.isHidden());
    EXPECT_TRUE(info.toolTip().isEmpty());

    const auto date = QDateTime::fromString("2022-05-24T22:22:08Z", Qt::ISODate);
    callback(date);
    EXPECT_EQ(UserInfoPopupTestAccess::followDate(popup),
              QStringLiteral("❤ Following since 2022-05-24"));
    EXPECT_TRUE(info.isHidden());

    const auto empty = Channel::getEmpty();
    popup.setData("another-viewer", empty, empty, MessagePlatform::YouTube);
    callback(date);
    failureCallback("Twitch could not load the follow date.");
    EXPECT_TRUE(UserInfoPopupTestAccess::followDate(popup).isEmpty());
    EXPECT_TRUE(info.isHidden());
}

TEST(UsercardModeration, KickUsesChatIdentityWithoutNeedingProfileMetadata)
{
    UsercardApplication app;
    Split split(nullptr);
    UserInfoPopup popup(false, &split);
    UserInfoPopupTestAccess::target(popup, MessagePlatform::Kick,
                                    "viewer_with_underscores", 987654);

    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "ban"), "/ban id:987654");
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "unban"),
              "/unban id:987654");
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "timeout", 600),
              "/timeout id:987654 600s");
    EXPECT_EQ(UserInfoPopupTestAccess::visibleID(popup), "987654");
    EXPECT_EQ(UserInfoPopupTestAccess::notesKey(popup), "kick:987654");
    EXPECT_FALSE(popup.isVisible());
}

TEST(UsercardModeration, ChangingTargetCannotReusePreviousKickIdentity)
{
    UsercardApplication app;
    Split split(nullptr);
    UserInfoPopup popup(false, &split);
    UserInfoPopupTestAccess::target(popup, MessagePlatform::Kick, "first", 123);
    UserInfoPopupTestAccess::target(popup, MessagePlatform::Kick, "second", 0);
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "ban"), "/ban second");
    EXPECT_TRUE(UserInfoPopupTestAccess::notesKey(popup).isEmpty());

    UserInfoPopupTestAccess::target(popup, MessagePlatform::Kick, "second", 456);
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "ban"), "/ban id:456");
}

TEST(UsercardModeration, TwitchKeepsItsUsernameCommandTarget)
{
    UsercardApplication app;
    Split split(nullptr);
    UserInfoPopup popup(false, &split);
    UserInfoPopupTestAccess::target(popup, MessagePlatform::AnyOrTwitch,
                                    "twitch_viewer", 123);
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "ban"),
              "/ban twitch_viewer");
    EXPECT_EQ(UserInfoPopupTestAccess::command(popup, "timeout", 1),
              "/timeout twitch_viewer 1s");
    EXPECT_TRUE(UserInfoPopupTestAccess::command(popup, "unsupported").isEmpty());
}

TEST(UsercardModeration, KickUsernameLinkKeepsOnlyTheMatchingAuthorsID)
{
    Message message;
    message.platform = MessagePlatform::Kick;
    message.loginName = "sender_with_underscores";
    message.displayName = "Sender_With_Underscores";
    message.userID = "123456";
    EXPECT_EQ(UserInfoPopup::userIDForLink("Sender_With_Underscores", message),
              "123456");
    EXPECT_EQ(UserInfoPopup::userIDForLink("sender_with_underscores", message),
              "123456");
    EXPECT_TRUE(UserInfoPopup::userIDForLink("reply_parent", message).isEmpty());
    EXPECT_TRUE(UserInfoPopup::userIDForLink("mentioned_viewer", message)
                    .isEmpty());
}

TEST(UsercardModeration, OtherPlatformsKeepTheirExistingLinkIdentity)
{
    Message message;
    message.platform = MessagePlatform::YouTube;
    message.displayName = "Channel display name";
    message.userID = "UC-channel-id";
    EXPECT_EQ(UserInfoPopup::userIDForLink("Channel display name", message),
              "UC-channel-id");
}

TEST(ModerationReview, PlatformAndStableIDsSeparateIdenticalNames)
{
    ModerationReview review;
    auto twitch = input("AbC");
    auto kick = twitch;
    kick.platform = MessagePlatform::Kick;
    auto other = twitch;
    other.userID = "abc";
    const auto a = review.ingest(twitch, 0);
    const auto b = review.ingest(kick, 0);
    const auto c = review.ingest(other, 0);
    EXPECT_NE(a, b);
    EXPECT_NE(a, c);
    EXPECT_EQ(review.keys().size(), 3);
    EXPECT_EQ(ModerationReview::notesKey(*review.find(a)), "AbC");
    EXPECT_EQ(ModerationReview::notesKey(*review.find(b)), "kick:AbC");
}

TEST(ModerationReview, SeedingAndDuplicateMessagesDoNotCreateBursts)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.bursts.enabled = true;
    rules.bursts.count = 3;
    review.setRules(rules);
    for (int i = 0; i < 20; ++i)
    {
        auto m = input("1");
        m.messageID = QString::number(i);
        review.ingest(m, 0, false);
    }
    auto live = input("1");
    live.messageID = "live";
    const auto key = review.ingest(live, 1);
    for (int i = 0; i < 20; ++i)
        review.ingest(live, 2);
    EXPECT_EQ(review.pendingCount(), 0);
    live.messageID = "second";
    review.ingest(live, 3);
    live.messageID = "third";
    review.ingest(live, 4);
    ASSERT_TRUE(review.find(key));
    EXPECT_TRUE(review.find(key)->pending);
    EXPECT_TRUE(review.find(key)->reasons.contains("burst"));
}

TEST(ModerationReview, LinksCountOnceAndExpiredObservationsDoNotAccumulate)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.links.enabled = true;
    rules.links.count = 3;
    rules.links.window = 2;
    review.setRules(rules);
    const auto key = review.ingest(input("1", "https://example.org"), 0);
    review.ingest(input("1", "https://example.org"), 500);
    EXPECT_FALSE(review.find(key)->pending);
    review.ingest(input("1", "https://example.org"), 3000);
    EXPECT_FALSE(review.find(key)->pending);
    review.ingest(input("1", "www.example.org https://example.org"), 3500);
    EXPECT_TRUE(review.find(key)->pending);
}

TEST(ModerationReview, RepeatsNormalizeTextAndRespectTheObservationWindow)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.repeats.enabled = true;
    rules.repeats.count = 3;
    rules.repeats.window = 2;
    review.setRules(rules);
    const auto key = review.ingest(input("1", "Repeated   Text"), 0);
    review.ingest(input("1", " repeated text "), 500);
    EXPECT_FALSE(review.find(key)->pending);
    review.ingest(input("1", "repeated text"), 3000);
    EXPECT_FALSE(review.find(key)->pending);
    review.ingest(input("1", "REPEATED TEXT"), 3100);
    review.ingest(input("1", "repeated text"), 3200);
    EXPECT_TRUE(review.find(key)->reasons.contains("repeat"));
}

TEST(ModerationReview, TrackingLimitAndSystemMessageExclusion)
{
    ModerationReview review;
    auto system = input("system");
    system.system = true;
    EXPECT_TRUE(review.ingest(system, 0).isEmpty());
    for (int i = 0; i <= ModerationReview::MAX_TRACKED; ++i)
    {
        const auto key = review.ingest(input(QString::number(i)), i);
        EXPECT_EQ(review.track(key, true), i < ModerationReview::MAX_TRACKED);
    }
    EXPECT_EQ(review.trackedCount(), ModerationReview::MAX_TRACKED);
    EXPECT_TRUE(review.track("twitch:0", false));
    EXPECT_TRUE(review.track("twitch:200", true));
}

TEST(ModerationReview, WholeWordsTrustedAccountsAndExclusionsAreRespected)
{
    ModerationReview review;
    auto rules = termsOnly();
    rules.allowList = {"kick:viewer2"};
    review.setRules(rules);
    const auto partial = review.ingest(input("1", "review terms"), 0);
    EXPECT_FALSE(review.find(partial)->pending);
    auto trusted = input("3", "review term");
    trusted.trusted = true;
    auto own = input("4", "review term");
    own.own = true;
    review.ingest(trusted, 0);
    review.ingest(own, 0);
    review.ingest(input("2", "review term", MessagePlatform::Kick), 0);
    EXPECT_EQ(review.pendingCount(), 0);
    review.ingest(input("2", "Review Term!"), 1);
    EXPECT_EQ(review.pendingCount(), 1);
    rules.excludeTrusted = false;
    review.setRules(rules);
    review.ingest(trusted, 2);
    EXPECT_EQ(review.pendingCount(), 2);
}

TEST(ModerationReview,
     DismissalCooldownAndManualRemarkHaveIndependentQueueEntries)
{
    ModerationReview review;
    review.setRules(termsOnly());
    const auto key = review.ingest(input("1", "review term"), 0);
    review.dismiss(key, 1);
    review.ingest(input("1", "review term"), 2);
    EXPECT_FALSE(review.find(key)->pending);
    review.ingest(input("1", "review term"), 30002);
    EXPECT_TRUE(review.find(key)->pending);
    for (int i = 0; i < 2000; ++i)
    {
        review.dismiss(key, i);
        review.mark(key, i);
    }
    EXPECT_EQ(review.pendingCount(), 1);
    EXPECT_TRUE(review.find(key)->pending);
}

TEST(ModerationReview,
     CoordinatedRuleRequiresDistinctAccountsAndMarksParticipants)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.coordinated.enabled = true;
    rules.coordinated.count = 3;
    review.setRules(rules);
    const QString text = "a sufficiently long repeated message";
    for (int i = 0; i < 10; ++i)
        review.ingest(input("1", text), i);
    EXPECT_EQ(review.pendingCount(), 0);
    review.ingest(input("2", text), 11);
    EXPECT_EQ(review.pendingCount(), 0);
    review.ingest(input("3", text), 12);
    EXPECT_EQ(review.pendingCount(), 3);
    EXPECT_TRUE(review.find("twitch:1")->reasons.contains("coordinated"));
}

TEST(ModerationReview,
     HistoriesAndUserQueueStayBoundedAndTrackedIdentitiesSurvive)
{
    ModerationReview review;
    review.setRules(termsOnly());
    auto key = review.ingest(input("pinned", "review term"), 0);
    review.track(key, true);
    for (int i = 0; i < 20000; ++i)
        review.ingest(
            input(QString::number(i), QString(1200, 'x') + " review term"), i);
    EXPECT_LE(review.keys().size(), ModerationReview::MAX_USERS);
    EXPECT_LE(review.pendingCount(), ModerationReview::MAX_PENDING);
    ASSERT_TRUE(review.find(key));
    EXPECT_TRUE(review.find(key)->tracked);
    for (int i = 0; i < 100; ++i)
        review.ingest(input("pinned", QString(1200, 'x')), i);
    EXPECT_LE(review.find(key)->history.size(),
              ModerationReview::MAX_USER_HISTORY);
    EXPECT_EQ(review.find(key)->history.back().text.size(),
              ModerationReview::MAX_TEXT);
    ModerationReview restored;
    restored.restoreTracked(review.trackedJson());
    ASSERT_TRUE(restored.find(key));
    EXPECT_TRUE(restored.find(key)->tracked);
    EXPECT_TRUE(restored.find(key)->history.empty());
}

TEST(ModerationReview, QueueLimitEvictsOldFlagsAndKeepsNewestReview)
{
    ModerationReview review;
    review.setRules(termsOnly());
    for (int i = 0; i < 400; ++i)
        review.ingest(input(QString::number(i), "review term"), i);
    EXPECT_EQ(review.pendingCount(), ModerationReview::MAX_PENDING);
    EXPECT_FALSE(review.find("twitch:0")->pending);
    EXPECT_TRUE(review.find("twitch:0")->reasons.isEmpty());
    EXPECT_TRUE(review.find("twitch:399")->pending);
}

TEST(ModerationReview, DeletionUpdatesContextWithoutEvaluatingRules)
{
    ModerationReview review;
    auto m = input("1");
    m.messageID = "message";
    const auto key = review.ingest(m, 0, false);
    m.text = "updated text";
    review.updateMessage(m, true);
    ASSERT_EQ(review.find(key)->history.size(), 1);
    EXPECT_TRUE(review.find(key)->history.front().deleted);
    EXPECT_EQ(review.find(key)->history.front().text, "updated text");
    EXPECT_EQ(review.pendingCount(), 0);
}

TEST(ModerationReview, MalformedPreferencesAreBoundedAndRulesRoundTrip)
{
    auto rules = termsOnly();
    rules.bursts.count = -1;
    rules.repeats.window = 9000;
    rules.lists.front().action = RuleAction::Timeout;
    rules.lists.front().timeout = 99999999;
    for (int i = 0; i < 12000; ++i)
        rules.lists.front().terms.append(QString::number(i));
    for (int i = 0; i < 40; ++i)
    {
        WordList extra;
        extra.name = QString("Extra %1").arg(i);
        rules.lists.push_back(extra);
    }
    const auto restored = ReviewRules::fromJson(rules.toJson());
    EXPECT_EQ(restored.bursts.count, 2);
    EXPECT_EQ(restored.repeats.window, 300);
    EXPECT_EQ(restored.lists.size(), 32);
    EXPECT_EQ(restored.lists.front().terms.size(), 10000);
    EXPECT_EQ(restored.lists.front().terms.front(), "review term");
    EXPECT_TRUE(restored.lists.front().enabled);
    EXPECT_EQ(restored.lists.front().action, RuleAction::Timeout);
    EXPECT_EQ(restored.lists.front().timeout, 1209600);
}

TEST(ModerationRules, SettingsFromBeforeProfilesAreRead)
{
    const QJsonObject old{
        {"enabled", true},          {"bursts", false},
        {"burstCount", 5},          {"repeats", true},
        {"links", true},            {"linkCount", 4},
        {"termsEnabled", true},     {"terms", QJsonArray{"old term"}},
        {"wholeWords", false},      {"excludedUsers", QJsonArray{"kick:Mod"}},
        {"inlineActions", false},
    };
    const auto rules = ReviewRules::fromJson(old);
    EXPECT_FALSE(rules.bursts.enabled);
    EXPECT_EQ(rules.bursts.count, 5);
    EXPECT_TRUE(rules.links.enabled);
    EXPECT_EQ(rules.links.count, 4);
    ASSERT_EQ(rules.lists.size(), 1);
    EXPECT_EQ(rules.lists.front().name, "Terms");
    EXPECT_TRUE(rules.lists.front().enabled);
    EXPECT_FALSE(rules.lists.front().wholeWords);
    EXPECT_EQ(rules.lists.front().terms, QStringList{"old term"});
    EXPECT_EQ(rules.allowList, QStringList{"kick:mod"});
}

TEST(ModerationRules, WordListsCatchCommonFilterDodges)
{
    WordList list;
    list.terms = {"badword", "two words", "carrot"};
    const TermMatcher matcher(list);
    for (const auto *text :
         {"BADWORD", "b4dw0rd", "b a d w o r d", "you badword!",
          "baaaaadword", "\uFF42\uFF41\uFF44\uFF57\uFF4F\uFF52\uFF44",
          "b\u00E1d\u200Bword", "badw\u00F6rd"})
        EXPECT_EQ(matcher.match(QString::fromUtf8(text)), "badword") << text;
    EXPECT_EQ(matcher.match("TWO   words here"), "two words");
    EXPECT_EQ(matcher.match("carrrrot"), "carrot");
    // Whole words only, and a different word that only shortens to the same
    // letters isn't a stretched one
    EXPECT_TRUE(matcher.match("badwords").isEmpty());
    EXPECT_TRUE(matcher.match("goodword").isEmpty());
    EXPECT_TRUE(matcher.match("carot").isEmpty());
    EXPECT_TRUE(matcher.match("words two").isEmpty());

    WordList wildcard;
    wildcard.terms = {"spam*", "*bot", "sc*m"};
    const TermMatcher wild(wildcard);
    EXPECT_EQ(wild.match("spammer here"), "spam*");
    EXPECT_EQ(wild.match("a viewbot"), "*bot");
    EXPECT_EQ(wild.match("total scam"), "sc*m");
    EXPECT_TRUE(wild.match("robots").isEmpty());

    WordList parts;
    parts.wholeWords = false;
    parts.terms = {"word"};
    EXPECT_EQ(TermMatcher(parts).match("swordfish"), "word");
}

TEST(ModerationRules, TermListsImportFromTextAndTaggedJson)
{
    EXPECT_EQ(parseTermList("# a comment\n  first \n\nsecond phrase\n"),
              QStringList({"first", "second phrase"}));
    const QString json =
        R"([{"match": "alpha|beta", "tags": ["general"]},
            {"match": "gamma", "tags": ["racial"]}, "delta"])";
    EXPECT_EQ(parseTermList(json),
              QStringList({"alpha", "beta", "gamma", "delta"}));
    EXPECT_EQ(parseTermList(json, {"racial"}), QStringList{"gamma"});
    EXPECT_EQ(parseTermList(R"({"words": ["one", "two"]})"),
              QStringList({"one", "two"}));
    const auto [url, tags] = splitListSource(
        "https://example.org/en.json#tags=racial,lgbtq");
    EXPECT_EQ(url, "https://example.org/en.json");
    EXPECT_EQ(tags, QStringList({"racial", "lgbtq"}));
    EXPECT_FALSE(suggestedLists().empty());
    for (const auto &suggested : suggestedLists())
        EXPECT_TRUE(suggested.source.startsWith("https://"));
}

TEST(ModerationRules, AllowListEntriesNamePlatformsAndIDs)
{
    const auto entry = AllowEntry::parse("kick:@SomeUser #123");
    ASSERT_TRUE(entry);
    EXPECT_EQ(entry->platform, MessagePlatform::Kick);
    EXPECT_EQ(entry->name, "someuser");
    EXPECT_EQ(entry->id, "123");
    EXPECT_EQ(entry->toString(), "kick:someuser #123");
    const auto any = AllowEntry::parse("plainname");
    ASSERT_TRUE(any);
    EXPECT_FALSE(any->platform);
    EXPECT_FALSE(AllowEntry::parse("   "));
    EXPECT_FALSE(AllowEntry::parse("myspace:name"));

    // An ID wins over the name, so a renamed account stays allowed
    ModerationReview review;
    auto rules = termsOnly();
    rules.allowList = {"twitch:oldname #7"};
    review.setRules(rules);
    auto renamed = input("7", "review term");
    renamed.login = "newname";
    review.ingest(renamed, 0);
    EXPECT_EQ(review.pendingCount(), 0);
    review.ingest(input("8", "review term"), 0);
    EXPECT_EQ(review.pendingCount(), 1);
}

TEST(ModerationRules, PresetsChangeSensitivityButKeepListsAndActions)
{
    ReviewRules rules;
    EXPECT_EQ(rules.matchingPreset(), RulePreset::Standard);
    rules.lists = termsOnly().lists;
    rules.lists.front().action = RuleAction::Ban;
    rules.links.action = RuleAction::Delete;
    rules.applyPreset(RulePreset::Strict);
    EXPECT_EQ(rules.matchingPreset(), RulePreset::Strict);
    EXPECT_TRUE(rules.caps.enabled);
    ASSERT_EQ(rules.lists.size(), 1);
    EXPECT_EQ(rules.lists.front().action, RuleAction::Ban);
    EXPECT_EQ(rules.links.action, RuleAction::Delete);
    rules.applyPreset(RulePreset::Raid);
    EXPECT_EQ(rules.matchingPreset(), RulePreset::Raid);
    EXPECT_TRUE(rules.firstMessages.enabled);
    rules.bursts.count = 9;
    EXPECT_FALSE(rules.matchingPreset());
}

TEST(ModerationRules, OnlyActionsThePlatformSupportsAreOffered)
{
    EXPECT_TRUE(actionSupported(RuleAction::Warn, MessagePlatform::AnyOrTwitch));
    EXPECT_FALSE(actionSupported(RuleAction::Warn, MessagePlatform::Kick));
    EXPECT_TRUE(actionSupported(RuleAction::Ban, MessagePlatform::YouTube));
    EXPECT_FALSE(actionSupported(RuleAction::Delete, MessagePlatform::TikTok));
    EXPECT_TRUE(actionSupported(RuleAction::Flag, MessagePlatform::TikTok));
}

TEST(ModerationReview, RuleActionsEscalateAndRespectTheCooldown)
{
    ModerationReview review;
    auto rules = termsOnly();
    rules.lists.front().action = RuleAction::Escalate;
    review.setRules(rules);
    auto m = input("1", "review term");
    m.messageID = "a";
    const auto key = review.ingest(m, 0);
    auto triggers = review.takeTriggers();
    ASSERT_EQ(triggers.size(), 1);
    EXPECT_EQ(triggers[0].key, key);
    EXPECT_EQ(triggers[0].action, RuleAction::Delete);
    EXPECT_EQ(triggers[0].messageID, "a");
    EXPECT_EQ(triggers[0].rule, "Terms");
    EXPECT_TRUE(review.find(key)->pending);

    // Each further offence steps up the ladder, even inside the cooldown
    m.messageID = "b";
    review.ingest(m, 1000);
    triggers = review.takeTriggers();
    ASSERT_EQ(triggers.size(), 1);
    EXPECT_EQ(triggers[0].action, RuleAction::Timeout);
    EXPECT_EQ(triggers[0].timeout, 60);
    EXPECT_EQ(triggers[0].messageID, "b");
    m.messageID = "c";
    review.ingest(m, 2000);
    triggers = review.takeTriggers();
    ASSERT_EQ(triggers.size(), 1);
    EXPECT_EQ(triggers[0].timeout, 600);
    // The last step only flags
    m.messageID = "d";
    review.ingest(m, 9000);
    EXPECT_TRUE(review.takeTriggers().empty());

    // The same action isn't repeated for messages sent before it landed
    ModerationReview fixed;
    rules.lists.front().action = RuleAction::Timeout;
    rules.lists.front().timeout = 300;
    fixed.setRules(rules);
    fixed.ingest(input("2", "review term"), 0);
    EXPECT_EQ(fixed.takeTriggers().size(), 1);
    fixed.ingest(input("2", "review term"), 1000);
    EXPECT_TRUE(fixed.takeTriggers().empty());
    fixed.ingest(input("2", "review term"), 7000);
    EXPECT_EQ(fixed.takeTriggers().size(), 1);
}

TEST(ModerationReview, SubscribersCanBeSkippedAndActionsSwitchedOff)
{
    ModerationReview review;
    auto rules = termsOnly();
    rules.lists.front().action = RuleAction::Ban;
    rules.lists.front().skipSubscribers = true;
    review.setRules(rules);
    auto subscriber = input("1", "review term");
    subscriber.subscriber = true;
    review.ingest(subscriber, 0);
    EXPECT_EQ(review.pendingCount(), 0);
    EXPECT_TRUE(review.takeTriggers().empty());
    review.ingest(input("2", "review term"), 0);
    EXPECT_EQ(review.pendingCount(), 1);
    EXPECT_EQ(review.takeTriggers().size(), 1);

    // Mods and the user's own messages never lead to actions
    auto mod = input("3", "review term");
    mod.trusted = true;
    auto own = input("4", "review term");
    own.own = true;
    review.ingest(mod, 0);
    review.ingest(own, 0);
    EXPECT_TRUE(review.takeTriggers().empty());

    rules.automaticActions = false;
    review.setRules(rules);
    review.ingest(input("5", "review term"), 0);
    EXPECT_EQ(review.pendingCount(), 2);
    EXPECT_TRUE(review.takeTriggers().empty());
}

TEST(ModerationReview, NewDetectorsFlagWhatTheyDescribe)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.applyPreset(RulePreset::Strict);
    rules.bursts.enabled = rules.repeats.enabled = rules.links.enabled =
        rules.coordinated.enabled = false;
    review.setRules(rules);
    const auto reasons = [&](const QString &id, const QString &text) {
        review.ingest(input(id, text), 0);
        const auto *user = review.find("twitch:" + id);
        return user ? user->reasons.keys() : QStringList();
    };
    EXPECT_TRUE(reasons("1", "WHY IS NOBODY TALKING ABOUT THIS").contains(
        "caps"));
    EXPECT_TRUE(reasons("2", "!!!! ???? #### $$$$ %%%%").contains("symbols"));
    EXPECT_TRUE(reasons("3", QString(400, 'a') + " b").contains("wall"));
    EXPECT_TRUE(reasons("4", "@a @b @c @d @e hi").contains("mentions"));
    EXPECT_TRUE(
        reasons("5", QString::fromUtf8("he\u0301\u0302\u0303llo")).contains(
            "unusual"));
    EXPECT_TRUE(reasons("6", "a normal message").isEmpty());

    // Returning offenders are known from the chat's timeout messages
    rules.returning.enabled = true;
    review.setRules(rules);
    auto timeout = input("", "viewer7 has been timed out");
    timeout.system = true;
    timeout.timeoutTarget = "viewer7";
    review.ingest(timeout, 0);
    EXPECT_TRUE(reasons("7", "i'm back").contains("returning"));
}

TEST(ModerationReview, NewAccountsAreLookedUpOnceAndFlagged)
{
    ModerationReview review;
    auto rules = nothingButCoordinated();
    rules.newAccounts.enabled = true;
    rules.newAccounts.limit = 7;
    review.setRules(rules);
    const auto young = review.ingest(input("1"), 0);
    const auto old = review.ingest(input("2"), 0);
    review.ingest(input("1"), 1);
    review.ingest(input("3", "kick", MessagePlatform::Kick), 0);
    EXPECT_EQ(review.takeAgeLookups(), QStringList({young, old}));
    EXPECT_TRUE(review.takeAgeLookups().isEmpty());
    const auto now = QDateTime::currentDateTimeUtc();
    review.applyAccountCreated(young, now.addDays(-2), 10);
    review.applyAccountCreated(old, now.addDays(-400), 10);
    EXPECT_TRUE(review.find(young)->reasons.contains("newaccount"));
    EXPECT_FALSE(review.find(old)->pending);
}

TEST(ModerationReview, SimulationReportsWithoutKeepingAnything)
{
    auto rules = termsOnly();
    rules.lists.front().action = RuleAction::Timeout;
    std::vector<ReviewInput> inputs{input("1", "hello"),
                                    input("2", "review term"),
                                    input("3", "nothing here")};
    auto own = input("4", "review term");
    own.own = true;
    inputs.push_back(own);
    const auto result = ModerationReview::simulate(rules, inputs);
    EXPECT_EQ(result.messages, 4);
    EXPECT_EQ(result.flagged, 1);
    EXPECT_EQ(result.actions, 1);
    ASSERT_EQ(result.examples.size(), 1);
    EXPECT_TRUE(result.examples.front().startsWith("viewer2: "));
}

TEST(ModerationProfiles, OldChatRulesBecomeProfilesThatChatsCanShare)
{
    const QJsonObject old{{"bursts", false},
                          {"termsEnabled", true},
                          {"terms", QJsonArray{"x"}},
                          {"inlineActions", false}};
    const QJsonObject root{
        {"scopeA", QJsonObject{{"rules", old}, {"width", 400}}},
        {"scopeB", QJsonObject{{"rules", ReviewRules().toJson()}}},
    };
    ModerationProfiles store(root);
    const auto id = store.profileFor("scopeA", "somechat");
    ASSERT_NE(id, ModerationProfiles::DEFAULT_ID);
    ASSERT_TRUE(store.find(id));
    EXPECT_EQ(store.find(id)->name, "somechat");
    EXPECT_FALSE(store.find(id)->rules.bursts.enabled);
    EXPECT_FALSE(store.chat("scopeA").contains("rules"));
    EXPECT_FALSE(store.chat("scopeA").value("inlineActions").toBool(true));
    EXPECT_EQ(store.chat("scopeA").value("width").toInt(), 400);
    // Rules that were never changed just use the default profile
    EXPECT_EQ(store.profileFor("scopeB", "other"),
              ModerationProfiles::DEFAULT_ID);

    store.setProfileFor("scopeC", id);
    EXPECT_EQ(store.usage(id), 2);
    const auto copy = store.create("somechat", ReviewRules());
    EXPECT_NE(store.find(copy)->name, "somechat");

    const auto document = ModerationProfiles::exportDocument(store.profiles());
    const auto imported =
        ModerationProfiles::importDocument(document, "file name");
    ASSERT_EQ(imported.size(), store.profiles().size());
    EXPECT_FALSE(imported[1].rules.bursts.enabled);
    const auto bare = ModerationProfiles::importDocument(
        termsOnly().toJson(), "Shared rules");
    ASSERT_EQ(bare.size(), 1);
    EXPECT_EQ(bare.front().name, "Shared rules");
    EXPECT_EQ(bare.front().rules.lists.size(), 1);

    EXPECT_FALSE(store.remove(ModerationProfiles::DEFAULT_ID));
    EXPECT_TRUE(store.remove(id));
    EXPECT_EQ(store.profileFor("scopeA", "somechat"),
              ModerationProfiles::DEFAULT_ID);
    EXPECT_EQ(store.profileFor("scopeC", "third"),
              ModerationProfiles::DEFAULT_ID);
}

TEST(ModViewWidget, TrackingAndRulesPersistPerChatAndNewSourceClearsSelection)
{
    ModViewApplication app;
    getSettings()->modViewPreferences = "{}";
    auto channel = std::make_shared<Channel>("first", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    view.inspectMessage(message("1"));
    click(view, "ModViewTrack");
    view.findChild<QCheckBox *>("ModViewInlineActions")->setChecked(false);
    ModViewTestAccess::saveRules(view);
    ModViewTestAccess::setPlatform(view, MessagePlatform::Kick, false);
    const auto firstScope = ModViewTestAccess::scope(view);
    auto other = std::make_shared<Channel>("second", Channel::Type::None);
    view.setChannel(other);
    auto oldMessage = message("old-channel");
    channel->messageAppended.invoke(oldMessage, std::nullopt);
    ModViewTestAccess::flush(view);
    EXPECT_TRUE(ModViewTestAccess::model(view).keys().isEmpty());
    EXPECT_NE(firstScope, ModViewTestAccess::scope(view));
    EXPECT_EQ(ModViewTestAccess::model(view).trackedCount(), 0);
    EXPECT_EQ(
        static_cast<Label *>(view.findChild<QWidget *>("ModViewSelectedUser"))
            ->getText(),
        "Select a user to review");
    EXPECT_TRUE(view.inlineActionsEnabled());
    EXPECT_FALSE(ModViewTestAccess::muted(view, MessagePlatform::Kick));
    view.setChannel(channel);
    EXPECT_EQ(ModViewTestAccess::model(view).trackedCount(), 1);
    EXPECT_FALSE(view.inlineActionsEnabled());
    EXPECT_TRUE(ModViewTestAccess::muted(view, MessagePlatform::Kick));
    EXPECT_FALSE(view.findChild<Button *>("ModViewBan")->enabled());
}

TEST(ModViewWidget, ProfilesAreSharedBetweenChats)
{
    ModViewApplication app;
    getSettings()->modViewPreferences = "{}";
    auto first = std::make_shared<Channel>("first", Channel::Type::None);
    auto second = std::make_shared<Channel>("second", Channel::Type::None);
    ModView a(nullptr);
    ModView b(nullptr);
    a.setChannel(first);
    b.setChannel(second);
    auto rules = ModViewTestAccess::rules(a);
    EXPECT_TRUE(rules.links.enabled);
    rules.links.enabled = false;
    rules.lists = termsOnly().lists;
    ModViewTestAccess::editRules(a, rules);
    // Both chats use the default profile, so the other one follows along
    EXPECT_FALSE(ModViewTestAccess::model(b).rules().links.enabled);
    EXPECT_EQ(ModViewTestAccess::model(b).rules().lists.size(), 1);
    ModView c(nullptr);
    c.setChannel(std::make_shared<Channel>("third", Channel::Type::None));
    EXPECT_FALSE(ModViewTestAccess::rules(c).links.enabled);
}

TEST(ModViewWidget, PlatformsSwitchedOffAreIgnored)
{
    ModViewApplication app;
    getSettings()->modViewPreferences = "{}";
    auto channel = std::make_shared<Channel>("platforms", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    ModViewTestAccess::setPlatform(view, MessagePlatform::Kick, false);
    auto kick = message("k", "test message", MessagePlatform::Kick);
    channel->messageAppended.invoke(kick, std::nullopt);
    ModViewTestAccess::flush(view);
    EXPECT_TRUE(ModViewTestAccess::model(view).keys().isEmpty());
    auto twitch = message("t");
    channel->messageAppended.invoke(twitch, std::nullopt);
    ModViewTestAccess::flush(view);
    EXPECT_EQ(ModViewTestAccess::model(view).keys().size(), 1);
    ModViewTestAccess::setPlatform(view, MessagePlatform::Kick, true);
    auto again = message("k2", "test message", MessagePlatform::Kick);
    channel->messageAppended.invoke(again, std::nullopt);
    ModViewTestAccess::flush(view);
    EXPECT_EQ(ModViewTestAccess::model(view).keys().size(), 2);
}

TEST(ModViewWidget, RuleActionsAreLoggedAndNeedASignedInModerator)
{
    ModViewApplication app;
    getSettings()->modViewPreferences = "{}";
    auto channel = std::make_shared<Channel>("actions", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto rules = termsOnly();
    rules.lists.front().action = RuleAction::Timeout;
    ModViewTestAccess::editRules(view, rules);
    auto hit = message("1", "a review term");
    channel->messageAppended.invoke(hit, std::nullopt);
    ModViewTestAccess::flush(view);
    ASSERT_EQ(ModViewTestAccess::logSize(view), 1);
    EXPECT_EQ(ModViewTestAccess::logSource(view, 0), "Auto: Terms");
    EXPECT_TRUE(ModViewTestAccess::logFailed(view, 0));
    EXPECT_TRUE(ModViewTestAccess::logStatus(view, 0).startsWith(
        "Not sent: sign in"))
        << ModViewTestAccess::logStatus(view, 0).toStdString();

    // TikTok can't be moderated, which is said once rather than per message
    for (int i = 0; i < 3; ++i)
    {
        auto tiktok = message(QString("tt%1").arg(i), "review term",
                              MessagePlatform::TikTok);
        channel->messageAppended.invoke(tiktok, std::nullopt);
        ModViewTestAccess::flush(view);
    }
    EXPECT_EQ(ModViewTestAccess::logSize(view), 2);
    EXPECT_TRUE(ModViewTestAccess::logStatus(view, 1).contains("TikTok"));
}

TEST(ModViewWidget, SelectingSeveralUsersOffersBulkActions)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("bulk", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto &review = ModViewTestAccess::model(view);
    for (int i = 0; i < 3; ++i)
        review.mark(review.ingest(input(QString::number(i)), i, false), i);
    ModViewTestAccess::refresh(view);
    auto *queue = view.findChild<QListWidget *>("ModViewReviewQueue");
    auto *bar = view.findChild<QWidget *>("ModViewBulk");
    ASSERT_TRUE(bar);
    queue->item(0)->setSelected(true);
    EXPECT_TRUE(bar->isHidden());
    queue->selectAll();
    EXPECT_FALSE(bar->isHidden());
    EXPECT_EQ(view.findChild<QLabel *>("ModViewBulkLabel")->text(),
              "3 selected");
    // A refresh keeps the selection
    review.mark(review.ingest(input("9"), 9, false), 9);
    ModViewTestAccess::refresh(view);
    EXPECT_EQ(queue->selectedItems().size(), 3);

    click(view, "ModViewBulkBan");
    EXPECT_TRUE(view.findChild<QLabel *>("ModViewBulkLabel")
                    ->text()
                    .startsWith("Ban 3 users"));
    EXPECT_FALSE(view.findChild<QWidget *>("ModViewBulkConfirm")->isHidden());
    click(view, "ModViewBulkCancel");
    EXPECT_TRUE(view.findChild<QWidget *>("ModViewBulkConfirm")->isHidden());
    click(view, "ModViewBulkDismiss");
    EXPECT_EQ(review.pendingCount(), 1);
    EXPECT_TRUE(bar->isHidden());
}

TEST(ModViewWidget, QueueKeysMoveAndDismiss)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("keys", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto &review = ModViewTestAccess::model(view);
    QStringList keys;
    for (int i = 0; i < 3; ++i)
    {
        keys.append(review.ingest(input(QString::number(i)), i, false));
        review.mark(keys.back(), i);
    }
    ModViewTestAccess::refresh(view);
    auto *queue = view.findChild<QListWidget *>("ModViewReviewQueue");
    const auto press = [&](Qt::Key key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QCoreApplication::sendEvent(queue, &event);
    };
    ModViewTestAccess::select(view, keys[2]);
    press(Qt::Key_J);
    EXPECT_EQ(ModViewTestAccess::selected(view), keys[1]);
    press(Qt::Key_K);
    EXPECT_EQ(ModViewTestAccess::selected(view), keys[2]);
    press(Qt::Key_D);
    EXPECT_FALSE(review.find(keys[2])->pending);
    EXPECT_EQ(ModViewTestAccess::selected(view), keys[1]);
}

TEST(ModViewWidget, MonitoringStopsWhenClosedAndDoesNotRetainChatMessages)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("bounded", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    std::weak_ptr<const Message> weak;
    {
        auto m = message("1");
        weak = m;
        channel->messageAppended.invoke(m, std::nullopt);
    }
    EXPECT_FALSE(weak.expired());
    ModViewTestAccess::flush(view);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(ModViewTestAccess::model(view).keys().size(), 1);
    view.setMonitoring(false);
    auto m = message("2");
    channel->messageAppended.invoke(m, std::nullopt);
    ModViewTestAccess::flush(view);
    EXPECT_EQ(ModViewTestAccess::model(view).keys().size(), 1);
}

TEST(ModViewWidget,
     MergedModerationResolvesTheMessagePlatformInsteadOfSendTarget)
{
    ModViewApplication app;
    MergedChannelConfig config;
    config.twitchEnabled = config.kickEnabled = false;
    auto merged = std::make_shared<MergedChannel>(config);
    auto twitch =
        std::make_shared<Channel>("twitch-source", Channel::Type::Twitch);
    auto kick = std::make_shared<Channel>("kick-source", Channel::Type::Kick);
    MergedChannelTestAccess::setSources(*merged, twitch, kick);
    ModView view(nullptr);
    view.setChannel(merged);
    view.setMonitoring(true);
    EXPECT_EQ(ModViewTestAccess::source(view, MessagePlatform::AnyOrTwitch),
              twitch);
    EXPECT_EQ(ModViewTestAccess::source(view, MessagePlatform::Kick), kick);
    EXPECT_FALSE(ModViewTestAccess::source(view, MessagePlatform::TikTok));
    EXPECT_FALSE(ModViewTestAccess::source(view, MessagePlatform::YouTube));
    view.inspectMessage(message("1"));
    EXPECT_FALSE(view.findChild<Button *>("ModViewBan")->enabled());
    EXPECT_TRUE(ModViewTestAccess::timeouts(view)->isHidden());
}

namespace {
class NativeModViewWidgets
{
public:
    NativeModViewWidgets()
        : previous_(QCoreApplication::testAttribute(
              Qt::AA_DontCreateNativeWidgetSiblings))
    {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    }
    ~NativeModViewWidgets()
    {
        QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings,
                                       previous_);
    }

private:
    bool previous_;
};
class ModViewPaneState : public ::testing::Test
{
public:
    NativeModViewWidgets nativeWidgets;
    ModViewApplication app;
    SplitContainer container{nullptr};
    void TearDown() override
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    Split *addChat(QString name = "native-mod")
    {
        MergedChannelConfig config;
        config.tabName = name;
        config.twitchEnabled = config.kickEnabled = false;
        auto *split = new Split(&container);
        split->setChannel(
            IndirectChannel(std::make_shared<MergedChannel>(config)));
        container.insertSplit(split);
        return split;
    }
    Split *pane(Split *owner)
    {
        for (auto *split : container.getSplits())
            if (split->isModViewPane() &&
                split->modViewOwnerId() == owner->modViewId())
                return split;
        return nullptr;
    }
};
}  // namespace
TEST_F(ModViewPaneState, NativePanePreservesChatAndReviewAcrossToggles)
{
    auto *owner = addChat();
    owner->inspectInModView(message("1", "Promotional message to review"));
    auto *panel = pane(owner);
    ASSERT_NE(panel, nullptr);
    auto *view = panel->findChild<ModView *>();
    ASSERT_NE(view, nullptr);
    EXPECT_EQ(container.getSplits().size(), 2);
    EXPECT_EQ(owner->getChannelView().parentWidget(), owner);
    EXPECT_EQ(view->parentWidget(), panel);
    container.setAttribute(Qt::WA_DontShowOnScreen);
    container.resize(1100, 740);
    container.show();
    QCoreApplication::processEvents();
    auto *header = panel->findChild<SplitHeader *>();
    ASSERT_NE(header, nullptr);
    EXPECT_LE(view->geometry().left(), 2);
    EXPECT_GE(view->geometry().right(), panel->width() - 3);
    EXPECT_GE(view->geometry().bottom(), panel->height() - 3);
    EXPECT_LE(view->geometry().top(), header->height() + 2);
    EXPECT_EQ(&panel->getInput(), &owner->getInput());
    EXPECT_FALSE(panel->inputEnabled());
    EXPECT_FALSE(panel->isActivityPane());
    EXPECT_FALSE(panel->isPriorityChatPane());
    EXPECT_TRUE(owner->getModerationMode());
    EXPECT_EQ(panel->getChannelView().underlyingChannel(), Channel::getEmpty());
    click(*view, "ModViewMark");
    click(*view, "ModViewTrack");
    EXPECT_EQ(ModViewTestAccess::model(*view).pendingCount(), 1);
    owner->setModerationMode(false);
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_EQ(view->parentWidget(), owner);
    EXPECT_TRUE(view->isHidden());
    EXPECT_FALSE(owner->getModerationMode());
    EXPECT_FALSE(owner->showInlineModerationActions());
    owner->setModerationMode(true);
    panel = pane(owner);
    ASSERT_NE(panel, nullptr);
    EXPECT_EQ(panel->findChild<ModView *>(), view);
    EXPECT_EQ(ModViewTestAccess::model(*view).trackedCount(), 1);
    EXPECT_EQ(ModViewTestAccess::model(*view).pendingCount(), 1);
    container.deleteSplit(panel);
    EXPECT_FALSE(owner->getModerationMode());
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_EQ(view->parentWidget(), owner);
}
TEST_F(ModViewPaneState, LayoutRestoresOneLinkedPaneAndMigratesInlineActivation)
{
    auto *owner = addChat();
    owner->setModerationMode(true);
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(container.buildDescriptor());
    ASSERT_EQ(restored.getSplits().size(), 2);
    auto *restoredOwner = restored.getSplits()[0];
    auto *restoredPane = restored.getSplits()[1];
    EXPECT_TRUE(restoredPane->isModViewPane());
    EXPECT_EQ(restoredPane->modViewOwner(), restoredOwner);
    EXPECT_TRUE(restoredOwner->getModerationMode());
    restoredOwner->normalizeModViewPanes();
    EXPECT_EQ(restored.getSplits().size(), 2);
    SplitNodeDescriptor legacy;
    legacy.type_ = "merged";
    legacy.channelName_ = "old-mod";
    legacy.moderationMode_ = true;
    legacy.activityPane_ = false;
    SplitContainer migrated(nullptr);
    migrated.applyFromDescriptor(legacy);
    ASSERT_EQ(migrated.getSplits().size(), 2);
    EXPECT_TRUE(migrated.getSplits()[0]->getModerationMode());
    EXPECT_TRUE(migrated.getSplits()[1]->isModViewPane());
    migrated.getSplits()[0]->normalizeModViewPanes();
    EXPECT_EQ(migrated.getSplits().size(), 2);
}
TEST_F(ModViewPaneState,
       MovingThePanePreservesItsReviewAndSurvivesSourceClosure)
{
    auto *owner = addChat();
    owner->inspectInModView(message("move"));
    auto *panel = pane(owner);
    ASSERT_NE(panel, nullptr);
    auto *view = panel->findChild<ModView *>();
    ASSERT_NE(view, nullptr);
    click(*view, "ModViewMark");
    click(*view, "ModViewTrack");
    SplitContainer target(nullptr);
    container.releaseSplit(panel);
    target.insertSplit(panel);
    EXPECT_TRUE(panel->modViewStandalone());
    EXPECT_EQ(panel->findChild<ModView *>(), view);
    EXPECT_FALSE(owner->getModerationMode());
    EXPECT_EQ(ModViewTestAccess::model(*view).pendingCount(), 1);
    container.deleteSplit(owner);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    EXPECT_EQ(view->parentWidget(), panel);
    EXPECT_EQ(ModViewTestAccess::model(*view).trackedCount(), 1);
    SplitContainer restored(nullptr);
    restored.applyFromDescriptor(target.buildDescriptor());
    ASSERT_EQ(restored.getSplits().size(), 1);
    EXPECT_TRUE(restored.getSplits()[0]->isModViewPane());
    EXPECT_TRUE(restored.getSplits()[0]->modViewStandalone());
}
TEST_F(ModViewPaneState,
       ActivityIsIndependentAndClosingSourceRemovesOnlyItsModPane)
{
    auto *owner = addChat("first-mod");
    auto *other = addChat("second-mod");
    owner->setModerationMode(true);
    other->setModerationMode(true);
    owner->openAlertsPane();
    EXPECT_EQ(container.getSplits().size(), 5);
    EXPECT_TRUE(owner->getModerationMode());
    EXPECT_TRUE(other->getModerationMode());
    container.deleteSplit(owner);
    EXPECT_EQ(container.getSplits().size(), 2);
    EXPECT_TRUE(other->getModerationMode());
    ASSERT_NE(pane(other), nullptr);
}
TEST_F(ModViewPaneState, ConvertingTheSourceToActivityCannotStrandItsModPane)
{
    auto *owner = addChat();
    owner->setModerationMode(true);
    ASSERT_EQ(container.getSplits().size(), 2);
    owner->setInputEnabled(false);
    EXPECT_TRUE(owner->isActivityPane());
    EXPECT_FALSE(owner->getModerationMode());
    EXPECT_EQ(container.getSplits().size(), 1);
    EXPECT_EQ(pane(owner), nullptr);
}

TEST_F(ModViewPaneState, FocusingAFilterKeepsFocusInsideThatField)
{
    auto *owner = addChat();
    owner->inspectInModView(message("focus"));
    auto *panel = pane(owner);
    ASSERT_NE(panel, nullptr);
    auto *view = panel->findChild<ModView *>();
    ASSERT_NE(view, nullptr);
    click(*view, "ModViewMark");
    container.setAttribute(Qt::WA_DontShowOnScreen);
    container.resize(1100, 740);
    container.show();
    QApplication::setActiveWindow(&container);
    QCoreApplication::processEvents();
    container.setSelected(owner);
    QLineEdit *filter = nullptr;
    for (auto *field : view->findChildren<QLineEdit *>())
        if (field->placeholderText() == "Filter flagged users")
            filter = field;
    ASSERT_NE(filter, nullptr);
    filter->setFocus();
    QCoreApplication::processEvents();
    EXPECT_TRUE(filter->hasFocus());
    filter->setText("viewerfocus");
    QCoreApplication::processEvents();
    EXPECT_TRUE(filter->hasFocus());
}

TEST(ModViewWidget, EmptyReviewTextStaysCenteredWithinNarrowPages)
{
    ModViewApplication app;
    ModView view(nullptr);
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.setChannel(
        std::make_shared<Channel>("empty-fit", Channel::Type::None));
    view.show();
    for (int width : {240, 260, 320, 420})
    {
        view.resize(width, 700);
        QCoreApplication::processEvents();
        EXPECT_EQ(view.width(), width);
        for (auto *label : ModViewTestAccess::emptyLabels(view))
        {
            const auto parent = label->parentWidget();
            EXPECT_LE(parent->width(), view.width());
            EXPECT_LE(label->geometry().right(), parent->width());
            EXPECT_NEAR(label->geometry().center().x(),
                        (parent->width() - 1) / 2.0, 1.0);
            EXPECT_TRUE(label->wordWrap());
        }
    }
}

TEST(ModerationReview, KeepsTheLastKnownChatColor)
{
    ModerationReview review;
    auto colored = input("1");
    colored.color = QColor("#ff7f50");
    const auto key = review.ingest(colored, 0);
    EXPECT_EQ(review.find(key)->color, QColor("#ff7f50"));
    review.ingest(input("1"), 1);
    EXPECT_EQ(review.find(key)->color, QColor("#ff7f50"));

    Message chat;
    chat.usernameColor = QColor("#123456");
    EXPECT_EQ(ReviewInput::fromMessage(chat).color, QColor("#123456"));
}

TEST(ModViewWidget, DetailsAppearOnlyWhileAUserIsSelected)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("details", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto *details = ModViewTestAccess::details(view);
    EXPECT_TRUE(details->isHidden());

    view.inspectMessage(message("1"));
    EXPECT_FALSE(details->isHidden());
    selectTab(view, 2);
    EXPECT_TRUE(details->isHidden());
    selectTab(view, 1);
    EXPECT_FALSE(details->isHidden());

    click(view, "ModViewCloseUser");
    EXPECT_TRUE(details->isHidden());
    EXPECT_TRUE(ModViewTestAccess::selected(view).isEmpty());
    EXPECT_EQ(
        view.findChild<QListWidget *>("ModViewTrackedUsers")->currentRow(), -1);
}

TEST(ModViewWidget, DismissingAdvancesThroughTheQueueUntilItIsEmpty)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("queue", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto &review = ModViewTestAccess::model(view);
    QStringList keys;
    for (int i = 0; i < 3; ++i)
    {
        keys.append(review.ingest(input(QString::number(i)), i, false));
        review.mark(keys.back(), i);
    }
    ModViewTestAccess::refresh(view);
    auto *queue = view.findChild<QListWidget *>("ModViewReviewQueue");
    ASSERT_EQ(queue->count(), 3);

    ModViewTestAccess::select(view, keys[2]);
    EXPECT_EQ(queue->currentRow(), 0);
    click(view, "ModViewDismiss");
    EXPECT_EQ(ModViewTestAccess::selected(view), keys[1]);
    EXPECT_EQ(queue->currentRow(), 0);
    click(view, "ModViewDismiss");
    EXPECT_EQ(ModViewTestAccess::selected(view), keys[0]);
    click(view, "ModViewDismiss");
    EXPECT_TRUE(ModViewTestAccess::selected(view).isEmpty());
    EXPECT_TRUE(ModViewTestAccess::details(view)->isHidden());
    EXPECT_EQ(queue->count(), 0);
}

TEST(ModViewWidget, TimeoutPresetsMatchWhatTheSourcePlatformAccepts)
{
    ModViewApplication app;
    getSettings()->timeoutButtons =
        std::vector<TimeoutButton>{{"s", 30}, {"m", 1}, {"d", 1}, {"w", 1}};
    auto channel = std::make_shared<Channel>("presets", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    view.setMonitoring(true);
    auto &review = ModViewTestAccess::model(view);
    const auto visiblePresets = [&] {
        QStringList visible;
        for (auto *button :
             ModViewTestAccess::timeouts(view)->findChildren<Button *>())
            if (!button->isHidden())
                visible.append(button->findChild<QLabel *>()->text());
        return visible;
    };

    ModViewTestAccess::select(view, review.ingest(input("1"), 0, false));
    EXPECT_EQ(visiblePresets(), QStringList({"30s", "1m", "1d", "1w"}));
    ModViewTestAccess::select(
        view,
        review.ingest(input("2", "kick", MessagePlatform::Kick), 0, false));
    EXPECT_EQ(visiblePresets(), QStringList({"1m", "1d", "1w"}));
    ModViewTestAccess::select(
        view,
        review.ingest(input("3", "yt", MessagePlatform::YouTube), 0, false));
    EXPECT_EQ(visiblePresets(), QStringList({"30s", "1m", "1d"}));
    getSettings()->timeoutButtons.setValue(
        getSettings()->timeoutButtons.getDefaultValue());
}

TEST(ModViewWidget, ThemeChangesReplaceThePaletteAfterTheFirstStyle)
{
    ModViewApplication app;
    auto channel = std::make_shared<Channel>("themes", Channel::Type::None);
    ModView view(nullptr);
    view.setChannel(channel);
    auto *theme = getApp()->getThemes();
    const auto dark = theme->splits.background;

    theme->appearanceMode = "Light";
    theme->themeName = "Light";
    theme->update();
    EXPECT_NE(theme->splits.background, dark);
    EXPECT_EQ(view.palette().color(QPalette::Window), theme->splits.background);

    theme->appearanceMode = "Dark";
    theme->themeName = "Dark";
    theme->update();
    EXPECT_EQ(view.palette().color(QPalette::Window), dark);
}

TEST(ModViewWidget, RenderPaneStatesWithoutShowingAWindow)
{
    const auto render = qEnvironmentVariable("MERGERINO_MODVIEW_RENDER");
    if (render.isEmpty())
        GTEST_SKIP();
    ModViewApplication app;
    QApplication::setFont(QFont("Segoe UI", 9));
    getSettings()->modViewPreferences = "{}";

    MergedChannelConfig config;
    config.twitchEnabled = config.kickEnabled = false;
    auto channel = std::make_shared<MergedChannel>(config);
    Split split(nullptr);
    split.setAttribute(Qt::WA_DontShowOnScreen);
    split.resize(980, 760);
    split.setChannel(IndirectChannel(channel));
    split.inspectInModView(message("seed"));
    auto *view = split.findChild<ModView *>();
    ASSERT_TRUE(view);
    auto &review = ModViewTestAccess::model(*view);
    click(*view, "ModViewCloseUser");
    selectTab(*view, 0);
    review = ModerationReview();
    split.show();

    // Long enough for switch and reveal animations to finish
    const auto wait = [](int ms) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < ms)
        {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
    };
    const auto settle = [&] {
        wait(260);
    };
    const auto save = [&](const QString &name) {
        settle();
        ASSERT_TRUE(view->grab().save(render + "-" + name + ".png"));
    };
    settle();
    ASSERT_TRUE(split.grab().save(render + "-split-empty.png"));
    save("empty");

    const auto now = QDateTime::currentDateTimeUtc();
    const auto chat = [&](const QString &id, const QString &name,
                          const QColor &color, MessagePlatform platform,
                          const QStringList &lines) {
        QString key;
        int index = 0;
        for (const auto &line : lines)
        {
            auto m = input(id, line, platform);
            m.displayName = name;
            m.login = name.toLower();
            m.color = color;
            m.messageID = id + "-" + QString::number(index);
            m.timestamp = now.addSecs((index++ - lines.size()) * 7);
            key = review.ingest(m, 0, false);
        }
        return key;
    };
    const auto kekw = chat(
        "4471", "KekwEnjoyer", QColor("#ff7f50"), MessagePlatform::AnyOrTwitch,
        {"he just pulled up", "bro is actually washed",
         "BUY FOLLOWERS at bestviewers dot com, cheap and fast, real "
         "viewers for your channel no bots no scam trusted by thousands "
         "of streamers worldwide",
         "BUY FOLLOWERS at bestviewers dot com",
         "BUY FOLLOWERS at bestviewers dot com",
         "BUY FOLLOWERS at bestviewers dot com"});
    auto removed = input("4471", "bro is actually washed");
    removed.messageID = "4471-1";
    review.updateMessage(removed, true);
    const auto kick = chat(
        "9182", "promo_clips_official", QColor("#53fc18"),
        MessagePlatform::Kick,
        {"check my clips kick dot com slash promo", "new clip just dropped"});
    const auto youtube =
        chat("UCs0", "Marcus Whitfield", QColor("#ff5c5c"),
             MessagePlatform::YouTube, {"first time here, love the stream"});
    const auto tiktok =
        chat("7731", "a_really_long_username_for_a_small_panel",
             QColor("#00f2ea"), MessagePlatform::TikTok, {"sent a rose"});
    review.mark(tiktok, 0);
    review.mark(youtube, 0);
    review.mark(kick, 1000);
    review.mark(kekw, 2000);
    review.track(kekw, true);
    review.track(youtube, true);
    ModViewTestAccess::refresh(*view);
    save("queue");

    ModViewTestAccess::select(*view, kekw);
    ModViewTestAccess::showActions(*view);
    settle();
    ASSERT_TRUE(split.grab().save(render + "-split-selected.png"));
    ModViewTestAccess::showActions(*view);
    save("selected");
    ModViewTestAccess::select(*view, tiktok);
    save("no-access");
    ModViewTestAccess::select(*view, kekw);
    ModViewTestAccess::showBanConfirmation(*view, kekw);
    save("confirm");

    ModViewTestAccess::select(*view, kekw);
    selectTab(*view, 1);
    ModViewTestAccess::showActions(*view);
    save("tracking");
    click(*view, "ModViewCloseUser");
    save("tracking-idle");
    selectTab(*view, 2);
    save("rules");
    auto *editor = qobject_cast<QScrollArea *>(
        view->findChild<QWidget *>("ModViewRulesViewport")->parentWidget());
    ASSERT_TRUE(editor);
    editor->verticalScrollBar()->setValue(560);
    save("rules-middle");
    // Mid-slide, then open and closed again
    auto *caps = view->findChild<QCheckBox *>("ModViewCaps");
    ASSERT_TRUE(caps);
    caps->setChecked(true);
    wait(70);
    ASSERT_TRUE(view->grab().save(render + "-rules-sliding.png"));
    save("rules-caps-open");
    caps->setChecked(false);
    save("rules-caps-closed");
    editor->verticalScrollBar()->setValue(0);
    {
        auto rules = ModViewTestAccess::rules(*view);
        rules.applyPreset(RulePreset::Strict);
        WordList slurs;
        slurs.name = "Slurs";
        slurs.enabled = true;
        slurs.action = RuleAction::Ban;
        slurs.terms = {"first", "second", "third"};
        slurs.source = "https://example.org/en.json#tags=racial";
        rules.lists = {slurs, termsOnly().lists.front()};
        rules.links.action = RuleAction::Timeout;
        rules.links.skipSubscribers = true;
        rules.allowList = {"twitch:trusted_friend #1001", "kick:clipbot"};
        ModViewTestAccess::editRules(*view, rules);
    }
    view->findChild<QCheckBox *>("ModViewLinks")->setChecked(true);
    save("rules-strict");
    const auto savePage = [&](const QString &name) {
        settle();
        auto *page = view->findChild<QWidget *>("ModViewRulesPage");
        ASSERT_TRUE(page);
        ASSERT_TRUE(page->grab().save(render + "-" + name + ".png"));
    };
    savePage("rules-page");
    view->findChild<QCheckBox *>("ModViewAutoMark")->setChecked(false);
    save("rules-off");
    view->findChild<QCheckBox *>("ModViewAutoMark")->setChecked(true);

    selectTab(*view, 3);
    save("log-empty");
    ModViewTestAccess::addLog(*view, "KekwEnjoyer", MessagePlatform::AnyOrTwitch,
                              600, "Auto: Links", "Done", false);
    ModViewTestAccess::addLog(*view, "promo_clips_official",
                              MessagePlatform::Kick, 0, "Bulk", "Done", false);
    ModViewTestAccess::addLog(
        *view, "a_really_long_username_for_a_small_panel",
        MessagePlatform::TikTok, 600, "Auto: Slurs",
        "Not sent: TikTok actions are unavailable", true);
    ModViewTestAccess::addLog(*view, "Marcus Whitfield",
                              MessagePlatform::YouTube, 300, "You",
                              "Sent to YouTube", false);
    save("log");

    selectTab(*view, 0);
    view->findChild<QListWidget *>("ModViewReviewQueue")->selectAll();
    save("bulk");
    click(*view, "ModViewBulkBan");
    save("bulk-confirm");
    click(*view, "ModViewBulkCancel");
    view->findChild<QListWidget *>("ModViewReviewQueue")->clearSelection();
    ModViewTestAccess::showLockdown(*view);
    ModViewTestAccess::select(*view, kekw);
    ModViewTestAccess::showActions(*view);
    save("lockdown");
    ModViewTestAccess::hideLockdown(*view);

    selectTab(*view, 1);
    view->findChild<QComboBox *>("ModViewSearchMode")->setCurrentIndex(1);
    view->findChild<QLineEdit *>()->clear();
    for (auto *edit : view->findChildren<QLineEdit *>())
        if (edit->placeholderText() == "Find what someone said")
            edit->setText("buy followers");
    click(*view, "ModViewCloseUser");
    save("search-messages");
    for (auto *edit : view->findChildren<QLineEdit *>())
        if (edit->placeholderText() == "Find what someone said")
            edit->clear();
    view->findChild<QComboBox *>("ModViewSearchMode")->setCurrentIndex(0);

    selectTab(*view, 0);
    ModViewTestAccess::select(*view, kekw);
    view->setFixedWidth(260);
    ModViewTestAccess::showActions(*view);
    save("narrow");
    selectTab(*view, 2);
    save("narrow-rules");
    selectTab(*view, 0);
    view->findChild<QListWidget *>("ModViewReviewQueue")->selectAll();
    save("narrow-bulk");
    view->findChild<QListWidget *>("ModViewReviewQueue")->clearSelection();
    ModViewTestAccess::select(*view, kekw);
    selectTab(*view, 0);
    view->setFixedWidth(720);
    ModViewTestAccess::showActions(*view);
    save("wide");
    view->setFixedWidth(370);

    for (int i = 0; i < 24; ++i)
    {
        const auto id = QString::number(5000 + i);
        const auto key =
            chat(id, "flagged_viewer_" + id, QColor::fromHsv(i * 15, 170, 240),
                 MessagePlatform::AnyOrTwitch, {"spam"});
        review.mark(key, 3000);
    }
    ModViewTestAccess::refresh(*view);
    ModViewTestAccess::select(*view, kekw);
    ModViewTestAccess::showActions(*view);
    ModViewTestAccess::feedback(*view, "Action completed for KekwEnjoyer.");
    save("busy");
    ModViewTestAccess::feedback(*view, {});

    auto *theme = getApp()->getThemes();
    theme->appearanceMode = "Light";
    theme->themeName = "Light";
    theme->update();
    ModViewTestAccess::select(*view, kekw);
    ModViewTestAccess::showActions(*view);
    save("light");
    selectTab(*view, 2);
    save("light-rules");
    selectTab(*view, 3);
    save("light-log");
    theme->appearanceMode = "Dark";
    theme->themeName = "Dark";
    theme->update();
    selectTab(*view, 0);

    for (auto *child : split.findChildren<BaseWidget *>())
        child->setOverrideScale(1.5F);
    split.setOverrideScale(1.5F);
    ModViewTestAccess::showActions(*view);
    save("scaled");
    selectTab(*view, 2);
    save("scaled-rules");
    selectTab(*view, 0);
    split.hide();
}
