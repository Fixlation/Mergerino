// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/AccountsPage.hpp"

#include "Application.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/accounts/AccountModel.hpp"
#include "providers/seventv/SeventvAccountManager.hpp"
#include "providers/seventv/SeventvBrowserAuth.hpp"
#include "providers/twitch/TwitchCommon.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "widgets/dialogs/LoginDialog.hpp"
#include "widgets/dialogs/SeventvAccountDialog.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QDesktopServices>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QGuiApplication>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTableView>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

namespace chatterino {

AccountsPage::AccountsPage()
{
    auto *app = getApp();

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);

    auto *scrollContent = new QWidget(scrollArea);
    auto *layout = new QVBoxLayout(scrollContent);
    layout->setContentsMargins(0, 0, 0, 0);
    scrollArea->setWidget(scrollContent);
    rootLayout->addWidget(scrollArea);

    auto *view = new EditableModelView(
        app->getAccounts()->createModel(nullptr), false);
    layout->addWidget(view);

    view->getTableView()->horizontalHeader()->setVisible(false);
    view->getTableView()->horizontalHeader()->setStretchLastSection(true);

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([this] {
        LoginDialog d(this);
        d.exec();
    });

    view->getTableView()->setStyleSheet("background: #333");

    auto *seventvFrame = new QFrame(scrollContent);
    seventvFrame->setFrameShape(QFrame::StyledPanel);
    auto *seventvLayout = new QVBoxLayout(seventvFrame);
    seventvLayout->setContentsMargins(10, 8, 10, 8);
    seventvLayout->setSpacing(6);

    auto *seventvTitleRow = new QHBoxLayout;
    seventvTitleRow->setContentsMargins(0, 0, 0, 0);
    seventvTitleRow->setSpacing(8);
    auto *seventvLogo = new QLabel(seventvFrame);
    const bool dark =
        seventvFrame->palette().color(QPalette::Window).lightness() < 128;
    const auto seventvIcon =
        QIcon(dark ? QStringLiteral(":/buttons/seventv.svg")
                   : QStringLiteral(":/buttons/seventvDark.svg"));
    seventvLogo->setPixmap(seventvIcon.pixmap(QSize(33, 24)));
    seventvLogo->setFixedSize(37, 28);
    seventvLogo->setAlignment(Qt::AlignCenter);
    seventvLogo->setToolTip(QStringLiteral("7TV"));
    seventvTitleRow->addWidget(seventvLogo);
    auto *seventvTitle =
        new QLabel(QStringLiteral("Account & cosmetics"), seventvFrame);
    seventvTitle->setStyleSheet(
        QStringLiteral("QLabel { font-weight: 700; }"));
    seventvTitleRow->addWidget(seventvTitle);
    seventvTitleRow->addStretch(1);
    seventvLayout->addLayout(seventvTitleRow);

    auto *seventvDescription = new QLabel(
        QStringLiteral("Manage owned paints, badges, channel-specific "
                       "cosmetics and your 7TV emote set."),
        seventvFrame);
    seventvDescription->setWordWrap(true);
    seventvLayout->addWidget(seventvDescription);

    auto *seventvRow = new QHBoxLayout;
    seventvRow->setContentsMargins(0, 0, 0, 0);
    seventvRow->setSpacing(8);
    this->seventvStatusLabel_ = new QLabel(seventvFrame);
    this->seventvManageButton_ =
        new QPushButton(QStringLiteral("Manage"), seventvFrame);
    this->seventvManageButton_->setIcon(seventvIcon);
    seventvRow->addWidget(this->seventvStatusLabel_, 1);
    seventvRow->addWidget(this->seventvManageButton_);
    seventvLayout->addLayout(seventvRow);
    layout->addWidget(seventvFrame);

