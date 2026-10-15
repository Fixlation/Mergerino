// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SettingsDialog.hpp"

#include "Application.hpp"
#include "common/Args.hpp"
#include "common/Common.hpp"
#include "common/QLogging.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/BaseWindow.hpp"
#include "widgets/helper/SettingsDialogTab.hpp"
#include "widgets/settingspages/AccountsPage.hpp"
#include "widgets/settingspages/CommandPage.hpp"
#include "widgets/settingspages/ExternalToolsPage.hpp"
#include "widgets/settingspages/FiltersPage.hpp"
#include "widgets/settingspages/GeneralPage.hpp"
#include "widgets/settingspages/HighlightingPage.hpp"
#include "widgets/settingspages/IgnoresPage.hpp"
#include "widgets/settingspages/KeyboardSettingsPage.hpp"
#include "widgets/settingspages/ModerationPage.hpp"
#include "widgets/settingspages/NicknamesPage.hpp"
#include "widgets/settingspages/NotificationPage.hpp"
#include "widgets/settingspages/PluginsPage.hpp"
#include "widgets/settingspages/ThemesPage.hpp"

#include <QApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QFile>
#include <QFontMetrics>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QPaintEvent>
#include <QShortcut>
#include <QShowEvent>
#include <QSize>
#include <QSizePolicy>
#include <QStyle>
#include <QStyleOptionButton>
#include <QUrl>
#include <QVariantAnimation>

namespace {

class SettingsDiscordButton final : public QPushButton
{
public:
    explicit SettingsDiscordButton(QWidget *parent = nullptr)
        : QPushButton(parent)
        , icon_(QStringLiteral(":/social/discord.svg"))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setFixedHeight(34);
        this->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
        this->setToolTip(QStringLiteral("Join the Mergerino Discord"));
        this->setText(this->normalText_);
        this->setAccessibleName(this->hoverText_);
        this->setAppearance(true);
        this->animation_.setDuration(160);
        this->animation_.setEasingCurve(QEasingCurve::InOutCubic);
        QObject::connect(&this->animation_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             this->hoverProgress_ = value.toReal();
                             this->updateAnimatedWidth();
                             this->update();
                         });
        QObject::connect(this, &QPushButton::clicked, this, [] {
            QDesktopServices::openUrl(
                QUrl(chatterino::LINK_MERGERINO_DISCORD.toString()));
        });

        this->updateAnimatedWidth();
    }

    void setAppearance(bool classicDark)
    {
        this->setStyleSheet(
            QStringLiteral(R"(
            QPushButton {
                background-color: %1;
                border: none;
                border-radius: 6px;
                color: %2;
                padding: 4px 10px;
                text-align: left;
            }
            QPushButton:hover {
                background-color: %3;
                color: %4;
            }
        )")
                .arg(
                    classicDark ? "rgba(255, 255, 255, 18)" : "palette(button)",
                    classicDark ? "white" : "palette(button-text)",
                    classicDark ? "rgba(88, 101, 242, 48)"
                                : "palette(highlight)",
                    classicDark ? "white" : "palette(highlighted-text)"));
    }

protected:
    void enterEvent(QEnterEvent *event) override
    {
        QPushButton::enterEvent(event);
        this->animateTo(1.0);
    }

    void leaveEvent(QEvent *event) override
    {
        QPushButton::leaveEvent(event);
        this->animateTo(0.0);
    }

    void showEvent(QShowEvent *event) override
    {
        QPushButton::showEvent(event);
        this->updateAnimatedWidth();
    }

    void changeEvent(QEvent *event) override
    {
        QPushButton::changeEvent(event);
        this->updateAnimatedWidth();
    }

    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)

        QPainter painter(this);
        QStyleOptionButton option;
        this->initStyleOption(&option);
        option.text.clear();
        option.icon = QIcon();
        this->style()->drawControl(QStyle::CE_PushButton, &option, &painter,
                                   this);

        const int iconSize = 16;
        const int leftPadding = 11;
        const int textSpacing = 7;
        const QRect iconRect(leftPadding, (this->height() - iconSize) / 2,
                             iconSize, iconSize);
        this->icon_.paint(&painter, iconRect, Qt::AlignCenter,
                          this->isEnabled() ? QIcon::Normal : QIcon::Disabled);

        const QRect textRect = this->rect().adjusted(
            leftPadding + iconSize + textSpacing, 0, -8, 0);
        const auto textColor = option.palette.color(QPalette::ButtonText);
        const auto drawText = [&](const QString &text, qreal opacity) {
            if (opacity <= 0.0)
            {
                return;
            }

            painter.setOpacity(opacity);
            painter.setPen(textColor);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                             this->fontMetrics().elidedText(
                                 text, Qt::ElideRight, textRect.width()));
        };

        drawText(this->normalText_, 1.0 - this->hoverProgress_);
        drawText(this->hoverText_, this->hoverProgress_);
        painter.setOpacity(1.0);
    }

