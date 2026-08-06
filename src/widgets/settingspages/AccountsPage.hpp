// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QLabel>
#include <QPushButton>
#include <QString>

namespace chatterino {

class AccountSwitchWidget;

class AccountsPage : public SettingsPage
{
public:
    AccountsPage();

private:
    void updateSeventvStatus();
    void updateModerationAuthStatus();
    void copyModerationAuthHelper();
    void pasteModerationAuthToken();
    void finishModerationAuthLogin(const QString &message, bool isError);
    void updateKickIdentityAuthStatus();
    void copyKickIdentityAuthHelper();
    void pasteKickIdentityAuthToken();
    void finishKickIdentityAuth(const QString &message, bool isError);

    QLabel *seventvStatusLabel_ = nullptr;
    QPushButton *seventvManageButton_ = nullptr;
    QString seventvError_;
    QLabel *moderationAuthCodeLabel_ = nullptr;
    QLabel *moderationAuthInstructionsLabel_ = nullptr;
    QLabel *moderationAuthStatusLabel_ = nullptr;
    QPushButton *moderationAuthLoginButton_ = nullptr;
    QPushButton *moderationAuthCopyButton_ = nullptr;
    QPushButton *moderationAuthClearButton_ = nullptr;
    int moderationAuthGeneration_ = 0;
    bool moderationAuthHelperCopied_ = false;
    bool moderationAuthInFlight_ = false;
    QLabel *kickIdentityAuthCodeLabel_ = nullptr;
    QLabel *kickIdentityAuthInstructionsLabel_ = nullptr;
    QLabel *kickIdentityAuthStatusLabel_ = nullptr;
    QPushButton *kickIdentityAuthHelperButton_ = nullptr;
    QPushButton *kickIdentityAuthPasteButton_ = nullptr;
    QPushButton *kickIdentityAuthClearButton_ = nullptr;
    int kickIdentityAuthGeneration_ = 0;
    bool kickIdentityAuthHelperCopied_ = false;
    bool kickIdentityAuthInFlight_ = false;
};

}  // namespace chatterino
