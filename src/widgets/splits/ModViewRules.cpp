// SPDX-License-Identifier: MIT
#include "widgets/splits/ModViewRules.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/moderation/ModerationProfiles.hpp"
#include "widgets/BaseWidget.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/splits/ModViewButton.hpp"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <utility>

namespace chatterino {
using namespace moderation;

// A field of the edited rules, as a reference the editor reads and writes
#define RULE(expr)                                \
    [](ReviewRules &rules) -> decltype(auto) {    \
        return (rules.expr);                      \
    }

namespace {
constexpr qint64 MAX_LIST_BYTES = 5 * 1024 * 1024;
constexpr int DURATIONS[] = {10,   30,    60,    300,   600,
                             1800, 3600, 21600, 86400, 604800};

QLabel *label(const QString &text, QWidget *parent, bool muted = true,
              bool wrap = true)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(wrap);
    // Lets narrow panes squeeze single-line labels instead of overflowing
    label->setMinimumWidth(1);
    label->setProperty("muted", muted);
    return label;
}

QString platformLabel(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return "Kick";
        case MessagePlatform::YouTube:
            return "YouTube";
        case MessagePlatform::TikTok:
            return "TikTok";
        case MessagePlatform::AnyOrTwitch:
        default:
            return "Twitch";
    }
}

QString platformIcon(MessagePlatform platform)
{
    return ":/platforms/" + platformKey(platform) + ".svg";
}

/// Holds a rule's settings and slides them open or closed. The settings keep
/// their full size and are uncovered from the top, rather than squeezed.
class RevealBox final : public QWidget
{
public:
    RevealBox(QWidget *content, QWidget *parent)
        : QWidget(parent)
        , content_(content)
    {
        content->setParent(this);
        this->slide_.setDuration(180);
        this->slide_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->slide_, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->shown_ = value.toInt();
                             this->updateGeometry();
                         });
        QObject::connect(&this->slide_, &QVariantAnimation::finished, this,
                         [this] {
                             this->shown_ = -1;
                             this->setVisible(this->open_);
                             this->updateGeometry();
                         });
    }

    void setOpen(bool open)
    {
        const bool animate = this->parentWidget() &&
                             this->parentWidget()->isVisible();
        const auto from = this->slide_.state() == QAbstractAnimation::Running
                              ? this->shown_
                          : this->isHidden() ? 0
                                             : this->height();
        this->open_ = open;
        this->slide_.stop();
        if (!animate)
        {
            this->shown_ = -1;
            this->setVisible(open);
            this->updateGeometry();
            return;
        }
        this->shown_ = from;
        this->show();
        this->slide_.setStartValue(from);
        this->slide_.setEndValue(open ? this->fullHeight(this->width()) : 0);
        this->slide_.start();
    }

    QSize sizeHint() const override
    {
        auto size = this->content_->sizeHint();
        if (this->shown_ >= 0)
            size.setHeight(this->shown_);
        return size;
    }

    QSize minimumSizeHint() const override
    {
        auto size = this->content_->minimumSizeHint();
        size.setHeight(this->shown_ >= 0 ? this->shown_
                                         : this->content_->sizeHint().height());
        return size;
    }

    bool hasHeightForWidth() const override
    {
        return this->content_->hasHeightForWidth();
    }

    int heightForWidth(int width) const override
    {
        return this->shown_ >= 0 ? this->shown_ : this->fullHeight(width);
    }

protected:
    bool event(QEvent *event) override
    {
        // The settings changed size, like a timeout length appearing
        if (event->type() == QEvent::LayoutRequest)
        {
            this->updateGeometry();
            this->place();
        }
        return QWidget::event(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        this->place();
    }

private:
    int fullHeight(int width) const
    {
        const auto hint = this->content_->sizeHint().height();
        return this->content_->hasHeightForWidth()
                   ? std::max(hint, this->content_->heightForWidth(width))
                   : hint;
    }

    void place()
    {
        this->content_->setGeometry(0, 0, this->width(),
                                    this->fullHeight(this->width()));
    }

    QWidget *content_;
    QVariantAnimation slide_;
    bool open_ = false;
    /// Height while sliding, or -1 when settled
    int shown_ = -1;
};

QStringList mergedTerms(const QStringList &existing, const QStringList &added)
{
    auto result = existing;
    QSet<QString> seen;
    for (const auto &term : existing)
        seen.insert(term.toCaseFolded());
    for (const auto &term : added)
        if (!seen.contains(term.toCaseFolded()))
        {
            seen.insert(term.toCaseFolded());
            result.append(term);
        }
    return result;
}
}  // namespace

ModViewRulesEditor::ModViewRulesEditor(BaseWidget *owner, QWidget *parent)
    : QScrollArea(parent)
    , owner_(owner)
{
    this->viewport()->setObjectName("ModViewRulesViewport");
    this->setWidgetResizable(true);
    this->setFrameShape(QFrame::NoFrame);
    this->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    this->page_ = new QWidget(this);
    this->page_->setObjectName("ModViewRulesPage");
    this->pageLayout_ = new QVBoxLayout(this->page_);
    this->pageLayout_->setContentsMargins(10, 8, 10, 12);
    this->pageLayout_->setSpacing(4);
    this->buildTop(this->pageLayout_);

    this->rulesArea_ = new QWidget(this->page_);
    auto *rules = new QVBoxLayout(this->rulesArea_);
    rules->setContentsMargins(0, 0, 0, 0);
    rules->setSpacing(4);
    this->pageLayout_->addWidget(this->rulesArea_);
    this->container_ = rules;
    this->buildBehaviour();
    this->buildAccounts();
    this->buildWordLists();
    this->buildExceptions();
    this->buildEscalation();
    this->container_ = this->pageLayout_;
    this->buildLockdown();
    this->buildChat();
    this->pageLayout_->addStretch();
    this->setWidget(this->page_);
    this->viewport()->installEventFilter(this);
    this->load();
}

QVBoxLayout *ModViewRulesEditor::section(const QString &title,
                                         const QString &hint)
{
    auto *box = new QWidget(this->page_);
    box->setObjectName("ModViewRulesSection");
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(0, 12, 0, 0);
    layout->setSpacing(3);
    auto *heading = label(title, box);
    heading->setProperty("section", true);
    layout->addWidget(heading);
    if (!hint.isEmpty())
        layout->addWidget(label(hint, box));
    this->container_->addWidget(box);
    return layout;
}

