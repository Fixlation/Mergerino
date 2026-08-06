#include "widgets/dialogs/KickPredictionDialog.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "widgets/Window.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

namespace chatterino {
namespace {

constexpr int DIALOG_WIDTH = 650;
constexpr int ACTIVE_HEIGHT = 350;
constexpr int CLOSED_HEIGHT = 345;
constexpr int CHOOSING_HEIGHT = 350;
constexpr int TIMER_BAR_HEIGHT = 6;
const QColor KICK_MINT("#18FBB0");
const QColor KICK_SALMON("#FEA0A0");
const QColor KICK_GREEN("#53FC18");

QColor outcomeColor(size_t i)
{
    return i == 0 ? KICK_MINT : KICK_SALMON;
}
QString outcomeBadgePath(size_t i)
{
    return i == 0 ? QStringLiteral(":/predictions/kick-outcome-1.svg")
                  : QStringLiteral(":/predictions/kick-outcome-2.svg");
}
QPixmap renderSvg(const QString &path, const QColor &color, int size)
{
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    QSvgRenderer renderer(path);
    renderer.setAspectRatioMode(Qt::KeepAspectRatio);
    renderer.render(&painter, QRectF(0, 0, size, size));
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(result.rect(), color);
    return result;
}
void clearLayout(QLayout *layout)
{
    while (auto *item = layout->takeAt(0))
    {
        if (auto *child = item->layout())
        {
            clearLayout(child);
        }
        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }
        delete item;
    }
}
void repolish(QWidget *widget)
{
    if (widget == nullptr)
    {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}
int totalPoints(const KickPrediction &prediction)
{
    int total = 0;
    for (const auto &outcome : prediction.outcomes)
    {
        total += outcome.totalVoteAmount;
    }
    return total;
}
int percentage(const KickPredictionOutcome &outcome, int total)
{
    return total <= 0
               ? 0
               : qRound(static_cast<double>(outcome.totalVoteAmount) * 100.0 /
                        static_cast<double>(total));
}
QString ratio(const KickPredictionOutcome &outcome, int total)
{
    if (total <= 0 || outcome.totalVoteAmount <= 0)
    {
        return QStringLiteral("-:-");
    }
    return QStringLiteral("1:%1").arg(
        QString::number(static_cast<double>(total) /
                            static_cast<double>(outcome.totalVoteAmount),
                        'f', 1));
}
QWidget *metricPair(const QString &iconPath, const QString &value, QColor color,
                    bool rightAligned, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(5);
    auto *icon = new QLabel(widget);
    icon->setFixedSize(15, 15);
    icon->setPixmap(renderSvg(iconPath, color, 15));
    auto *label = new QLabel(value, widget);
    label->setStyleSheet(
        QStringLiteral("color:%1; font-size:12px;").arg(color.name()));
    if (rightAligned)
    {
        layout->addStretch(1);
        layout->addWidget(label);
        layout->addWidget(icon);
    }
    else
    {
        layout->addWidget(icon);
        layout->addWidget(label);
        layout->addStretch(1);
    }
    return widget;
}

}  // namespace

class KickPredictionTimerBar final : public QWidget
{
public:
    explicit KickPredictionTimerBar(QWidget *parent = nullptr) : QWidget(parent)
    {
        this->setFixedHeight(TIMER_BAR_HEIGHT);
    }
    void setColors(QColor track, QColor fill)
    {
        this->track_ = std::move(track);
        this->fill_ = std::move(fill);
        this->update();
    }
    void setProgress(double progress)
    {
        this->progress_ = std::clamp(progress, 0.0, 1.0);
        this->update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->track_);
        painter.drawRoundedRect(QRectF(this->rect()), this->height() / 2.0,
                                this->height() / 2.0);
        painter.setBrush(this->fill_);
        painter.drawRoundedRect(
            QRectF(0, 0, this->width() * this->progress_, this->height()),
            this->height() / 2.0, this->height() / 2.0);
    }
private:
    QColor track_{"#303036"};
    QColor fill_{KICK_GREEN};
    double progress_ = 1.0;
};

