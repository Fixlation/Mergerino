// SPDX-License-Identifier: MIT
#pragma once

#include "common/Channel.hpp"
#include "controllers/priority/PriorityChatModel.hpp"
#include "widgets/BaseWidget.hpp"

#include <boost/signals2/connection.hpp>
#include <QPointer>
#include <QTimer>

#include <array>
#include <memory>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QStringListModel;
class QTabWidget;

namespace chatterino {
class ChannelView;
class Split;
class BasePopup;
class SvgButton;

class PriorityChat final : public BaseWidget
{
    friend class PriorityChatTestAccess;

public:
    explicit PriorityChat(Split *split, QWidget *parent,
                          ChannelView *feedView = nullptr);
    ~PriorityChat() override;
    void setChannel(ChannelPtr channel);
    void setMonitoring(bool enabled);
    void refreshFilters();
    void setUserChoice(const MessagePtr &message, priority::Choice choice);
    priority::Choice userChoice(const Message &message) const;
    bool includesUser(const Message &message) const;
    int preferredWidth() const;
    void setPreferredWidth(int width);
    QString title() const;
    void showRules();
    void showSettings();

    pajlada::Signals::NoArgSignal titleChanged;

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Inbox;
    void connectChannel();
    void flush();
    void queueRefresh();
    void refreshFeed();
    void rememberRetainedPeople();
    bool accepts(const MessagePtr &message) const;
    void loadPreferences();
    void savePreferences();
    void loadRules();
    void saveRules();
    void refreshPeople();
    void removeUser(const QString &key);
    void showUserCard(const QString &key);
    void updateNameSuggestions();
    void setAddBusy(bool busy);
    void resetAddHint();
    void addPerson(priority::Choice choice);
    void addResolvedPerson(
        quint64 generation, quint64 settingsGeneration,
        const priority::Person &person,
        priority::Choice choice = priority::Choice::Include);
    void addChosenPerson(const priority::Person &person,
                         priority::Choice choice);
    void acceptSettings();
    priority::Model &settingsModel();
    void lookUpNext();
    void lookupFinished(quint64 generation, bool refresh);
    QString sourceName(MessagePlatform platform) const;
    void accountsChanged();

    Split *split_;
    ChannelPtr channel_;
    priority::Model model_;
    QHash<QString, priority::Person> retainedPeople_;
    std::optional<priority::Model> settingsDraft_;
    priority::Model settingsOriginal_;
    quint64 settingsGeneration_ = 0;
    QString scope_;
    QString selectedKey_;
    QString followStatus_;
    QByteArray savedScope_;
    bool monitoring_ = false;
    bool loading_ = false;
    quint64 generation_ = 0;
    int inFlight_ = 0;
    qint64 nextLookup_ = 0;
    qint64 nextAgeRefresh_ = 0;
    bool addBusy_ = false;
    bool followPermissionDenied_ = false;
    std::shared_ptr<Inbox> inbox_;
    pajlada::Signals::SignalHolder channelConnections_;
    pajlada::Signals::SignalHolder connections_;
    boost::signals2::scoped_connection accountConnection_;
    QTimer timer_;
    QTimer refreshTimer_;
    QTimer saveTimer_;

    QTabWidget *tabs_;
    QPointer<ChannelView> view_;
    QPointer<BasePopup> settingsPopup_;
    struct RuleControls {
        QCheckBox *bots{};
        QLineEdit *botAccounts{};
        QCheckBox *moderators{};
        QCheckBox *vips{};
        QCheckBox *subscriptions{};
        QSpinBox *subMonths{};
        QCheckBox *activeOnly{};
        QCheckBox *follows{};
        QSpinBox *followMonths{};
        QCheckBox *matching{};
    };
    std::array<RuleControls, 4> ruleControls_;
    QListWidget *users_{};
    QLineEdit *addName_;
    QComboBox *addPlatform_;
    QStringListModel *nameSuggestions_;
    QPushButton *add_;
    QLabel *addHint_;
    QDialogButtonBox *buttons_;
};
}  // namespace chatterino
