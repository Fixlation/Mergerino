// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "providers/twitch/api/TwitchWebApi.hpp"
#include "widgets/BasePopup.hpp"

#include <QString>
#include <QVector>

class QLabel;
class QListWidget;
class QPushButton;

namespace chatterino {

class TwitchModCommentsDialog : public BasePopup
{
public:
    TwitchModCommentsDialog(const QString &channelId,
                            const QString &channelLogin,
                            const QString &targetId,
                            const QString &targetLogin,
                            QWidget *parent = nullptr);

protected:
    void themeChangedEvent() override;

private:
    void loadComments();
    void deleteSelectedComment();
    void setBusy(bool busy);
    void updateDeleteButton();
    const TwitchModComment *selectedComment() const;

    QString channelId_;
    QString channelLogin_;
    QString targetId_;
    QString targetLogin_;
    QString oauthClient_;
    QString oauthToken_;
    QVector<TwitchModComment> comments_;

    QLabel *statusLabel_ = nullptr;
    QListWidget *commentsList_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *deleteButton_ = nullptr;
    bool canDelete_ = false;
    bool busy_ = false;
};

}  // namespace chatterino