class KickPredictionOutcomeWidget final : public QWidget
{
public:
    explicit KickPredictionOutcomeWidget(QString outcomeID,
                                         QWidget *parent = nullptr)
        : QWidget(parent), outcomeID_(std::move(outcomeID))
    {
        this->setCursor(Qt::PointingHandCursor);
    }
    const QString &outcomeID() const { return this->outcomeID_; }
    void setSelected(bool selected)
    {
        this->selected_ = selected;
        this->update();
    }
    void setChrome(QColor background, QColor border, QColor selected)
    {
        this->background_ = std::move(background);
        this->border_ = std::move(border);
        this->selectedBorder_ = std::move(selected);
        this->update();
    }
    std::function<void()> clicked;
protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && this->clicked)
        {
            this->clicked();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(this->selected_ ? this->selectedBorder_
                                           : this->border_,
                            this->selected_ ? 2 : 1));
        painter.setBrush(this->background_);
        painter.drawRoundedRect(this->rect().adjusted(1, 1, -1, -1), 5, 5);
    }
private:
    QString outcomeID_;
    QColor background_{"#2c2c31"};
    QColor border_{"#3b3b43"};
    QColor selectedBorder_{KICK_GREEN};
    bool selected_ = false;
};

void KickPredictionDialog::showDialog(
    const std::shared_ptr<KickChannel> &channel,
    const std::shared_ptr<Channel> &messageChannel)
{
    if (channel == nullptr || !channel->activePrediction())
    {
        return;
    }
    auto *dialog = new KickPredictionDialog(
        channel, messageChannel,
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

KickPredictionDialog::KickPredictionDialog(std::shared_ptr<KickChannel> channel,
                                           std::shared_ptr<Channel> messageChannel,
                                           QWidget *parent)
    : BasePopup({BaseWindow::EnableCustomFrame,
                 BaseWindow::CloseButtonOnly,
                 BaseWindow::DisableLayoutSave,
                 BaseWindow::BoundsCheckOnShow}, parent)
    , channel_(channel)
    , messageChannel_(messageChannel != nullptr ? std::move(messageChannel)
                                                : channel)
    , channelSlug_(channel != nullptr ? channel->slug() : QString{})
{
    this->setWindowTitle(QStringLiteral("Manage Prediction"));
    this->setScaleIndependentSize(DIALOG_WIDTH, ACTIVE_HEIGHT);
    this->setAutoFillBackground(true);
    this->getLayoutContainer()->setObjectName(QStringLiteral("KickPredictionRoot"));
    this->getLayoutContainer()->setAutoFillBackground(true);

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(20, 14, 20, 16);
    root->setSpacing(6);
    root->setAlignment(Qt::AlignTop);
    this->statusLabel_ = new QLabel(this);
    this->statusLabel_->setObjectName(QStringLiteral("KickPredictionStatus"));
    root->addWidget(this->statusLabel_);
    this->descriptionLabel_ = new QLabel(
        QStringLiteral("Select the result and reward the viewers who voted "
                       "for it with Channel Points."), this);
    this->descriptionLabel_->setObjectName(QStringLiteral("KickPredictionDescription"));
    this->descriptionLabel_->setWordWrap(true);
    this->descriptionLabel_->hide();
    root->addWidget(this->descriptionLabel_);
    this->titleLabel_ = new QLabel(this);
    this->titleLabel_->setObjectName(QStringLiteral("KickPredictionTitle"));
    this->titleLabel_->setWordWrap(true);
    root->addWidget(this->titleLabel_);
    this->timerBar_ = new KickPredictionTimerBar(this);
    this->timerBar_->setFixedWidth(610);
    root->addWidget(this->timerBar_, 0, Qt::AlignLeft);
    this->outcomesLayout_ = new QVBoxLayout;
    this->outcomesLayout_->setContentsMargins(0, 5, 0, 0);
    this->outcomesLayout_->setSpacing(5);
    root->addLayout(this->outcomesLayout_);
    this->errorLabel_ = new QLabel(this);
    this->errorLabel_->setObjectName(QStringLiteral("KickPredictionError"));
    this->errorLabel_->setWordWrap(true);
    this->errorLabel_->hide();
    root->addWidget(this->errorLabel_);

    auto *buttons = new QHBoxLayout;
    buttons->setContentsMargins(0, 8, 0, 0);
    buttons->setSpacing(8);
    this->deleteButton_ = new QPushButton(QStringLiteral("Delete"), this);
    this->backButton_ = new QPushButton(QStringLiteral("Back"), this);
    this->actionButton_ = new QPushButton(this);
    for (auto *button : {this->deleteButton_, this->backButton_,
                         this->actionButton_})
    {
        button->setObjectName(QStringLiteral("KickPredictionButton"));
        button->setCursor(Qt::PointingHandCursor);
    }
    this->backButton_->hide();
    buttons->addStretch(1);
    buttons->addWidget(this->deleteButton_);
    buttons->addWidget(this->backButton_);
    buttons->addWidget(this->actionButton_);
    root->addLayout(buttons);

    QObject::connect(this->deleteButton_, &QPushButton::clicked, this,
                     [this] { this->refundPrediction(); });
    QObject::connect(this->backButton_, &QPushButton::clicked, this,
                     [this] { this->setChoosingOutcome(false); });
    QObject::connect(this->actionButton_, &QPushButton::clicked, this, [this] {
        if (this->submissionsOpen())
            this->endSubmissions();
        else if (!this->choosingOutcome_)
            this->setChoosingOutcome(true);
        else
            this->resolveSelectedOutcome();
    });
    if (channel != nullptr)
    {
        this->signalHolder_.managedConnect(channel->predictionChanged,
                                           [this] { this->refresh(); });
    }
    this->timer_ = new QTimer(this);
    this->timer_->setInterval(250);
    QObject::connect(this->timer_, &QTimer::timeout, this,
                     [this] { this->updateTimerUi(); });
    this->timer_->start();
    this->applyTheme();
    this->refresh();
}

void KickPredictionDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->applyTheme();
    this->rebuildOutcomes();
    this->updateTimerUi();
}

void KickPredictionDialog::refresh()
{
    const auto channel = this->channel_.lock();
    if (channel == nullptr || !channel->activePrediction())
    {
        this->close();
        return;
    }
    const auto &prediction = *channel->activePrediction();
    this->titleLabel_->setText(prediction.title);
    if (this->choosingOutcome_ && this->submissionsOpen())
    {
        this->choosingOutcome_ = false;
        this->selectedOutcomeID_.clear();
    }
    const bool selectedExists = std::ranges::any_of(
        prediction.outcomes, [this](const KickPredictionOutcome &outcome) {
            return outcome.id == this->selectedOutcomeID_;
        });
    if (this->choosingOutcome_ && !selectedExists && !prediction.outcomes.empty())
        this->selectedOutcomeID_ = prediction.outcomes.front().id;

    if (this->managementPending_)
    {
        this->errorLabel_->setText(QStringLiteral("Updating the Kick prediction&"));
        this->errorLabel_->setProperty("pending", true);
        this->errorLabel_->show();
    }
    else if (!this->errorLabel_->text().isEmpty())
    {
        this->errorLabel_->setProperty("pending", false);
        this->errorLabel_->show();
    }
    else
        this->errorLabel_->hide();
    repolish(this->errorLabel_);
    this->rebuildOutcomes();
    this->updateTimerUi();
    this->updateDialogSize();
}

void KickPredictionDialog::applyTheme()
{
    const auto *theme = getTheme();
    if (theme == nullptr)
        return;
    const auto background = theme->isLightTheme() ? QColor("#f7f7f8")
                                                   : QColor("#18181b");
    const auto text = theme->isLightTheme() ? QColor("#1f1f23")
                                             : QColor("#efeff1");
    const auto muted = theme->isLightTheme() ? QColor("#60606b")
                                              : QColor("#adadb8");
    const auto surface = theme->isLightTheme() ? QColor("#f0f0f3")
                                                : QColor("#2c2c31");
    const auto border = theme->isLightTheme() ? QColor("#d6d6dc")
                                               : QColor("#3b3b43");
    const auto hover = theme->isLightTheme() ? QColor("#e3e3e8")
                                              : QColor("#35353b");
    this->overrideBackgroundColor_ = background;
    auto palette = this->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::WindowText, text);
    this->setPalette(palette);
    this->getLayoutContainer()->setPalette(palette);
    this->timerBar_->setColors(border, KICK_GREEN);
    this->getLayoutContainer()->setStyleSheet(
        QStringLiteral(R"(
QWidget#KickPredictionRoot { background:%1; color:%2; }
QLabel { color:%2; }
QLabel#KickPredictionStatus { font-size:18px; font-weight:700; padding-bottom:2px; }
QLabel#KickPredictionStatus[chooseMode="true"] { color:%7; }
QLabel#KickPredictionDescription { color:%3; font-size:13px; }
QLabel#KickPredictionTitle { font-size:20px; font-weight:700; }
QLabel#KickPredictionError { color:#ff6b6b; font-size:12px; }
QLabel#KickPredictionError[pending="true"] { color:%3; }
QPushButton#KickPredictionButton {
 background:%4; color:%2; border:1px solid %5; border-radius:4px;
 padding:7px 14px; font-weight:600;
}
QPushButton#KickPredictionButton:hover { background:%6; }
QPushButton#KickPredictionButton:disabled { color:%3; }
QPushButton#KickPredictionButton[primary="true"] {
 background:%7; border-color:%7; color:#111111;
}
)")
            .arg(background.name(), text.name(), muted.name(), surface.name(),
                 border.name(), hover.name(), KICK_GREEN.name()));
}

