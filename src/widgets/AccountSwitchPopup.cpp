// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/AccountSwitchPopup.hpp"

#include "Application.hpp"
#include "common/Common.hpp"
#include "common/Literals.hpp"
#include "common/ProviderId.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/seventv/SeventvAccountManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/AccountSwitchWidget.hpp"
#include "widgets/dialogs/KickLoginPage.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/dialogs/TwitchLoginPage.hpp"
#include "widgets/dialogs/YouTubeLoginPage.hpp"
#include "widgets/helper/KickAccountSwitchWidget.hpp"

#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QVBoxLayout>

namespace chatterino {

using namespace literals;

namespace {

constexpr QSize PROVIDER_BUTTON_SIZE{32, 24};
constexpr QSize PROVIDER_BUTTON_ICON_SIZE{16, 16};

QString providerName(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return "Kick";
        case ProviderId::YouTube:
            return "YouTube";
        case ProviderId::Twitch:
        default:
            return "Twitch";
    }
}

QString providerIconPath(ProviderId provider)
{
    switch (provider)
    {
        case ProviderId::Kick:
            return u":/platforms/kick.svg"_s;
        case ProviderId::YouTube:
            return u":/platforms/youtube.svg"_s;
        case ProviderId::Twitch:
        default:
            return u":/platforms/twitch.svg"_s;
    }
}

QString youtubeAccountLabel(const std::shared_ptr<YouTubeAccount> &account)
{
    if (account->displayName().isEmpty())
    {
        return account->channelID();
    }
    return account->displayName();
}

void refreshYouTubeAccountSwitcher(QListWidget *list)
{
    QSignalBlocker blocker(list);
    list->clear();

    auto *anonymous = new QListWidgetItem(ANONYMOUS_USERNAME_LABEL, list);
    anonymous->setData(Qt::UserRole, QString());

    const auto current = getApp()->getAccounts()->youtube.current();
    for (const auto &account : getApp()->getAccounts()->youtube.accounts.raw())
    {
        auto *item = new QListWidgetItem(youtubeAccountLabel(account), list);
        item->setData(Qt::UserRole, account->channelID());
    }

    if (current->isAnonymous())
    {
        list->setCurrentRow(0);
        return;
    }

    const auto channelID = current->channelID();
    for (int i = 0; i < list->count(); ++i)
    {
        if (list->item(i)->data(Qt::UserRole).toString() == channelID)
        {
            list->setCurrentRow(i);
            return;
        }
    }
}

}  // namespace

