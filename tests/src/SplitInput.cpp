// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitInput.hpp"

#include "common/Channel.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/Command.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/completion/TabCompletionModel.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/EmoteController.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "Test.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/helper/ResizingTextEdit.hpp"
#include "widgets/splits/Split.hpp"

#include <QDebug>
#include <QLabel>
#include <QMouseEvent>
#include <QFocusEvent>
#include <QString>
#include <QTextEdit>

#include <algorithm>

using namespace chatterino;
using ::testing::Exactly;

namespace {

class MockApplication : public mock::BaseApplication
{
public:
    MockApplication()
        : windowManager(this->args, this->paths_, this->settings, this->theme,
                        this->fonts)
        , commands(this->paths_)
    {
    }

    HotkeyController *getHotkeys() override
    {
        return &this->hotkeys;
    }

    WindowManager *getWindows() override
    {
        return &this->windowManager;
    }

    AccountController *getAccounts() override
    {
        return &this->accounts;
    }

    CommandController *getCommands() override
    {
        return &this->commands;
    }

    EmoteController *getEmotes() override
    {
        return &this->emotes;
    }

    HotkeyController hotkeys;
    WindowManager windowManager;
    AccountController accounts;
    CommandController commands;
    mock::EmoteController emotes;
};

class SplitInputTest
    : public ::testing::TestWithParam<std::tuple<QString, QString>>
{
public:
    SplitInputTest()
        : split(new Split(nullptr))
        , input(this->split)
    {
    }

    MockApplication mockApplication;
    Split *split;
    SplitInput input;
};

// Record the focus traversal Qt attempts after a child ignores Tab. Keep the
// entire widget tree hidden so this never interacts with desktop focus.
class TabFocusProbe : public QWidget
{
public:
    int traversals = 0;

protected:
    bool focusNextPrevChild(bool) override
    {
        ++this->traversals;
        return true;
    }
};

}  // namespace

TEST(ChatInputTabFocus, EmptyShortAndUnmatchedInputCannotTriggerFocusTraversal)
{
    MockApplication app;
    TabFocusProbe window;
    Channel channel("completion-test", Channel::Type::None);
    ResizingTextEdit editor;
    editor.setParent(&window);
    QCompleter completer;
    completer.setModel(new TabCompletionModel(channel, &completer));

    for (const bool withCompleter : {false, true})
    {
        editor.setCompleter(withCompleter ? &completer : nullptr);
        for (const auto &text : {QString{}, QString("a"), QString("unmatched")})
        {
            SCOPED_TRACE(text.toStdString());
            editor.setPlainText(text);
            editor.moveCursor(QTextCursor::End);
            for (const auto key : {Qt::Key_Tab, Qt::Key_Backtab})
            {
                QKeyEvent event(QEvent::KeyPress, key,
                                key == Qt::Key_Backtab ? Qt::ShiftModifier
                                                      : Qt::NoModifier);
                QCoreApplication::sendEvent(&editor, &event);
                EXPECT_EQ(window.traversals, 0);
                EXPECT_EQ(editor.toPlainText(), text);
            }
        }
    }
    EXPECT_FALSE(window.isVisible());
}

TEST(ChatInputTabFocus, TabAndShiftTabStillCycleCompletions)
{
    MockApplication app;
    TabFocusProbe window;
    Channel channel("completion-test", Channel::Type::None);
    ResizingTextEdit editor;
    editor.setParent(&window);
    QCompleter completer;
    auto *model = new TabCompletionModel(channel, &completer);
    // Supply deterministic results; this test exercises input dispatch and
    // inline completion without depending on live channel/emote data.
    model->setStringList({"PogOne", "PogTwo"});
    completer.setModel(model);
    editor.setCompleter(&completer);
    editor.setPlainText("Pog");
    editor.moveCursor(QTextCursor::End);

    const auto press = [&](Qt::Key key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent event(QEvent::KeyPress, key, modifiers);
        QCoreApplication::sendEvent(&editor, &event);
        EXPECT_EQ(window.traversals, 0);
    };
    press(Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(editor.toPlainText(), "PogOne");
    press(Qt::Key_Tab, Qt::NoModifier);
    EXPECT_EQ(editor.toPlainText(), "PogTwo");
    press(Qt::Key_Backtab, Qt::ShiftModifier);
    EXPECT_EQ(editor.toPlainText(), "PogOne");
    EXPECT_FALSE(window.isVisible());
}

TEST_P(SplitInputTest, Reply)
{
    std::tuple<QString, QString> params = this->GetParam();
    auto [inputText, expected] = params;
    ASSERT_EQ("", this->input.getInputText());
    this->input.setInputText(inputText);
    ASSERT_EQ(inputText, this->input.getInputText());

    auto *message = new Message();
    message->displayName = "forsen";
    auto reply = MessagePtr(message);
    this->input.setReply(reply);
    QString actual = this->input.getInputText();
    ASSERT_EQ(expected, actual) << "Input text after setReply should be '"
                                << expected << "', but got '" << actual << "'";
}

INSTANTIATE_TEST_SUITE_P(
    SplitInput, SplitInputTest,
    testing::Values(
        // Ensure message is retained
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen,",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure mention with comma is stripped, retain message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen Test message",
            // Expected text after replying to forsen
            "@forsen Test message "),

        // Ensure mention with comma and space is stripped, no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "@forsen, ",
            // Expected text after replying to forsen
            "@forsen "),

        // Ensure it works with no message
        std::make_tuple<QString, QString>(
            // Pre-existing text in the input
            "",
            // Expected text after replying to forsen
            "@forsen ")));