QHBoxLayout *ModViewRulesEditor::row(QBoxLayout *target, int indent)
{
    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(indent, 0, 0, 0);
    layout->setSpacing(4);
    target->addLayout(layout);
    return layout;
}

QCheckBox *ModViewRulesEditor::toggle(QWidget *parent, const QString &text,
                                      const QString &name, Field<bool> field)
{
    auto *box = new ModViewSwitch(text, parent);
    box->setObjectName(name);
    this->target_->push_back([this, box, field] {
        box->setChecked(field(this->rules_));
    });
    QObject::connect(box, &QCheckBox::toggled, this, [this, field](bool on) {
        if (this->loading_)
            return;
        field(this->rules_) = on;
        this->edited();
    });
    return box;
}

QSpinBox *ModViewRulesEditor::number(QWidget *parent, int minimum,
                                     int maximum, Field<int> field,
                                     const QString &suffix, int width)
{
    auto *box = new QSpinBox(parent);
    box->setRange(minimum, maximum);
    box->setButtonSymbols(QAbstractSpinBox::NoButtons);
    box->setAlignment(Qt::AlignCenter);
    box->setSuffix(suffix);
    box->setFocusPolicy(Qt::StrongFocus);
    box->installEventFilter(this);
    box->setFixedWidth(int(width * this->scale_));
    this->spins_.emplace_back(box, width);
    this->target_->push_back([this, box, field] {
        box->setValue(field(this->rules_));
    });
    QObject::connect(box, &QSpinBox::valueChanged, this,
                     [this, field](int value) {
                         if (this->loading_)
                             return;
                         field(this->rules_) = value;
                         this->edited();
                     });
    return box;
}

QPlainTextEdit *ModViewRulesEditor::textList(QWidget *parent,
                                             Field<QStringList> field,
                                             const QString &placeholder,
                                             const QString &name)
{
    auto *edit = new QPlainTextEdit(parent);
    edit->setObjectName(name);
    edit->setPlaceholderText(placeholder);
    edit->setTabChangesFocus(true);
    edit->setMaximumHeight(int(76 * this->scale_));
    this->edits_.emplace_back(edit);
    this->target_->push_back([this, edit, field] {
        const auto text = field(this->rules_).join('\n');
        if (edit->toPlainText() != text)
            edit->setPlainText(text);
    });
    QObject::connect(edit, &QPlainTextEdit::textChanged, this,
                     [this, edit, field] {
                         if (this->loading_)
                             return;
                         QStringList lines;
                         for (auto line : edit->toPlainText().split('\n'))
                             if (!(line = line.trimmed()).isEmpty())
                                 lines.append(line);
                         field(this->rules_) = lines;
                         this->edited();
                     });
    return edit;
}

QComboBox *ModViewRulesEditor::combo(QWidget *parent, int length)
{
    auto *box = new ModViewComboBox(parent);
    box->setMinimumContentsLength(length);
    box->setFocusPolicy(Qt::StrongFocus);
    return box;
}

QComboBox *ModViewRulesEditor::actionCombo(QWidget *parent,
                                           Field<RuleAction> action,
                                           bool escalate)
{
    auto *box = this->combo(parent, 9);
    const std::pair<RuleAction, QString> items[] = {
        {RuleAction::Flag, "Only add them to the review queue"},
        {RuleAction::Delete, "Delete the message on its platform"},
        {RuleAction::Warn,
         "Send a Twitch warning they have to acknowledge. Flags on other "
         "platforms."},
        {RuleAction::Timeout, "Time them out on their platform"},
        {RuleAction::Ban, "Ban them on their platform"},
        {RuleAction::Escalate,
         "Use the escalation steps, based on how often they were actioned "
         "this session"},
    };
    for (const auto &[value, tooltip] : items)
    {
        if (value == RuleAction::Escalate && !escalate)
            continue;
        box->addItem(value == RuleAction::Warn ? "Warn (Twitch)"
                                               : actionLabel(value),
                     int(value));
        box->setItemData(box->count() - 1, tooltip, Qt::ToolTipRole);
    }
    this->target_->push_back([this, box, action] {
        box->setCurrentIndex(
            std::max(0, box->findData(int(action(this->rules_)))));
    });
    QObject::connect(box, &QComboBox::currentIndexChanged, this,
                     [this, box, action] {
                         if (this->loading_)
                             return;
                         action(this->rules_) =
                             RuleAction(box->currentData().toInt());
                         this->edited();
                     });
    return box;
}

QComboBox *ModViewRulesEditor::durationCombo(QWidget *parent,
                                             Field<int> seconds)
{
    auto *box = this->combo(parent, 3);
    for (const auto value : DURATIONS)
        box->addItem(durationLabel(value), value);
    box->setToolTip("Timeout length");
    this->target_->push_back([this, box, seconds] {
        const auto value = seconds(this->rules_);
        if (box->findData(value) < 0)
            box->addItem(durationLabel(value), value);
        box->setCurrentIndex(box->findData(value));
    });
    QObject::connect(box, &QComboBox::currentIndexChanged, this,
                     [this, box, seconds] {
                         if (this->loading_)
                             return;
                         seconds(this->rules_) = box->currentData().toInt();
                         this->edited();
                     });
    return box;
}

QWidget *ModViewRulesEditor::responseRow(QWidget *parent,
                                         Field<RuleBase> rule)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    layout->addWidget(label("Then", widget, true, false));
    auto *action = this->actionCombo(
        widget, [rule](Rules &rules) -> RuleAction & {
            return rule(rules).action;
        },
        true);
    action->setObjectName("ModViewRuleAction");
    auto *duration =
        this->durationCombo(widget, [rule](Rules &rules) -> int & {
            return rule(rules).timeout;
        });
    auto *audience = this->combo(widget, 7);
    audience->setObjectName("ModViewRuleAudience");
    audience->addItem("Everyone", false);
    audience->addItem("Skip subscribers", true);
    audience->setToolTip(
        "Leave subscribers and channel members out of this rule");
    this->target_->push_back([this, audience, rule] {
        audience->setCurrentIndex(rule(this->rules_).skipSubscribers ? 1 : 0);
    });
    QObject::connect(audience, &QComboBox::currentIndexChanged, this,
                     [this, audience, rule] {
                         if (this->loading_)
                             return;
                         rule(this->rules_).skipSubscribers =
                             audience->currentData().toBool();
                         this->edited();
                     });
    const auto syncDuration = [action, duration] {
        duration->setVisible(RuleAction(action->currentData().toInt()) ==
                             RuleAction::Timeout);
    };
    QObject::connect(action, &QComboBox::currentIndexChanged, duration,
                     syncDuration);
    syncDuration();
    layout->addWidget(action, 3);
    layout->addWidget(duration, 1);
    layout->addWidget(audience, 3);
    return widget;
}