AccountSwitchPopup::AccountSwitchPopup(QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::TopMost,
              BaseWindow::Frameless,
              BaseWindow::DisableLayoutSave,
              BaseWindow::LinuxPopup,
          },
          parent)
{
    this->focusOutAction = FocusOutAction::Hide;

    this->setContentsMargins(0, 0, 0, 0);

    auto *vbox = new QVBoxLayout(this);
    vbox->setContentsMargins(10, 8, 10, 10);
    vbox->setSpacing(6);

    auto *providerHbox = new QHBoxLayout();
    providerHbox->setContentsMargins(0, 0, 0, 0);
    providerHbox->setSpacing(4);

    this->ui_.twitchProviderButton = new QPushButton(this);
    this->ui_.twitchProviderButton->setObjectName("providerButton");
    this->ui_.twitchProviderButton->setIcon(
        QIcon(providerIconPath(ProviderId::Twitch)));
    this->ui_.twitchProviderButton->setIconSize(PROVIDER_BUTTON_ICON_SIZE);
    this->ui_.twitchProviderButton->setCheckable(true);
    this->ui_.twitchProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.twitchProviderButton->setFixedSize(PROVIDER_BUTTON_SIZE);
    providerHbox->addWidget(this->ui_.twitchProviderButton);

    this->ui_.kickProviderButton = new QPushButton(this);
    this->ui_.kickProviderButton->setObjectName("providerButton");
    this->ui_.kickProviderButton->setIcon(
        QIcon(providerIconPath(ProviderId::Kick)));
    this->ui_.kickProviderButton->setIconSize(PROVIDER_BUTTON_ICON_SIZE);
    this->ui_.kickProviderButton->setCheckable(true);
    this->ui_.kickProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.kickProviderButton->setFixedSize(PROVIDER_BUTTON_SIZE);
    providerHbox->addWidget(this->ui_.kickProviderButton);

    this->ui_.youtubeProviderButton = new QPushButton(this);
    this->ui_.youtubeProviderButton->setObjectName("providerButton");
    this->ui_.youtubeProviderButton->setIcon(
        QIcon(providerIconPath(ProviderId::YouTube)));
    this->ui_.youtubeProviderButton->setIconSize(PROVIDER_BUTTON_ICON_SIZE);
    this->ui_.youtubeProviderButton->setCheckable(true);
    this->ui_.youtubeProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.youtubeProviderButton->setFixedSize(PROVIDER_BUTTON_SIZE);
    providerHbox->addWidget(this->ui_.youtubeProviderButton);

    this->ui_.sevenTVProviderButton = new QPushButton(this);
    this->ui_.sevenTVProviderButton->setObjectName("providerButton");
    this->ui_.sevenTVProviderButton->setIcon(
        QIcon(QStringLiteral(":/buttons/seventv.svg")));
    this->ui_.sevenTVProviderButton->setIconSize(PROVIDER_BUTTON_ICON_SIZE);
    this->ui_.sevenTVProviderButton->setCheckable(true);
    this->ui_.sevenTVProviderButton->setFocusPolicy(Qt::NoFocus);
    this->ui_.sevenTVProviderButton->setFixedSize(PROVIDER_BUTTON_SIZE);
    this->ui_.sevenTVProviderButton->setToolTip(
        QStringLiteral("7TV account"));
    providerHbox->addWidget(this->ui_.sevenTVProviderButton);
    providerHbox->addStretch(1);
    vbox->addLayout(providerHbox);

    this->ui_.accountStack = new QStackedWidget(this);
    vbox->addWidget(this->ui_.accountStack, 1);

    this->ui_.accountSwitchWidget = new AccountSwitchWidget(this);
    this->ui_.accountSwitchWidget->setFocusPolicy(Qt::NoFocus);
    this->ui_.kickAccountSwitcher = new KickAccountSwitchWidget(this);
    this->ui_.kickAccountSwitcher->setFocusPolicy(Qt::NoFocus);

    this->ui_.youtubeAccountPage = new QWidget(this);
    auto *youtubeAccountLayout =
        new QVBoxLayout(this->ui_.youtubeAccountPage);
    youtubeAccountLayout->setContentsMargins(0, 0, 0, 0);
    youtubeAccountLayout->setSpacing(0);
    this->ui_.youtubeReviewNotice =
        YouTubeLoginPage::createReviewNotice(this->ui_.youtubeAccountPage);
    youtubeAccountLayout->addWidget(this->ui_.youtubeReviewNotice, 1);

    this->ui_.youtubeAccountSwitcher =
        new QListWidget(this->ui_.youtubeAccountPage);
    this->ui_.youtubeAccountSwitcher->setFocusPolicy(Qt::NoFocus);
    youtubeAccountLayout->addWidget(this->ui_.youtubeAccountSwitcher, 1);

    this->ui_.sevenTVAccountPage = new QWidget(this);
    auto *sevenTVAccountLayout =
        new QVBoxLayout(this->ui_.sevenTVAccountPage);
    sevenTVAccountLayout->setContentsMargins(14, 14, 14, 14);
    this->ui_.sevenTVDescription = new QLabel(
        QStringLiteral(
            "Connect your 7TV account to manage cosmetics, emotes, and "
            "editor channels from Mergerino."),
        this->ui_.sevenTVAccountPage);
    this->ui_.sevenTVDescription->setAlignment(Qt::AlignCenter);
    this->ui_.sevenTVDescription->setWordWrap(true);

    this->ui_.sevenTVProfile = new QWidget(this->ui_.sevenTVAccountPage);
    auto *sevenTVProfileLayout = new QVBoxLayout(this->ui_.sevenTVProfile);
    sevenTVProfileLayout->setContentsMargins(0, 0, 0, 0);
    sevenTVProfileLayout->setSpacing(6);
    sevenTVProfileLayout->setAlignment(Qt::AlignCenter);
    this->ui_.sevenTVProfileIcon = new QLabel(this->ui_.sevenTVProfile);
    this->ui_.sevenTVProfileIcon->setAlignment(Qt::AlignCenter);
    this->ui_.sevenTVProfileIcon->setFixedHeight(48);
    sevenTVProfileLayout->addWidget(this->ui_.sevenTVProfileIcon);
    this->ui_.sevenTVProfileName = new QLabel(this->ui_.sevenTVProfile);
    this->ui_.sevenTVProfileName->setAlignment(Qt::AlignCenter);
    auto sevenTVNameFont = this->ui_.sevenTVProfileName->font();
    sevenTVNameFont.setBold(true);
    sevenTVNameFont.setPointSize(sevenTVNameFont.pointSize() + 2);
    this->ui_.sevenTVProfileName->setFont(sevenTVNameFont);
    sevenTVProfileLayout->addWidget(this->ui_.sevenTVProfileName);
    this->ui_.sevenTVProfileHandle = new QLabel(this->ui_.sevenTVProfile);
    this->ui_.sevenTVProfileHandle->setAlignment(Qt::AlignCenter);
    this->ui_.sevenTVProfileHandle->setStyleSheet(
        QStringLiteral("QLabel { color: #9aa0a6; }"));
    sevenTVProfileLayout->addWidget(this->ui_.sevenTVProfileHandle);
    this->ui_.sevenTVProfile->hide();

    sevenTVAccountLayout->addStretch(1);
    sevenTVAccountLayout->addWidget(this->ui_.sevenTVDescription);
    sevenTVAccountLayout->addWidget(this->ui_.sevenTVProfile);
    sevenTVAccountLayout->addStretch(1);

    this->ui_.accountStack->addWidget(this->ui_.accountSwitchWidget);
    this->ui_.accountStack->addWidget(this->ui_.kickAccountSwitcher);
    this->ui_.accountStack->addWidget(this->ui_.youtubeAccountPage);
    this->ui_.accountStack->addWidget(this->ui_.sevenTVAccountPage);

    this->ui_.statusLabel = new QLabel(this);
    this->ui_.statusLabel->setWordWrap(true);
    vbox->addWidget(this->ui_.statusLabel);

    auto *hbox = new QHBoxLayout();
    this->ui_.loginButton = new QPushButton(this);
    this->ui_.loginButton->setFocusPolicy(Qt::NoFocus);
    hbox->addWidget(this->ui_.loginButton, 1);

    this->ui_.manageAccountsButton = new QPushButton(this);
    this->ui_.manageAccountsButton->setText("Manage Accounts");
    this->ui_.manageAccountsButton->setFocusPolicy(Qt::NoFocus);
    hbox->addWidget(this->ui_.manageAccountsButton, 1);
    vbox->addLayout(hbox);

    QObject::connect(this->ui_.loginButton, &QPushButton::clicked, this,
                     [this]() {
                         if (this->sevenTVPageSelected_)
                         {
                             auto &sevenTV = SeventvAccountManager::instance();
                             if (sevenTV.isBusy())
                             {
                                 return;
                             }
                             if (sevenTV.isLoggedIn())
                             {
                                 sevenTV.logout();
                                 this->refresh();
                             }
                             else
                             {
                                 sevenTV.beginSignIn();
                                 this->hide();
                             }
                             return;
                         }

                         const auto provider =
                             getApp()->getWindows()->activeAccountProvider();
                         if (provider == ProviderId::YouTube)
                         {
                             auto current =
                                 getApp()->getAccounts()->youtube.current();
                             if (!current->isAnonymous())
                             {
                                 auto &youtubeAccounts =
                                     getApp()->getAccounts()->youtube;
                                 youtubeAccounts.accounts.removeFirstMatching(
                                     [&](const auto &account) {
                                         return account == current;
                                     });
                                 youtubeAccounts.currentChannelID = "";
                                 getSettings()->requestSave();
                                 this->refresh();
                                 return;
                             }

                             YouTubeLoginPage::startLoginFlow(
                                 this->parentWidget(),
                                 [this]() { this->refresh(); });
                             this->hide();
                             return;
                         }
                         if (provider == ProviderId::Kick)
                         {
                             auto current = getApp()->getAccounts()->kick.current();
                             if (!current->isAnonymous())
                             {
                                 auto &kickAccounts =
                                     getApp()->getAccounts()->kick;
                                 kickAccounts.accounts.removeFirstMatching(
                                     [&](const auto &account) {
                                         return account == current;
                                     });
                                 kickAccounts.currentUsername = "";
                                 getSettings()->requestSave();
                                 this->refresh();
                                 return;
                             }

                             KickLoginPage::startLoginFlow(
                                 this->parentWidget(),
                                 [this]() { this->refresh(); });
                             this->hide();
                             return;
                         }

                         auto current = getApp()->getAccounts()->twitch.getCurrent();
                         if (!current->isAnon())
                         {
                             auto &twitchAccounts =
                                 getApp()->getAccounts()->twitch;
                             twitchAccounts.accounts.removeFirstMatching(
                                 [&](const auto &account) {
                                     return account == current;
                                 });
                             twitchAccounts.currentUsername = "";
                             getSettings()->requestSave();
                             this->refresh();
                             return;
                         }

                         TwitchLoginPage::startLoginFlow(
                             this->parentWidget(),
                             [this]() { this->refresh(); });
                         this->hide();
                     });
    QObject::connect(this->ui_.twitchProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         this->sevenTVPageSelected_ = false;
                         getApp()->getWindows()->setActiveAccountProvider(
                             ProviderId::Twitch);
                         this->refresh();
                     });
    QObject::connect(this->ui_.kickProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         this->sevenTVPageSelected_ = false;
                         getApp()->getWindows()->setActiveAccountProvider(
                             ProviderId::Kick);
                         this->refresh();
                     });
    QObject::connect(this->ui_.youtubeProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         this->sevenTVPageSelected_ = false;
                         getApp()->getWindows()->setActiveAccountProvider(
                             ProviderId::YouTube);
                         this->refresh();
                     });
    QObject::connect(this->ui_.sevenTVProviderButton, &QPushButton::clicked,
                     this, [this]() {
                         this->sevenTVPageSelected_ = true;
                         this->refresh();
                     });
    QObject::connect(this->ui_.youtubeAccountSwitcher, &QListWidget::clicked,
                     this, [this] {
                         if (this->ui_.youtubeAccountSwitcher->selectedItems()
                                 .isEmpty())
                         {
                             return;
                         }

                         const auto channelID =
                             this->ui_.youtubeAccountSwitcher->currentItem()
                                 ->data(Qt::UserRole)
                                 .toString();
                         getApp()->getAccounts()->youtube.currentChannelID =
                             channelID;
                         std::ignore = getSettings()->requestSave();
                     });
    QObject::connect(this->ui_.manageAccountsButton, &QPushButton::clicked,
                     [this]() {
        SettingsDialog::showDialog(this->parentWidget(),
                                   SettingsDialogPreference::Accounts);
    });

    this->getLayoutContainer()->setLayout(vbox);

    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.userListUpdated,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.userListUpdated,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->youtube.currentUserChanged,
        [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        getApp()->getWindows()->activeAccountProviderChanged,
        [this](ProviderId) { this->refresh(); });
    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->refresh();
        }));
    this->signalHolder_.managedConnect(
        getApp()->getAccounts()->twitch.userListUpdated,
        [this]() { this->refresh(); });
    auto &sevenTV = SeventvAccountManager::instance();
    this->signalHolder_.managedConnect(sevenTV.stateChanged,
                                       [this]() { this->refresh(); });
    this->signalHolder_.managedConnect(
        sevenTV.busyChanged, [this](bool) { this->refresh(); });

    this->setScaleIndependentSize(300, 300);
    this->refresh();
    this->themeChangedEvent();
}