    QObject::connect(this->seventvManageButton_, &QPushButton::clicked, this,
                     [this] {
                         auto &manager = SeventvAccountManager::instance();
                         if (manager.isLoggedIn())
                         {
                             SeventvAccountDialog::showDialog(this);
                         }
                         else
                         {
                             this->seventvError_.clear();
                             manager.beginSignIn();
                         }
                     });
    auto &seventv = SeventvAccountManager::instance();
    this->managedConnections_.managedConnect(seventv.stateChanged, [this] {
        this->updateSeventvStatus();
    });
    this->managedConnections_.managedConnect(seventv.busyChanged, [this](bool) {
        this->updateSeventvStatus();
    });
    this->managedConnections_.managedConnect(
        seventv.feedback,
        [this](const QString &message, bool isError) {
            this->seventvError_ = isError ? message : QString{};
            this->updateSeventvStatus();
        });
    this->updateSeventvStatus();

    auto *moderationAuthFrame = new QFrame(scrollContent);
    moderationAuthFrame->setFrameShape(QFrame::StyledPanel);
    auto *moderationAuthLayout = new QVBoxLayout(moderationAuthFrame);
    moderationAuthLayout->setContentsMargins(10, 8, 10, 8);
    moderationAuthLayout->setSpacing(6);

    auto *moderationAuthTitle = new QLabel(
        QStringLiteral("Twitch website features"), moderationAuthFrame);
    moderationAuthTitle->setStyleSheet(
        QStringLiteral("QLabel { font-weight: 700; }"));
    moderationAuthLayout->addWidget(moderationAuthTitle);

    auto *moderationAuthDescription = new QLabel(
        QStringLiteral(
            "Connect Twitch’s website session to change Twitch badges and name "
            "colour, and to create Twitch polls and predictions."),
        moderationAuthFrame);
    moderationAuthDescription->setWordWrap(true);
    moderationAuthLayout->addWidget(moderationAuthDescription);

    this->moderationAuthInstructionsLabel_ = new QLabel(
        QStringLiteral(
            "1. Click Copy Helper; twitch.tv opens.\n"
            "2. Sign in to the selected Twitch account.\n"
            "3. Press F12, open Console, paste the helper, and press Enter.\n"
            "4. Return to Mergerino and click Paste Token."),
        moderationAuthFrame);
    this->moderationAuthInstructionsLabel_->setWordWrap(true);
    this->moderationAuthInstructionsLabel_->setStyleSheet(
        QStringLiteral("QLabel { color: #9aa0a6; }"));
    moderationAuthLayout->addWidget(this->moderationAuthInstructionsLabel_);

    auto *moderationAuthButtons = new QHBoxLayout;
    moderationAuthButtons->setContentsMargins(0, 0, 0, 0);
    moderationAuthButtons->setSpacing(8);

    this->moderationAuthLoginButton_ =
        new QPushButton(QStringLiteral("Copy Helper"), moderationAuthFrame);
    this->moderationAuthCopyButton_ =
        new QPushButton(QStringLiteral("Paste Token"), moderationAuthFrame);
    this->moderationAuthClearButton_ =
        new QPushButton(QStringLiteral("Logout"), moderationAuthFrame);

    moderationAuthButtons->addWidget(this->moderationAuthLoginButton_);
    moderationAuthButtons->addWidget(this->moderationAuthCopyButton_);
    moderationAuthButtons->addWidget(this->moderationAuthClearButton_);
    moderationAuthButtons->addStretch(1);
    moderationAuthLayout->addLayout(moderationAuthButtons);

    this->moderationAuthCodeLabel_ =
        new QLabel(QStringLiteral("Helper: not copied"), moderationAuthFrame);
    this->moderationAuthCodeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { font-family: monospace; font-weight: 700; }"));
    moderationAuthLayout->addWidget(this->moderationAuthCodeLabel_);

    this->moderationAuthStatusLabel_ = new QLabel(moderationAuthFrame);
    this->moderationAuthStatusLabel_->setWordWrap(true);
    moderationAuthLayout->addWidget(this->moderationAuthStatusLabel_);

    layout->addWidget(moderationAuthFrame);