QWidget *ModViewRulesEditor::card(
    QVBoxLayout *target, const QString &title, const QString &tooltip,
    const QString &name, Field<RuleBase> rule,
    const std::function<void(QHBoxLayout *, QWidget *)> &params, bool actions)
{
    auto *card = new QWidget(this->page_);
    card->setObjectName("ModViewRuleCard");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(3);
    auto *enabled = this->toggle(card, title, name,
                                 [rule](Rules &rules) -> bool & {
                                     return rule(rules).enabled;
                                 });
    enabled->setToolTip(tooltip);
    layout->addWidget(enabled);
    if (params || actions)
    {
        auto *body = new QWidget(card);
        auto *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(
            ModViewSwitch::textIndent(this->owner_->font()), 0, 0, 4);
        bodyLayout->setSpacing(4);
        if (params)
        {
            auto *paramsRow = this->row(bodyLayout);
            params(paramsRow, body);
            paramsRow->addStretch();
        }
        if (actions)
            bodyLayout->addWidget(this->responseRow(body, rule));
        auto *reveal = new RevealBox(body, card);
        reveal->setOpen(false);
        layout->addWidget(reveal);
        QObject::connect(enabled, &QCheckBox::toggled, reveal,
                         [reveal](bool on) {
                             reveal->setOpen(on);
                         });
    }
    target->addWidget(card);
    return card;
}

void ModViewRulesEditor::buildTop(QVBoxLayout *layout)
{
    auto *profileTitle = label("Profile", this->page_);
    profileTitle->setProperty("section", true);
    layout->addWidget(profileTitle);
    auto *profileRow = this->row(layout);
    this->profiles_ = this->combo(this->page_, 12);
    this->profiles_->setObjectName("ModViewProfile");
    this->profiles_->setToolTip(
        "Rules are saved in profiles that several chats can share");
    profileRow->addWidget(this->profiles_, 1);
    this->profileMenu_ =
        new DrawnButton(DrawnButton::Symbol::Kebab, {}, this->owner_);
    this->profileMenu_->setObjectName("ModViewProfileMenu");
    this->profileMenu_->setToolTip("Profile options");
    profileRow->addWidget(this->profileMenu_);
    this->profileHint_ = label({}, this->page_);
    layout->addWidget(this->profileHint_);
    QObject::connect(this->profiles_, &QComboBox::activated, this, [this] {
        if (this->profileSelected)
            this->profileSelected(this->profiles_->currentData().toString());
    });
    QObject::connect(this->profileMenu_, &Button::leftClicked, this, [this] {
        QMenu menu(this);
        const auto command = [this](const QString &name) {
            return [this, name] {
                if (this->profileCommand)
                    this->profileCommand(name);
            };
        };
        menu.addAction("New profile...", this, command("new"));
        menu.addAction("Duplicate...", this, command("duplicate"));
        menu.addAction("Rename...", this, command("rename"));
        auto *remove = menu.addAction("Delete", this, command("delete"));
        remove->setEnabled(this->profiles_->currentData().toString() !=
                           ModerationProfiles::DEFAULT_ID);
        menu.addSeparator();
        menu.addAction("Import profiles...", this, command("import"));
        menu.addAction("Export this profile...", this, command("export"));
        menu.addAction("Export all profiles...", this, command("exportAll"));
        menu.exec(this->profileMenu_->mapToGlobal(
            QPoint(0, this->profileMenu_->height())));
    });

    auto *presetRow = this->row(layout);
    presetRow->setSpacing(1);
    for (const auto preset : {RulePreset::Relaxed, RulePreset::Standard,
                              RulePreset::Strict, RulePreset::Raid})
    {
        auto *chip =
            new ModViewButton(presetName(preset), this->owner_, {0, 4});
        chip->setObjectName("ModViewPreset" + presetName(preset));
        chip->setToolTip(
            "Set which rules run and how sensitive they are. Word lists, "
            "actions and exceptions stay as they are.");
        presetRow->addWidget(chip, 1);
        this->presets_.emplace_back(preset, chip);
        QObject::connect(chip, &Button::leftClicked, this, [this, preset] {
            this->rules_.applyPreset(preset);
            this->load();
            this->edited();
        });
    }
    this->presetHint_ = label({}, this->page_);
    layout->addWidget(this->presetHint_);

    auto *testRow = this->row(layout);
    this->test_ = new ModViewButton("Test on recent chat", this->owner_);
    this->test_->setObjectName("ModViewTestRules");
    this->test_->setToolTip(
        "See who these rules would have flagged in the chat you've loaded. "
        "Nothing is sent.");
    testRow->addWidget(this->test_);
    testRow->addStretch();
    QObject::connect(this->test_, &Button::leftClicked, this, [this] {
        if (this->testRequested)
            this->testRequested();
    });
    this->testSummary_ = label({}, this->page_, false);
    this->testSummary_->setObjectName("ModViewTestSummary");
    this->testSummary_->hide();
    layout->addWidget(this->testSummary_);
    this->testExamples_ = label({}, this->page_);
    this->testExamples_->hide();
    layout->addWidget(this->testExamples_);

    this->platformsRow_ = new QWidget(this->page_);
    auto *platforms = new QVBoxLayout(this->platformsRow_);
    platforms->setContentsMargins(0, 8, 0, 0);
    platforms->setSpacing(3);
    auto *platformsTitle = label("Active on", this->platformsRow_);
    platformsTitle->setProperty("section", true);
    platforms->addWidget(platformsTitle);
    this->platformBoxes_ = this->row(platforms);
    this->platformsRow_->hide();
    layout->addWidget(this->platformsRow_);

    auto *master = new QWidget(this->page_);
    auto *masterLayout = new QVBoxLayout(master);
    masterLayout->setContentsMargins(0, 8, 0, 0);
    masterLayout->setSpacing(3);
    masterLayout->addWidget(this->toggle(master, "Flag users automatically",
                                         "ModViewAutoMark", RULE(enabled)));
    masterLayout->addWidget(this->toggle(master, "Run automatic actions",
                                         "ModViewAutoActions",
                                         RULE(automaticActions)));
    masterLayout->addWidget(label(
        "Rules flag people for review. Rules set to delete, warn, time out "
        "or ban do so on the platform the message came from, while Mod View "
        "is open. Mods, VIPs, owners and you are never actioned.",
        master));
    layout->addWidget(master);
}

