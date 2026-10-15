// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/MassModerationDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/commands/builtin/MassModeration.hpp"
#include "controllers/moderation/MassModerationQueue.hpp"
#include "messages/Message.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QCloseEvent>
#include <QFormLayout>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QSpinBox>
#include <QTextDocument>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace chatterino {
namespace {

std::vector<QPointer<MassModerationDialog>> &dialogs()
{
    static std::vector<QPointer<MassModerationDialog>> instances;
    return instances;
}

class PreviewActionButton final : public QToolButton
{
public:
    explicit PreviewActionButton(bool close, QWidget *parent)
        : QToolButton(parent)
        , close_(close)
    {
        this->setObjectName(close ? "MassModerationPreviewClose"
                                  : "MassModerationPreviewPause");
        this->setCursor(Qt::PointingHandCursor);
        this->setAutoRaise(true);
    }

    void setPaused(bool paused)
    {
        this->setProperty("paused", paused);
        this->setAccessibleName(paused ? "Resume mass moderation"
                                       : "Pause mass moderation");
        this->setToolTip(paused ? "Resume" : "Pause");
        this->update();
    }

    void setOpacity(qreal opacity)
    {
        this->opacity_ = opacity;
        this->setProperty("opacity", opacity);
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setOpacity(this->opacity_);
        auto color = this->palette().color(QPalette::ButtonText);
        if (this->isEnabled() && (this->underMouse() || this->hasFocus()))
        {
            auto hover = color;
            hover.setAlpha(this->isDown() ? 35 : 20);
            painter.setPen(Qt::NoPen);
            painter.setBrush(hover);
            painter.drawRoundedRect(this->rect().adjusted(1, 1, -1, -1), 4, 4);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        const auto size = this->width() * 0.45;
        const QPointF center(this->width() / 2.0, this->height() / 2.0);
        if (this->close_)
        {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(color,
                                std::max<qreal>(1.25, this->width() / 16.0),
                                Qt::SolidLine, Qt::RoundCap));
            const auto offset = size * 0.4;
            painter.drawLine(center + QPointF(-offset, -offset),
                             center + QPointF(offset, offset));
            painter.drawLine(center + QPointF(-offset, offset),
                             center + QPointF(offset, -offset));
        }
        else if (this->property("paused").toBool())
        {
            QPolygonF triangle;
            triangle << center + QPointF(-size * 0.35, -size / 2)
                     << center + QPointF(size * 0.5, 0)
                     << center + QPointF(-size * 0.35, size / 2);
            painter.drawPolygon(triangle);
        }
        else
        {
            painter.drawRoundedRect(
                QRectF(center.x() - size / 2, center.y() - size / 2, size * 0.3,
                       size),
                0.5, 0.5);
            painter.drawRoundedRect(
                QRectF(center.x() + size * 0.2, center.y() - size / 2,
                       size * 0.3, size),
                0.5, 0.5);
        }
    }

private:
    bool close_;
    qreal opacity_ = 1.0;
};

// Animate only the horizontal control space. The row keeps its height, and
// repeated progress updates do not restart a transition already in progress.
class PreviewControls final : public QWidget
{
public:
    explicit PreviewControls(QWidget *parent)
        : QWidget(parent)
        , pause(new PreviewActionButton(false, this))
        , close(new PreviewActionButton(true, this))
        , transition_(this)
    {
        this->setObjectName("MassModerationPreviewControls");
        this->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        this->transition_.setDuration(180);
        this->transition_.setEasingCurve(QEasingCurve::OutCubic);
        this->transition_.setStartValue(0.0);
        this->transition_.setEndValue(1.0);
        QObject::connect(
            &this->transition_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                const auto progress = value.toDouble();
                this->appearance_.slots =
                    this->from_.slots +
                    (this->target_.slots - this->from_.slots) * progress;
                // Fade an outgoing icon before the buttons approach
                // each other; bring X in after space opens for it.
                const auto pauseProgress = this->target_.pause == 0.0
                                               ? std::min(1.0, progress * 2.0)
                                               : progress;
                const auto closeProgress =
                    this->target_.close > this->from_.close
                        ? std::max(0.0, (progress - 0.35) / 0.65)
                        : std::min(1.0, progress * 2.0);
                this->appearance_.pause =
                    this->from_.pause +
                    (this->target_.pause - this->from_.pause) * pauseProgress;
                this->appearance_.close =
                    this->from_.close +
                    (this->target_.close - this->from_.close) * closeProgress;
                this->applyAppearance();
            });
    }

