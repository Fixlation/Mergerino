// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/settingspages/SettingsPage.hpp"

#include <QStringListModel>

#include <vector>

class QVBoxLayout;
class QListView;
class QPushButton;
class QLabel;

namespace chatterino {

class IgnoresPage : public SettingsPage
{
public:
    IgnoresPage();

    void onShow() final;

private:
    void updateKickBlockedUsers();

    QStringListModel userListModel_;
    QStringListModel kickBlockedUserListModel_;
    pajlada::Settings::Setting<std::vector<QString>> kickBlockedUsersSetting_;
    QListView *kickBlockedUsersView_ = nullptr;
    QPushButton *unblockKickUsersButton_ = nullptr;
    QLabel *emptyKickBlocksLabel_ = nullptr;
};

}  // namespace chatterino