void ModViewRulesEditor::buildBehaviour()
{
    auto *layout = this->section(
        "Chat behaviour",
        "Switch a rule on to set its limits and what happens.");
    const auto text = [](QHBoxLayout *row, QWidget *parent,
                         const QString &value) {
        row->addWidget(label(value, parent, true, false));
    };
    this->card(layout, "Message bursts",
               "Many messages from one person in a short time",
               "ModViewBursts", RULE(bursts),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(
                       this->number(body, 2, 100, RULE(bursts.count)));
                   text(row, body, "messages within");
                   row->addWidget(this->number(body, 2, 120,
                                               RULE(bursts.window), " s", 44));
               });
    this->card(
        layout, "Repeated messages", "The same message from one person",
        "ModViewRepeats", RULE(repeats), [&](QHBoxLayout *row, QWidget *body) {
            row->addWidget(this->number(body, 2, 30, RULE(repeats.count)));
            text(row, body, "times");
            auto *mode = this->combo(body, 5);
            mode->setObjectName("ModViewRepeatMode");
            mode->addItem("within", false);
            mode->addItem("in a row", true);
            row->addWidget(mode);
            auto *window =
                this->number(body, 2, 300, RULE(repeats.window), " s", 44);
            row->addWidget(window);
            this->target_->push_back([this, mode] {
                mode->setCurrentIndex(this->rules_.repeats.inARow ? 1 : 0);
            });
            QObject::connect(mode, &QComboBox::currentIndexChanged, this,
                             [this, mode, window] {
                                 window->setVisible(
                                     !mode->currentData().toBool());
                                 if (this->loading_)
                                     return;
                                 this->rules_.repeats.inARow =
                                     mode->currentData().toBool();
                                 this->edited();
                             });
        });
    this->card(
        layout, "Links", "Links posted by one person, and blocked domains",
        "ModViewLinks", RULE(links), [&](QHBoxLayout *row, QWidget *body) {
            row->addWidget(this->number(body, 1, 30, RULE(links.count)));
            text(row, body, "links within");
            row->addWidget(
                this->number(body, 2, 300, RULE(links.window), " s", 44));
            auto *lists = body->layout();
            lists->addWidget(this->textList(
                body, RULE(links.allowed),
                "Allowed domains, one per line, like clips.twitch.tv",
                "ModViewAllowedDomains"));
            lists->addWidget(this->textList(
                body, RULE(links.blocked),
                "Blocked domains, flagged right away, like bit.ly",
                "ModViewBlockedDomains"));
        });
    this->card(layout, "Same text, many users",
               "Cross-account matches are leads for review, not proof of "
               "botting.",
               "ModViewCoordinated", RULE(coordinated),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(
                       this->number(body, 3, 30, RULE(coordinated.count)));
                   text(row, body, "accounts within");
                   row->addWidget(this->number(
                       body, 2, 120, RULE(coordinated.window), " s", 44));
               });
    this->card(layout, "Caps spam", "Messages that are mostly capital letters",
               "ModViewCaps", RULE(caps),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(this->number(body, 20, 100,
                                               RULE(caps.percent), "%", 44));
                   text(row, body, "capitals, at least");
                   row->addWidget(
                       this->number(body, 5, 500, RULE(caps.minLength)));
                   text(row, body, "letters");
               });
    this->card(layout, "Symbol spam", "Messages that are mostly symbols",
               "ModViewSymbols", RULE(symbols),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(this->number(
                       body, 20, 100, RULE(symbols.percent), "%", 44));
                   text(row, body, "symbols, at least");
                   row->addWidget(
                       this->number(body, 5, 500, RULE(symbols.minLength)));
                   text(row, body, "characters");
               });
    this->card(layout, "Emote spam", "Many emotes in one message",
               "ModViewEmotes", RULE(emotes),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(
                       this->number(body, 2, 100, RULE(emotes.limit)));
                   text(row, body, "or more emotes in a message");
               });
    this->card(layout, "Walls of text and ASCII art",
               "Very long messages, or drawings made of characters",
               "ModViewWalls", RULE(walls),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(this->number(body, 100, 2000,
                                               RULE(walls.length), {}, 44));
                   text(row, body, "characters or more");
               });
    this->card(layout, "Mention spam", "Tagging many people at once",
               "ModViewMentions", RULE(mentions),
               [&](QHBoxLayout *row, QWidget *body) {
                   row->addWidget(
                       this->number(body, 2, 30, RULE(mentions.limit)));
                   text(row, body, "or more people mentioned");
               });
    this->card(layout, "Unusual characters",
               "Zalgo text, and hidden characters used to get around filters",
               "ModViewUnusual", RULE(unusual), {});
}

void ModViewRulesEditor::buildAccounts()
{
    auto *layout = this->section("Who's talking");
    const auto text = [](QHBoxLayout *row, QWidget *parent,
                         const QString &value) {
        row->addWidget(label(value, parent, true, false));
    };
    this->card(layout, "Platform flags",
               "Messages held or flagged by AutoMod and other platform tools",
               "ModViewPlatformFlags", RULE(platformAlerts), {});
    this->card(layout, "First-time chatters",
               "Someone's first message in this chat. Twitch only.",
               "ModViewFirstMessages", RULE(firstMessages), {});
    this->card(layout, "New accounts",
               "Accounts created recently. Twitch only, looked up once per "
               "person.",
               "ModViewNewAccounts", RULE(newAccounts),
               [&](QHBoxLayout *row, QWidget *body) {
                   text(row, body, "Created");
                   row->addWidget(
                       this->number(body, 1, 365, RULE(newAccounts.limit)));
                   text(row, body, "days ago or less");
               });
    this->card(layout, "Username patterns",
               "Names like known spam bots or someone evading a ban",
               "ModViewNames", RULE(names), [&](QHBoxLayout *, QWidget *body) {
                   body->layout()->addWidget(this->textList(
                       body, RULE(names.patterns),
                       "One pattern per line. * matches anything, like "
                       "*_bot or hoss*",
                       "ModViewNamePatterns"));
               });
    this->card(layout, "Returning offenders",
               "Someone timed out or banned this session starts talking again",
               "ModViewReturning", RULE(returning), {});
}