private:
    int widthForText(const QString &text) const
    {
        return 11 + 16 + 7 + this->fontMetrics().horizontalAdvance(text) + 18;
    }

    void updateAnimatedWidth()
    {
        const int collapsedWidth = this->widthForText(this->normalText_);
        const int expandedWidth = this->widthForText(this->hoverText_);
        const auto progress = this->hoverProgress_;
        const int width = static_cast<int>(
            collapsedWidth + (expandedWidth - collapsedWidth) * progress + 0.5);
        this->setMinimumWidth(width);
        this->setMaximumWidth(width);
        this->updateGeometry();
    }

    void animateTo(qreal target)
    {
        if (this->animation_.state() == QAbstractAnimation::Running)
        {
            this->animation_.stop();
        }

        this->animation_.setStartValue(this->hoverProgress_);
        this->animation_.setEndValue(target);
        this->animation_.start();
    }

    const QString normalText_ = QStringLiteral("Discord");
    const QString hoverText_ = QStringLiteral("Join Mergerino");
    QIcon icon_;
    QVariantAnimation animation_;
    qreal hoverProgress_ = 0.0;
};

}  // namespace

namespace chatterino {

SettingsDialog::SettingsDialog(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::DisableCustomScaling,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
{
    this->setObjectName("SettingsDialog");
    this->setWindowTitle("Mergerino Settings");
    // Disable the ? button in the titlebar until we decide to use it
    this->setWindowFlags(this->windowFlags() &
                         ~Qt::WindowContextHelpButtonHint);

    this->resize(915, 600);
    this->initUi();
    this->addTabs();
    this->themeChangedEvent();

    this->addShortcuts();
    this->signalHolder_.managedConnect(getApp()->getHotkeys()->onItemsUpdated,
                                       [this]() {
                                           this->clearShortcuts();
                                           this->addShortcuts();
                                       });
}

void SettingsDialog::addShortcuts()
{
    this->setSearchPlaceholderText();
    HotkeyController::HotkeyMap actions{
        {"search",
         [this](std::vector<QString>) -> QString {
             this->ui_.search->setFocus();
             this->ui_.search->selectAll();
             return "";
         }},
        {"delete", nullptr},
        {"accept", nullptr},
        {"reject", nullptr},
        {"scrollPage", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);

    auto *cancelShortcut = new QShortcut(QKeySequence::Cancel, this);
    cancelShortcut->setContext(Qt::WindowShortcut);
    QObject::connect(cancelShortcut, &QShortcut::activated, this,
                     &SettingsDialog::onCancelClicked);
    this->shortcuts_.push_back(cancelShortcut);
}
void SettingsDialog::setSearchPlaceholderText()
{
    QString searchHotkey;
    auto searchSeq = getApp()->getHotkeys()->getDisplaySequence(
        HotkeyCategory::PopupWindow, "search");
    if (!searchSeq.isEmpty())
    {
        searchHotkey =
            "(" + searchSeq.toString(QKeySequence::SequenceFormat::NativeText) +
            ")";
    }
    this->ui_.search->setPlaceholderText("Find in settings... " + searchHotkey);
}

void SettingsDialog::initUi()
{
    auto outerBox = LayoutCreator<QWidget>(this->getLayoutContainer())
                        .setLayoutType<QVBoxLayout>()
                        .withoutSpacing();

    // TOP
    auto title = outerBox.emplace<PageHeader>();
    auto edit = LayoutCreator<PageHeader>(title.getElement())
                    .setLayoutType<QHBoxLayout>()
                    .withoutMargin()
                    .emplace<QLineEdit>()
                    .assign(&this->ui_.search);
    this->setSearchPlaceholderText();
    edit->setClearButtonEnabled(true);
    edit->findChild<QAbstractButton *>()->setIcon(
        QPixmap(":/buttons/clearSearch.png"));
    this->ui_.search->installEventFilter(this);

    this->searchFilterTimer_.setSingleShot(true);
    this->searchFilterTimer_.setInterval(75);
    QObject::connect(&this->searchFilterTimer_, &QTimer::timeout, this, [this] {
        this->filterElements(this->pendingFilterText_);
    });

    QObject::connect(edit.getElement(), &QLineEdit::textChanged, this,
                     &SettingsDialog::scheduleFilterElements);

    // CENTER
    auto centerBox =
        outerBox.emplace<QHBoxLayout>().withoutMargin().withoutSpacing();

    // left side (tabs)
    centerBox.emplace<QWidget>()
        .assign(&this->ui_.tabContainerContainer)
        .setLayoutType<QVBoxLayout>()
        .withoutMargin()
        .assign(&this->ui_.tabContainer);
    this->ui_.tabContainerContainer->setFixedWidth(
        static_cast<int>(150 * this->dpi_));

    // right side (pages)
    centerBox.emplace<QStackedLayout>()
        .assign(&this->ui_.pageStack)
        .withoutMargin();

    this->ui_.pageStack->setContentsMargins(0, 0, 0, 0);

    outerBox->addSpacing(12);

    // BOTTOM
    auto buttons = outerBox.emplace<QDialogButtonBox>(Qt::Horizontal);
    {
        this->ui_.okButton =
            buttons->addButton("Ok", QDialogButtonBox::YesRole);
        this->ui_.cancelButton =
            buttons->addButton("Cancel", QDialogButtonBox::NoRole);
    }

    // ---- misc
    this->ui_.tabContainerContainer->setObjectName("tabWidget");
    this->ui_.pageStack->setObjectName("pages");

    QObject::connect(this->ui_.okButton, &QPushButton::clicked, this,
                     &SettingsDialog::onOkClicked);
    QObject::connect(this->ui_.cancelButton, &QPushButton::clicked, this,
                     &SettingsDialog::onCancelClicked);
}

void SettingsDialog::filterElements(const QString &text)
{
    // filter elements and hide pages
    for (auto &&tab : this->tabs_)
    {
        auto *page = tab->createdPage();
        const auto tabMatched = tab->matchesSearch(text);

        // filterElements returns true if anything on the page matches the search query
        const auto pageMatched = page != nullptr && page->filterElements(text);

        tab->setVisible(tabMatched || pageMatched);
    }

    // find next visible page
    if (this->lastSelectedByUser_ && this->lastSelectedByUser_->isVisible())
    {
        this->selectTab(this->lastSelectedByUser_, false);
    }
    else if (!this->selectedTab_->isVisible())
    {
        for (auto &&tab : this->tabs_)
        {
            if (tab->isVisible())
            {
                this->selectTab(tab, false);
                break;
            }
        }
    }

    // remove duplicate spaces
    bool shouldShowSpace = false;

    for (int i = 0; i < this->ui_.tabContainer->count(); i++)
    {
        auto *item = this->ui_.tabContainer->itemAt(i);
        if (auto *x = dynamic_cast<QSpacerItem *>(item); x)
        {
            x->changeSize(10, shouldShowSpace ? 16 : 0);
            shouldShowSpace = false;
        }
        else if (item->widget())
        {
            shouldShowSpace |= item->widget()->isVisible();
        }
    }
}

void SettingsDialog::scheduleFilterElements(const QString &query)
{
    this->pendingFilterText_ = query;
    this->searchFilterTimer_.start();
}

void SettingsDialog::setElementFilter(const QString &query)
{
    this->ui_.search->setText(query);
}

bool SettingsDialog::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->ui_.search && event->type() == QEvent::KeyPress)
    {
        auto *keyEvent = dynamic_cast<QKeyEvent *>(event);
        if (keyEvent == QKeySequence::DeleteStartOfWord &&
            this->ui_.search->selectionLength() > 0)
        {
            this->ui_.search->backspace();
            return true;
        }
    }
    return false;
}

void SettingsDialog::addTabs()
{
    this->ui_.tabContainer->setSpacing(0);
    this->ui_.tabContainer->setContentsMargins(0, 20, 0, 20);

    // Constructors are wrapped in std::function to remove some strain from first time loading.

    // clang-format off
    this->addTab([]{return new GeneralPage;},          "General",        ":/settings/about.svg", SettingsTabId::General,
                 {"interface", "chat", "messages", "emotes", "streamer mode", "link previews", "browser integration", "appdata", "cache", "sound", "advanced", "search"});
    this->addTab([]{return new ThemesPage;},           "Themes",         ":/settings/themes.svg",
                 SettingsTabId::None, {"appearance", "theme", "preset", "color", "font", "share code", "custom"});
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new AccountsPage;},         "Accounts",       ":/settings/accounts.svg", SettingsTabId::Accounts,
                 {"twitch", "kick", "login", "account", "oauth", "7tv", "cosmetics", "paint", "badge", "emotes"});
    this->addTab([]{return new NicknamesPage;},        "Nicknames",      ":/settings/accounts.svg",
                 SettingsTabId::None, {"username", "nickname", "regex"});
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new CommandPage;},          "Commands",       ":/settings/commands.svg",
                 SettingsTabId::None, {"command", "custom command"});
    this->addTab([]{return new HighlightingPage;},     "Highlights",     ":/settings/notifications.svg",
                 SettingsTabId::None, {"highlight", "messages", "users", "badges", "blacklisted users", "sound", "regex"});
    this->addTab([]{return new IgnoresPage;},          "Ignores",        ":/settings/ignore.svg",
                 SettingsTabId::None, {"ignore", "blocked users", "block", "unblock", "messages", "users", "replacement", "regex"});
    this->addTab([]{return new FiltersPage;},          "Filters",        ":/settings/filters.svg",
                 SettingsTabId::None, {"filter"});
    this->ui_.tabContainer->addSpacing(16);
    this->addTab([]{return new KeyboardSettingsPage;}, "Hotkeys",        ":/settings/keybinds.svg",
                 SettingsTabId::None, {"keybinds", "keyboard", "shortcuts"});
    this->addTab([]{return new ModerationPage;},       "Moderation",     ":/settings/moderation.svg", SettingsTabId::Moderation,
                 {"logs", "logging", "log", "channels", "users", "moderation buttons", "moderation mode", "timeout", "user timeout buttons", "twitch timestamps", "stream logs"});
    this->addTab([]{return new NotificationPage;},     "Live Notifications",  ":/settings/notification2.svg",
                 SettingsTabId::None, {"notification", "live"});
    this->addTab([]{return new ExternalToolsPage;},    "External tools", ":/settings/externaltools.svg",
                 SettingsTabId::None, {"external", "tools", "streamlink", "browser", "url", "links", "obs", "overlay", "browser source", "stream", "font", "emotes", "badges", "transparent"});
