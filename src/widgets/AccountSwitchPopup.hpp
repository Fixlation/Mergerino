// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/BaseWindow.hpp"

#include <boost/signals2/connection.hpp>
#include <QElapsedTimer>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QWidget>

namespace chatterino {

class AccountSwitchWidget;
class KickAccountSwitchWidget;

class AccountSwitchPopup : public BaseWindow
{
    Q_OBJECT

public:
    AccountSwitchPopup(QWidget *parent = nullptr);

    void refresh();
    bool wasHiddenRecently(int thresholdMs) const;

protected:
    void hideEvent(QHideEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

    void themeChangedEvent() override;

private:
    void updateCurrentPage();
    void updateStatusText();

    struct {
        QStackedWidget *accountStack = nullptr;
        QPushButton *twitchProviderButton = nullptr;
        QPushButton *kickProviderButton = nullptr;
        QPushButton *youtubeProviderButton = nullptr;
        QPushButton *sevenTVProviderButton = nullptr;
        AccountSwitchWidget *accountSwitchWidget = nullptr;
        KickAccountSwitchWidget *kickAccountSwitcher = nullptr;
        QWidget *youtubeAccountPage = nullptr;
        QWidget *youtubeReviewNotice = nullptr;
        QListWidget *youtubeAccountSwitcher = nullptr;
        QWidget *sevenTVAccountPage = nullptr;
        QLabel *sevenTVDescription = nullptr;
        QWidget *sevenTVProfile = nullptr;
        QLabel *sevenTVProfileIcon = nullptr;
        QLabel *sevenTVProfileName = nullptr;
        QLabel *sevenTVProfileHandle = nullptr;
        QLabel *statusLabel = nullptr;
        QPushButton *loginButton = nullptr;
        QPushButton *manageAccountsButton = nullptr;
    } ui_;

    bool sevenTVPageSelected_ = false;
    std::vector<boost::signals2::scoped_connection> bSignals_;
    QElapsedTimer lastHideTimer_;
};

}  // namespace chatterino