    void refresh(MassModerationQueue::State state)
    {
        using State = MassModerationQueue::State;
        this->pause->setPaused(state == State::Paused);
        this->pause->setEnabled(state == State::Running ||
                                state == State::Paused);
        const bool terminal =
            state == State::Stopped || state == State::Finished;
        this->canClose_ = state == State::Paused || terminal;
        this->close->setAccessibleName(state == State::Paused
                                           ? "Cancel mass moderation and close"
                                           : "Close mass moderation preview");
        this->close->setToolTip(state == State::Paused
                                    ? "Cancel queue and close"
                                    : "Close preview");
        if (this->initialized_ && this->state_ == state)
        {
            return;
        }
        this->state_ = state;
        this->transition_.stop();
        this->target_ = {state == State::Paused ? 2.0 : 1.0,
                         terminal ? 0.0 : 1.0,
                         state == State::Paused || terminal ? 1.0 : 0.0};
        if (!this->initialized_)
        {
            this->initialized_ = true;
            this->appearance_ = this->target_;
            this->applyAppearance();
            return;
        }
        this->from_ = this->appearance_;
        this->transition_.start();
    }

    void applyScale(float scale)
    {
        this->buttonSize_ = std::max(18, qRound(20 * scale));
        this->gap_ = std::max(3, qRound(4 * scale));
        this->setFixedHeight(this->buttonSize_);
        this->applyAppearance();
    }

    PreviewActionButton *pause;
    PreviewActionButton *close;

protected:
    void resizeEvent(QResizeEvent *) override
    {
        this->positionButtons();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        event->accept();
    }

private:
    struct Appearance {
        qreal slots = 1.0;
        qreal pause = 1.0;
        qreal close = 0.0;
    };

    void applyAppearance()
    {
        this->setFixedWidth(this->buttonSize_ +
                            qRound((this->buttonSize_ + this->gap_) *
                                   (this->appearance_.slots - 1.0)));
        this->pause->setOpacity(this->appearance_.pause);
        this->close->setOpacity(this->appearance_.close);
        this->close->setEnabled(this->canClose_ &&
                                this->appearance_.close >= 0.6);
        this->close->setAttribute(Qt::WA_TransparentForMouseEvents,
                                  !this->close->isEnabled());
        this->pause->setVisible(this->appearance_.pause > 0 ||
                                this->target_.pause > 0);
        this->close->setVisible(this->appearance_.close > 0 ||
                                this->target_.close > 0);
        this->positionButtons();
    }

    void positionButtons()
    {
        this->pause->setGeometry(0, 0, this->buttonSize_, this->buttonSize_);
        this->close->setGeometry(this->width() - this->buttonSize_, 0,
                                 this->buttonSize_, this->buttonSize_);
    }

    QVariantAnimation transition_;
    Appearance appearance_;
    Appearance from_;
    Appearance target_;
    MassModerationQueue::State state_ = MassModerationQueue::State::Idle;
    int buttonSize_ = 20;
    int gap_ = 4;
    bool initialized_ = false;
    bool canClose_ = false;
};