    QObject::connect(this->moderationAuthLoginButton_, &QPushButton::clicked,
                     this, [this] {
                         this->copyModerationAuthHelper();
                     });
    QObject::connect(this->moderationAuthCopyButton_, &QPushButton::clicked,
                     this, [this] {
                         this->pasteModerationAuthToken();
                     });
    QObject::connect(this->moderationAuthClearButton_, &QPushButton::clicked,
                     this, [this] {
                         TwitchModerationAuth::clearSavedAccount();
                         ++this->moderationAuthGeneration_;
                         this->moderationAuthHelperCopied_ = false;
                         this->moderationAuthInFlight_ = false;
                         this->updateModerationAuthStatus();
                     });

    this->managedConnections_.managedConnect(
        TwitchModerationAuth::accountChanged(), [this] {
            this->updateModerationAuthStatus();
        });

    this->updateModerationAuthStatus();

    auto *kickIdentityAuthFrame = new QFrame(scrollContent);
    kickIdentityAuthFrame->setFrameShape(QFrame::StyledPanel);
    auto *kickIdentityAuthLayout = new QVBoxLayout(kickIdentityAuthFrame);
    kickIdentityAuthLayout->setContentsMargins(10, 8, 10, 8);
    kickIdentityAuthLayout->setSpacing(6);

    auto *kickIdentityAuthTitle = new QLabel(
        QStringLiteral("Kick website features"), kickIdentityAuthFrame);
    kickIdentityAuthTitle->setStyleSheet(
        QStringLiteral("QLabel { font-weight: 700; }"));
    kickIdentityAuthLayout->addWidget(kickIdentityAuthTitle);

    auto *kickIdentityAuthDescription = new QLabel(
        QStringLiteral(
            "Connect Kick’s website session to change Kick badges and name "
            "colour, and to create Kick polls and predictions."),
        kickIdentityAuthFrame);
    kickIdentityAuthDescription->setWordWrap(true);
    kickIdentityAuthLayout->addWidget(kickIdentityAuthDescription);

    this->kickIdentityAuthInstructionsLabel_ = new QLabel(
        QStringLiteral(
            "1. Click Copy Helper; kick.com opens.\n"
            "2. Sign in to the selected Kick account.\n"
            "3. Press F12, open Console, paste the helper, and press Enter.\n"
            "4. Return to Mergerino and click Paste Token."),
        kickIdentityAuthFrame);
    this->kickIdentityAuthInstructionsLabel_->setWordWrap(true);
    this->kickIdentityAuthInstructionsLabel_->setStyleSheet(
        QStringLiteral("QLabel { color: #9aa0a6; }"));
    kickIdentityAuthLayout->addWidget(this->kickIdentityAuthInstructionsLabel_);

    auto *kickIdentityAuthButtons = new QHBoxLayout;
    kickIdentityAuthButtons->setContentsMargins(0, 0, 0, 0);
    kickIdentityAuthButtons->setSpacing(8);
    this->kickIdentityAuthHelperButton_ =
        new QPushButton(QStringLiteral("Copy Helper"), kickIdentityAuthFrame);
    this->kickIdentityAuthPasteButton_ =
        new QPushButton(QStringLiteral("Paste Token"), kickIdentityAuthFrame);
    this->kickIdentityAuthClearButton_ =
        new QPushButton(QStringLiteral("Logout"), kickIdentityAuthFrame);
    kickIdentityAuthButtons->addWidget(this->kickIdentityAuthHelperButton_);
    kickIdentityAuthButtons->addWidget(this->kickIdentityAuthPasteButton_);
    kickIdentityAuthButtons->addWidget(this->kickIdentityAuthClearButton_);
    kickIdentityAuthButtons->addStretch(1);
    kickIdentityAuthLayout->addLayout(kickIdentityAuthButtons);