void KickPredictionDialog::rebuildOutcomes()
{
    if (this->outcomesLayout_ == nullptr)
        return;
    clearLayout(this->outcomesLayout_);
    this->outcomeChoices_.clear();
    const auto channel = this->channel_.lock();
    if (channel == nullptr || !channel->activePrediction())
        return;
    const auto &prediction = *channel->activePrediction();
    if (prediction.outcomes.empty())
        return;

    const auto *theme = getTheme();
    const auto text = theme != nullptr && theme->isLightTheme()
                          ? QColor("#1f1f23") : QColor("#efeff1");
    const auto surface = theme != nullptr && theme->isLightTheme()
                             ? QColor("#f0f0f3") : QColor("#2c2c31");
    const auto border = theme != nullptr && theme->isLightTheme()
                            ? QColor("#d6d6dc") : QColor("#3b3b43");
    const auto total = totalPoints(prediction);

    if (this->choosingOutcome_)
    {
        this->outcomesLayout_->setSpacing(10);
        for (size_t i = 0; i < prediction.outcomes.size(); ++i)
        {
            const auto &outcome = prediction.outcomes[i];
            auto *row = new KickPredictionOutcomeWidget(outcome.id, this);
            row->setFixedHeight(54);
            row->setChrome(surface, border, outcomeColor(i));
            row->setSelected(outcome.id == this->selectedOutcomeID_);
            row->clicked = [this, id = outcome.id] { this->selectOutcome(id); };
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(12, 0, 12, 0);
            auto *badge = new QLabel(row);
            badge->setFixedSize(20, 21);
            badge->setPixmap(renderSvg(outcomeBadgePath(i), outcomeColor(i), 20));
            auto *title = new QLabel(outcome.title, row);
            title->setStyleSheet(
                QStringLiteral("color:%1;font-size:16px;font-weight:700;")
                    .arg(text.name()));
            layout->addWidget(badge);
            layout->addWidget(title, 1);
            this->outcomeChoices_.push_back(row);
            this->outcomesLayout_->addWidget(row);
        }
        return;
    }

    auto *overview = new QWidget(this);
    overview->setFixedHeight(154);
    auto *layout = new QHBoxLayout(overview);
    layout->setContentsMargins(0, 6, 0, 0);
    layout->setSpacing(12);
    for (size_t i = 0; i < std::min<size_t>(2, prediction.outcomes.size()); ++i)
    {
        const auto &outcome = prediction.outcomes[i];
        auto *card = new KickPredictionOutcomeWidget(outcome.id, overview);
        auto background = outcomeColor(i);
        background.setAlpha(28);
        card->setChrome(background, outcomeColor(i), outcomeColor(i));
        auto *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(12, 9, 12, 8);
        cardLayout->setSpacing(3);
        auto *heading = new QHBoxLayout;
        auto *badge = new QLabel(card);
        badge->setFixedSize(20, 21);
        badge->setPixmap(renderSvg(outcomeBadgePath(i), outcomeColor(i), 20));
        auto *title = new QLabel(outcome.title, card);
        title->setStyleSheet(
            QStringLiteral("color:%1;font-size:16px;font-weight:700;")
                .arg(outcomeColor(i).name()));
        heading->addWidget(badge);
        heading->addWidget(title, 1);
        auto *percentLabel =
            new QLabel(QStringLiteral("%1%").arg(percentage(outcome, total)), card);
        percentLabel->setAlignment(Qt::AlignCenter);
        percentLabel->setStyleSheet(
            QStringLiteral("color:%1;font-size:24px;font-weight:700;")
                .arg(outcomeColor(i).name()));
        auto *metrics = new QHBoxLayout;
        metrics->addWidget(metricPair(
            QStringLiteral(":/predictions/kick-predictors.svg"),
            QLocale().toString(outcome.voteCount), outcomeColor(i), false, card));
        metrics->addWidget(metricPair(
            QStringLiteral(":/predictions/kick-ratio.svg"),
            ratio(outcome, total), outcomeColor(i), true, card));
        cardLayout->addLayout(heading);
        cardLayout->addWidget(percentLabel);
        cardLayout->addLayout(metrics);
        this->outcomeChoices_.push_back(card);
        layout->addWidget(card, 1);
    }
    this->outcomesLayout_->addWidget(overview);
}