#ifdef CHATTERINO_HAVE_PLUGINS
    this->addTab([]{return new PluginsPage;},          "Plugins",        ":/settings/plugins.svg",
                 SettingsTabId::None, {"plugin"});
#endif
    this->ui_.tabContainer->addStretch(1);

    auto *discordButton = new SettingsDiscordButton(this);
    this->ui_.tabContainer->addWidget(discordButton, 0, Qt::AlignBottom);
    // clang-format on
}

void SettingsDialog::addTab(std::function<SettingsPage *()> page,
                            const QString &name, const QString &iconPath,
                            SettingsTabId id, QStringList searchKeywords,
                            Qt::Alignment alignment)
{
    auto *tab = new SettingsDialogTab(this, std::move(page), name, iconPath, id,
                                      std::move(searchKeywords));
    tab->setFixedHeight(static_cast<int>(30 * this->dpi_));

    this->ui_.tabContainer->addWidget(tab, 0, alignment);
    this->tabs_.push_back(tab);

    if (this->tabs_.size() == 1)
    {
        this->selectTab(tab);
    }
}

void SettingsDialog::selectTab(SettingsDialogTab *tab, bool byUser)
{
    // add page if it's not been added yet
    [&] {
        for (int i = 0; i < this->ui_.pageStack->count(); i++)
        {
            if (this->ui_.pageStack->itemAt(i)->widget() == tab->page())
            {
                return;
            }
        }

        this->ui_.pageStack->addWidget(tab->page());
    }();

    this->ui_.pageStack->setCurrentWidget(tab->page());

    if (this->selectedTab_ != nullptr)
    {
        this->selectedTab_->setSelected(false);
    }

    tab->setSelected(true);
    this->selectedTab_ = tab;
    this->themeChangedEvent();
    if (byUser)
    {
        this->lastSelectedByUser_ = tab;
    }
}