    this->kickIdentityAuthCodeLabel_ =
        new QLabel(QStringLiteral("Helper: not copied"),
                   kickIdentityAuthFrame);
    this->kickIdentityAuthCodeLabel_->setStyleSheet(QStringLiteral(
        "QLabel { font-family: monospace; font-weight: 700; }"));
    kickIdentityAuthLayout->addWidget(this->kickIdentityAuthCodeLabel_);
    this->kickIdentityAuthStatusLabel_ = new QLabel(kickIdentityAuthFrame);
    this->kickIdentityAuthStatusLabel_->setWordWrap(true);
    kickIdentityAuthLayout->addWidget(this->kickIdentityAuthStatusLabel_);
    layout->addWidget(kickIdentityAuthFrame);

    QObject::connect(this->kickIdentityAuthHelperButton_,
                     &QPushButton::clicked, this, [this] {
                         this->copyKickIdentityAuthHelper();
                     });
    QObject::connect(this->kickIdentityAuthPasteButton_,
                     &QPushButton::clicked, this, [this] {
                         this->pasteKickIdentityAuthToken();
                     });
    QObject::connect(this->kickIdentityAuthClearButton_,
                     &QPushButton::clicked, this, [this] {
                         auto account =
                             getApp()->getAccounts()->kick.current();
                         if (account != nullptr && !account->isAnonymous())
                         {
                             account->setChatIdentityToken({});
                         }
                         ++this->kickIdentityAuthGeneration_;
                         this->kickIdentityAuthHelperCopied_ = false;
                         this->kickIdentityAuthInFlight_ = false;
                         this->updateKickIdentityAuthStatus();
                     });
    this->managedConnections_.managedConnect(
        getApp()->getAccounts()->kick.currentUserChanged, [this] {
            ++this->kickIdentityAuthGeneration_;
            this->kickIdentityAuthHelperCopied_ = false;
            this->kickIdentityAuthInFlight_ = false;
            this->updateKickIdentityAuthStatus();
        });
    this->updateKickIdentityAuthStatus();

    //    auto buttons = layout.emplace<QDialogButtonBox>();
    //    {
    //        this->addButton = buttons->addButton("Add",
    //        QDialogButtonBox::YesRole); this->removeButton =
    //        buttons->addButton("Remove", QDialogButtonBox::NoRole);
    //    }

    //    layout.emplace<AccountSwitchWidget>(this).assign(&this->accSwitchWidget);

    // ----
    //    QObject::connect(this->addButton, &QPushButton::clicked, []() {
    //        static auto loginWidget = new LoginWidget();
    //        loginWidget->show();
    //    });

    //    QObject::connect(this->removeButton, &QPushButton::clicked, [this] {
    //        auto selectedUser = this->accSwitchWidget->currentItem()->text();
    //        if (selectedUser == ANONYMOUS_USERNAME_LABEL) {
    //            // Do nothing
    //            return;
    //        }

    //        getApp()->getAccounts()->Twitch.removeUser(selectedUser);
    //    });
}

void AccountsPage::updateSeventvStatus()
{
    auto &manager = SeventvAccountManager::instance();
    if (manager.isLoggedIn())
    {
        this->seventvError_.clear();
        const auto name = manager.displayName().isEmpty()
                              ? QStringLiteral("7TV")
                              : manager.displayName();
        if (manager.isBusy() && manager.displayName().isEmpty())
        {
            this->seventvStatusLabel_->setText(
                QStringLiteral("Validating 7TV account…"));
        }
        else
        {
            this->seventvStatusLabel_->setText(
                manager.isBusy()
                    ? QStringLiteral("Connected as %1 — refreshing…").arg(name)
                    : QStringLiteral("Connected as %1").arg(name));
        }
        this->seventvStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: #47d16c; }"));
    }
    else if (SeventvBrowserAuth::instance().isRunning())
    {
        this->seventvStatusLabel_->setText(
            QStringLiteral("Waiting for 7TV sign-in in your browser…"));
        this->seventvStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: #9aa0a6; }"));
    }
    else if (!this->seventvError_.isEmpty())
    {
        this->seventvStatusLabel_->setText(this->seventvError_);
        this->seventvStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: #ff7b72; }"));
    }
    else
    {
        this->seventvStatusLabel_->setText(QStringLiteral("Not connected"));
        this->seventvStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: #9aa0a6; }"));
    }
    this->seventvManageButton_->setText(
        manager.isLoggedIn() ? QStringLiteral("Manage")
        : SeventvBrowserAuth::instance().isRunning()
            ? QStringLiteral("Connecting…")
                             : QStringLiteral("Connect account"));
}