void KickPredictionDialog::updateTimerUi()
{
    const bool open = this->submissionsOpen();
    const bool choosing = this->choosingOutcome_ && !open;
    this->setWindowTitle(choosing ? QStringLiteral("Choose Outcome")
                                  : QStringLiteral("Manage Prediction"));
    this->statusLabel_->setText(choosing ? QStringLiteral("Choose Outcome")
        : open ? QStringLiteral("Submissions Open")
               : QStringLiteral("Submissions Closed"));
    this->statusLabel_->setProperty("chooseMode", choosing);
    repolish(this->statusLabel_);
    this->descriptionLabel_->setVisible(choosing);
    this->timerBar_->setVisible(open && !choosing);
    this->timerBar_->setProgress(this->timerProgress());
    this->actionButton_->setText(open ? QStringLiteral("End Submissions")
        : choosing ? QStringLiteral("Complete Prediction")
                   : QStringLiteral("Choose Outcome"));
    this->updateActionButton();
}

void KickPredictionDialog::updateActionButton()
{
    const auto channel = this->channel_.lock();
    const bool hasID = channel != nullptr && channel->activePrediction() &&
        !channel->activePrediction()->id.trimmed().isEmpty();
    const bool choosing = this->choosingOutcome_ && !this->submissionsOpen();
    const bool enabled = hasID && !this->managementPending_;
    this->deleteButton_->setVisible(!choosing);
    this->backButton_->setVisible(choosing);
    this->deleteButton_->setEnabled(enabled);
    this->backButton_->setEnabled(enabled);
    this->actionButton_->setEnabled(
        enabled && (!choosing || !this->selectedOutcomeID_.isEmpty()));
    this->actionButton_->setProperty("primary", true);
    repolish(this->actionButton_);
}