bool AccountSwitchPopup::wasHiddenRecently(int thresholdMs) const
{
    return this->lastHideTimer_.isValid() &&
           this->lastHideTimer_.elapsed() < thresholdMs;
}

void AccountSwitchPopup::hideEvent(QHideEvent *event)
{
    this->lastHideTimer_.restart();
    BaseWindow::hideEvent(event);
}

void AccountSwitchPopup::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();

    auto *t = getTheme();
    const auto sevenTVIconPath =
        t->isLightTheme() ? QStringLiteral(":/buttons/seventvDark.svg")
                          : QStringLiteral(":/buttons/seventv.svg");
    this->ui_.sevenTVProviderButton->setIcon(QIcon(sevenTVIconPath));
    if (this->ui_.sevenTVProfileIcon != nullptr)
    {
        this->ui_.sevenTVProfileIcon->setPixmap(
            QIcon(sevenTVIconPath).pixmap(QSize(44, 44)));
    }
    auto color = [](const QColor &c) {
        return c.name(QColor::HexArgb);
    };
    this->setStyleSheet(uR"(
        QListView {
            color: %1;
            background: %2;
        }
        QLabel {
            color: %1;
        }
        QListView::item:hover {
            background: %3;
        }
        QListView::item:selected {
            background: %4;
        }

        QPushButton {
            background: %5;
            color: %1;
        }
        QPushButton:checked {
            background: %4;
        }
        QPushButton:hover {
            background: %3;
        }
        QPushButton:pressed {
            background: %6;
        }
        QPushButton#providerButton {
            background: transparent;
            border: 1px solid #12555555;
            min-width: 32px;
            max-width: 32px;
            min-height: 24px;
            max-height: 24px;
            padding: 0;
        }
        QPushButton#providerButton[selectedProvider="true"] {
            background: transparent;
            border: 1px solid #6a6a6a;
        }
        QPushButton#providerButton:hover {
            background: transparent;
            border: 1px solid #24555555;
        }
        QPushButton#providerButton[selectedProvider="true"]:hover {
            background: transparent;
            border: 1px solid #777777;
        }
        QPushButton#providerButton[selectedProvider="true"]:pressed {
            background: transparent;
            border: 1px solid #858585;
        }

        chatterino--AccountSwitchPopup {
            background: %7;
        }
    )"_s.arg(color(t->window.text), color(t->splits.header.background),
             color(t->splits.header.focusedBackground), color(t->accent),
             color(t->tabs.regular.backgrounds.regular),
             color(t->tabs.selected.backgrounds.regular),
             color(t->window.background)));
}

