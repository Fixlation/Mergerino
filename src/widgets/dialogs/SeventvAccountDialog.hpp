// SPDX-License-Identifier: MIT

#pragma once

#include "providers/seventv/SeventvAccountManager.hpp"

#include <QDialog>
#include <QString>
#include <pajlada/signals/signalholder.hpp>

#include <optional>
#include <vector>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace chatterino {

class SeventvAccountDialog final : public QDialog
{
public:
    static void showDialog(QWidget *parent = nullptr,
                           const QString &channelID = {},
                           const QString &channelLogin = {},
                           const QString &channelDisplayName = {});

    explicit SeventvAccountDialog(QWidget *parent = nullptr,
                                  QString channelID = {},
                                  QString channelLogin = {},
                                  QString channelDisplayName = {});

private:
    void applyTheme();
    void updateAccountState();
    void rebuildInventory();
    void rebuildDefaultControls();
    void rebuildChannelRows();
    void rebuildEmoteChannels();
    void rebuildEmotePlatformIndicators();
    void loadSelectedEmoteChannel(bool force = false);
    void rebuildEmoteRows();
    void rebuildSearchRows();
    void addChannelByLogin();
    void addResolvedChannelPreset(const QString &platform,
                                  const QString &channelID,
                                  const QString &channelLogin,
                                  const QString &channelDisplayName);
    void setFeedback(const QString &message, bool error);
    std::optional<SeventvEditorChannel> selectedEmoteChannel() const;
    QComboBox *makeCosmeticCombo(const QString &kind,
                                 const QString &selectedID,
                                 bool allowInherit,
                                 QWidget *parent) const;

    QString contextChannelID_;
    QString contextChannelLogin_;
    QString contextChannelDisplayName_;

    QLabel *accountStatus_ = nullptr;
    QLabel *feedback_ = nullptr;
    QPushButton *signInButton_ = nullptr;
    QPushButton *refreshButton_ = nullptr;
    QPushButton *logoutButton_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QLabel *defaultsDescription_ = nullptr;
    QComboBox *defaultPaint_ = nullptr;
    QComboBox *defaultBadge_ = nullptr;
    QPushButton *applyDefaultsButton_ = nullptr;
    QLineEdit *channelInput_ = nullptr;
    QPushButton *addChannelButton_ = nullptr;
    QTableWidget *channelTable_ = nullptr;
    QComboBox *emoteChannel_ = nullptr;
    std::vector<QLabel *> emotePlatformIndicators_;
    QLabel *emoteSetTitle_ = nullptr;
    QLineEdit *emoteFilter_ = nullptr;
    QTableWidget *emoteTable_ = nullptr;
    QLineEdit *emoteSearch_ = nullptr;
    QPushButton *emoteSearchButton_ = nullptr;
    QTableWidget *searchTable_ = nullptr;
    std::vector<SeventvEditorChannel> emoteChannels_;
    std::vector<std::vector<SeventvEditorChannel>> emoteChannelGroups_;
    std::vector<SeventvManagedEmote> selectedEmotes_;
    QString loadedEmoteSetID_;
    int emoteLoadGeneration_ = 0;
    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