class ProgressButton final : public QPushButton
{
public:
    explicit ProgressButton(QWidget *parent)
        : QPushButton(parent)
        , animation_(this)
    {
        this->setObjectName("MassModerationPreviewButton");
        this->setFlat(true);
        this->setAttribute(Qt::WA_TranslucentBackground);
        this->setCursor(Qt::PointingHandCursor);
        auto *layout = new QHBoxLayout(this);
        auto *content = new QVBoxLayout;
        this->label_ = new QLabel(this);
        this->label_->setTextFormat(Qt::PlainText);
        this->label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        this->label_->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->bar_ = new QProgressBar(this);
        this->bar_->setObjectName("MassModerationPreviewBar");
        this->bar_->setAttribute(Qt::WA_TransparentForMouseEvents);
        this->bar_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        this->bar_->setTextVisible(false);
        this->bar_->setRange(0, 10000);
        this->bar_->setValue(0);
        content->addWidget(this->label_);
        content->addWidget(this->bar_);
        layout->addLayout(content, 1);
        this->controls_ = new PreviewControls(this);
        layout->addWidget(this->controls_, 0, Qt::AlignVCenter);
        this->animation_.setDuration(180);
        this->animation_.setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(&this->animation_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             this->bar_->setValue(value.toInt());
                         });
        QObject::connect(this->controls_->pause, &QToolButton::clicked, this,
                         [this] {
                             if (this->dialog_)
                             {
                                 this->dialog_->togglePause();
                             }
                         });
        QObject::connect(this->controls_->close, &QToolButton::clicked, this,
                         [this] {
                             if (this->dialog_)
                             {
                                 this->dialog_->cancelAndDismissPreview();
                             }
                         });
    }

    void refresh(MassModerationDialog &dialog, bool merged)
    {
        this->dialog_ = &dialog;
        auto text = dialog.previewText();
        if (merged)
        {
            text += dialog.property("massModerationPlatform").toString();
        }
        this->setAccessibleName(text);
        this->label_->setText(text);
        this->setToolTip("Open the full mass moderation menu");
        this->controls_->refresh(dialog.state());
        const auto target =
            dialog.total() > 0
                ? int(10000LL * dialog.previewCompleted() / dialog.total())
                : 0;
        if (target != this->target_)
        {
            this->target_ = target;
            this->animation_.stop();
            this->animation_.setStartValue(this->bar_->value());
            this->animation_.setEndValue(target);
            this->animation_.start();
        }
    }

    void applyTheme(const Theme &theme, float scale)
    {
        // Use readable UI text rather than shrinking the user's chat font.
        auto font = getApp()->getFonts()->getFont(FontStyle::UiMedium, scale);
        font.setPointSizeF(9.0 * scale);
        font.setStyleStrategy(QFont::PreferAntialias);
        font.setHintingPreference(QFont::PreferNoHinting);
        font.setFeature("tnum", 1);
        this->setFont(font);
        auto *layout = static_cast<QHBoxLayout *>(this->layout());
        layout->setContentsMargins(qRound(10 * scale), qRound(5 * scale),
                                   qRound(8 * scale), qRound(6 * scale));
        layout->setSpacing(qRound(6 * scale));
        layout->itemAt(0)->layout()->setSpacing(qRound(4 * scale));
        this->setMinimumHeight(std::max(30, qRound(32 * scale)));
        this->controls_->applyScale(scale);
        this->divider_ = theme.splits.header.border;
        this->divider_.setAlpha(70);
        this->hover_ = theme.splits.header.focusedBackground;
        this->hover_.setAlpha(90);
        this->bar_->setFixedHeight(std::max(2, qRound(3 * scale)));
        this->bar_->setStyleSheet(
            QString("QProgressBar { border: 0; border-radius: 2px; "
                    "background: %1; } QProgressBar::chunk { "
                    "border-radius: 2px; background: %2; }")
                .arg(theme.splits.input.border.name(), theme.accent.name()));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // SplitInput paints the shared rounded composer behind this row.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        if (this->underMouse() || this->hasFocus())
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(this->hover_);
            painter.drawRoundedRect(this->rect().adjusted(3, 3, -3, -3), 4, 4);
        }
        painter.setPen(this->divider_);
        painter.drawLine(1, this->height() - 1, this->width() - 2,
                         this->height() - 1);
    }

private:
    QColor divider_;
    QColor hover_;
    QLabel *label_;
    QProgressBar *bar_;
    PreviewControls *controls_;
    QPointer<MassModerationDialog> dialog_;
    QVariantAnimation animation_;
    int target_ = 0;
};

// Clip a fixed-height content row while its wrapper collapses. The text and
// controls never get squeezed or reflowed during dismissal.
class ProgressRow final : public QWidget
{
public:
    explicit ProgressRow(QWidget *parent)
        : QWidget(parent)
        , button(new ProgressButton(this))
        , dismissal_(this)
    {
        this->setObjectName("MassModerationPreviewRow");
        this->opacity_ = new QGraphicsOpacityEffect(this->button);
        this->opacity_->setOpacity(1);
        this->opacity_->setEnabled(false);
        this->button->setGraphicsEffect(this->opacity_);
        this->dismissal_.setDuration(200);
        this->dismissal_.setEasingCurve(QEasingCurve::InOutCubic);
        this->dismissal_.setStartValue(0.0);
        this->dismissal_.setEndValue(1.0);
        QObject::connect(&this->dismissal_, &QVariantAnimation::valueChanged,
                         this, [this](const QVariant &value) {
                             const auto progress = value.toDouble();
                             this->opacity_->setOpacity(1.0 - progress);
                             this->setFixedHeight(qRound(this->naturalHeight_ *
                                                         (1.0 - progress)));
                             if (this->heightChanged)
                             {
                                 this->heightChanged();
                             }
                         });
        QObject::connect(&this->dismissal_, &QVariantAnimation::finished, this,
                         [this] {
                             this->dismissed = true;
                             if (this->finished)
                             {
                                 this->finished();
                             }
                         });
    }