void AccountsPage::updateModerationAuthStatus()
{
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setText(
            this->moderationAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
    }

    const auto account = TwitchModerationAuth::savedAccount();
    const bool hasAccount = account.isValid();
    const bool hasUsableAccount = account.supportsWebGql();
    if (!this->moderationAuthInFlight_ &&
        this->moderationAuthStatusLabel_ != nullptr)
    {
        if (hasUsableAccount)
        {
            this->moderationAuthStatusLabel_->setText(
                QStringLiteral("Logged in as %1.").arg(account.displayLabel()));
        }
        else if (hasAccount)
        {
            this->moderationAuthStatusLabel_->setText(QStringLiteral(
                "Saved activation login is not usable for Twitch website "
                "features. Copy the helper and paste the token."));
        }
        else
        {
            this->moderationAuthStatusLabel_->setText(QStringLiteral(
                "Not connected for Twitch website features."));
        }
        this->moderationAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(hasUsableAccount
                         ? QStringLiteral("#47d16c")
                         : hasAccount ? QStringLiteral("#ff7b72")
                                      : QStringLiteral("#9aa0a6")));
    }

    if (this->moderationAuthLoginButton_ != nullptr)
    {
        this->moderationAuthLoginButton_->setVisible(!hasUsableAccount);
        this->moderationAuthLoginButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
        this->moderationAuthLoginButton_->setText(
            hasUsableAccount ? QStringLiteral("Copy Helper Again")
                             : QStringLiteral("Copy Helper"));
    }
    if (this->moderationAuthCopyButton_ != nullptr)
    {
        this->moderationAuthCopyButton_->setVisible(!hasUsableAccount);
        this->moderationAuthCopyButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
    }
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthInstructionsLabel_ != nullptr)
    {
        this->moderationAuthInstructionsLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthClearButton_ != nullptr)
    {
        this->moderationAuthClearButton_->setEnabled(
            hasAccount || this->moderationAuthHelperCopied_ ||
            this->moderationAuthInFlight_);
    }
}

void AccountsPage::copyModerationAuthHelper()
{
    if (this->moderationAuthInFlight_)
    {
        return;
    }

    ++this->moderationAuthGeneration_;
    this->moderationAuthHelperCopied_ = true;
    TwitchModerationAuth::copyHelperToClipboard();
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://www.twitch.tv/")));
    this->finishModerationAuthLogin(
        QStringLiteral("Helper copied. On twitch.tv, press F12 to open "
                       "DevTools, switch to Console, paste it, then return "
                       "here and click Paste Token."),
        false);
}

