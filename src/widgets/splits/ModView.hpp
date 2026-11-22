// SPDX-License-Identifier: MIT
#pragma once

#include "common/Channel.hpp"
#include "controllers/moderation/ModerationReview.hpp"
#include "widgets/BaseWidget.hpp"

#include <QColor>
#include <QDateTime>
#include <QElapsedTimer>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <deque>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QFrame;
class QHBoxLayout;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSpinBox;
class QSplitter;
class QStackedWidget;
class QTabBar;

namespace chatterino::moderation {
class ModerationProfiles;
}  // namespace chatterino::moderation

namespace chatterino {
class Button;
class Label;
class ModViewButton;
class ModViewRulesEditor;
class Split;

/// Theme colors and scale shared by Mod View and its list delegates.
struct ModViewStyle {
    QColor text;
    QColor muted;
    QColor hover;
    QColor selected;
    QColor accent;
    QColor attention;
    float scale = 1.F;
};

class ModView final : public BaseWidget
{
    Q_OBJECT
    friend class ModViewTestAccess;
    friend class Split;

public:
    explicit ModView(Split *split, QWidget *parent = nullptr);
    ~ModView() override;

    void setChannel(ChannelPtr channel);
    void setMonitoring(bool enabled);
    void inspectMessage(const MessagePtr &message);
    void showRules();
    bool inlineActionsEnabled() const;
    static std::optional<bool> inlineActionsPreference(
        const ChannelPtr &channel);
    static void setInlineActionsPreference(const ChannelPtr &channel,
                                           bool enabled);
    int preferredWidth() const;
    void setPreferredWidth(int width);

    pajlada::Signals::NoArgSignal closeRequested;
    pajlada::Signals::NoArgSignal popOutRequested;

protected:
    void themeChangedEvent() override;
    void scaleChangedEvent(float scale) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    struct Inbox;
    struct TimeoutPreset {
        ModViewButton *button;
        int seconds;
    };
    enum class Action { Timeout, Ban, Unban, Delete, Warn };
    /// One moderation action for one user, sent on their own platform by
    /// their platform user ID.
    struct ActionRequest {
        QString key;
        MessagePlatform platform = MessagePlatform::AnyOrTwitch;
        QString userID;
        QString name;
        Action action = Action::Timeout;
        int seconds = 0;
        QString messageID;
        QString reason;
        /// "You", "Bulk", or "Auto: rule"
        QString source;
        QString actor;
        quint64 logID = 0;
    };
    struct LogEntry {
        quint64 id = 0;
        qint64 at = 0;
        QDateTime time;
        ActionRequest request;
        QString status;
        bool failed = false;
        bool done = false;
    };
    /// What "Lock down chat" turned on, so ending it only undoes that.
    struct Lockdown {
        bool active = false;
        bool followers = false;
        bool slow = false;
        bool emotes = false;
        bool subscribers = false;
        bool shield = false;
    };
    QWidget *buildHeader();
    QWidget *buildReviewPage();
    QWidget *buildTrackingPage();
    QWidget *buildRulesPage();
    QWidget *buildLogPage();
    QWidget *buildDetails();
    QWidget *buildBulkBar();
    QWidget *buildLockdownBar();
    void connectControls();
    void rebuildTimeouts();
    QColor presetFill() const;
    void updateFonts();
    void connectChannel();
    void flushMessages();
    void refreshLists(bool force = false);
    void refreshEmptyStates(bool queueFiltered, bool trackingFiltered);
    void syncList(QListWidget *list, const QStringList &keys);
    void syncSelection(QListWidget *list);
    void describeRow(QListWidgetItem *item, const moderation::ReviewUser &user,
                     bool queue) const;
    void selectUser(const QString &key);
    void advanceQueue(int row);
    void refreshDetails();
    void updateCapabilities();
    void updateDetailsVisibility();
    void clearConfirmation();
    void loadPreferences();
    /// Saves this chat's choices. Profile rules are saved by saveProfile().
    void savePreferences();
    void saveProfile();
    void writeProfiles(const moderation::ModerationProfiles &profiles);
    void profilesChanged();
    void updateProfileList(const moderation::ModerationProfiles &profiles);
    void selectProfile(const QString &id);
    void runProfileCommand(const QString &command);
    moderation::ReviewRules effectiveRules() const;
    void applyRules();
    void loadRulesEditor();
    void saveRulesEditor();
    void testRules();
    std::vector<MessagePlatform> availablePlatforms() const;
    void setPlatformActive(MessagePlatform platform, bool active);
    void editNotes();
    void addToAllowList();
    void showTools();
    void showMoreActions();
    void requestAction(Action action, int seconds = 0);
    void executeAction(Action action, const QString &key,
                       const QString &messageID, const QString &actor,
                       int seconds, const QString &reason = {});
    static QString actionText(Action action, int seconds);
    static bool actionAvailable(Action action, MessagePlatform platform);
    void processTriggers();
    void lookUpAccountAges();
    void enqueueAction(ActionRequest request);
    void dispatchAction();
    void sendAction(const ActionRequest &request);
    /// Why the action can't be sent, or empty when it can. Automatic and bulk
    /// actions never touch mods, VIPs or owners.
    QString actionBlocker(MessagePlatform platform, const QString &userID,
                          bool trusted, bool ignoreCooldown,
                          bool automatic) const;
    bool canAct(const moderation::ReviewUser &user, QString *reason = nullptr,
                bool ignoreCooldown = false) const;
    quint64 addLog(const ActionRequest &request, const QString &status,
                   bool failed = false);
    void updateLog(quint64 id, const QString &status, bool failed, bool done);
    void refreshLog();
    void exportLog();
    void undoLogEntry();
    QStringList bulkKeys() const;
    void updateBulkBar();
    void bulkAction(Action action, int seconds = 0);
    bool lockdownAvailable() const;
    void toggleLockdown();
    void updateLockdownBar();
    bool handleListKey(QListWidget *list, QKeyEvent *event);
    ChannelPtr platformChannel(MessagePlatform platform) const;
    QString actorID(MessagePlatform platform) const;
    void feedback(const QString &text);
    qint64 now() const;