    void applyTheme(const Theme &theme, float scale)
    {
        this->button->applyTheme(theme, scale);
        this->naturalHeight_ =
            std::max(this->button->minimumHeight(),
                     this->button->layout()->sizeHint().height());
        this->button->setFixedHeight(this->naturalHeight_);
        const auto progress = this->dismissing_
                                  ? this->dismissal_.currentValue().toDouble()
                                  : 0.0;
        this->setFixedHeight(qRound(this->naturalHeight_ * (1.0 - progress)));
        this->button->resize(this->width(), this->naturalHeight_);
    }

    void dismiss()
    {
        if (this->dismissing_)
        {
            return;
        }
        this->dismissing_ = true;
        this->opacity_->setEnabled(true);
        this->button->setEnabled(false);
        this->dismissal_.start();
    }

    ProgressButton *button;
    std::function<void()> heightChanged;
    std::function<void()> finished;
    bool dismissed = false;

protected:
    void resizeEvent(QResizeEvent *) override
    {
        this->button->resize(this->width(), this->naturalHeight_);
    }

private:
    QGraphicsOpacityEffect *opacity_;
    QVariantAnimation dismissal_;
    int naturalHeight_ = 0;
    bool dismissing_ = false;
};

}  // namespace

MassModerationDialog::MassModerationDialog(ChannelPtr channel, bool ban,
                                           QWidget *parent)
    : BasePopup({BaseWindow::EnableCustomFrame, BaseWindow::CloseButtonOnly,
                 BaseWindow::DisableLayoutSave, BaseWindow::BoundsCheckOnShow},
                parent)
    , channel_(std::move(channel))
    , ban_(ban)
    , kick_(this->channel_->isKickChannel())
{
    this->setWindowTitle(ban ? "Mass ban" : "Mass unban");
    this->setProperty("massModerationPlatform",
                      this->kick_ ? " · Kick" : " · Twitch");
    this->setScaleIndependentSize(600, 610);
    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(16, 14, 16, 14);
    auto *target = new QLabel(
        QString("%1 in %2 / %3")
            .arg(ban ? "Ban accounts" : "Unban accounts",
                 kick_ ? "Kick" : "Twitch", this->channel_->getName()),
        this);
    target->setTextFormat(Qt::PlainText);
    root->addWidget(target);
    auto *instructions = new QLabel(
        "Paste usernames, one per line or separated by commas. Format List "
        "puts each name on its own line and removes duplicates.",
        this);
    instructions->setWordWrap(true);
    root->addWidget(instructions);

    this->names_ = new QPlainTextEdit(this);
    this->names_->setPlaceholderText("name1\nname2\nname3");
    root->addWidget(this->names_, 2);
    auto *listRow = new QHBoxLayout;
    this->format_ = new QPushButton("Format List", this);
    this->listStatus_ = new QLabel(this);
    this->listStatus_->setTextFormat(Qt::PlainText);
    this->listStatus_->setWordWrap(true);
    listRow->addWidget(this->listStatus_, 1);
    listRow->addWidget(this->format_);
    root->addLayout(listRow);

    auto *options = new QFormLayout;
    this->interval_ = new QSpinBox(this);
    this->interval_->setRange(1, 3600);
    this->interval_->setValue(1);
    this->interval_->setSuffix(" seconds");
    this->interval_->setToolTip("Minimum wait after each account. You can "
                                "change this while running or paused.");
    options->addRow("Wait between accounts:", this->interval_);
    if (ban)
    {
        this->reason_ = new QLineEdit(this);
        this->reason_->setMaxLength(500);
        this->reason_->setPlaceholderText("Optional reason for all bans");
        options->addRow("Ban reason:", this->reason_);
    }
    root->addLayout(options);
    this->progress_ = new QProgressBar(this);
    this->progress_->setRange(0, 1);
    this->progress_->setValue(0);
    root->addWidget(this->progress_);
    this->progressStatus_ =
        new QLabel("Ready. No actions are sent until you press Start.", this);
    this->progressStatus_->setWordWrap(true);
    this->progressStatus_->setTextFormat(Qt::PlainText);
    root->addWidget(this->progressStatus_);
    this->log_ = new QPlainTextEdit(this);
    this->log_->setReadOnly(true);
    this->log_->setPlaceholderText("Results will appear here.");
    this->log_->document()->setMaximumBlockCount(2000);
    root->addWidget(this->log_, 1);

    auto *buttons = new QHBoxLayout;
    this->cancel_ = new QPushButton("Cancel", this);
    this->pause_ = new QPushButton("Pause", this);
    this->stop_ = new QPushButton("Stop", this);
    this->start_ = new QPushButton(ban ? "Start bans" : "Start unbans", this);
    this->pause_->hide();
    this->stop_->hide();
    buttons->addWidget(this->cancel_);
    buttons->addStretch();
    buttons->addWidget(this->pause_);
    buttons->addWidget(this->stop_);
    buttons->addWidget(this->start_);
    root->addLayout(buttons);

    QObject::connect(this->names_, &QPlainTextEdit::textChanged, this, [this] {
        this->updateList();
    });
    QObject::connect(this->format_, &QPushButton::clicked, this, [this] {
        const auto list =
            parseMassModerationList(this->names_->toPlainText(), this->kick_);
        this->names_->setPlainText(list.formatted.join('\n'));
    });
    QObject::connect(this->start_, &QPushButton::clicked, this, [this] {
        if (this->queue_)
        {
            this->reset();
        }
        else
        {
            this->start();
        }
    });
    QObject::connect(this->cancel_, &QPushButton::clicked, this, [this] {
        this->close();
    });
    QObject::connect(this->pause_, &QPushButton::clicked, this, [this] {
        this->togglePause();
    });
    QObject::connect(this->stop_, &QPushButton::clicked, this, [this] {
        this->queue_->stop();
    });
    QObject::connect(this->interval_,
                     QOverload<int>::of(&QSpinBox::valueChanged), this,
                     [this](int seconds) {
                         if (this->queue_)
                         {
                             this->queue_->setInterval(seconds * 1000);
                         }
                     });
    this->previewExpiry_.setSingleShot(true);
    this->previewExpiry_.setInterval(90'000);
    this->previewExpiry_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&this->previewExpiry_, &QTimer::timeout, this, [this] {
        this->previewExpired_ = true;
        progressChanged().invoke();
    });
    this->previewFallback_.setSingleShot(true);
    this->previewFallback_.setInterval(250);
    QObject::connect(&this->previewFallback_, &QTimer::timeout, this, [this] {
        this->publishPreviewProgress();
    });
    this->signalHolder_.managedConnect(this->channel_->messageAppended,
                                       [this](MessagePtr &message, auto) {
                                           this->observePreviewMessage(message);
                                       });
    this->signalHolder_.managedConnect(
        this->channel_->messageReplaced,
        [this](size_t, const MessagePtr &, const MessagePtr &message) {
            this->observePreviewMessage(message);
        });
    this->updateList();
    this->themeChangedEvent();
    dialogs().push_back(this);
}