void SettingsDialog::selectTab(SettingsTabId id)
{
    auto *t = this->tab(id);
    assert(t);
    if (!t)
    {
        return;
    }

    this->selectTab(t);
}

SettingsDialogTab *SettingsDialog::tab(SettingsTabId id)
{
    for (auto &&tab : this->tabs_)
    {
        if (tab->id() == id)
        {
            return tab;
        }
    }

    assert(false);
    return nullptr;
}

void SettingsDialog::showDialog(QWidget *parent,
                                SettingsDialogPreference preferredTab)
{
    static SettingsDialog *instance = new SettingsDialog(parent);
    static bool hasShownBefore = false;
    if (hasShownBefore)
    {
        instance->refresh();
    }
    hasShownBefore = true;

    // Resets the cancel button.
    getSettings()->saveSnapshot();

    switch (preferredTab)
    {
        case SettingsDialogPreference::Accounts:
            instance->selectTab(SettingsTabId::Accounts);
            break;

        case SettingsDialogPreference::ModerationActions:
            if (auto *tab = instance->tab(SettingsTabId::Moderation))
            {
                instance->selectTab(tab);
                if (auto *page = dynamic_cast<ModerationPage *>(tab->page()))
                {
                    page->selectModerationActions();
                }
            }
            break;

        case SettingsDialogPreference::StreamerMode: {
            instance->selectTab(SettingsTabId::General);
        }
        break;

        default:;
    }

    instance->show();
    if (preferredTab == SettingsDialogPreference::StreamerMode)
    {
        // this is needed because each time the settings are opened, the query is reset
        instance->setElementFilter("Streamer Mode");
    }
    instance->activateWindow();
    instance->raise();
    instance->setFocus();
}