    QPointer<Split> split_;
    ChannelPtr channel_;
    QString scope_;
    bool merged_ = false;
    moderation::ModerationReview review_;
    std::shared_ptr<Inbox> inbox_;
    pajlada::Signals::SignalHolder connections_;
    QElapsedTimer clock_;
    QTimer refreshTimer_;
    QTimer rulesTimer_;
    QTimer feedbackTimer_;
    QTimer actionTimer_;
    QTimer ageTimer_;
    bool monitoring_ = false;
    bool loadingRules_ = false;
    bool writingPreferences_ = false;
    bool ageLookupBusy_ = false;
    bool confirmingBulk_ = false;
    QString profileID_;
    QString profileName_;
    moderation::ReviewRules profileRules_;
    QSet<MessagePlatform> mutedPlatforms_;
    std::deque<ActionRequest> actionQueue_;
    QStringList ageKeys_;
    std::deque<LogEntry> log_;
    quint64 logSequence_ = 0;
    Lockdown lockdown_;
    /// Rule actions a platform can't do, reported once per session.
    QSet<QString> unsupportedNoted_;
    int preferredWidth_ = 370;
    quint64 renderedRevision_ = 0;
    quint64 selectionRevision_ = 0;
    QString selectedKey_;
    QString confirmationKey_;
    QString confirmationMessageID_;
    QString confirmationActor_;
    Action confirmationAction_ = Action::Ban;
    qint64 lastCapabilityRefresh_ = -1000;
    qint64 lastAction_ = -1000;
    QList<int> detailSizes_;
    bool compactIdentity_ = false;
    ModViewStyle style_;

    QFrame *header_;
    QFrame *lockdownBar_;
    QLabel *lockdownLabel_;
    ModViewButton *endLockdown_;
    QTabBar *tabs_;
    Button *tools_;
    Button *close_;
    QSplitter *content_;
    QStackedWidget *pages_;
    QWidget *reviewTools_;
    QLineEdit *reviewSearch_;
    ModViewButton *dismissAll_;
    QLineEdit *userSearch_;
    QComboBox *searchMode_;
    QCheckBox *showRecent_;
    QListWidget *queue_;
    QListWidget *tracked_;
    QWidget *emptyQueue_;
    QLabel *emptyQueueTitle_;
    QLabel *emptyQueueHint_;
    QWidget *emptyTracked_;
    QLabel *emptyTrackedTitle_;
    QLabel *emptyTrackedHint_;
    QWidget *details_;
    QHBoxLayout *identityRow_;
    QHBoxLayout *wrappedActions_;
    QWidget *identityActions_;
    QLabel *platformIcon_;
    Label *userLabel_;
    QLabel *identityLabel_;
    QLabel *reasons_;
    QListWidget *history_;
    QLabel *emptyHistory_;
    ModViewButton *track_;
    ModViewButton *notes_;
    ModViewButton *userCard_;
    Button *closeUser_;
    QFrame *actions_;
    QWidget *timeouts_;
    std::vector<TimeoutPreset> timeoutPresets_;
    QLabel *accessLabel_;
    QWidget *primaryActions_;
    ModViewButton *dismiss_;
    ModViewButton *mark_;
    Button *more_;
    ModViewButton *ban_;
    QWidget *confirmation_;
    QLabel *confirmationLabel_;
    QSpinBox *duration_;
    ModViewButton *cancel_;
    ModViewButton *confirm_;
    QLabel *feedback_;
    ModViewRulesEditor *rulesEditor_;
    QListWidget *logList_;
    QWidget *emptyLog_;
    QLabel *emptyLogTitle_;
    QLabel *emptyLogHint_;
    ModViewButton *undo_;
    ModViewButton *exportLog_;
    QFrame *bulkBar_;
    QLabel *bulkLabel_;
    ModViewButton *bulkTimeout_;
    ModViewButton *bulkBan_;
    ModViewButton *bulkDismiss_;
    ModViewButton *bulkCancel_;
    ModViewButton *bulkConfirm_;
};
}  // namespace chatterino