MassModerationDialog::~MassModerationDialog()
{
    this->signalHolder_.clear();
    this->queue_.reset();
    std::erase_if(dialogs(), [this](const auto &dialog) {
        return !dialog || dialog.data() == this;
    });
    progressChanged().invoke();
}

MassModerationDialog *MassModerationDialog::find(const ChannelPtr &channel,
                                                 bool ban)
{
    for (const auto &dialog : dialogs())
    {
        if (dialog && !dialog->sessionClosed_ && dialog->channel_ == channel &&
            dialog->ban_ == ban)
        {
            return dialog.data();
        }
    }
    return nullptr;
}

std::vector<MassModerationDialog *> MassModerationDialog::forChannel(
    const ChannelPtr &channel)
{
    std::vector<MassModerationDialog *> result;
    const auto merged = std::dynamic_pointer_cast<MergedChannel>(channel);
    for (const auto &dialog : dialogs())
    {
        if (dialog && dialog->queue_ &&
            (dialog->channel_ == channel ||
             (merged && (dialog->channel_ == merged->twitchChannel() ||
                         dialog->channel_ == merged->kickChannel()))))
        {
            result.push_back(dialog.data());
        }
    }
    return result;
}

pajlada::Signals::NoArgSignal &MassModerationDialog::progressChanged()
{
    static pajlada::Signals::NoArgSignal signal;
    return signal;
}

void MassModerationDialog::reopen()
{
    if (this->sessionClosed_)
    {
        return;
    }
    this->show();
    this->raise();
    this->activateWindow();
}

int MassModerationDialog::completed() const
{
    return this->queue_ ? this->queue_->completed() : 0;
}

int MassModerationDialog::total() const
{
    return this->queue_ ? this->queue_->total() : 0;
}

int MassModerationDialog::previewCompleted() const
{
    return this->previewCompleted_;
}

bool MassModerationDialog::previewExpired() const
{
    return this->previewExpired_;
}

MassModerationQueue::State MassModerationDialog::state() const
{
    return this->queue_ ? this->queue_->state()
                        : MassModerationQueue::State::Idle;
}

void MassModerationDialog::togglePause()
{
    if (!this->queue_)
    {
        return;
    }
    if (this->queue_->state() == MassModerationQueue::State::Paused)
    {
        this->queue_->resume();
    }
    else
    {
        this->queue_->pause();
    }
}