void AccountsPage::pasteModerationAuthToken()
{
    if (this->moderationAuthInFlight_)
    {
        return;
    }

    const auto clipboardText = TwitchModerationAuth::clipboardText().trimmed();
    if (clipboardText.isEmpty() ||
        clipboardText.contains(QStringLiteral("localStorage")) ||
        clipboardText.contains(QStringLiteral("Mergerino token copied")))
    {
        this->finishModerationAuthLogin(
            QStringLiteral("Run the copied helper in the Twitch console first. "
                           "On twitch.tv, press F12 to open DevTools, switch "
                           "to Console, paste it, then click Paste Token."),
            true);
        return;
    }

    const auto payload = TwitchModerationAuth::parseClipboardPayload(clipboardText);
    if (payload.oauthToken.isEmpty())
    {
        this->finishModerationAuthLogin(
            QStringLiteral(
                "Clipboard text is not a Twitch token. Run the copied helper "
                "on twitch.tv, then click Paste Token."),
            true);
        return;
    }

    if (payload.oauthToken.size() > TwitchModerationAuth::maxTokenLength())
    {
        this->finishModerationAuthLogin(
            QStringLiteral(
                "Clipboard token is too long to be a Twitch token. Run the "
                "copied helper on twitch.tv and paste only the copied token."),
            true);
        return;
    }

    this->moderationAuthInFlight_ = true;
    const int generation = ++this->moderationAuthGeneration_;
    QPointer<AccountsPage> self(this);
    this->finishModerationAuthLogin(QStringLiteral("Validating Twitch token..."),
                                    false);
    TwitchModerationAuth::validateToken(
        payload.oauthToken,
        [self, generation, payload](TwitchModerationAuth::Account account) {
            if (self == nullptr)
            {
                return;
            }

            QTimer::singleShot(0, self.data(),
                               [self, generation, payload, account]() mutable {
                if (self == nullptr ||
                    generation != self->moderationAuthGeneration_)
                {
                    return;
                }

                self->moderationAuthInFlight_ = false;
                if (!account.supportsWebGql())
                {
                    self->finishModerationAuthLogin(
                        QStringLiteral(
                            "That token is not a Twitch browser token. Run the "
                            "copied helper on twitch.tv."),
                        true);
                    return;
                }

                account.clientIntegrity = payload.clientIntegrity;
                account.deviceId = payload.deviceId;
                TwitchModerationAuth::saveAccount(account);
                self->finishModerationAuthLogin(
                    QStringLiteral("Logged in as %1.")
                        .arg(account.displayLabel()),
                    false);
                self->updateModerationAuthStatus();
            });
        },
        [self, generation](const QString &error) {
            if (self == nullptr)
            {
                return;
            }

            QTimer::singleShot(0, self.data(), [self, generation, error] {
                if (self == nullptr ||
                    generation != self->moderationAuthGeneration_)
                {
                    return;
                }

                self->moderationAuthInFlight_ = false;
                self->finishModerationAuthLogin(error, true);
            });
        });
}

void AccountsPage::finishModerationAuthLogin(const QString &message,
                                             bool isError)
{
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setText(
            this->moderationAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
    }

    if (this->moderationAuthStatusLabel_ != nullptr)
    {
        this->moderationAuthStatusLabel_->setText(message);
        this->moderationAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(isError ? QStringLiteral("#ff7b72")
                             : QStringLiteral("#9aa0a6")));
    }

    const auto account = TwitchModerationAuth::savedAccount();
    const bool hasAccount = account.isValid();
    const bool hasUsableAccount = account.supportsWebGql();
    if (this->moderationAuthLoginButton_ != nullptr)
    {
        this->moderationAuthLoginButton_->setVisible(!hasUsableAccount);
        this->moderationAuthLoginButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
        this->moderationAuthLoginButton_->setText(
            hasUsableAccount ? QStringLiteral("Copy Helper Again")
                             : QStringLiteral("Copy Helper"));
    }
    if (this->moderationAuthCopyButton_ != nullptr)
    {
        this->moderationAuthCopyButton_->setVisible(!hasUsableAccount);
        this->moderationAuthCopyButton_->setEnabled(
            !hasUsableAccount && !this->moderationAuthInFlight_);
    }
    if (this->moderationAuthCodeLabel_ != nullptr)
    {
        this->moderationAuthCodeLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthInstructionsLabel_ != nullptr)
    {
        this->moderationAuthInstructionsLabel_->setVisible(!hasUsableAccount);
    }
    if (this->moderationAuthClearButton_ != nullptr)
    {
        this->moderationAuthClearButton_->setEnabled(
            hasAccount || this->moderationAuthHelperCopied_ ||
            this->moderationAuthInFlight_);
    }
}

