// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/settingspages/IgnoresPage.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/ignores/IgnoreModel.hpp"
#include "controllers/ignores/IgnorePhrase.hpp"
#include "providers/kick/KickUserBlock.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchUser.hpp"
#include "singletons/Settings.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/EditableModelView.hpp"

#include <QCheckBox>
#include <QGroupBox>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

namespace chatterino {

using namespace literals;

static void addPhrasesTab(LayoutCreator<QVBoxLayout> box);
static void addUsersTab(IgnoresPage &page, LayoutCreator<QVBoxLayout> box,
                        QStringListModel &model);

IgnoresPage::IgnoresPage()
    : kickBlockedUsersSetting_(KICK_LOCAL_BLOCKED_USERS,
                               std::vector<QString>{})
{
    LayoutCreator<IgnoresPage> layoutCreator(this);
    auto layout = layoutCreator.setLayoutType<QVBoxLayout>();
    auto tabs = layout.emplace<QTabWidget>();

    addPhrasesTab(tabs.appendTab(new QVBoxLayout, "Messages"));
    addUsersTab(*this, tabs.appendTab(new QVBoxLayout, "Users"),
                this->userListModel_);

    auto blockedAccounts =
        tabs.appendTab(new QVBoxLayout, "Blocked accounts");
    auto help = blockedAccounts.emplace<QLabel>(
        "Kick accounts blocked in Mergerino. These blocks only hide messages "
        "here; they don't mute users on Kick. Unblocking takes effect "
        "immediately.");
    help->setWordWrap(true);
    blockedAccounts.emplace<QLabel>("No Kick accounts are blocked in Mergerino.")
        .assign(&this->emptyKickBlocksLabel_);
    auto list = blockedAccounts.emplace<QListView>();
    list.assign(&this->kickBlockedUsersView_);
    list->setObjectName("kickBlockedAccountsList");
    list->setAccessibleName("Blocked Kick accounts");
    list->setModel(&this->kickBlockedUserListModel_);
    list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    auto actions = blockedAccounts.emplace<QHBoxLayout>();
    actions.emplace<QPushButton>("Unblock selected")
        .assign(&this->unblockKickUsersButton_);
    this->unblockKickUsersButton_->setObjectName("unblockKickAccountsButton");
    actions->addStretch(1);
    QObject::connect(
        list->selectionModel(), &QItemSelectionModel::selectionChanged, this,
        [this] {
            this->unblockKickUsersButton_->setEnabled(
                this->kickBlockedUsersView_->selectionModel()->hasSelection());
        });
    QObject::connect(this->unblockKickUsersButton_, &QPushButton::clicked, this,
                     [this] {
                         // Collect names before changing the setting: each
                         // change refreshes the model and invalidates indexes.
                         QStringList users;
                         for (const auto &index :
                              this->kickBlockedUsersView_->selectionModel()
                                  ->selectedRows())
                         {
                             users.push_back(this->kickBlockedUserListModel_
                                                 .data(index)
                                                 .toString());
                         }
                         for (const auto &user : users)
                         {
                             setKickUserLocallyBlocked(user, false);
                         }
                     });
    this->kickBlockedUsersSetting_.connect(
        [this] { this->updateKickBlockedUsers(); }, this->managedConnections_);
    this->onShow();
}

void addPhrasesTab(LayoutCreator<QVBoxLayout> layout)
{
    layout.emplace<QLabel>("Ignore messages based certain patterns.");
    EditableModelView *view =
        layout
            .emplace<EditableModelView>(
                (new IgnoreModel(nullptr))
                    ->initialized(&getSettings()->ignoredMessages))
            .getElement();
    view->setTitles(
        {"Pattern", "Regex", "Case-sensitive", "Block", "Replacement"});
    view->getTableView()->horizontalHeader()->setSectionResizeMode(
        QHeaderView::Fixed);
    view->getTableView()->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::Stretch);
    view->addRegexHelpLink();

    QTimer::singleShot(1, view, [view] {
        view->getTableView()->resizeColumnsToContents();
        view->getTableView()->setColumnWidth(0, 200);
    });

    // We can safely ignore this signal connection since we own the view
    std::ignore = view->addButtonPressed.connect([] {
        getSettings()->ignoredMessages.append(IgnorePhrase{
            "my pattern",
            false,
            false,
            DEFAULT_IGNORE_PHRASE_REPLACE.toString(),
            true,
        });
    });
}

void addUsersTab(IgnoresPage &page, LayoutCreator<QVBoxLayout> users,
                 QStringListModel &userModel)
{
    auto label = users.emplace<QLabel>(
        u"/block <user> in chat blocks a user.\n/unblock <user> in chat unblocks a user.\nYou can also click on a user to open the usercard."_s);
    label->setWordWrap(true);
    users.append(page.createCheckBox("Enable Twitch blocked users",
                                     getSettings()->enableTwitchBlockedUsers));

    auto anyways = users.emplace<QHBoxLayout>().withoutMargin();
    {
        anyways.emplace<QLabel>("Show messages from blocked users:");

        auto *combo = anyways.emplace<QComboBox>().getElement();
        combo->addItems(
            {"Never", "If you are Moderator", "If you are Broadcaster"});

        auto &setting = getSettings()->showBlockedUsersMessages;

        setting.connect([combo](const int value) {
            combo->setCurrentIndex(value);
        });

        QObject::connect(combo,
                         QOverload<int>::of(&QComboBox::currentIndexChanged),
                         [&setting](int index) {
                             if (index != -1)
                             {
                                 setting = index;
                             }
                         });

        anyways->addStretch(1);
    }

    /*auto addremove = users.emplace<QHBoxLayout>().withoutMargin();
    {
        auto add = addremove.emplace<QPushButton>("Block user");
        auto remove = addremove.emplace<QPushButton>("Unblock User");
        addremove->addStretch(1);
    }*/

    users.emplace<QLabel>("List of blocked users:");
    users.emplace<QListView>()->setModel(&userModel);
}

void IgnoresPage::onShow()
{
    this->updateKickBlockedUsers();
    auto *app = getApp();

    auto user = app->getAccounts()->twitch.getCurrent();

    if (user->isAnon())
    {
        this->userListModel_.setStringList({});
        return;
    }

    QStringList users;
    users.reserve(user->blocks().size());

    for (const auto &blockedUser : user->blocks())
    {
        users << blockedUser.name;
    }
    users.sort(Qt::CaseInsensitive);
    this->userListModel_.setStringList(users);
}

void IgnoresPage::updateKickBlockedUsers()
{
    QStringList users;
    for (const auto &user : this->kickBlockedUsersSetting_.getValueCopy())
    {
        const auto normalized = user.trimmed().toCaseFolded();
        if (!normalized.isEmpty() && !users.contains(normalized))
        {
            users.push_back(normalized);
        }
    }
    users.sort(Qt::CaseInsensitive);
    if (this->kickBlockedUserListModel_.stringList() != users)
    {
        this->kickBlockedUserListModel_.setStringList(users);
    }
    this->emptyKickBlocksLabel_->setVisible(users.isEmpty());
    this->kickBlockedUsersView_->setVisible(!users.isEmpty());
    this->unblockKickUsersButton_->setEnabled(
        this->kickBlockedUsersView_->selectionModel()->hasSelection());
}

}  // namespace chatterino