void MassModerationDialog::cancelAndDismissPreview()
{
    using State = MassModerationQueue::State;
    if (!this->queue_ || this->state() == State::Running ||
        this->state() == State::Idle || this->previewExpired_)
    {
        return;
    }
    // Retire the session before notifying previews, so reopening the command
    // immediately creates a fresh dialog even during the dismissal animation.
    this->sessionClosed_ = true;
    this->queue_->stop();
    this->previewExpiry_.stop();
    this->previewExpired_ = true;
    this->hide();
    progressChanged().invoke();
    // Keep the old row alive for its 200 ms fade/collapse, then release the
    // command-owned dialog. Queue and API callbacks already use weak guards.
    QTimer::singleShot(220, this, [this] {
        if (this->testAttribute(Qt::WA_DeleteOnClose))
        {
            this->deleteLater();
        }
    });
}

void MassModerationDialog::observePreviewMessage(const MessagePtr &message)
{
    if (!this->queue_ || !message ||
        message->flags.has(MessageFlag::RecentMessage) ||
        !message->flags.has(this->ban_ ? MessageFlag::Timeout
                                       : MessageFlag::Untimeout))
    {
        return;
    }
    const auto name = message->timeoutUser.toLower();
    if (name.isEmpty())
    {
        return;
    }
    if (name == this->previewCurrentUser_)
    {
        this->previewNoticeSeen_ = true;
    }
    if (name == this->previewWaitingUser_)
    {
        // Let the channel finish delivering the message to every chat view
        // before advancing both the label and fill on the next UI turn.
        QTimer::singleShot(0, this, [this] {
            this->publishPreviewProgress();
        });
    }
}

void MassModerationDialog::publishPreviewProgress()
{
    this->previewFallback_.stop();
    this->previewWaitingUser_.clear();
    if (this->queue_ && this->previewCompleted_ != this->queue_->completed())
    {
        this->previewCompleted_ = this->queue_->completed();
        progressChanged().invoke();
    }
}

QString MassModerationDialog::previewText() const
{
    auto text = QString("%1: %2/%3")
                    .arg(this->ban_ ? "Mass bans" : "Mass unbans")
                    .arg(this->previewCompleted())
                    .arg(this->total());
    if (this->queue_)
    {
        using State = MassModerationQueue::State;
        switch (this->queue_->state())
        {
            case State::Paused:
                text += " (paused)";
                break;
            case State::Stopped:
                text += " (stopped)";
                break;
            case State::Finished:
                if (this->previewCompleted() == this->total())
                {
                    text += " (finished)";
                }
                break;
            default:
                break;
        }
    }
    return text;
}

void MassModerationDialog::updateList()
{
    if (this->queue_)
    {
        return;
    }
    const auto list =
        parseMassModerationList(this->names_->toPlainText(), this->kick_);
    auto status = QString("%1 unique names; %2 duplicates removed.")
                      .arg(list.names.size())
                      .arg(list.duplicates);
    if (!list.invalid.isEmpty())
    {
        status += QString(" Fix %1 invalid entries, including: %2")
                      .arg(list.invalid.size())
                      .arg(list.invalid.first().left(80));
    }
    this->listStatus_->setText(status);
    this->start_->setEnabled(!list.names.isEmpty() && list.invalid.isEmpty());
}

void MassModerationDialog::start()
{
    const auto list =
        parseMassModerationList(this->names_->toPlainText(), this->kick_);
    if (this->queue_ || list.names.isEmpty() || !list.invalid.isEmpty())
    {
        return;
    }
    auto action = makeMassModerationAction(
        this->channel_, this->ban_,
        this->reason_ ? this->reason_->text() : QString{}, this);
    if (!action)
    {
        this->progressStatus_->setText(action.error());
        return;
    }
    this->startQueue(std::move(*action), list.names);
}