void ModViewRulesEditor::buildWordLists()
{
    auto *layout = this->section(
        "Word lists",
        "Catches spelling tricks like l33t, s p a c e s, dots and stretched "
        "words.");
    this->listsBox_ = new QWidget(this->page_);
    this->listsLayout_ = new QVBoxLayout(this->listsBox_);
    this->listsLayout_->setContentsMargins(0, 0, 0, 0);
    this->listsLayout_->setSpacing(2);
    layout->addWidget(this->listsBox_);
    auto *buttons = this->row(layout);
    auto *create = new ModViewButton("New list", this->owner_);
    create->setObjectName("ModViewNewWordList");
    auto *url = new ModViewButton("From URL...", this->owner_);
    url->setObjectName("ModViewImportWordListUrl");
    url->setToolTip("Import a word list from a link, like a public list");
    auto *file = new ModViewButton("From file...", this->owner_);
    file->setToolTip("Import a word list from a text or JSON file");
    for (auto *button : {create, url, file})
        buttons->addWidget(button);
    buttons->addStretch();
    this->status_ = label({}, this->page_);
    this->status_->setObjectName("ModViewRulesStatus");
    this->status_->hide();
    layout->addWidget(this->status_);
    QObject::connect(create, &Button::leftClicked, this, [this] {
        this->addTerms(-1, "Word list", {}, {}, false);
    });
    QObject::connect(url, &Button::leftClicked, this, [this] {
        this->importListUrl(-1);
    });
    QObject::connect(file, &Button::leftClicked, this, [this] {
        this->importListFile(-1);
    });
}

void ModViewRulesEditor::buildExceptions()
{
    auto *layout =
        this->section("Exceptions", "Never flagged or acted on by rules.");
    layout->addWidget(this->toggle(this->page_,
                                   "Skip moderators, VIPs and owners",
                                   "ModViewTrusted", RULE(excludeTrusted)));
    layout->addWidget(this->textList(
        this->page_, RULE(allowList),
        "Allow-list, one per line: name, platform:name, or name #id",
        "ModViewAllowList"));
    auto *buttons = this->row(layout);
    auto *importButton = new ModViewButton("Import...", this->owner_);
    auto *exportButton = new ModViewButton("Export...", this->owner_);
    buttons->addWidget(importButton);
    buttons->addWidget(exportButton);
    buttons->addStretch();
    QObject::connect(importButton, &Button::leftClicked, this,
                     &ModViewRulesEditor::importAllowList);
    QObject::connect(exportButton, &Button::leftClicked, this,
                     &ModViewRulesEditor::exportAllowList);
}

void ModViewRulesEditor::buildEscalation()
{
    auto *layout = this->section(
        "Escalation",
        "Rules set to Escalate step up each time someone is actioned "
        "automatically this session.");
    const QString steps[] = {"1st time", "2nd time", "3rd time",
                             "After that"};
    for (size_t i = 0; i < std::size(steps); ++i)
    {
        auto *line = this->row(layout);
        auto *name = label(steps[i], this->page_, true, false);
        name->setMinimumWidth(int(60 * this->scale_));
        line->addWidget(name);
        auto *action = this->actionCombo(
            this->page_,
            [i](Rules &rules) -> RuleAction & {
                return rules.escalation[i].action;
            },
            false);
        auto *duration =
            this->durationCombo(this->page_, [i](Rules &rules) -> int & {
                return rules.escalation[i].timeout;
            });
        const auto sync = [action, duration] {
            duration->setVisible(RuleAction(action->currentData().toInt()) ==
                                 RuleAction::Timeout);
        };
        QObject::connect(action, &QComboBox::currentIndexChanged, duration,
                         sync);
        sync();
        line->addWidget(action, 3);
        line->addWidget(duration, 1);
        line->addStretch(1);
    }
}

void ModViewRulesEditor::buildLockdown()
{
    auto *layout = this->section(
        "Raid lockdown",
        "Tightens chat in one click. Ending it only undoes what it turned "
        "on.");
    auto *top = this->row(layout);
    this->lockdown_ = new ModViewButton("Lock down chat", this->owner_);
    this->lockdown_->setObjectName("ModViewLockdown");
    top->addWidget(this->lockdown_);
    top->addStretch();
    QObject::connect(this->lockdown_, &Button::leftClicked, this, [this] {
        if (this->lockdownToggled)
            this->lockdownToggled();
    });
    this->lockdownHint_ = label({}, this->page_);
    layout->addWidget(this->lockdownHint_);

    const auto option = [&](const QString &text, const QString &name,
                            Field<bool> field) {
        auto *line = this->row(layout);
        line->addWidget(this->toggle(this->page_, text, name, field));
        return line;
    };
    auto *followers = option("Followers-only, followed", "ModViewLockFollowers",
                             RULE(lockdown.followersOnly));
    followers->addWidget(this->number(this->page_, 0, 129600,
                                      RULE(lockdown.followersMinutes), " min",
                                      54));
    followers->addStretch();
    auto *slow =
        option("Slow mode", "ModViewLockSlow", RULE(lockdown.slowMode));
    slow->addWidget(this->number(this->page_, 3, 120,
                                 RULE(lockdown.slowSeconds), " s", 44));
    slow->addStretch();
    option("Emote-only", "ModViewLockEmotes", RULE(lockdown.emoteOnly));
    option("Subscribers-only", "ModViewLockSubscribers",
           RULE(lockdown.subscribersOnly));
    option("Shield mode", "ModViewLockShield", RULE(lockdown.shieldMode));
    option("Use the Raid preset's rules while locked down",
           "ModViewLockRules", RULE(lockdown.raidRules));
}