void AccountsPage::updateKickIdentityAuthStatus()
{
    const auto account = getApp()->getAccounts()->kick.current();
    const bool hasAccount =
        account != nullptr && !account->isAnonymous();
    const bool connected =
        hasAccount && !account->chatIdentityToken().isEmpty();

    if (this->kickIdentityAuthCodeLabel_ != nullptr)
    {
        this->kickIdentityAuthCodeLabel_->setText(
            this->kickIdentityAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
        this->kickIdentityAuthCodeLabel_->setVisible(hasAccount && !connected);
    }
    if (this->kickIdentityAuthInstructionsLabel_ != nullptr)
    {
        this->kickIdentityAuthInstructionsLabel_->setVisible(hasAccount &&
                                                             !connected);
    }
    if (!this->kickIdentityAuthInFlight_ &&
        this->kickIdentityAuthStatusLabel_ != nullptr)
    {
        if (connected)
        {
            this->kickIdentityAuthStatusLabel_->setText(
                QStringLiteral("Connected as %1 for Kick website features.")
                    .arg(account->username()));
        }
        else if (hasAccount)
        {
            this->kickIdentityAuthStatusLabel_->setText(QStringLiteral(
                "Not connected for Kick website features."));
        }
        else
        {
            this->kickIdentityAuthStatusLabel_->setText(QStringLiteral(
                "Log in to a Kick account above first."));
        }
        this->kickIdentityAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(connected ? QStringLiteral("#47d16c")
                               : QStringLiteral("#9aa0a6")));
    }

    if (this->kickIdentityAuthHelperButton_ != nullptr)
    {
        this->kickIdentityAuthHelperButton_->setVisible(hasAccount &&
                                                        !connected);
        this->kickIdentityAuthHelperButton_->setEnabled(
            hasAccount && !connected && !this->kickIdentityAuthInFlight_);
    }
    if (this->kickIdentityAuthPasteButton_ != nullptr)
    {
        this->kickIdentityAuthPasteButton_->setVisible(hasAccount &&
                                                       !connected);
        this->kickIdentityAuthPasteButton_->setEnabled(
            hasAccount && !connected && !this->kickIdentityAuthInFlight_);
    }
    if (this->kickIdentityAuthClearButton_ != nullptr)
    {
        this->kickIdentityAuthClearButton_->setVisible(connected);
        this->kickIdentityAuthClearButton_->setEnabled(
            connected && !this->kickIdentityAuthInFlight_);
    }
}

void AccountsPage::copyKickIdentityAuthHelper()
{
    if (this->kickIdentityAuthInFlight_)
    {
        return;
    }
    const auto account = getApp()->getAccounts()->kick.current();
    if (account == nullptr || account->isAnonymous())
    {
        this->finishKickIdentityAuth(
            QStringLiteral("Log in to a Kick account above first."), true);
        return;
    }

    ++this->kickIdentityAuthGeneration_;
    this->kickIdentityAuthHelperCopied_ = true;
    QGuiApplication::clipboard()->setText(kickIdentityAuthHelper());
    QDesktopServices::openUrl(QUrl(QStringLiteral("https://kick.com/")));
    this->finishKickIdentityAuth(
        QStringLiteral(
            "Helper copied. On kick.com, press F12 to open DevTools, switch "
            "to Console, paste it, then return here and click Paste Token."),
        false);
}

