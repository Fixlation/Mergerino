// SPDX-License-Identifier: MIT
#pragma once

#include "controllers/moderation/ModerationRules.hpp"

#include <QPointer>
#include <QScrollArea>
#include <QSet>

#include <functional>
#include <vector>

class QBoxLayout;
class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QVBoxLayout;

namespace chatterino {
class BaseWidget;
class Button;
class ModViewButton;

/// The Rules tab of Mod View. Every rule is a card with the same parts: what
/// it watches for, what happens when it matches, and who it applies to.
class ModViewRulesEditor final : public QScrollArea
{
public:
    struct ProfileItem {
        QString id;
        QString name;
    };

    ModViewRulesEditor(BaseWidget *owner, QWidget *parent);

    void setRules(const moderation::ReviewRules &rules);
    const moderation::ReviewRules &rules() const;
    void setProfiles(const std::vector<ProfileItem> &profiles,
                     const QString &current, int sharedWith);
    void setPlatforms(const std::vector<MessagePlatform> &available,
                      const QSet<MessagePlatform> &active);
    void setInlineActions(bool enabled);
    void setTestResult(const QString &summary, const QString &examples);
    void setLockdown(bool available, bool active);
    void setStatus(const QString &text);
    void applyScale(float scale);
    void applyColors(const QColor &fill, const QColor &selectedFill);

    /// The rules were edited.
    std::function<void()> changed;
    std::function<void(const QString &id)> profileSelected;
    /// "new", "duplicate", "rename", "delete", "import", "export" or
    /// "exportAll".
    std::function<void(const QString &command)> profileCommand;
    std::function<void(MessagePlatform platform, bool active)> platformToggled;
    std::function<void(bool enabled)> inlineActionsToggled;
    std::function<void()> testRequested;
    std::function<void()> lockdownToggled;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using Rules = moderation::ReviewRules;
    template <typename T>
    using Field = std::function<T &(Rules &)>;

    QVBoxLayout *section(const QString &title, const QString &hint = {});
    QCheckBox *toggle(QWidget *parent, const QString &text,
                      const QString &name, Field<bool> field);
    QSpinBox *number(QWidget *parent, int minimum, int maximum,
                     Field<int> field, const QString &suffix = {},
                     int width = 34);
    QPlainTextEdit *textList(QWidget *parent, Field<QStringList> field,
                             const QString &placeholder, const QString &name);
    QWidget *card(QVBoxLayout *target, const QString &title,
                  const QString &tooltip, const QString &name,
                  Field<moderation::RuleBase> rule,
                  const std::function<void(QHBoxLayout *, QWidget *)> &params,
                  bool actions = true);
    QWidget *responseRow(QWidget *parent, Field<moderation::RuleBase> rule);
    QComboBox *actionCombo(QWidget *parent,
                           Field<moderation::RuleAction> action,
                           bool escalate);
    QComboBox *durationCombo(QWidget *parent, Field<int> seconds);
    QComboBox *combo(QWidget *parent, int length);
    QHBoxLayout *row(QBoxLayout *target, int indent = 0);

    void buildTop(QVBoxLayout *layout);
    void buildBehaviour();
    void buildAccounts();
    void buildWordLists();
    void buildExceptions();
    void buildEscalation();
    void buildLockdown();
    void buildChat();

    void rebuildListCards();
    QWidget *listCard(int index);
    void showListMenu(int index, QWidget *anchor);
    void importListFile(int index);
    void importListUrl(int index);
    void exportList(int index);
    void fetchList(int index, const QString &source, const QString &name);
    /// Adds terms to a list, or to a new one when index is -1.
    void addTerms(int index, const QString &name, const QStringList &terms,
                  const QString &source, bool replace);
    void importAllowList();
    void exportAllowList();

    void edited();
    void load();
    void updatePresets();

    BaseWidget *owner_;
    QWidget *page_ = nullptr;
    QVBoxLayout *pageLayout_ = nullptr;
    /// Where section() adds new sections.
    QVBoxLayout *container_ = nullptr;
    Rules rules_;
    bool loading_ = false;
    float scale_ = 1.F;
    QColor fill_;
    QColor selectedFill_;
    std::vector<std::function<void()>> loaders_;
    std::vector<std::function<void()>> listLoaders_;
    std::vector<std::function<void()>> *target_ = &loaders_;
    std::vector<std::pair<QPointer<QSpinBox>, int>> spins_;
    std::vector<QPointer<QPlainTextEdit>> edits_;

    QComboBox *profiles_ = nullptr;
    Button *profileMenu_ = nullptr;
    QLabel *profileHint_ = nullptr;
    std::vector<std::pair<moderation::RulePreset, ModViewButton *>> presets_;
    QLabel *presetHint_ = nullptr;
    ModViewButton *test_ = nullptr;
    QLabel *testSummary_ = nullptr;
    QLabel *testExamples_ = nullptr;
    QWidget *platformsRow_ = nullptr;
    QHBoxLayout *platformBoxes_ = nullptr;
    QWidget *rulesArea_ = nullptr;
    QWidget *listsBox_ = nullptr;
    QVBoxLayout *listsLayout_ = nullptr;
    QLabel *status_ = nullptr;
    ModViewButton *lockdown_ = nullptr;
    QLabel *lockdownHint_ = nullptr;
    QCheckBox *inlineActions_ = nullptr;
};

}  // namespace chatterino