void AccountSwitchPopup::refresh()
{
    this->ui_.accountSwitchWidget->refresh();
    this->ui_.kickAccountSwitcher->refresh();
    refreshYouTubeAccountSwitcher(this->ui_.youtubeAccountSwitcher);

    const auto hasYouTubeAccounts =
        !getApp()->getAccounts()->youtube.accounts.raw().empty();
    this->ui_.youtubeReviewNotice->setVisible(!hasYouTubeAccounts);
    this->ui_.youtubeAccountSwitcher->setVisible(hasYouTubeAccounts);

    this->updateCurrentPage();
    this->updateStatusText();
}

void AccountSwitchPopup::paintEvent(QPaintEvent *)
{
    QPainter painter(this);

    painter.setPen(QColor("#999"));
    painter.drawRect(0, 0, this->width() - 1, this->height() - 1);
}

void AccountSwitchPopup::updateCurrentPage()
{
    const auto provider = getApp()->getWindows()->activeAccountProvider();
    auto setProviderButtonSelected = [](QPushButton *button, bool selected) {
        button->setChecked(selected);
        button->setProperty("selectedProvider", selected);
        button->style()->unpolish(button);
        button->style()->polish(button);
        button->update();
    };

    if (this->sevenTVPageSelected_)
    {
        this->ui_.accountStack->setCurrentWidget(this->ui_.sevenTVAccountPage);
        setProviderButtonSelected(this->ui_.twitchProviderButton, false);
        setProviderButtonSelected(this->ui_.kickProviderButton, false);
        setProviderButtonSelected(this->ui_.youtubeProviderButton, false);
        setProviderButtonSelected(this->ui_.sevenTVProviderButton, true);

        const auto &sevenTV = SeventvAccountManager::instance();
        const bool loggedIn = sevenTV.isLoggedIn();
        this->ui_.loginButton->setEnabled(!sevenTV.isBusy());
        this->ui_.loginButton->setText(
            loggedIn ? QStringLiteral("Sign out of 7TV")
                     : QStringLiteral("Connect 7TV"));
        this->ui_.manageAccountsButton->setText(
            QStringLiteral("Manage 7TV account"));
        this->ui_.sevenTVDescription->setVisible(!loggedIn);
        this->ui_.sevenTVProfile->setVisible(loggedIn);
        if (loggedIn)
        {
            const auto displayName =
                sevenTV.displayName().isEmpty()
                    ? (sevenTV.username().isEmpty()
                           ? QStringLiteral("7TV")
                           : sevenTV.username())
                    : sevenTV.displayName();
            this->ui_.sevenTVProfileName->setText(displayName);
            const auto username = sevenTV.username().trimmed();
            this->ui_.sevenTVProfileHandle->setText(
                username.isEmpty() ? QString{}
                                   : QStringLiteral("@%1").arg(username));
            this->ui_.sevenTVProfileHandle->setVisible(!username.isEmpty());
        }
        return;
    }

    QWidget *accountWidget = this->ui_.accountSwitchWidget;
    switch (provider)
    {
        case ProviderId::Kick:
            accountWidget = this->ui_.kickAccountSwitcher;
            break;
        case ProviderId::YouTube:
            accountWidget = this->ui_.youtubeAccountPage;
            break;
        case ProviderId::Twitch:
            break;
    }
    this->ui_.accountStack->setCurrentWidget(accountWidget);
    this->ui_.manageAccountsButton->setText(QStringLiteral("Manage accounts"));
    setProviderButtonSelected(this->ui_.twitchProviderButton,
                              provider == ProviderId::Twitch);
    setProviderButtonSelected(this->ui_.kickProviderButton,
                              provider == ProviderId::Kick);
    setProviderButtonSelected(this->ui_.youtubeProviderButton,
                              provider == ProviderId::YouTube);
    setProviderButtonSelected(this->ui_.sevenTVProviderButton, false);

    this->ui_.loginButton->setEnabled(true);
    bool loggedIn = false;
    switch (provider)
    {
        case ProviderId::Kick:
            loggedIn = !getApp()->getAccounts()->kick.current()->isAnonymous();
            break;
        case ProviderId::YouTube:
            loggedIn =
                !getApp()->getAccounts()->youtube.current()->isAnonymous();
            break;
        case ProviderId::Twitch:
            loggedIn = !getApp()->getAccounts()->twitch.getCurrent()->isAnon();
            break;
    }
    this->ui_.loginButton->setText(
        QString("%1 %2")
            .arg(loggedIn ? "Log out of" : "Log in to")
            .arg(providerName(provider)));
}