void AccountsPage::pasteKickIdentityAuthToken()
{
    if (this->kickIdentityAuthInFlight_)
    {
        return;
    }
    const auto account = getApp()->getAccounts()->kick.current();
    if (account == nullptr || account->isAnonymous())
    {
        this->finishKickIdentityAuth(
            QStringLiteral("Log in to a Kick account above first."), true);
        return;
    }

    const auto clipboardText =
        QGuiApplication::clipboard()->text().trimmed();
    if (clipboardText.isEmpty() ||
        clipboardText.contains(QStringLiteral("document.cookie")) ||
        clipboardText.contains(
            QStringLiteral("Mergerino Kick token copied")))
    {
        this->finishKickIdentityAuth(
            QStringLiteral(
                "Run the copied helper in the Kick console first, then click "
                "Paste Token."),
            true);
        return;
    }
    const auto token = parseKickIdentityToken(clipboardText);
    if (token.isEmpty())
    {
        this->finishKickIdentityAuth(
            QStringLiteral("Clipboard text is not a Kick session token."), true);
        return;
    }
    if (token.size() > 8192)
    {
        this->finishKickIdentityAuth(
            QStringLiteral("Clipboard token is too long to be a Kick session "
                           "token."),
            true);
        return;
    }

    this->kickIdentityAuthInFlight_ = true;
    const int generation = ++this->kickIdentityAuthGeneration_;
    const auto expectedUserID = account->userID();
    QPointer<AccountsPage> self(this);
    this->finishKickIdentityAuth(
        QStringLiteral("Validating Kick website session…"), false);
    getKickApi()->validateChatIdentityToken(
        token, expectedUserID,
        [self, generation, account,
         token](const ExpectedStr<void> &result) {
            if (self == nullptr ||
                generation != self->kickIdentityAuthGeneration_)
            {
                return;
            }
            self->kickIdentityAuthInFlight_ = false;
            if (!result)
            {
                self->finishKickIdentityAuth(result.error(), true);
                return;
            }
            account->setChatIdentityToken(token);
            if (QGuiApplication::clipboard()->text().trimmed() ==
                token ||
                parseKickIdentityToken(
                    QGuiApplication::clipboard()->text().trimmed()) == token)
            {
                QGuiApplication::clipboard()->clear();
            }
            self->kickIdentityAuthHelperCopied_ = false;
            self->finishKickIdentityAuth(
                QStringLiteral("Connected as %1 for Kick website features.")
                    .arg(account->username()),
                false);
            self->updateKickIdentityAuthStatus();
        });
}

void AccountsPage::finishKickIdentityAuth(const QString &message, bool isError)
{
    if (this->kickIdentityAuthCodeLabel_ != nullptr)
    {
        this->kickIdentityAuthCodeLabel_->setText(
            this->kickIdentityAuthHelperCopied_
                ? QStringLiteral("Helper: copied to clipboard")
                : QStringLiteral("Helper: not copied"));
    }
    if (this->kickIdentityAuthStatusLabel_ != nullptr)
    {
        this->kickIdentityAuthStatusLabel_->setText(message);
        this->kickIdentityAuthStatusLabel_->setStyleSheet(
            QStringLiteral("QLabel { color: %1; }")
                .arg(isError ? QStringLiteral("#ff7b72")
                             : QStringLiteral("#9aa0a6")));
    }

    const auto account = getApp()->getAccounts()->kick.current();
    const bool hasAccount =
        account != nullptr && !account->isAnonymous();
    const bool connected =
        hasAccount && !account->chatIdentityToken().isEmpty();
    if (this->kickIdentityAuthHelperButton_ != nullptr)
    {
        this->kickIdentityAuthHelperButton_->setVisible(hasAccount &&
                                                        !connected);
        this->kickIdentityAuthHelperButton_->setEnabled(
            hasAccount && !connected && !this->kickIdentityAuthInFlight_);
    }
    if (this->kickIdentityAuthPasteButton_ != nullptr)
    {
        this->kickIdentityAuthPasteButton_->setVisible(hasAccount &&
                                                       !connected);
        this->kickIdentityAuthPasteButton_->setEnabled(
            hasAccount && !connected && !this->kickIdentityAuthInFlight_);
    }
    if (this->kickIdentityAuthCodeLabel_ != nullptr)
    {
        this->kickIdentityAuthCodeLabel_->setVisible(hasAccount && !connected);
    }
    if (this->kickIdentityAuthInstructionsLabel_ != nullptr)
    {
        this->kickIdentityAuthInstructionsLabel_->setVisible(hasAccount &&
                                                             !connected);
    }
    if (this->kickIdentityAuthClearButton_ != nullptr)
    {
        this->kickIdentityAuthClearButton_->setVisible(connected);
        this->kickIdentityAuthClearButton_->setEnabled(
            connected && !this->kickIdentityAuthInFlight_);
    }
}

}  // namespace chatterino