void ModViewRulesEditor::buildChat()
{
    auto *layout = this->section("In chat");
    auto *line = this->row(layout);
    this->inlineActions_ =
        new ModViewSwitch("Inline moderation buttons", this->page_);
    this->inlineActions_->setObjectName("ModViewInlineActions");
    line->addWidget(this->inlineActions_);
    line->addStretch();
    auto *configure = new ModViewButton("Configure", this->owner_);
    line->addWidget(configure);
    QObject::connect(this->inlineActions_, &QCheckBox::toggled, this,
                     [this](bool on) {
                         if (!this->loading_ && this->inlineActionsToggled)
                             this->inlineActionsToggled(on);
                     });
    QObject::connect(configure, &Button::leftClicked, this, [this] {
        SettingsDialog::showDialog(this,
                                   SettingsDialogPreference::ModerationActions);
    });
    layout->addSpacing(6);
    layout->addWidget(label(
        "Profiles are saved in your settings. Message history stays in "
        "memory.",
        this->page_));
}

void ModViewRulesEditor::rebuildListCards()
{
    this->listLoaders_.clear();
    while (auto *item = this->listsLayout_->takeAt(0))
    {
        if (auto *widget = item->widget())
        {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    this->target_ = &this->listLoaders_;
    for (int i = 0; i < int(this->rules_.lists.size()); ++i)
        this->listsLayout_->addWidget(this->listCard(i));
    this->target_ = &this->loaders_;
    if (this->rules_.lists.empty())
        this->listsLayout_->addWidget(
            label("No word lists yet. Make one, or import a public list.",
                  this->listsBox_));
    const bool loading = std::exchange(this->loading_, true);
    for (const auto &loader : this->listLoaders_)
        loader();
    this->loading_ = loading;
}

QWidget *ModViewRulesEditor::listCard(int index)
{
    const auto rule = [index](Rules &rules) -> RuleBase & {
        return rules.lists[size_t(index)];
    };
    const auto &list = this->rules_.lists[size_t(index)];
    auto *card = new QWidget(this->listsBox_);
    card->setObjectName("ModViewWordList");
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(0, 2, 0, 2);
    layout->setSpacing(3);
    auto *header = this->row(layout);
    auto *enabled = this->toggle(card, list.name, "ModViewWordListEnabled",
                                 [rule](Rules &rules) -> bool & {
                                     return rule(rules).enabled;
                                 });
    enabled->setToolTip(list.source.isEmpty() ? QString()
                                              : "From " + list.source);
    header->addWidget(enabled, 1);
    const auto countText = [](qsizetype count) {
        return QString("%1 %2").arg(count).arg(count == 1 ? "word" : "words");
    };
    auto *count = label(countText(list.terms.size()), card, true, false);
    header->addWidget(count);
    auto *menu = new DrawnButton(DrawnButton::Symbol::Kebab, {}, this->owner_);
    menu->setObjectName("ModViewWordListMenu");
    menu->setToolTip("List options");
    menu->setFixedSize(int(22 * this->scale_), int(22 * this->scale_));
    header->addWidget(menu);
    QObject::connect(menu, &Button::leftClicked, this, [this, index, menu] {
        this->showListMenu(index, menu);
    });

    auto *body = new QWidget(card);
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(
        ModViewSwitch::textIndent(this->owner_->font()), 0, 0, 4);
    bodyLayout->setSpacing(4);
    bodyLayout->addWidget(this->responseRow(body, rule));
    auto *options = this->row(bodyLayout);
    options->addWidget(this->toggle(body, "Whole words only",
                                    "ModViewWordListWhole",
                                    [index](Rules &rules) -> bool & {
                                        return rules.lists[size_t(index)]
                                            .wholeWords;
                                    }));
    options->addStretch();
    auto *show = new ModViewButton("Show words", this->owner_);
    options->addWidget(show);
    auto *terms = this->textList(
        body,
        [index](Rules &rules) -> QStringList & {
            return rules.lists[size_t(index)].terms;
        },
        "One word or phrase per line. * matches anything, like noob*",
        "ModViewWordListTerms");
    terms->setMaximumHeight(int(150 * this->scale_));
    terms->hide();
    bodyLayout->addWidget(terms);
    QObject::connect(show, &Button::leftClicked, terms, [terms, show] {
        terms->setVisible(terms->isHidden());
        show->setText(terms->isHidden() ? "Show words" : "Hide words");
    });
    QObject::connect(terms, &QPlainTextEdit::textChanged, count,
                     [this, index, count, countText] {
                         if (size_t(index) < this->rules_.lists.size())
                             count->setText(countText(
                                 this->rules_.lists[size_t(index)]
                                     .terms.size()));
                     });
    auto *reveal = new RevealBox(body, card);
    reveal->setOpen(list.enabled);
    layout->addWidget(reveal);
    QObject::connect(enabled, &QCheckBox::toggled, reveal, [reveal](bool on) {
        reveal->setOpen(on);
    });
    return card;
}

void ModViewRulesEditor::showListMenu(int index, QWidget *anchor)
{
    if (size_t(index) >= this->rules_.lists.size())
        return;
    const auto list = this->rules_.lists[size_t(index)];
    QMenu menu(this);
    menu.addAction("Add words from a file...", this, [this, index] {
        this->importListFile(index);
    });
    menu.addAction("Add words from a URL...", this, [this, index] {
        this->importListUrl(index);
    });
    auto *refresh = menu.addAction("Refresh from source", this, [this, list] {
        this->fetchList(-2, list.source, list.name);
    });
    refresh->setEnabled(!list.source.isEmpty());
    menu.addAction("Export...", this, [this, index] {
        this->exportList(index);
    });
    menu.addSeparator();
    menu.addAction("Rename...", this, [this, index, list] {
        bool ok = false;
        const auto name = QInputDialog::getText(
            this, "Rename word list", "Name", QLineEdit::Normal, list.name,
            &ok);
        if (!ok || name.trimmed().isEmpty() ||
            size_t(index) >= this->rules_.lists.size())
            return;
        this->rules_.lists[size_t(index)].name = name.trimmed().left(60);
        this->load();
        this->edited();
    });
    menu.addAction("Delete list", this, [this, index, list] {
        const auto answer = QMessageBox::question(
            this, "Delete word list",
            QString("Delete \"%1\" and its %2 words from this profile?")
                .arg(list.name)
                .arg(list.terms.size()));
        if (answer != QMessageBox::Yes ||
            size_t(index) >= this->rules_.lists.size())
            return;
        this->rules_.lists.erase(this->rules_.lists.begin() + index);
        this->load();
        this->edited();
    });
    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));
}