void SettingsDialog::refresh()
{
    // Updates tabs.
    for (auto *tab : this->tabs_)
    {
        if (auto *page = tab->createdPage())
        {
            page->onShow();
        }
    }
}

void SettingsDialog::scaleChangedEvent(float newScale)
{
    assert(newScale == 1.F &&
           "Scaling is disabled for the settings dialog - its scale should "
           "always be 1");

    for (SettingsDialogTab *tab : this->tabs_)
    {
        tab->setFixedHeight(30);
    }

    if (this->ui_.tabContainerContainer)
    {
        this->ui_.tabContainerContainer->setFixedWidth(150);
    }
}

void SettingsDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    const auto classicDark = this->theme->themeName.getValue() == "Dark" &&
                             !this->theme->isLightTheme();
    const auto background = this->theme->window.background;
    const auto text = this->theme->window.text;
    // Keep the original Fusion control palette for Default/Dark. Chat-input
    // colours are not a suitable replacement for native button/bevel roles.
    auto palette = classicDark ? QApplication::palette() : this->theme->palette;
    const auto panel = classicDark ? palette.color(QPalette::Window)
                                   : this->theme->messages.backgrounds.regular;
    const auto accent = this->theme->accent;
    const auto secondary = this->theme->messages.textColors.system;
    if (!classicDark)
    {
        const auto button = this->theme->splits.header.focusedBackground;
        palette.setColor(QPalette::Button, button);
        palette.setColor(QPalette::Light, button.lighter(130));
        palette.setColor(QPalette::Midlight, button.lighter(110));
        palette.setColor(QPalette::Mid, button.darker(130));
        palette.setColor(QPalette::Dark, button.darker(160));
        palette.setColor(QPalette::Shadow, button.darker(200));
    }
    palette.setColor(QPalette::Window, background);
    if (!classicDark)
    {
        palette.setColor(QPalette::WindowText, text);
    }
    const auto selectionText = palette.color(QPalette::HighlightedText);
    this->overrideBackgroundColor_ = background;

    QFile styleFile(":/qss/settings.qss");
    if (!styleFile.open(QFile::ReadOnly))
    {
        qCWarning(chatterinoWidget) << "Settings stylesheet not available";
        return;
    }

    auto stylesheet = QString::fromUtf8(styleFile.readAll());
    if (!classicDark)
    {
        stylesheet +=
            QStringLiteral(R"(
        chatterino--SettingsDialog {
            background-color: %1;
            color: %2;
        }
        chatterino--SettingsPage, QWidget#tabWidget {
            background-color: %1;
            color: %2;
        }
        chatterino--SettingsPage #generalSettingsScrollContent {
            background-color: %3;
            color: %2;
        }
        QScrollArea {
            background-color: %3;
            color: %2;
        }
        QLabel, QCheckBox, QRadioButton, QGroupBox {
            color: %2;
        }
        chatterino--SettingsPage QToolTip, QToolTip {
            background-color: %1;
            color: %2;
            border: 1px solid %7;
        }
        QComboBox QFrame, QComboBox QAbstractItemView {
            background-color: %1;
            color: %2;
            border: none;
            selection-background-color: %4;
            selection-color: %6;
        }
        chatterino--TitleLabel, chatterino--SubtitleLabel {
            color: %4;
        }
        chatterino--DescriptionLabel, QLabel#description {
            color: %5;
        }
        chatterino--NavigationLabel {
            color: %4;
        }
    )")
                .arg(background.name(QColor::HexArgb),
                     text.name(QColor::HexArgb), panel.name(QColor::HexArgb),
                     accent.name(QColor::HexArgb),
                     secondary.name(QColor::HexArgb),
                     selectionText.name(QColor::HexArgb),
                     palette.color(QPalette::Dark).name(QColor::HexArgb));
    }
    this->setStyleSheet(stylesheet);
    // Reapply after stylesheet repolishing, which may restore cached roles.
    this->setPalette(palette);
    for (auto *button : this->findChildren<QPushButton *>())
    {
        if (auto *discord = dynamic_cast<SettingsDiscordButton *>(button))
        {
            discord->setAppearance(classicDark);
        }
    }

    for (auto *tab : this->tabs_)
    {
        const auto selected = tab == this->selectedTab_;
        if (classicDark)
        {
            tab->setStyleSheet(selected
                                   ? "background: #222; color: #4FC3F7;"
                                   : "background: transparent; color: #FFF;");
            continue;
        }
        tab->setStyleSheet(QStringLiteral("background-color: %1; color: %2;")
                               .arg(selected ? panel.name(QColor::HexArgb)
                                             : QStringLiteral("transparent"),
                                    selected ? accent.name(QColor::HexArgb)
                                             : text.name(QColor::HexArgb)));
    }
}

void SettingsDialog::showEvent(QShowEvent *e)
{
    this->ui_.search->setText("");
    BaseWindow::showEvent(e);
}

///// Widget creation helpers
void SettingsDialog::onOkClicked()
{
    if (!getApp()->getArgs().dontSaveSettings)
    {
        getApp()->getCommands()->save();
    }

    getSettings()->requestSave();

    this->close();
}

void SettingsDialog::onCancelClicked()
{
    getSettings()->restoreSnapshot();

    this->close();
}

}  // namespace chatterino
