// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/TwitchModCommentsDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "singletons/Theme.hpp"
#include "util/LayoutCreator.hpp"

#include <QDateTime>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <algorithm>

namespace chatterino {

TwitchModCommentsDialog::TwitchModCommentsDialog(
    const QString &channelId, const QString &channelLogin,
    const QString &targetId, const QString &targetLogin, QWidget *parent)
    : BasePopup(
          {
              BaseWindow::EnableCustomFrame,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , channelId_(channelId)
    , channelLogin_(channelLogin)
    , targetId_(targetId)
    , targetLogin_(targetLogin)
{
    this->setWindowTitle(
        QString("Moderator comments for %1 - #%2")
            .arg(this->targetLogin_, this->channelLogin_));
    this->setScaleIndependentSize(680, 430);

    auto layout = LayoutCreator<QWidget>(this->getLayoutContainer())
                      .setLayoutType<QVBoxLayout>();
    layout.emplace<QLabel>("Loading moderator comments...")
        .assign(&this->statusLabel_);
    this->statusLabel_->setWordWrap(true);

    layout.emplace<QListWidget>().assign(&this->commentsList_);
    this->commentsList_->setSelectionMode(
        QAbstractItemView::SingleSelection);

    auto buttons = layout.emplace<QHBoxLayout>();
    buttons->addStretch(1);
    buttons.emplace<QPushButton>("Refresh").assign(&this->refreshButton_);
    buttons.emplace<QPushButton>("Delete selected")
        .assign(&this->deleteButton_);
    auto closeButton = buttons.emplace<QPushButton>("Close");

    QObject::connect(this->refreshButton_, &QPushButton::clicked, this,
                     &TwitchModCommentsDialog::loadComments);
    QObject::connect(this->deleteButton_, &QPushButton::clicked, this,
                     &TwitchModCommentsDialog::deleteSelectedComment);
    QObject::connect(this->commentsList_, &QListWidget::itemSelectionChanged,
                     this, &TwitchModCommentsDialog::updateDeleteButton);
    QObject::connect(closeButton.getElement(), &QPushButton::clicked, this,
                     &QWidget::close);

    this->themeChangedEvent();
    this->setBusy(true);
    QTimer::singleShot(0, this, &TwitchModCommentsDialog::loadComments);
}

void TwitchModCommentsDialog::loadComments()
{
    this->setBusy(true);
    this->canDelete_ = false;
    this->comments_.clear();
    this->commentsList_->clear();
    this->statusLabel_->setText("Loading moderator comments...");

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser == nullptr || currentUser->isAnon())
    {
        this->statusLabel_->setText(
            "You must be logged in to view moderator comments.");
        this->setBusy(false);
        return;
    }

    QString authError;
    const auto account = TwitchModerationAuth::resolveForCurrentUser(
        currentUser->getUserId(), &authError);
    if (!account.supportsWebGql())
    {
        this->statusLabel_->setText(
            "Moderator comments require Twitch mod actions to be connected "
            "in Settings -> Accounts. " +
            authError);
        this->setBusy(false);
        return;
    }
    this->oauthClient_ = account.clientId;
    this->oauthToken_ = account.oauthToken;

    TwitchWebApi::getChannelModerationPermissions(
        this->channelId_, account.userId, this->oauthClient_,
        this->oauthToken_,
        [self = QPointer(this)](
            const TwitchChannelModerationPermissions &permissions) {
            if (self == nullptr)
            {
                return;
            }
            if (!permissions.deleteModComments)
            {
                self->statusLabel_->setText(
                    "Deleting moderator comments is available to the "
                    "broadcaster and Lead Moderators.");
                self->setBusy(false);
                return;
            }
            self->canDelete_ = true;
            TwitchWebApi::getModComments(
                self->channelId_, self->targetId_, self->oauthClient_,
                self->oauthToken_,
                [self](const QVector<TwitchModComment> &comments) {
                    if (self == nullptr)
                    {
                        return;
                    }

                    self->comments_ = comments;
                    for (const auto &comment : self->comments_)
                    {
                        auto timestamp = QDateTime::fromString(
                            comment.timestamp, Qt::ISODate);
                        const auto timestampText =
                            timestamp.isValid()
                                ? timestamp.toLocalTime().toString(
                                      "dd MMM yyyy HH:mm")
                                : comment.timestamp;
                        auto author = comment.authorDisplayName;
                        if (author.isEmpty())
                        {
                            author = comment.authorLogin;
                        }
                        if (author.isEmpty())
                        {
                            author = "Unknown moderator";
                        }
                        const auto visibility =
                            comment.isShareable ? "Shared" : "Private";

                        auto *item = new QListWidgetItem(
                            QString("%1 - %2 - %3\n%4")
                                .arg(timestampText, author, visibility,
                                     comment.text),
                            self->commentsList_);
                        item->setData(Qt::UserRole, comment.id);
                    }

                    if (self->comments_.isEmpty())
                    {
                        self->statusLabel_->setText(
                            QString("No moderator comments for %1 in #%2.")
                                .arg(self->targetLogin_,
                                     self->channelLogin_));
                    }
                    else
                    {
                        self->statusLabel_->setText(
                            QString("%1 moderator comment(s) for %2 in #%3. "
                                    "Select one to delete it.")
                                .arg(self->comments_.size())
                                .arg(self->targetLogin_,
                                     self->channelLogin_));
                    }
                    self->setBusy(false);
                },
                [self](const QString &error) {
                    if (self == nullptr)
                    {
                        return;
                    }
                    self->statusLabel_->setText(
                        "Failed to load moderator comments: " + error);
                    self->setBusy(false);
                });
        },
        [self = QPointer(this)](const QString &error) {
            if (self == nullptr)
            {
                return;
            }
            self->statusLabel_->setText(
                "Failed to check moderator-comment permission: " + error);
            self->setBusy(false);
        });
}

void TwitchModCommentsDialog::deleteSelectedComment()
{
    const auto *selected = this->selectedComment();
    if (selected == nullptr || !this->canDelete_ || this->busy_)
    {
        return;
    }
    const auto comment = *selected;
    auto author = comment.authorDisplayName;
    if (author.isEmpty())
    {
        author = comment.authorLogin;
    }
    if (author.isEmpty())
    {
        author = "Unknown moderator";
    }

    QMessageBox confirm(this);
    confirm.setIcon(QMessageBox::Warning);
    confirm.setWindowTitle("Delete moderator comment");
    confirm.setTextFormat(Qt::PlainText);
    confirm.setText(
        QString("Delete this moderator comment by %1?\n\n%2\n\n"
                "This cannot be undone.")
            .arg(author, comment.text));
    confirm.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    confirm.setDefaultButton(QMessageBox::Cancel);
    if (confirm.exec() != QMessageBox::Yes)
    {
        return;
    }

    this->setBusy(true);
    this->statusLabel_->setText("Deleting moderator comment...");
    TwitchWebApi::deleteModComment(
        this->channelId_, comment.id, this->oauthClient_, this->oauthToken_,
        [self = QPointer(this), commentId = comment.id] {
            if (self == nullptr)
            {
                return;
            }

            for (int row = 0; row < self->commentsList_->count(); ++row)
            {
                auto *item = self->commentsList_->item(row);
                if (item->data(Qt::UserRole).toString() == commentId)
                {
                    delete self->commentsList_->takeItem(row);
                    break;
                }
            }
            self->comments_.erase(
                std::remove_if(
                    self->comments_.begin(), self->comments_.end(),
                    [&commentId](const TwitchModComment &candidate) {
                        return candidate.id == commentId;
                    }),
                self->comments_.end());
            self->statusLabel_->setText(
                "Moderator comment deleted from Twitch.");
            self->setBusy(false);
        },
        [self = QPointer(this)](const QString &error) {
            if (self == nullptr)
            {
                return;
            }
            self->statusLabel_->setText(
                "Failed to delete moderator comment: " + error);
            self->setBusy(false);
        });
}

void TwitchModCommentsDialog::themeChangedEvent()
{
    BasePopup::themeChangedEvent();
    if (!this->theme)
    {
        return;
    }

    auto palette = this->palette();
    palette.setColor(QPalette::Window,
                     this->theme->tabs.selected.backgrounds.regular);
    palette.setColor(QPalette::Base, this->theme->splits.background);
    palette.setColor(QPalette::Text, this->theme->window.text);
    this->setPalette(palette);
    if (this->commentsList_)
    {
        this->commentsList_->setPalette(palette);
    }
}

void TwitchModCommentsDialog::setBusy(bool busy)
{
    this->busy_ = busy;
    this->commentsList_->setEnabled(!busy);
    this->refreshButton_->setEnabled(!busy);
    this->updateDeleteButton();
}

void TwitchModCommentsDialog::updateDeleteButton()
{
    this->deleteButton_->setEnabled(
        !this->busy_ && this->canDelete_ &&
        this->commentsList_->currentItem() != nullptr);
}

const TwitchModComment *TwitchModCommentsDialog::selectedComment() const
{
    const auto *item = this->commentsList_->currentItem();
    if (item == nullptr)
    {
        return nullptr;
    }

    const auto id = item->data(Qt::UserRole).toString();
    const auto it = std::find_if(
        this->comments_.cbegin(), this->comments_.cend(),
        [&id](const TwitchModComment &comment) { return comment.id == id; });
    return it == this->comments_.cend() ? nullptr : &*it;
}

}  // namespace chatterino