void ModViewRulesEditor::importListFile(int index)
{
    const auto path = QFileDialog::getOpenFileName(
        this, "Import word list", {},
        "Word lists (*.txt *.json);;All files (*)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        this->setStatus("Couldn't open " + QFileInfo(path).fileName() + ".");
        return;
    }
    if (file.size() > MAX_LIST_BYTES)
    {
        this->setStatus("That file is too large for a word list.");
        return;
    }
    const auto terms = parseTermList(QString::fromUtf8(file.readAll()));
    if (terms.isEmpty())
    {
        this->setStatus("No words found in " + QFileInfo(path).fileName() +
                        ".");
        return;
    }
    bool replace = false;
    if (size_t(index) < this->rules_.lists.size() &&
        !this->rules_.lists[size_t(index)].terms.isEmpty())
    {
        QMessageBox box(QMessageBox::Question, "Import word list",
                        QString("Add %1 words to \"%2\", or replace the "
                                "words it has?")
                            .arg(terms.size())
                            .arg(this->rules_.lists[size_t(index)].name),
                        QMessageBox::Cancel, this);
        auto *add = box.addButton("Add", QMessageBox::AcceptRole);
        auto *swap = box.addButton("Replace", QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() != add && box.clickedButton() != swap)
            return;
        replace = box.clickedButton() == swap;
    }
    this->addTerms(index, QFileInfo(path).completeBaseName(), terms, {},
                   replace);
}