void MassModerationDialog::startQueue(MassModerationQueue::Action action,
                                      const QStringList &names)
{
    this->names_->setPlainText(names.join('\n'));
    this->names_->setReadOnly(true);
    this->format_->setEnabled(false);
    if (this->reason_)
    {
        this->reason_->setEnabled(false);
    }
    this->start_->hide();
    this->pause_->show();
    this->stop_->show();
    this->cancel_->setText("Hide");
    this->cancel_->setToolTip("The queue keeps running when hidden. Use Stop "
                              "to cancel remaining accounts.");
    this->previewExpiry_.stop();
    this->previewFallback_.stop();
    this->previewCompleted_ = 0;
    this->previewExpired_ = false;
    this->previewRetentionStarted_ = false;
    this->previewCurrentUser_.clear();
    this->previewWaitingUser_.clear();
    this->queue_ = std::make_unique<MassModerationQueue>(
        [this, action = std::move(action)](
            const QString &name, MassModerationQueue::Guard guard,
            MassModerationQueue::Completion done) {
            // Normally the visual fallback completes before the next request.
            // Flush it here too if an unusually short interval is used.
            this->publishPreviewProgress();
            this->previewCurrentUser_ = name.toLower();
            this->previewNoticeSeen_ = false;
            action(name, std::move(guard), std::move(done));
        });
    this->queue_->changed = [this] {
        this->updateProgress();
    };
    this->queue_->reported = [this](const QString &name,
                                    const MassModerationQueue::Result &result) {
        this->log_->appendPlainText(QString("%1: %2").arg(
            name, result.outcome == MassModerationQueue::Outcome::Success
                      ? (this->ban_ ? "Banned" : "Unbanned")
                      : result.message));
        using Outcome = MassModerationQueue::Outcome;
        if (result.outcome == Outcome::Success && !this->previewNoticeSeen_)
        {
            this->previewWaitingUser_ = name.toLower();
            // Some feeds omit moderation notices, especially Twitch unbans.
            // Never hold the preview indefinitely waiting for one.
            this->previewFallback_.start();
        }
        else if (result.outcome == Outcome::Success ||
                 result.outcome == Outcome::Failed)
        {
            QTimer::singleShot(0, this, [this] {
                this->publishPreviewProgress();
            });
        }
    };
    this->queue_->start(names, this->interval_->value() * 1000);
}

void MassModerationDialog::updateProgress()
{
    using State = MassModerationQueue::State;
    const auto state = this->queue_->state();
    const bool active = state == State::Running || state == State::Paused;
    this->pause_->setEnabled(active);
    this->stop_->setEnabled(active);
    this->pause_->setText(state == State::Paused ? "Resume" : "Pause");
    this->progress_->setRange(0, this->queue_->total());
    this->progress_->setValue(this->queue_->completed());
    const auto label = state == State::Paused     ? "Paused"
                       : state == State::Stopped  ? "Stopped"
                       : state == State::Finished ? "Finished"
                                                  : "Running";
    auto status = QString("%1 — %2 / %3 processed; %4 succeeded; %5 failed.")
                      .arg(label)
                      .arg(this->queue_->completed())
                      .arg(this->queue_->total())
                      .arg(this->queue_->succeeded())
                      .arg(this->queue_->failed());
    if (!active && state == State::Stopped)
    {
        status += " Remaining names cancelled.";
    }
    if (this->queue_->inFlight() && state != State::Running)
    {
        status += " A request already sent may still finish.";
    }
    this->progressStatus_->setText(status);
    const bool canReset = !active && !this->queue_->inFlight();
    if (canReset && !this->previewRetentionStarted_ && !this->previewExpired_)
    {
        this->previewRetentionStarted_ = true;
        this->previewExpiry_.start();
    }
    this->cancel_->setText(canReset ? "Close" : "Hide");
    this->start_->setText("New list");
    this->start_->setEnabled(canReset);
    this->start_->setVisible(canReset);
    progressChanged().invoke();
}

void MassModerationDialog::reset()
{
    using State = MassModerationQueue::State;
    if (!this->queue_ || this->queue_->inFlight() ||
        this->queue_->state() == State::Running ||
        this->queue_->state() == State::Paused)
    {
        return;
    }
    this->previewExpiry_.stop();
    this->previewFallback_.stop();
    this->previewCurrentUser_.clear();
    this->previewWaitingUser_.clear();
    this->previewNoticeSeen_ = false;
    this->previewCompleted_ = 0;
    this->previewExpired_ = false;
    this->previewRetentionStarted_ = false;
    this->queue_.reset();
    this->names_->setReadOnly(false);
    this->names_->clear();
    this->log_->clear();
    this->format_->setEnabled(true);
    if (this->reason_)
    {
        this->reason_->setEnabled(true);
    }
    this->pause_->hide();
    this->stop_->hide();
    this->cancel_->setText("Cancel");
    this->cancel_->setToolTip({});
    this->start_->setText(this->ban_ ? "Start bans" : "Start unbans");
    this->progress_->setRange(0, 1);
    this->progress_->setValue(0);
    this->progressStatus_->setText(
        "Ready. No actions are sent until you press Start.");
    this->updateList();
    progressChanged().invoke();
}

void MassModerationDialog::closeEvent(QCloseEvent *event)
{
    using State = MassModerationQueue::State;
    if (this->queue_ &&
        (this->queue_->state() == State::Running ||
         this->queue_->state() == State::Paused || this->queue_->inFlight()))
    {
        event->ignore();
        this->hide();
        return;
    }
    BasePopup::closeEvent(event);
}