void AccountSwitchPopup::updateStatusText()
{
    if (this->sevenTVPageSelected_)
    {
        const auto &sevenTV = SeventvAccountManager::instance();
        if (sevenTV.isLoggedIn())
        {
            const auto name =
                sevenTV.displayName().isEmpty()
                    ? (sevenTV.username().isEmpty()
                           ? QStringLiteral("7TV")
                           : sevenTV.username())
                    : sevenTV.displayName();
            this->ui_.statusLabel->setText(
                sevenTV.isBusy()
                    ? QStringLiteral("Connected 7TV account: %1 — working…")
                          .arg(name)
                    : QStringLiteral("Connected 7TV account: %1").arg(name));
        }
        else
        {
            this->ui_.statusLabel->setText(
                QStringLiteral(
                    "No 7TV account connected. Click below to sign in."));
        }
        return;
    }

    const auto provider = getApp()->getWindows()->activeAccountProvider();
    if (provider == ProviderId::Kick)
    {
        const auto usernames = getApp()->getAccounts()->kick.usernames();
        const auto current = getApp()->getAccounts()->kick.current();
        if (!current->isAnonymous())
        {
            this->ui_.statusLabel->setText(
                QString("Current Kick account: %1").arg(current->username()));
            return;
        }
        if (!usernames.empty())
        {
            this->ui_.statusLabel->setText(
                "Kick is currently set to anonymous. Select an account above "
                "or log in.");
            return;
        }

        this->ui_.statusLabel->setText(
            "No Kick account added yet. Click below to log in.");
        return;
    }

    if (provider == ProviderId::YouTube)
    {
        const auto &accounts = getApp()->getAccounts()->youtube.accounts.raw();
        const auto current = getApp()->getAccounts()->youtube.current();
        if (!current->isAnonymous())
        {
            this->ui_.statusLabel->setText(
                QString("Current YouTube account: %1")
                    .arg(youtubeAccountLabel(current)));
            return;
        }
        if (!accounts.empty())
        {
            this->ui_.statusLabel->setText(
                "YouTube is currently set to anonymous. Manage accounts to "
                "switch accounts or log in.");
            return;
        }

        this->ui_.statusLabel->setText(
            "No YouTube account added yet. Click below to log in.");
        return;
    }

    const auto usernames = getApp()->getAccounts()->twitch.getUsernames();
    const auto current = getApp()->getAccounts()->twitch.getCurrent();
    if (!current->isAnon())
    {
        this->ui_.statusLabel->setText(
            QString("Current Twitch account: %1").arg(current->getUserName()));
        return;
    }
    if (!usernames.empty())
    {
        this->ui_.statusLabel->setText(
            "Twitch is currently set to anonymous. Select an account above "
            "or log in.");
        return;
    }

    this->ui_.statusLabel->setText(
        "No Twitch account added yet. Click below to log in.");
}

}  // namespace chatterino