TEST(SplitInputAnonymous, BlockedBoxIsReadOnlyAndNamesWhereYouAreAnonymous)
{
    MockApplication app;
    Split split(nullptr);
    SplitInput input(&split);
    auto *editor = input.findChild<QTextEdit *>();
    ASSERT_NE(editor, nullptr);
    const auto hasToolTip = [&](const QString &text) {
        return std::ranges::any_of(input.findChildren<QLabel *>(),
                                   [&](const QLabel *label) {
                                       return label->toolTip() == text;
                                   });
    };

    // Anonymous everywhere this chat sends to: nothing gets into the box.
    // No saved accounts, so there's nothing a click could switch to.
    input.setAnonymousState({"Twitch", "Kick"});
    EXPECT_TRUE(input.isAnonymousBlocked());
    EXPECT_TRUE(editor->isReadOnly());
    input.insertText("Kappa");
    EXPECT_EQ(input.getInputText(), "");
    EXPECT_TRUE(input.switchableAnonymousPlatforms().isEmpty());
    EXPECT_TRUE(hasToolTip("Anonymous on Twitch and Kick."));

    // One saved Twitch account: a click goes back to it, Kick stays anonymous
    auto &twitch = app.accounts.twitch;
    twitch.accounts.insert(
        std::make_shared<TwitchAccount>("forsen", "token", "client", "22484632"));
    input.setAnonymousState({"Twitch", "Kick"});
    EXPECT_EQ(input.switchableAnonymousPlatforms(), QStringList{"Twitch"});
    EXPECT_TRUE(
        hasToolTip("Anonymous on Twitch and Kick. Click to disable on Twitch."));
    input.disableAnonymousMode();
    EXPECT_EQ(twitch.currentUsername.getValue(), "forsen");
    twitch.currentUsername = "";

    // Two saved accounts: the user has to pick, so no click
    twitch.accounts.insert(
        std::make_shared<TwitchAccount>("pajlada", "token", "client", "11148817"));
    input.setAnonymousState({"Twitch"});
    EXPECT_TRUE(input.switchableAnonymousPlatforms().isEmpty());
    EXPECT_TRUE(hasToolTip("Anonymous on Twitch."));
    input.disableAnonymousMode();
    EXPECT_EQ(twitch.currentUsername.getValue(), "");

    // Logged in where the message goes: a normal box, no ghost
    input.setAnonymousState({});
    EXPECT_FALSE(input.isAnonymousBlocked());
    EXPECT_FALSE(editor->isReadOnly());
    input.insertText("Kappa");
    EXPECT_EQ(input.getInputText(), "Kappa");
    EXPECT_FALSE(std::ranges::any_of(
        input.findChildren<QLabel *>(), [](const QLabel *label) {
            return label->toolTip().startsWith("Anonymous");
        }));
}

TEST(SplitInputAnonymous, ClickingOrFocusingTheComposerDoesNotSelectASavedAccount)
{
    MockApplication app;
    Split split(nullptr);
    SplitInput input(&split);
    auto &accounts = app.accounts.twitch;
    accounts.accounts.append(std::make_shared<TwitchAccount>(
        "anonymous_input_test", "fake_token", "fake_client", "123"));
    accounts.currentUsername = "";
    input.setAnonymousState({"Twitch"});
    auto *editor = input.findChild<QTextEdit *>();
    ASSERT_TRUE(editor);
    EXPECT_EQ(editor->viewport()->cursor().shape(), Qt::ArrowCursor);
    QFocusEvent focus(QEvent::FocusIn);
    QCoreApplication::sendEvent(editor, &focus);
    QMouseEvent click(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5),
                      Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(editor->viewport(), &click);
    EXPECT_TRUE(accounts.currentUsername.getValue().isEmpty());
    EXPECT_TRUE(editor->isReadOnly());
    EXPECT_TRUE(input.isAnonymousBlocked());
    input.insertText("must remain blocked");
    EXPECT_TRUE(input.getInputText().isEmpty());

    QLabel *ghost = nullptr;
    for (auto *label : input.findChildren<QLabel *>())
        if (label->toolTip() == "Anonymous on Twitch. Click to disable.") ghost = label;
    ASSERT_TRUE(ghost);
    QMouseEvent ghostClick(QEvent::MouseButtonRelease, QPointF(5, 5), QPointF(5, 5),
                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(ghost, &ghostClick);
    EXPECT_EQ(accounts.currentUsername.getValue(), "anonymous_input_test");
}

TEST(SplitInputAppearance, DefaultDarkRestoresOriginalComposerPalette)
{
    MockApplication app;
    app.theme.themeName = "Dark";
    app.theme.appearanceMode = "Dark";
    Split split(nullptr);
    SplitInput input(&split);
    // Apply the startup theme event after constructing the mock widgets.
    app.theme.update();
    const auto *editor = input.findChild<QTextEdit *>();
    ASSERT_NE(editor, nullptr);
    const auto expectClassicPalette = [&] {
        EXPECT_EQ(editor->palette().color(QPalette::PlaceholderText),
                  QColor("#5d5555"));
        EXPECT_EQ(editor->palette().color(QPalette::Text), QColor("#ffffff"));
        EXPECT_EQ(editor->palette().color(QPalette::Highlight),
                  QColor("#555555"));
        EXPECT_EQ(editor->palette().color(QPalette::Base), QColor("#242424"));
    };
    expectClassicPalette();
    app.theme.themeName = "preset.crimson.dark";
    app.theme.appearanceMode = "Light";
    auto placeholder = app.theme.splits.input.text;
    placeholder.setAlpha(150);
    EXPECT_EQ(editor->palette().color(QPalette::PlaceholderText), placeholder);
    EXPECT_EQ(editor->palette().color(QPalette::Highlight),
              app.theme.splits.input.border);
    app.theme.themeName = "Dark";
    app.theme.appearanceMode = "Dark";
    expectClassicPalette();
}