void MassModerationDialog::themeChangedEvent()
{
    BasePopup::themeChangedEvent();
    // Preserve the theme's control roles instead of painting fields and
    // buttons with the window background. Default/Dark uses the original
    // Fusion greys; other presets keep their authored palette.
    auto palette = this->theme->palette;
    palette.setColor(QPalette::Window, this->theme->window.background);
    palette.setColor(QPalette::WindowText, this->theme->window.text);
    this->getLayoutContainer()->setPalette(palette);
    this->getLayoutContainer()->setAutoFillBackground(true);
}

struct MassModerationPreview::Impl {
    ChannelPtr channel;
    QVBoxLayout *layout;
    std::vector<std::pair<QPointer<MassModerationDialog>, ProgressRow *>> rows;
};

MassModerationPreview::MassModerationPreview(QWidget *parent)
    : BaseWidget(parent)
    , impl_(std::make_unique<Impl>())
{
    this->setObjectName("MassModerationPreview");
    this->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    this->impl_->layout = new QVBoxLayout(this);
    this->impl_->layout->setContentsMargins(0, 0, 0, 0);
    this->impl_->layout->setSpacing(0);
    this->hide();
    this->signalHolder_.managedConnect(MassModerationDialog::progressChanged(),
                                       [this] {
                                           this->refresh();
                                       });
    this->themeChangedEvent();
}

MassModerationPreview::~MassModerationPreview()
{
    this->signalHolder_.clear();
}

void MassModerationPreview::setChannel(ChannelPtr channel)
{
    this->impl_->channel = std::move(channel);
    this->refresh();
}

int MassModerationPreview::preferredHeight() const
{
    int height = 0;
    for (const auto &row : this->impl_->rows)
    {
        height += row.second->height();
    }
    return height;
}

void MassModerationPreview::refresh()
{
    const auto oldHeight = this->preferredHeight();
    const auto jobs = MassModerationDialog::forChannel(this->impl_->channel);
    std::erase_if(this->impl_->rows, [&jobs](const auto &row) {
        if (!row.first || row.second->dismissed ||
            std::ranges::find(jobs, row.first.data()) == jobs.end())
        {
            delete row.second;
            return true;
        }
        return false;
    });
    for (auto *dialog : jobs)
    {
        auto row =
            std::ranges::find_if(this->impl_->rows, [dialog](const auto &item) {
                return item.first.data() == dialog;
            });
        ProgressRow *progressRow;
        if (row == this->impl_->rows.end())
        {
            if (dialog->previewExpired())
            {
                continue;
            }
            progressRow = new ProgressRow(this);
            progressRow->applyTheme(*this->theme, this->scale());
            progressRow->heightChanged = [this] {
                this->impl_->layout->invalidate();
                this->setFixedHeight(this->preferredHeight());
                this->heightChanged.invoke();
            };
            progressRow->finished = [this] {
                // Removal must happen after the animation callback returns.
                QTimer::singleShot(0, this, [this] {
                    this->refresh();
                });
            };
            QObject::connect(progressRow->button, &QPushButton::clicked, this,
                             [guard = QPointer<MassModerationDialog>(dialog)] {
                                 if (guard)
                                 {
                                     guard->reopen();
                                 }
                             });
            this->impl_->layout->addWidget(progressRow);
            this->impl_->rows.emplace_back(dialog, progressRow);
        }
        else
        {
            progressRow = row->second;
        }
        progressRow->button->refresh(
            *dialog,
            this->impl_->channel && this->impl_->channel->isMergedChannel());
        if (dialog->previewExpired())
        {
            progressRow->dismiss();
        }
    }
    this->setFixedHeight(this->preferredHeight());
    this->setVisible(!this->impl_->rows.empty());
    this->updateGeometry();
    if (oldHeight != this->preferredHeight())
    {
        this->heightChanged.invoke();
    }
}

void MassModerationPreview::themeChangedEvent()
{
    BaseWidget::themeChangedEvent();
    auto palette = this->theme->palette;
    palette.setColor(QPalette::WindowText, this->theme->splits.input.text);
    palette.setColor(QPalette::ButtonText, this->theme->splits.input.text);
    this->setPalette(palette);
    for (const auto &row : this->impl_->rows)
    {
        row.second->applyTheme(*this->theme, this->scale());
    }
    this->setFixedHeight(this->preferredHeight());
}

void MassModerationPreview::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    this->themeChangedEvent();
    this->heightChanged.invoke();
}

}  // namespace chatterino