void ModViewRulesEditor::importListUrl(int index)
{
    const bool existing = size_t(index) < this->rules_.lists.size();
    QDialog dialog(this);
    dialog.setWindowTitle(
        existing ? "Add words to " + this->rules_.lists[size_t(index)].name
                 : QString("Import a word list"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *suggested = new QComboBox(&dialog);
    suggested->addItem("Choose a public list...");
    for (const auto &list : suggestedLists())
        suggested->addItem(list.title, list.source);
    layout->addWidget(new QLabel("Public lists", &dialog));
    layout->addWidget(suggested);
    auto *url = new QLineEdit(&dialog);
    url->setPlaceholderText("https://... plain text or JSON");
    layout->addWidget(new QLabel("Address", &dialog));
    layout->addWidget(url);
    QLineEdit *name = nullptr;
    if (!existing)
    {
        name = new QLineEdit(&dialog);
        name->setPlaceholderText("Word list");
        layout->addWidget(new QLabel("Name", &dialog));
        layout->addWidget(name);
    }
    auto *note = new QLabel(
        "The words are copied into this profile. Check what a list contains "
        "before letting it time out or ban anyone.",
        &dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("Import");
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
    QObject::connect(
        suggested, &QComboBox::currentIndexChanged, &dialog, [=](int row) {
            if (row <= 0)
                return;
            const auto &list = suggestedLists()[size_t(row - 1)];
            url->setText(list.source);
            if (name)
                name->setText(list.name);
        });
    dialog.resize(int(420 * this->scale_), dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted || url->text().trimmed().isEmpty())
        return;
    this->fetchList(existing ? index : -1, url->text().trimmed(),
                    existing ? this->rules_.lists[size_t(index)].name
                             : (name->text().trimmed().isEmpty()
                                    ? QString("Word list")
                                    : name->text().trimmed()));
}

void ModViewRulesEditor::fetchList(int index, const QString &source,
                                   const QString &name)
{
    const auto [address, tags] = splitListSource(source);
    const QUrl url(address);
    if (!url.isValid() ||
        (url.scheme() != "https" && url.scheme() != "http"))
    {
        this->setStatus("That isn't a web address.");
        return;
    }
    this->setStatus("Downloading the list from " + url.host() + "...");
    // -1 makes a new list, -2 refreshes the list with this name
    NetworkRequest(url)
        .timeout(20000)
        .followRedirects(true)
        .caller(this)
        .onSuccess([this, index, source, name,
                    tags = tags](const NetworkResult &result) {
            const auto &data = result.getData();
            if (data.size() > MAX_LIST_BYTES)
            {
                this->setStatus("That list is too large to import.");
                return;
            }
            const auto terms = parseTermList(QString::fromUtf8(data), tags);
            if (terms.isEmpty())
            {
                this->setStatus("No words found at that address.");
                return;
            }
            int target = -1;
            for (size_t i = 0; i < this->rules_.lists.size(); ++i)
                if (index != -1 && this->rules_.lists[i].name == name)
                    target = int(i);
            this->addTerms(target, name, terms, source, index == -2);
        })
        .onError([this](const NetworkResult &result) {
            this->setStatus("Couldn't download the list: " +
                            result.formatError());
        })
        .execute();
}

void ModViewRulesEditor::addTerms(int index, const QString &name,
                                  const QStringList &terms,
                                  const QString &source, bool replace)
{
    QString target;
    qsizetype added = terms.size();
    if (size_t(index) >= this->rules_.lists.size())
    {
        if (this->rules_.lists.size() >= 32)
        {
            this->setStatus("A profile can have up to 32 word lists.");
            return;
        }
        WordList list;
        list.name = name.trimmed().isEmpty() ? QString("Word list")
                                             : name.trimmed().left(60);
        list.enabled = true;
        list.terms = terms;
        list.source = source;
        this->rules_.lists.push_back(list);
        target = list.name;
    }
    else
    {
        auto &list = this->rules_.lists[size_t(index)];
        const auto before = list.terms.size();
        list.terms = replace ? terms : mergedTerms(list.terms, terms);
        added = replace ? terms.size() : list.terms.size() - before;
        if (!source.isEmpty())
            list.source = source;
        target = list.name;
    }
    if (!terms.isEmpty())
        this->setStatus(QString("%1 %2 words in \"%3\".")
                            .arg(replace ? "Loaded" : "Added")
                            .arg(added)
                            .arg(target));
    this->load();
    this->edited();
}

void ModViewRulesEditor::exportList(int index)
{
    if (size_t(index) >= this->rules_.lists.size())
        return;
    const auto &list = this->rules_.lists[size_t(index)];
    const auto path = QFileDialog::getSaveFileName(
        this, "Export word list", list.name + ".txt", "Text files (*.txt)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write((list.terms.join('\n') + '\n').toUtf8()) < 0)
    {
        this->setStatus("Couldn't save " + QFileInfo(path).fileName() + ".");
        return;
    }
    this->setStatus(QString("Saved %1 words to %2.")
                        .arg(list.terms.size())
                        .arg(QFileInfo(path).fileName()));
}

void ModViewRulesEditor::importAllowList()
{
    const auto path = QFileDialog::getOpenFileName(
        this, "Import allow-list", {}, "Text files (*.txt);;All files (*)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > MAX_LIST_BYTES)
    {
        this->setStatus("Couldn't open " + QFileInfo(path).fileName() + ".");
        return;
    }
    QStringList entries;
    for (const auto &line : parseTermList(QString::fromUtf8(file.readAll())))
        if (auto entry = AllowEntry::parse(line))
            entries.append(entry->toString());
    const auto before = this->rules_.allowList.size();
    this->rules_.allowList = mergedTerms(this->rules_.allowList, entries);
    this->setStatus(QString("Added %1 people to the allow-list.")
                        .arg(this->rules_.allowList.size() - before));
    this->load();
    this->edited();
}

void ModViewRulesEditor::exportAllowList()
{
    const auto path = QFileDialog::getSaveFileName(
        this, "Export allow-list", "allow-list.txt", "Text files (*.txt)");
    if (path.isEmpty())
        return;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write((this->rules_.allowList.join('\n') + '\n').toUtf8()) < 0)
        this->setStatus("Couldn't save " + QFileInfo(path).fileName() + ".");
}

void ModViewRulesEditor::setStatus(const QString &text)
{
    this->status_->setText(text);
    this->status_->setVisible(!text.isEmpty());
}

void ModViewRulesEditor::setRules(const ReviewRules &rules)
{
    this->rules_ = rules;
    this->load();
}

const ReviewRules &ModViewRulesEditor::rules() const
{
    return this->rules_;
}

void ModViewRulesEditor::load()
{
    this->loading_ = true;
    for (const auto &loader : this->loaders_)
        loader();
    this->loading_ = false;
    this->rebuildListCards();
    this->updatePresets();
    this->rulesArea_->setEnabled(this->rules_.enabled);
}

void ModViewRulesEditor::edited()
{
    this->updatePresets();
    this->rulesArea_->setEnabled(this->rules_.enabled);
    if (this->changed)
        this->changed();
}

void ModViewRulesEditor::updatePresets()
{
    const auto match = this->rules_.matchingPreset();
    for (const auto &[preset, chip] : this->presets_)
        chip->setFill(match == preset ? this->selectedFill_ : this->fill_);
    this->presetHint_->setText(
        match ? QString("Sensitivity: %1 preset").arg(presetName(*match))
              : QString("Sensitivity: custom"));
}

void ModViewRulesEditor::setProfiles(const std::vector<ProfileItem> &profiles,
                                     const QString &current, int sharedWith)
{
    QSignalBlocker blocker(this->profiles_);
    this->profiles_->clear();
    for (const auto &profile : profiles)
        this->profiles_->addItem(profile.name, profile.id);
    this->profiles_->setCurrentIndex(
        std::max(0, this->profiles_->findData(current)));
    this->profileHint_->setText(
        sharedWith > 1
            ? QString("Shared with %1 other %2. Changes apply to all of them.")
                  .arg(sharedWith - 1)
                  .arg(sharedWith == 2 ? "chat" : "chats")
            : QString("Only this chat uses this profile."));
}

void ModViewRulesEditor::setPlatforms(
    const std::vector<MessagePlatform> &available,
    const QSet<MessagePlatform> &active)
{
    while (auto *item = this->platformBoxes_->takeAt(0))
    {
        if (auto *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (const auto platform : available)
    {
        auto *box =
            new ModViewSwitch(platformLabel(platform), this->platformsRow_);
        box->setObjectName("ModViewPlatform" + platformLabel(platform));
        box->setIcon(QIcon(platformIcon(platform)));
        box->setChecked(active.contains(platform));
        box->setToolTip(
            "Review and act on messages from " + platformLabel(platform));
        this->platformBoxes_->addWidget(box);
        QObject::connect(box, &QCheckBox::toggled, this,
                         [this, platform](bool on) {
                             if (this->platformToggled)
                                 this->platformToggled(platform, on);
                         });
    }
    this->platformBoxes_->addStretch();
    this->platformsRow_->setVisible(available.size() > 1);
}

void ModViewRulesEditor::setInlineActions(bool enabled)
{
    const bool loading = std::exchange(this->loading_, true);
    this->inlineActions_->setChecked(enabled);
    this->loading_ = loading;
}

void ModViewRulesEditor::setTestResult(const QString &summary,
                                       const QString &examples)
{
    this->testSummary_->setText(summary);
    this->testSummary_->setVisible(!summary.isEmpty());
    this->testExamples_->setText(examples);
    this->testExamples_->setVisible(!examples.isEmpty());
}

void ModViewRulesEditor::setLockdown(bool available, bool active)
{
    this->lockdown_->setText(active ? "End lockdown" : "Lock down chat");
    this->lockdown_->setEnabled(available || active);
    this->lockdownHint_->setText(
        active ? QString("Chat is locked down.")
        : available
            ? QString("Changes chat modes on Twitch. Other platforms only "
                      "get the stricter rules.")
            : QString("Needs moderator access on Twitch, or a Twitch chat "
                      "in this tab, to change chat modes."));
}

void ModViewRulesEditor::applyScale(float scale)
{
    this->scale_ = scale;
    for (const auto &[box, width] : this->spins_)
        if (box)
            box->setFixedWidth(int(width * scale));
    for (const auto &edit : this->edits_)
        if (edit)
            edit->setMaximumHeight(
                int((edit->objectName() == "ModViewWordListTerms" ? 150 : 76) *
                    scale));
    const auto menuSize = int(24 * scale);
    this->profileMenu_->setFixedSize(menuSize, menuSize);
}

void ModViewRulesEditor::applyColors(const QColor &fill,
                                     const QColor &selectedFill)
{
    this->fill_ = fill;
    this->selectedFill_ = selectedFill;
    this->updatePresets();
}

bool ModViewRulesEditor::eventFilter(QObject *watched, QEvent *event)
{
    // The page never gets wider than the pane, so text wraps instead of
    // running off the edge
    if (watched == this->viewport() && event->type() == QEvent::Resize)
        this->page_->setMaximumWidth(this->viewport()->width());
    // Scrolling the page must not change a number under the cursor
    if (event->type() == QEvent::Wheel)
    {
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched);
            spin && !spin->hasFocus())
        {
            QCoreApplication::sendEvent(this->verticalScrollBar(), event);
            return true;
        }
    }
    return QScrollArea::eventFilter(watched, event);
}

}  // namespace chatterino