void KickPredictionDialog::updateDialogSize()
{
    this->setScaleIndependentSize(
        DIALOG_WIDTH, this->choosingOutcome_ ? CHOOSING_HEIGHT
          : this->submissionsOpen() ? ACTIVE_HEIGHT : CLOSED_HEIGHT);
    this->timerBar_->setFixedWidth(610);
}

void KickPredictionDialog::setChoosingOutcome(bool choosing)
{
    const auto channel = this->channel_.lock();
    if (channel == nullptr || !channel->activePrediction() ||
        (choosing && this->submissionsOpen()) ||
        this->choosingOutcome_ == choosing)
        return;
    this->choosingOutcome_ = choosing;
    this->errorLabel_->clear();
    if (choosing)
    {
        const auto &outcomes = channel->activePrediction()->outcomes;
        if (outcomes.empty())
        {
            this->choosingOutcome_ = false;
            return;
        }
        const bool selectedExists = std::ranges::any_of(
            outcomes, [this](const KickPredictionOutcome &outcome) {
                return outcome.id == this->selectedOutcomeID_;
            });
        if (!selectedExists)
            this->selectedOutcomeID_ = outcomes.front().id;
    }
    else
        this->selectedOutcomeID_.clear();
    this->rebuildOutcomes();
    this->updateTimerUi();
    this->updateDialogSize();
}

void KickPredictionDialog::selectOutcome(const QString &outcomeID)
{
    if (!this->choosingOutcome_)
        return;
    this->selectedOutcomeID_ = outcomeID;
    for (auto *choice : this->outcomeChoices_)
        choice->setSelected(choice->outcomeID() == outcomeID);
    this->updateActionButton();
}
void KickPredictionDialog::endSubmissions()
{
    if (!this->managementPending_)
        this->updatePrediction(QStringLiteral("LOCKED"));
}
void KickPredictionDialog::resolveSelectedOutcome()
{
    if (!this->managementPending_ && this->choosingOutcome_ &&
        !this->selectedOutcomeID_.isEmpty())
        this->updatePrediction(QStringLiteral("RESOLVED"),
                               this->selectedOutcomeID_);
}
void KickPredictionDialog::refundPrediction()
{
    if (!this->managementPending_)
        this->updatePrediction(QStringLiteral("CANCELLED"));
}
bool KickPredictionDialog::submissionsOpen() const
{
    const auto channel = this->channel_.lock();
    return channel != nullptr && channel->activePrediction() &&
        channel->activePrediction()->state == QStringLiteral("ACTIVE");
}
double KickPredictionDialog::timerProgress() const
{
    const auto channel = this->channel_.lock();
    if (channel == nullptr || !channel->activePrediction() ||
        channel->activePrediction()->state != QStringLiteral("ACTIVE"))
        return 0.0;
    const auto &prediction = *channel->activePrediction();
    if (!prediction.createdAt.isValid() || prediction.durationSeconds <= 0)
        return 1.0;
    const auto elapsed = prediction.createdAt.toUTC().msecsTo(
        QDateTime::currentDateTimeUtc());
    const auto total = static_cast<double>(prediction.durationSeconds) * 1000.0;
    return std::clamp(1.0 - static_cast<double>(elapsed) / total, 0.0, 1.0);
}

void KickPredictionDialog::updatePrediction(
    const QString &state, const QString &winningOutcomeID)
{
    const auto channel = this->channel_.lock();
    if (channel == nullptr || !channel->activePrediction())
        return;
    const auto originalPrediction = *channel->activePrediction();
    const auto predictionID = originalPrediction.id.trimmed();
    if (predictionID.isEmpty())
    {
        this->errorLabel_->setText(
            QStringLiteral("Kick has not supplied the prediction ID yet."));
        this->refresh();
        return;
    }
    auto account = getApp()->getAccounts()->kick.current();
    if (account == nullptr || account->isAnonymous() ||
        account->chatIdentityToken().trimmed().isEmpty())
    {
        this->errorLabel_->setText(QStringLiteral(
            "Connect Kick's website session under Settings > Accounts "
            "before managing predictions."));
        this->refresh();
        return;
    }
    auto slug = this->channelSlug_.trimmed();
    if (slug.isEmpty())
        slug = channel->getName().trimmed();

    this->managementPending_ = true;
    this->errorLabel_->clear();
    this->refresh();
    QPointer<KickPredictionDialog> self(this);
    const std::weak_ptr<KickChannel> weak = channel;
    const auto messageChannelWeak = this->messageChannel_;
    getKickApi()->updatePrediction(
        slug, account->chatIdentityToken().trimmed(), predictionID, state,
        winningOutcomeID,
        [self, weak, messageChannelWeak, originalPrediction, predictionID, state,
         winningOutcomeID](ExpectedStr<void> result) mutable {
            auto handleResult =
                [self, weak, messageChannelWeak, originalPrediction,
                 predictionID, state, winningOutcomeID,
                 result = std::move(result)] {
                    if (self != nullptr)
                    {
                        self->managementPending_ = false;
                    }
                    if (!result)
                    {
                        if (self != nullptr)
                        {
                            self->errorLabel_->setText(
                                QStringLiteral(
                                    "Kick couldn't update the prediction: %1")
                                    .arg(result.error()));
                            self->refresh();
                        }
                        return;
                    }
                    if (self != nullptr)
                    {
                        self->errorLabel_->clear();
                    }
                    auto channel = weak.lock();
                    auto messageChannel = messageChannelWeak.lock();
                    const bool predictionStillActive =
                        channel != nullptr && channel->activePrediction() &&
                        channel->activePrediction()->id == predictionID;
                    auto updated = predictionStillActive
                                       ? *channel->activePrediction()
                                       : originalPrediction;
                    updated.state = state;
                    updated.updatedAt = QDateTime::currentDateTimeUtc();
                    if (state == QStringLiteral("LOCKED"))
                    {
                        updated.lockedAt = updated.updatedAt;
                        if (messageChannel != nullptr)
                        {
                            messageChannel->addSystemMessage(QStringLiteral(
                                "Ended submissions for Kick prediction: '%1'")
                                                                 .arg(updated.title));
                        }
                    }
                    else if (state == QStringLiteral("RESOLVED"))
                    {
                        updated.winningOutcomeID = winningOutcomeID;
                        QString winner;
                        for (const auto &outcome : updated.outcomes)
                            if (outcome.id == winningOutcomeID)
                                winner = outcome.title;
                        if (messageChannel != nullptr)
                        {
                            messageChannel->addSystemMessage(QStringLiteral(
                                "Completed Kick prediction: %1 - '%2' won")
                                                                 .arg(updated.title, winner));
                        }
                    }
                    else
                    {
                        if (messageChannel != nullptr)
                        {
                            messageChannel->addSystemMessage(
                                QStringLiteral("Deleted Kick prediction: '%1'")
                                    .arg(updated.title));
                        }
                    }
                    if (predictionStillActive)
                    {
                        channel->updatePrediction(std::move(updated));
                    }
                    if (self != nullptr)
                    {
                        self->refresh();
                    }
                };

            if (auto *application = QCoreApplication::instance())
            {
                QTimer::singleShot(0, application, std::move(handleResult));
            }
        });
}

}  // namespace chatterino
