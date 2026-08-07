// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/ManagePredictionDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Helpers.hpp"
#include "widgets/Window.hpp"
#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QSvgRenderer>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <utility>

namespace chatterino {
namespace {

constexpr int DIALOG_WIDTH = 650;
constexpr int TWO_OUTCOME_ACTIVE_HEIGHT = 350;
constexpr int TWO_OUTCOME_CLOSED_HEIGHT = 345;
constexpr int MULTI_OUTCOME_BASE_HEIGHT = 178;
constexpr int MULTI_OUTCOME_ROW_HEIGHT = 38;
constexpr int OUTCOME_SELECTION_BASE_HEIGHT = 205;
constexpr int OUTCOME_SELECTION_MIN_HEIGHT = 340;
constexpr int OUTCOME_SELECTION_MAX_HEIGHT = 760;
constexpr int OUTCOME_SELECTION_ROW_HEIGHT = 56;
constexpr int OUTCOME_SELECTION_ROW_WIDGET_HEIGHT = 47;
constexpr int VOTING_CONTROLS_HEIGHT = 52;
constexpr int BADGE_SIZE = 18;
constexpr int METRIC_ICON_SIZE = 15;
constexpr int TIMER_BAR_HEIGHT = 7;
constexpr int TIMER_BAR_WIDTH = DIALOG_WIDTH - 40;

const QColor BLUE_OUTCOME("#2f7cff");
const QColor PINK_OUTCOME("#f5009b");

QString rgba(QColor color)
{
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(alpha);
    return color;
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

QColor outcomeColor(size_t index, int outcomeCount)
{
    if (outcomeCount <= 2 && index == 1)
    {
        return PINK_OUTCOME;
    }

    return BLUE_OUTCOME;
}

QString badgeFileName(size_t index, int outcomeCount)
{
    if (outcomeCount <= 2 && index == 1)
    {
        return QStringLiteral("pink-2.png");
    }

    return QStringLiteral("blue-%1.png").arg(index + 1);
}

QString badgeResource(size_t index, int outcomeCount)
{
    return QStringLiteral(":/predictions/%1").arg(
        badgeFileName(index, outcomeCount));
}

QPixmap loadBadgePixmap(size_t index, int outcomeCount)
{
    QPixmap badge(badgeResource(index, outcomeCount));
    if (!badge.isNull())
    {
        return badge;
    }

    const auto fileName = badgeFileName(index, outcomeCount);
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = QDir::cleanPath(appDir.filePath(
        QStringLiteral("../../MergerinoSource/resources/predictions/%1").arg(
            fileName)));

    badge.load(sourcePath);
    return badge;
}

void clearLayout(QLayout *layout)
{
    if (layout == nullptr)
    {
        return;
    }

    while (auto *item = layout->takeAt(0))
    {
        if (auto *childLayout = item->layout())
        {
            clearLayout(childLayout);
        }

        if (auto *widget = item->widget())
        {
            widget->deleteLater();
        }

        delete item;
    }
}

std::shared_ptr<TwitchChannel> resolveTwitchChannel(const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    if (auto twitch = std::dynamic_pointer_cast<TwitchChannel>(channel))
    {
        return twitch;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<TwitchChannel>(
            merged->twitchChannel());
    }

    return nullptr;
}

void notifyPollsAndPredictionsChanged(const ChannelPtr &channel)
{
    if (auto twitch = resolveTwitchChannel(channel))
    {
        twitch->streamStatusChanged.invoke();
    }
}

QString metricSvgPaths(int metric)
{
    switch (metric)
    {
        case 0:
            return QStringLiteral(
                R"(<path d="M12 5v2a5 5 0 0 1 5 5h2a7 7 0 0 0-7-7Z"></path><path fill-rule="evenodd" d="M1 12C1 5.925 5.925 1 12 1s11 4.925 11 11-4.925 11-11 11S1 18.075 1 12Zm11 9a9 9 0 1 1 0-18 9 9 0 0 1 0 18Z" clip-rule="evenodd"></path>)");
        case 1:
            return QStringLiteral(
                R"(<path fill-rule="evenodd" d="M18 4V2H6v2H2v5a3 3 0 0 0 3 3h1.083A6.005 6.005 0 0 0 11 16.917V20H8v2h8v-2h-3v-3.083A6.005 6.005 0 0 0 17.917 12H19a3 3 0 0 0 3-3V4h-4Zm-2 0H8v7a4 4 0 0 0 8 0V4Zm2 2v4h1a1 1 0 0 0 1-1V6h-2ZM6 10V6H4v3a1 1 0 0 0 1 1h1Z" clip-rule="evenodd"></path>)");
        case 2:
            return QStringLiteral(
                R"(<path fill-rule="evenodd" d="M8 2a5 5 0 0 0-1 9.9v.1a1 1 0 0 1-1 1H5a3 3 0 0 0-3 3v6h2v-6a1 1 0 0 1 1-1h1a2.99 2.99 0 0 0 2-.764A2.99 2.99 0 0 0 10 15h1a1 1 0 0 1 1 1v6h2v-6a3 3 0 0 0-3-3h-1a1 1 0 0 1-1-1v-.1A5.002 5.002 0 0 0 8 2ZM5 7a3 3 0 1 0 6 0 3 3 0 0 0-6 0Z" clip-rule="evenodd"></path><path d="M22 11a4.002 4.002 0 0 1-2.956 3.862A1.5 1.5 0 0 0 20.5 16a1.5 1.5 0 0 1 1.5 1.5V22h-5v-7.126A4.002 4.002 0 0 1 18 7a4 4 0 0 1 4 4Z"></path>)");
        default:
            return QStringLiteral(
                R"(<path d="M11 2h2v4h-2V2Zm8.634 1.778 1.732 1-2 3.464-1.732-1 2-3.464Zm-17 1 1.732-1 2 3.464-1.732 1-2-3.464Z"></path><path fill-rule="evenodd" d="M17 10v2h5v10H2v-8h5v-4h10Zm-2 4h5v6H4v-4h5v-4h6v2Z" clip-rule="evenodd"></path>)");
    }
}

QPixmap renderSvgIcon(const QString &paths, QColor color, int size)
{
    const auto svg = QStringLiteral(
                         R"(<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24" fill="%1">%2</svg>)")
                         .arg(color.name(QColor::HexRgb), paths);

    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);

    QSvgRenderer renderer(svg.toUtf8());
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter);

    return pixmap;
}

QString summarySvgPaths()
{
    return QStringLiteral(
        R"(<path d="M14 2v2h4.586l-8.293 8.293 1.414 1.414L20 5.414V10h2V2h-8Z"></path><path d="M4 4h8v2H4v14h14v-8h2v8a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2Z"></path>)");
}

int totalChannelPoints(const HelixPrediction &prediction)
{
    return std::accumulate(
        prediction.outcomes.begin(), prediction.outcomes.end(), 0,
        [](int total, const HelixPredictionOutcome &outcome) {
            return total + outcome.channelPoints;
        });
}

int totalUsers(const HelixPrediction &prediction)
{
    return std::accumulate(
        prediction.outcomes.begin(), prediction.outcomes.end(), 0,
        [](int total, const HelixPredictionOutcome &outcome) {
            return total + outcome.users;
        });
}

int percentageForOutcome(const HelixPredictionOutcome &outcome, int totalPoints)
{
    if (totalPoints <= 0)
    {
        return 0;
    }

    return qRound(static_cast<double>(outcome.channelPoints) * 100.0 /
                  static_cast<double>(totalPoints));
}

QString returnRatioForOutcome(const HelixPredictionOutcome &outcome,
                              int totalPoints)
{
    if (totalPoints <= 0 || outcome.channelPoints <= 0)
    {
        return QStringLiteral("-:-");
    }

    return QStringLiteral("1:%1").arg(
        QString::number(static_cast<double>(totalPoints) /
                            static_cast<double>(outcome.channelPoints),
                        'f', 1));
}

QLabel *metricIconLabel(int metric, QColor color, QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setFixedSize(METRIC_ICON_SIZE, METRIC_ICON_SIZE);
    label->setPixmap(renderSvgIcon(metricSvgPaths(metric), color,
                                   METRIC_ICON_SIZE));
    return label;
}

QLabel *textLabel(const QString &text, const QString &objectName,
                  QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(objectName);
    return label;
}

QWidget *metricPair(int metric, const QString &value, QColor color,
                    bool reversed, QWidget *parent)
{
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(3);

    auto *icon = metricIconLabel(metric, color, widget);
    auto *label = textLabel(value, QStringLiteral("PredictionMetricValue"),
                            widget);

    if (reversed)
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

QString firstWinningOutcomeTitle(const HelixPrediction &prediction)
{
    for (const auto &outcome : prediction.outcomes)
    {
        if (outcome.id == prediction.winningOutcomeID)
        {
            return outcome.title;
        }
    }

    return prediction.outcomes.empty() ? QString{} : prediction.outcomes.front().title;
}

}  // namespace

class PredictionTimerBar final : public QWidget
{
public:
    explicit PredictionTimerBar(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        this->setFixedHeight(TIMER_BAR_HEIGHT);
    }

    void setColors(QColor track, QColor fill)
    {
        this->trackColor_ = track;
        this->fillColor_ = fill;
        this->update();
    }

    void setProgress(double progress)
    {
        const auto clamped = std::clamp(progress, 0.0, 1.0);
        if (qFuzzyCompare(this->progress_, clamped))
        {
            return;
        }

        this->progress_ = clamped;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const QRectF fullRect(0.0, 0.0, this->width(), this->height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->trackColor_);
        painter.drawRoundedRect(fullRect, this->height() / 2.0,
                                this->height() / 2.0);

        const QRectF fillRect(0.0, 0.0, this->width() * this->progress_,
                              this->height());
        painter.setBrush(this->fillColor_);
        painter.drawRoundedRect(fillRect, this->height() / 2.0,
                                this->height() / 2.0);
    }

private:
    QColor trackColor_{QColor("#2a2a2f")};
    QColor fillColor_{QColor("#8b8b95")};
    double progress_ = 1.0;
};

class OutcomeChoiceWidget final : public QWidget
{
public:
    explicit OutcomeChoiceWidget(QString outcomeID, QWidget *parent = nullptr)
        : QWidget(parent)
        , outcomeID_(std::move(outcomeID))
    {
        this->setCursor(Qt::PointingHandCursor);
        this->setMouseTracking(true);
    }

    const QString &outcomeID() const
    {
        return this->outcomeID_;
    }

    void setSelected(bool selected)
    {
        if (this->selected_ == selected)
        {
            return;
        }

        this->selected_ = selected;
        this->update();
    }

    void setChromeColors(QColor selectedBackground, QColor selectedBorder)
    {
        this->selectedBackground_ = selectedBackground;
        this->selectedBorder_ = selectedBorder;
        this->update();
    }

    void setBaseChrome(QColor background, QColor border)
    {
        this->baseBackground_ = background;
        this->baseBorder_ = border;
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

    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);

        if (!this->selected_ && this->baseBackground_.alpha() == 0 &&
            this->baseBorder_.alpha() == 0)
        {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect = this->rect().adjusted(0.5, 0.5, -0.5, -0.5);

        if (this->baseBackground_.alpha() > 0 ||
            this->baseBorder_.alpha() > 0)
        {
            painter.setPen(QPen(this->baseBorder_, 1));
            painter.setBrush(this->baseBackground_);
            painter.drawRoundedRect(rect, 5, 5);
        }

        if (!this->selected_)
        {
            return;
        }

        painter.setPen(QPen(this->selectedBorder_, 1));
        painter.setBrush(this->selectedBackground_);
        painter.drawRoundedRect(rect, 5, 5);
    }

private:
    QString outcomeID_;
    QColor baseBackground_{Qt::transparent};
    QColor baseBorder_{Qt::transparent};
    QColor selectedBackground_{QColor(63, 134, 255, 35)};
    QColor selectedBorder_{QColor("#3f86ff")};
    bool selected_ = false;
};

void ManagePredictionDialog::showDialog(ChannelPtr channel,
                                        QString broadcasterID,
                                        QString channelLogin,
                                        const HelixPrediction &prediction,
                                        bool useModerationAuth)
{
    auto *dialog = new ManagePredictionDialog(
        std::move(channel), std::move(broadcasterID), std::move(channelLogin),
        prediction, useModerationAuth, true,
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

void ManagePredictionDialog::showViewerDialog(
    ChannelPtr channel, QString broadcasterID, QString channelLogin,
    const HelixPrediction &prediction)
{
    auto *dialog = new ManagePredictionDialog(
        std::move(channel), std::move(broadcasterID), std::move(channelLogin),
        prediction, false, false,
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

ManagePredictionDialog::ManagePredictionDialog(
    ChannelPtr channel, QString broadcasterID, QString channelLogin,
    const HelixPrediction &prediction, bool useModerationAuth, bool canManage,
    QWidget *parent)
    : BasePopup(
          {
              BaseWindow::EnableCustomFrame,
              BaseWindow::CloseButtonOnly,
              BaseWindow::FixedSizeCustomFrame,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , channel_(std::move(channel))
    , broadcasterID_(std::move(broadcasterID))
    , channelLogin_(std::move(channelLogin))
    , prediction_(prediction)
    , useModerationAuth_(useModerationAuth)
    , canManage_(canManage)
{
    this->setWindowTitle(QStringLiteral("Manage Prediction"));
    this->setScaleIndependentSize(DIALOG_WIDTH, TWO_OUTCOME_ACTIVE_HEIGHT);
    this->setAutoFillBackground(true);
    this->getLayoutContainer()->setObjectName(
        QStringLiteral("ManagePredictionRoot"));
    this->getLayoutContainer()->setAutoFillBackground(true);

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(20, 14, 20, 16);
    root->setSpacing(6);
    root->setAlignment(Qt::AlignTop);

    this->statusLabel_ =
        textLabel(QString{}, QStringLiteral("PredictionManageStatus"), this);
    root->addWidget(this->statusLabel_);

    this->descriptionLabel_ = textLabel(
        QStringLiteral("Select the result and reward the viewers who voted for "
                       "it with Channel Points."),
        QStringLiteral("PredictionChooseDescription"), this);
    this->descriptionLabel_->setWordWrap(true);
    this->descriptionLabel_->hide();
    root->addWidget(this->descriptionLabel_);

    this->titleLabel_ =
        textLabel(this->prediction_.title,
                  QStringLiteral("PredictionManageTitle"), this);
    this->titleLabel_->setWordWrap(true);
    root->addWidget(this->titleLabel_);

    this->timerBar_ = new PredictionTimerBar(this);
    root->addWidget(this->timerBar_, 0, Qt::AlignLeft);

    this->outcomesLayout_ = new QVBoxLayout;
    this->outcomesLayout_->setContentsMargins(0, 5, 0, 0);
    this->outcomesLayout_->setSpacing(5);
    root->addLayout(this->outcomesLayout_);

    this->voteControls_ = new QWidget(this);
    auto *voteControlsLayout = new QHBoxLayout(this->voteControls_);
    voteControlsLayout->setContentsMargins(0, 3, 0, 0);
    voteControlsLayout->setSpacing(8);
    auto *amountLabel =
        new QLabel(QStringLiteral("Channel Points"), this->voteControls_);
    this->voteAmount_ = new QSpinBox(this->voteControls_);
    this->voteAmount_->setRange(10, 250000);
    this->voteAmount_->setValue(10);
    this->voteAmount_->setGroupSeparatorShown(true);
    this->voteAmount_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    this->voteAmount_->setFixedWidth(90);
    voteControlsLayout->addWidget(amountLabel);
    voteControlsLayout->addWidget(this->voteAmount_);
    voteControlsLayout->addStretch(1);
    this->voteControls_->hide();
    root->addWidget(this->voteControls_);

    this->errorLabel_ =
        textLabel(QString{}, QStringLiteral("PredictionManageError"), this);
    this->errorLabel_->setWordWrap(true);
    this->errorLabel_->hide();
    root->addWidget(this->errorLabel_);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 8, 0, 0);
    buttonRow->setSpacing(8);

    this->deleteButton_ =
        new QPushButton(QStringLiteral("Delete"), this);
    this->deleteButton_->setObjectName(
        QStringLiteral("PredictionManageButton"));
    this->deleteButton_->setCursor(Qt::PointingHandCursor);

    this->summaryButton_ =
        new QPushButton(QStringLiteral("Summary"), this);
    this->summaryButton_->setObjectName(
        QStringLiteral("PredictionManageButton"));
    this->summaryButton_->setCursor(Qt::PointingHandCursor);

    this->predictButton_ =
        new QPushButton(QStringLiteral("Predict"), this);
    this->predictButton_->setObjectName(
        QStringLiteral("PredictionManageButton"));
    this->predictButton_->setCursor(Qt::PointingHandCursor);
    this->predictButton_->hide();

    this->backButton_ = new QPushButton(QStringLiteral("Back"), this);
    this->backButton_->setObjectName(
        QStringLiteral("PredictionManageButton"));
    this->backButton_->setCursor(Qt::PointingHandCursor);
    this->backButton_->hide();

    this->actionButton_ = new QPushButton(this);
    this->actionButton_->setObjectName(
        QStringLiteral("PredictionManageButton"));
    this->actionButton_->setCursor(Qt::PointingHandCursor);

    buttonRow->addStretch(1);
    buttonRow->addWidget(this->predictButton_);
    buttonRow->addWidget(this->summaryButton_);
    buttonRow->addWidget(this->deleteButton_);
    buttonRow->addWidget(this->backButton_);
    buttonRow->addWidget(this->actionButton_);
    root->addLayout(buttonRow);

    QObject::connect(this->deleteButton_, &QPushButton::clicked, this, [this] {
        this->cancelPrediction();
    });
    QObject::connect(this->summaryButton_, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl(QStringLiteral(
            "https://www.twitch.tv/popout/%1/predictions/summary")
                                           .arg(this->channelLogin_)));
    });
    QObject::connect(this->predictButton_, &QPushButton::clicked, this,
                     [this] { this->setVoting(true); });
    QObject::connect(this->backButton_, &QPushButton::clicked, this, [this] {
        if (this->voting_)
        {
            this->setVoting(false);
        }
        else
        {
            this->setChoosingOutcome(false);
        }
    });
    QObject::connect(this->actionButton_, &QPushButton::clicked, this, [this] {
        if (this->voting_)
        {
            this->submitPrediction();
        }
        else if (this->submissionsOpen() && this->canManage_)
        {
            this->lockPrediction();
        }
        else if (this->submissionsOpen() && this->canVote())
        {
            this->setVoting(true);
        }
        else if (!this->choosingOutcome_)
        {
            this->setChoosingOutcome(true);
        }
        else
        {
            this->resolvePrediction();
        }
    });

    this->timer_ = new QTimer(this);
    this->timer_->setInterval(250);
    QObject::connect(this->timer_, &QTimer::timeout, this, [this] {
        this->updateTimerUi();
    });
    this->timer_->start();

    this->lastSubmissionsOpen_ = this->submissionsOpen();
    this->themeChangedEvent();
    this->updateTimerUi();
    this->updateDialogSize();
}

void ManagePredictionDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->applyTheme();
    this->rebuildOutcomes();
    this->updateTimerUi();
}

void ManagePredictionDialog::applyTheme()
{
    const auto *theme = getTheme();
    if (theme == nullptr)
    {
        return;
    }

    const auto background = theme->isLightTheme() ? QColor("#f7f7f8")
                                                  : QColor("#18181b");
    const auto text = theme->isLightTheme() ? QColor("#1f1f23")
                                            : QColor("#efeff1");
    const auto mutedText = withAlpha(text, 190);
    const auto disabledText = withAlpha(text, 130);
    const auto track = theme->isLightTheme() ? QColor("#e3e3e8")
                                             : QColor("#2a2a2f");
    const auto timerFill = theme->isLightTheme() ? QColor("#7c7c86")
                                                 : QColor("#8b8b95");
    const auto selectedBackground =
        theme->isLightTheme() ? QColor(63, 134, 255, 32)
                              : QColor(63, 134, 255, 38);
    const auto neutralButton = theme->isLightTheme() ? QColor("#d7d7de")
                                                     : QColor("#3d3d44");
    const auto neutralButtonHover = theme->isLightTheme() ? QColor("#c9cad2")
                                                          : QColor("#4a4a52");

    this->overrideBackgroundColor_ = background;

    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::Base, background);
    palette.setColor(QPalette::WindowText, text);
    this->setPalette(palette);
    this->getLayoutContainer()->setPalette(palette);

    if (this->timerBar_ != nullptr)
    {
        this->timerBar_->setColors(track, timerFill);
    }
    if (this->summaryButton_ != nullptr)
    {
        this->summaryButton_->setIcon(
            QIcon(renderSvgIcon(summarySvgPaths(), text, 16)));
        this->summaryButton_->setIconSize(QSize(16, 16));
    }

    for (auto *choice : this->outcomeChoices_)
    {
        choice->setChromeColors(selectedBackground, BLUE_OUTCOME);
    }

    this->getLayoutContainer()->setStyleSheet(QStringLiteral(R"(
QWidget#ManagePredictionRoot {
    background: %1;
    color: %2;
}
QLabel {
    color: %2;
}
QLabel#PredictionManageStatus {
    color: %3;
    font-size: 16px;
}
QLabel#PredictionManageStatus[chooseMode="true"] {
    color: %2;
    font-size: 21px;
    font-weight: 600;
}
QLabel#PredictionChooseDescription {
    color: %2;
    font-size: 15px;
    font-weight: 600;
}
QLabel#PredictionManageTitle {
    color: %2;
    font-size: 15px;
    font-weight: 600;
}
QLabel#PredictionMetricValue {
    color: %2;
    font-size: 13px;
}
QLabel#PredictionManageError {
    color: #ff6b6b;
}
QSpinBox {
    background: %5;
    color: %2;
    border: 1px solid %6;
    border-radius: 4px;
    padding: 4px 7px;
    selection-background-color: palette(highlight);
    selection-color: palette(highlighted-text);
}
QPushButton#PredictionManageButton {
    background: %5;
    color: %2;
    border: none;
    border-radius: 4px;
    padding: 7px 14px;
    font-weight: 700;
}
QPushButton#PredictionManageButton:hover {
    background: %6;
}
QPushButton#PredictionManageButton:disabled {
    background: %5;
    color: %4;
}
)")
                                                 .arg(rgba(background),
                                                      rgba(text),
                                                      rgba(mutedText),
                                                      rgba(disabledText),
                                                      rgba(neutralButton),
                                                      rgba(neutralButtonHover)));
}

void ManagePredictionDialog::rebuildOutcomes()
{
    if (this->outcomesLayout_ == nullptr)
    {
        return;
    }

    clearLayout(this->outcomesLayout_);
    this->outcomeChoices_.clear();

    const auto outcomeCount =
        static_cast<int>(this->prediction_.outcomes.size());
    const auto totalPoints = totalChannelPoints(this->prediction_);
    if (outcomeCount <= 0)
    {
        return;
    }

    const auto *theme = getTheme();
    const auto background =
        theme != nullptr && theme->isLightTheme() ? QColor(63, 134, 255, 32)
                                                  : QColor(63, 134, 255, 38);
    const bool selecting = this->choosingOutcome_ || this->voting_;
    this->outcomesLayout_->setContentsMargins(0, selecting ? 7 : 5, 0, 0);
    this->outcomesLayout_->setSpacing(selecting ? 10 : 5);

    if (selecting)
    {
        if (this->selectedOutcomeID_.isEmpty())
        {
            this->selectedOutcomeID_ = this->prediction_.outcomes.front().id;
        }

        const auto text =
            theme != nullptr && theme->isLightTheme() ? QColor("#1f1f23")
                                                      : QColor("#efeff1");
        const auto rowBackground =
            theme != nullptr && theme->isLightTheme() ? QColor("#f0f0f3")
                                                      : QColor("#2c2c31");
        const auto rowBorder =
            theme != nullptr && theme->isLightTheme() ? QColor("#d6d6dc")
                                                      : QColor("#3b3b43");
        const auto selectedBorder =
            theme != nullptr && theme->isLightTheme() ? QColor("#1f1f23")
                                                      : QColor("#efeff1");

        for (size_t i = 0; i < this->prediction_.outcomes.size(); ++i)
        {
            const auto &outcome = this->prediction_.outcomes[i];

            auto *row = new OutcomeChoiceWidget(outcome.id, this);
            row->setFixedHeight(OUTCOME_SELECTION_ROW_WIDGET_HEIGHT);
            row->setBaseChrome(rowBackground, rowBorder);
            row->setChromeColors(rowBackground, selectedBorder);
            row->setSelected(outcome.id == this->selectedOutcomeID_);
            row->clicked = [this, outcomeID = outcome.id] {
                this->selectOutcome(outcomeID);
            };

            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(12, 0, 12, 0);
            layout->setSpacing(10);

            auto *badge = new QLabel(row);
            badge->setFixedSize(BADGE_SIZE, BADGE_SIZE);
            badge->setScaledContents(true);
            const auto badgePixmap = loadBadgePixmap(i, outcomeCount);
            badge->setPixmap(badgePixmap.scaled(BADGE_SIZE, BADGE_SIZE,
                                                Qt::KeepAspectRatio,
                                                Qt::SmoothTransformation));

            auto *title =
                textLabel(outcome.title, QStringLiteral("PredictionOutcomeName"),
                          row);
            title->setStyleSheet(QStringLiteral(
                                     "color: %1; font-size: 16px; font-weight: "
                                     "700;")
                                     .arg(rgba(text)));

            layout->addWidget(badge, 0, Qt::AlignVCenter);
            layout->addWidget(title, 1, Qt::AlignVCenter);

            this->outcomeChoices_.push_back(row);
            this->outcomesLayout_->addWidget(row);
        }

        return;
    }

    if (outcomeCount <= 2)
    {
        auto *container =
            new OutcomeChoiceWidget(QStringLiteral("__two_outcome_area"), this);
        container->setCursor(Qt::ArrowCursor);
        container->setFixedHeight(154);
        auto *layout = new QHBoxLayout(container);
        layout->setContentsMargins(0, 4, 0, 0);
        layout->setSpacing(10);

        auto *leftStats = new QWidget(container);
        leftStats->setFixedWidth(105);
        auto *leftStatsLayout = new QVBoxLayout(leftStats);
        leftStatsLayout->setContentsMargins(0, 13, 0, 0);
        leftStatsLayout->setSpacing(3);

        auto *center = new QWidget(container);
        auto *centerLayout = new QVBoxLayout(center);
        centerLayout->setContentsMargins(0, 9, 0, 0);
        centerLayout->setSpacing(5);

        auto *rightStats = new QWidget(container);
        rightStats->setFixedWidth(105);
        auto *rightStatsLayout = new QVBoxLayout(rightStats);
        rightStatsLayout->setContentsMargins(0, 13, 0, 0);
        rightStatsLayout->setSpacing(3);

        const auto &left = this->prediction_.outcomes[0];
        const auto &right = this->prediction_.outcomes.size() > 1
                                ? this->prediction_.outcomes[1]
                                : this->prediction_.outcomes[0];
        const auto leftColor = outcomeColor(0, outcomeCount);
        const auto rightColor = outcomeColor(1, outcomeCount);

        leftStatsLayout->addWidget(metricPair(
            0, localizeNumbers(left.channelPoints), leftColor, false, leftStats));
        leftStatsLayout->addWidget(metricPair(
            1, returnRatioForOutcome(left, totalPoints), leftColor, false,
            leftStats));
        leftStatsLayout->addWidget(metricPair(
            2, localizeNumbers(left.users), leftColor, false, leftStats));
        leftStatsLayout->addWidget(metricPair(
            3, QStringLiteral("0"), leftColor, false, leftStats));
        leftStatsLayout->addStretch(1);

        rightStatsLayout->addWidget(metricPair(
            0, localizeNumbers(right.channelPoints), rightColor, true,
            rightStats));
        rightStatsLayout->addWidget(metricPair(
            1, returnRatioForOutcome(right, totalPoints), rightColor, true,
            rightStats));
        rightStatsLayout->addWidget(metricPair(
            2, localizeNumbers(right.users), rightColor, true, rightStats));
        rightStatsLayout->addWidget(metricPair(
            3, QStringLiteral("0"), rightColor, true, rightStats));
        rightStatsLayout->addStretch(1);

        auto *choices = new QWidget(center);
        auto *choicesLayout = new QHBoxLayout(choices);
        choicesLayout->setContentsMargins(0, 0, 0, 0);
        choicesLayout->setSpacing(8);

        const auto makeChoice =
            [this, background, totalPoints](
                const HelixPredictionOutcome &outcome, QColor color,
                QWidget *parent) {
                auto *choice = new OutcomeChoiceWidget(outcome.id, parent);
                choice->setFixedSize(124, 96);
                choice->setCursor(Qt::ArrowCursor);
                choice->setChromeColors(background, color);

                auto *choiceLayout = new QVBoxLayout(choice);
                choiceLayout->setContentsMargins(8, 5, 8, 4);
                choiceLayout->setSpacing(2);

                auto *title =
                    textLabel(outcome.title,
                              QStringLiteral("PredictionOutcomeName"), choice);
                title->setAlignment(Qt::AlignCenter);
                title->setStyleSheet(QStringLiteral(
                                         "color: %1; font-size: 18px; "
                                         "font-weight: 600;")
                                         .arg(color.name()));

                auto *percent =
                    textLabel(QStringLiteral("%1%").arg(
                                  percentageForOutcome(outcome, totalPoints)),
                              QStringLiteral("PredictionOutcomePercent"),
                              choice);
                percent->setAlignment(Qt::AlignCenter);
                percent->setStyleSheet(QStringLiteral(
                                           "color: %1; font-size: 22px; "
                                           "font-weight: 700;")
                                           .arg(rgba(withAlpha(color, 215))));

                auto *dot =
                    textLabel(QStringLiteral("●"), QStringLiteral(""), choice);
                dot->setAlignment(Qt::AlignCenter);
                dot->setStyleSheet(QStringLiteral("color: %1; font-size: 18px;")
                                       .arg(color.name()));

                choiceLayout->addWidget(title);
                choiceLayout->addWidget(percent);
                choiceLayout->addWidget(dot);

                this->outcomeChoices_.push_back(choice);
                return choice;
            };

        choicesLayout->addStretch(1);
        choicesLayout->addWidget(makeChoice(left, leftColor, choices));
        choicesLayout->addWidget(makeChoice(right, rightColor, choices));
        choicesLayout->addStretch(1);

        centerLayout->addWidget(choices);
        centerLayout->addStretch(1);

        layout->addWidget(leftStats);
        layout->addWidget(center, 1);
        layout->addWidget(rightStats);
        this->outcomesLayout_->addWidget(container);

        return;
    }

    for (size_t i = 0; i < this->prediction_.outcomes.size(); ++i)
    {
        const auto &outcome = this->prediction_.outcomes[i];
        const auto color = outcomeColor(i, outcomeCount);

        auto *row = new OutcomeChoiceWidget(outcome.id, this);
        row->setFixedHeight(MULTI_OUTCOME_ROW_HEIGHT);
        row->setCursor(Qt::ArrowCursor);
        row->setChromeColors(background, color);

        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(4, 2, 4, 2);
        layout->setSpacing(8);

        auto *badgeWrapper = new QWidget(row);
        badgeWrapper->setFixedSize(BADGE_SIZE, MULTI_OUTCOME_ROW_HEIGHT - 4);
        auto *badgeLayout = new QVBoxLayout(badgeWrapper);
        badgeLayout->setContentsMargins(0, 5, 0, 0);
        badgeLayout->setSpacing(0);

        auto *badge = new QLabel(badgeWrapper);
        badge->setFixedSize(BADGE_SIZE, BADGE_SIZE);
        badge->setScaledContents(true);
        const auto badgePixmap = loadBadgePixmap(i, outcomeCount);
        badge->setPixmap(badgePixmap.scaled(BADGE_SIZE, BADGE_SIZE,
                                            Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation));
        badgeLayout->addWidget(badge, 0, Qt::AlignTop | Qt::AlignHCenter);
        badgeLayout->addStretch(1);

        auto *titleBlock = new QWidget(row);
        auto *titleLayout = new QVBoxLayout(titleBlock);
        titleLayout->setContentsMargins(0, 0, 0, 0);
        titleLayout->setSpacing(0);

        auto *title =
            textLabel(outcome.title, QStringLiteral("PredictionOutcomeName"),
                      titleBlock);
        title->setStyleSheet(QStringLiteral(
                                 "color: %1; font-size: 18px; font-weight: "
                                 "600;")
                                 .arg(color.name()));

        auto *dot = textLabel(QStringLiteral("●"), QStringLiteral(""), titleBlock);
        dot->setStyleSheet(
            QStringLiteral("color: %1; font-size: 14px;").arg(color.name()));

        titleLayout->addWidget(title);
        titleLayout->addWidget(dot);

        auto *percent =
            textLabel(QStringLiteral("%1%").arg(
                          percentageForOutcome(outcome, totalPoints)),
                      QStringLiteral("PredictionOutcomePercent"), row);
        percent->setFixedWidth(60);
        percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        percent->setStyleSheet(QStringLiteral(
                                   "color: %1; font-size: 22px; font-weight: "
                                   "700;")
                                   .arg(rgba(withAlpha(color, 215))));

        layout->addWidget(badgeWrapper, 0, Qt::AlignTop);
        layout->addWidget(titleBlock, 1);
        layout->addWidget(percent);
        layout->addWidget(metricPair(
            0, localizeNumbers(outcome.channelPoints), color, false, row));
        layout->addWidget(metricPair(
            1, returnRatioForOutcome(outcome, totalPoints), color, false, row));
        layout->addWidget(metricPair(
            2, localizeNumbers(outcome.users), color, false, row));
        layout->addWidget(
            metricPair(3, QStringLiteral("0"), color, false, row));

        this->outcomeChoices_.push_back(row);
        this->outcomesLayout_->addWidget(row);
    }
}

void ManagePredictionDialog::updateTimerUi()
{
    const auto open = this->submissionsOpen();
    if (this->voting_ && !open)
    {
        this->voting_ = false;
        this->selectedOutcomeID_.clear();
        this->rebuildOutcomes();
    }

    const auto choosing = this->choosingOutcome_ && !open && this->canManage_;
    const auto voting = this->voting_ && open;

    this->setWindowTitle(voting ? QStringLiteral("Make a Prediction")
        : choosing             ? QStringLiteral("Choose Outcome")
        : this->canManage_     ? QStringLiteral("Manage Prediction")
                               : QStringLiteral("Prediction"));
    this->statusLabel_->setText(voting ? QStringLiteral("Make a Prediction")
        : choosing                  ? QStringLiteral("Choose Outcome")
        : open                      ? QStringLiteral("Submissions Open")
                                    : QStringLiteral("Submissions Closed"));
    this->statusLabel_->setProperty("chooseMode", choosing || voting);
    repolish(this->statusLabel_);
    this->descriptionLabel_->setText(
        voting
            ? QStringLiteral(
                  "Choose an outcome and wager Channel Points. This cannot be "
                  "changed after submitting.")
            : QStringLiteral(
                  "Select the result and reward the viewers who voted for it "
                  "with Channel Points."));
    this->descriptionLabel_->setVisible(choosing || voting);
    this->timerBar_->setProgress(this->timerProgress());
    this->timerBar_->setVisible(open && !choosing);
    this->voteControls_->setVisible(voting);
    this->actionButton_->setText(
        voting ? QStringLiteral("Predict")
        : open && this->canManage_ ? QStringLiteral("End Submissions")
        : open && this->canVote()  ? QStringLiteral("Predict")
        : choosing                 ? QStringLiteral("Complete Prediction")
                                   : QStringLiteral("Choose Outcome"));
    this->updateActionButton();

    if (this->lastSubmissionsOpen_ != open)
    {
        this->lastSubmissionsOpen_ = open;
        this->updateDialogSize();
    }
}

void ManagePredictionDialog::updateActionButton()
{
    const auto open = this->submissionsOpen();
    const auto choosing = this->choosingOutcome_ && !open && this->canManage_;
    const auto voting = this->voting_ && open;
    const auto selecting = choosing || voting;
    const auto canVote = this->canVote();

    this->predictButton_->setVisible(open && this->canManage_ && canVote &&
                                     !voting);
    this->summaryButton_->setVisible(!selecting);
    this->deleteButton_->setVisible(this->canManage_ && !selecting);
    this->backButton_->setVisible(selecting);
    this->actionButton_->setVisible(
        voting || (open && (this->canManage_ || canVote)) ||
        (!open && this->canManage_));
    this->actionButton_->setProperty("primary", false);
    repolish(this->actionButton_);

    if (this->submitting_)
    {
        this->predictButton_->setEnabled(false);
        this->summaryButton_->setEnabled(false);
        this->deleteButton_->setEnabled(false);
        this->backButton_->setEnabled(false);
        this->actionButton_->setEnabled(false);
        return;
    }

    this->predictButton_->setEnabled(canVote);
    this->deleteButton_->setEnabled(
        this->canManage_ && (this->prediction_.status == "ACTIVE" ||
                             this->prediction_.status == "LOCKED"));
    this->summaryButton_->setEnabled(true);
    this->backButton_->setEnabled(selecting);
    this->actionButton_->setEnabled(
        (voting && canVote && !this->selectedOutcomeID_.isEmpty() &&
         this->voteAmount_->value() >= 10) ||
        (open && this->canManage_ && !voting) ||
        (open && canVote && !this->canManage_ && !voting) ||
        (choosing && !this->selectedOutcomeID_.isEmpty()) ||
        (!open && this->canManage_ && !choosing &&
         !this->prediction_.outcomes.empty()));
}

void ManagePredictionDialog::updateDialogSize()
{
    const auto outcomeCount =
        static_cast<int>(this->prediction_.outcomes.size());
    const auto voting = this->voting_ && this->submissionsOpen();
    if ((this->choosingOutcome_ && !this->submissionsOpen()) || voting)
    {
        const auto targetHeight = OUTCOME_SELECTION_BASE_HEIGHT +
                                  outcomeCount * OUTCOME_SELECTION_ROW_HEIGHT +
                                  (voting ? VOTING_CONTROLS_HEIGHT : 0);
        this->setScaleIndependentSize(
            DIALOG_WIDTH,
            std::clamp(targetHeight,
                       voting ? OUTCOME_SELECTION_MIN_HEIGHT +
                                    VOTING_CONTROLS_HEIGHT
                              : OUTCOME_SELECTION_MIN_HEIGHT,
                       voting ? OUTCOME_SELECTION_MAX_HEIGHT +
                                    VOTING_CONTROLS_HEIGHT
                              : OUTCOME_SELECTION_MAX_HEIGHT));
        this->timerBar_->setFixedWidth(TIMER_BAR_WIDTH);
        return;
    }

    if (outcomeCount <= 2)
    {
        this->setScaleIndependentSize(
            DIALOG_WIDTH,
            this->submissionsOpen() ? TWO_OUTCOME_ACTIVE_HEIGHT
                                    : TWO_OUTCOME_CLOSED_HEIGHT);
        this->timerBar_->setFixedWidth(TIMER_BAR_WIDTH);
        return;
    }

    const auto timerHeight = this->submissionsOpen() ? TIMER_BAR_HEIGHT + 4 : 0;
    const auto targetHeight = MULTI_OUTCOME_BASE_HEIGHT + timerHeight +
                              outcomeCount * MULTI_OUTCOME_ROW_HEIGHT;
    this->setScaleIndependentSize(DIALOG_WIDTH,
                                  std::clamp(targetHeight, 280, 650));
    this->timerBar_->setFixedWidth(TIMER_BAR_WIDTH);
}

void ManagePredictionDialog::setChoosingOutcome(bool choosing)
{
    if (choosing && (!this->canManage_ || this->submissionsOpen() ||
                     this->prediction_.outcomes.empty()))
    {
        return;
    }

    if (this->choosingOutcome_ == choosing)
    {
        return;
    }

    this->clearError();
    this->voting_ = false;
    this->choosingOutcome_ = choosing;
    if (choosing)
    {
        const auto selectionExists = std::any_of(
            this->prediction_.outcomes.begin(), this->prediction_.outcomes.end(),
            [this](const HelixPredictionOutcome &outcome) {
                return outcome.id == this->selectedOutcomeID_;
            });
        if (!selectionExists)
        {
            this->selectedOutcomeID_ = this->prediction_.outcomes.front().id;
        }
    }
    else
    {
        this->selectedOutcomeID_.clear();
    }

    this->rebuildOutcomes();
    this->updateTimerUi();
    this->updateDialogSize();
}

void ManagePredictionDialog::setVoting(bool voting)
{
    if ((voting && (!this->canVote() || this->prediction_.outcomes.empty())) ||
        this->voting_ == voting)
    {
        return;
    }

    this->clearError();
    this->choosingOutcome_ = false;
    this->voting_ = voting;
    this->selectedOutcomeID_.clear();
    this->rebuildOutcomes();
    this->updateTimerUi();
    this->updateDialogSize();
}

void ManagePredictionDialog::selectOutcome(const QString &outcomeID)
{
    if (!this->choosingOutcome_ && !this->voting_)
    {
        return;
    }

    this->selectedOutcomeID_ = outcomeID;
    for (auto *choice : this->outcomeChoices_)
    {
        choice->setSelected(choice->outcomeID() == outcomeID);
    }
    this->updateActionButton();
}

void ManagePredictionDialog::submitPrediction()
{
    if (this->submitting_ || !this->voting_ || !this->canVote() ||
        this->selectedOutcomeID_.isEmpty())
    {
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser == nullptr || currentUser->isAnon())
    {
        this->showError(
            QStringLiteral("Log in to Twitch before making a prediction."));
        return;
    }

    QString oauthClient = currentUser->getOAuthClient();
    QString oauthToken = currentUser->getOAuthToken();
    if (this->useModerationAuth_)
    {
        QString authError;
        const auto moderationAccount =
            TwitchModerationAuth::resolveForCurrentUser(
                currentUser->getUserId(), &authError);
        if (!moderationAccount.isValid())
        {
            this->showError(authError.isEmpty()
                                ? QStringLiteral(
                                      "Reconnect Twitch mod actions before "
                                      "making a prediction.")
                                : authError);
            return;
        }
        oauthClient = moderationAccount.clientId;
        oauthToken = moderationAccount.oauthToken;
    }

    QString outcomeTitle;
    for (const auto &outcome : this->prediction_.outcomes)
    {
        if (outcome.id == this->selectedOutcomeID_)
        {
            outcomeTitle = outcome.title;
            break;
        }
    }

    const auto points = this->voteAmount_->value();
    this->clearError();
    this->submitting_ = true;
    this->updateActionButton();

    QPointer<ManagePredictionDialog> self(this);
    TwitchWebApi::makePrediction(
        this->prediction_.id, this->selectedOutcomeID_, points, oauthClient,
        oauthToken,
        [self, channel = this->channel_, outcomeTitle, points] {
            notifyPollsAndPredictionsChanged(channel);
            if (channel != nullptr)
            {
                channel->addSystemMessage(
                    QStringLiteral("Predicted %1 Channel Points on '%2'.")
                        .arg(localizeNumbers(points), outcomeTitle));
            }
            if (self != nullptr)
            {
                self->close();
            }
        },
        [self](const QString &error) {
            if (self != nullptr)
            {
                self->submitting_ = false;
                self->showError(QStringLiteral("Failed to make prediction - ") +
                                error);
                self->updateActionButton();
            }
        });
}

void ManagePredictionDialog::endPrediction(
    bool refundPoints, QString winningOutcomeID,
    std::function<void(const HelixPrediction &)> successCallback,
    std::function<void(const QString &)> failureCallback)
{
    if (!this->canManage_)
    {
        failureCallback(QStringLiteral(
            "This Twitch account cannot manage this prediction."));
        return;
    }

    if (this->prediction_.id.trimmed().isEmpty())
    {
        failureCallback(QStringLiteral(
            "Mergerino does not have the Twitch prediction ID yet. Try again "
            "in a moment."));
        return;
    }

    if (!this->useModerationAuth_)
    {
        getHelix()->endPrediction(this->broadcasterID_, this->prediction_.id,
                                  refundPoints, winningOutcomeID,
                                  std::move(successCallback),
                                  std::move(failureCallback));
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    QString authError;
    const auto moderationAccount =
        TwitchModerationAuth::resolveForCurrentUser(
            currentUser != nullptr ? currentUser->getUserId() : QString{},
            &authError);
    if (!moderationAccount.isValid())
    {
        failureCallback(authError);
        return;
    }

    QPointer<ManagePredictionDialog> self(this);
    TwitchWebApi::endPredictionEvent(
        this->prediction_.id, refundPoints, winningOutcomeID,
        moderationAccount.clientId, moderationAccount.oauthToken,
        [self, refundPoints, winningOutcomeID,
         successCallback = std::move(successCallback)]() mutable {
            if (self == nullptr)
            {
                return;
            }

            auto data = self->prediction_;
            const auto now = QDateTime::currentDateTimeUtc();
            if (refundPoints)
            {
                data.status = QStringLiteral("CANCELED");
                data.endedAt = now;
            }
            else if (winningOutcomeID.trimmed().isEmpty())
            {
                data.status = QStringLiteral("LOCKED");
                data.lockedAt = now;
            }
            else
            {
                data.status = QStringLiteral("RESOLVED");
                data.winningOutcomeID = winningOutcomeID.trimmed();
                data.endedAt = now;
            }

            successCallback(data);
        },
        std::move(failureCallback));
}

void ManagePredictionDialog::cancelPrediction()
{
    if (this->submitting_ || !this->canManage_)
    {
        return;
    }

    this->clearError();
    this->submitting_ = true;
    this->updateActionButton();

    QPointer<ManagePredictionDialog> self(this);
    this->endPrediction(
        true, {},
        [channel = this->channel_, broadcasterID = this->broadcasterID_,
         self](const HelixPrediction &data) {
            TwitchPollsAndPredictionsBar::clearLocalPrediction(broadcasterID);
            notifyPollsAndPredictionsChanged(channel);
            if (channel != nullptr)
            {
                channel->addSystemMessage(QStringLiteral(
                    "Deleted Twitch prediction: '%1'").arg(data.title));
            }
            if (self != nullptr)
            {
                self->close();
            }
        },
        [self](const auto &error) {
            if (self != nullptr)
            {
                self->submitting_ = false;
                self->showError("Failed to delete prediction - " + error);
                self->updateActionButton();
            }
        });
}

void ManagePredictionDialog::lockPrediction()
{
    if (this->submitting_ || !this->canManage_)
    {
        return;
    }

    this->clearError();
    this->submitting_ = true;
    this->updateActionButton();

    QPointer<ManagePredictionDialog> self(this);
    this->endPrediction(
        false, {},
        [channel = this->channel_, broadcasterID = this->broadcasterID_,
         self](const HelixPrediction &data) {
            TwitchPollsAndPredictionsBar::rememberLocalPrediction(
                broadcasterID, data);
            notifyPollsAndPredictionsChanged(channel);
            if (channel != nullptr)
            {
                channel->addSystemMessage(QStringLiteral(
                    "Locked prediction: '%1'").arg(data.title));
            }
            if (self != nullptr)
            {
                self->prediction_ = data;
                self->choosingOutcome_ = false;
                self->selectedOutcomeID_.clear();
                self->submitting_ = false;
                self->rebuildOutcomes();
                self->updateTimerUi();
                self->updateDialogSize();
            }
        },
        [self](const auto &error) {
            if (self != nullptr)
            {
                self->submitting_ = false;
                self->showError("Failed to end submissions - " + error);
                self->updateActionButton();
            }
        });
}

void ManagePredictionDialog::resolvePrediction()
{
    if (this->submitting_ || !this->canManage_ || !this->choosingOutcome_ ||
        this->selectedOutcomeID_.isEmpty())
    {
        return;
    }

    this->clearError();
    this->submitting_ = true;
    this->updateActionButton();

    QPointer<ManagePredictionDialog> self(this);
    this->endPrediction(
        false, this->selectedOutcomeID_,
        [channel = this->channel_, broadcasterID = this->broadcasterID_,
         self](const HelixPrediction &data) {
            TwitchPollsAndPredictionsBar::clearLocalPrediction(broadcasterID);
            notifyPollsAndPredictionsChanged(channel);
            if (channel != nullptr)
            {
                channel->addSystemMessage(
                    QStringLiteral("Completed prediction: %1 - '%2' won")
                        .arg(data.title, firstWinningOutcomeTitle(data)));
            }
            if (self != nullptr)
            {
                self->close();
            }
        },
        [self](const auto &error) {
            if (self != nullptr)
            {
                self->submitting_ = false;
                self->showError("Failed to choose outcome - " + error);
                self->updateActionButton();
            }
        });
}

void ManagePredictionDialog::showError(const QString &message)
{
    this->errorLabel_->setText(message);
    this->errorLabel_->show();
}

void ManagePredictionDialog::clearError()
{
    this->errorLabel_->clear();
    this->errorLabel_->hide();
}

bool ManagePredictionDialog::submissionsOpen() const
{
    if (this->prediction_.status != "ACTIVE")
    {
        return false;
    }

    if (!this->prediction_.createdAt.isValid() ||
        this->prediction_.predictionWindow <= 0)
    {
        return true;
    }

    return this->timerProgress() > 0.0;
}

bool ManagePredictionDialog::canVote() const
{
    if (!this->submissionsOpen())
    {
        return false;
    }

    const auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    return currentUser != nullptr && !currentUser->isAnon() &&
           !currentUser->getOAuthToken().trimmed().isEmpty() &&
           currentUser->getUserId() != this->broadcasterID_;
}

double ManagePredictionDialog::timerProgress() const
{
    if (this->prediction_.status != "ACTIVE")
    {
        return 0.0;
    }

    if (!this->prediction_.createdAt.isValid() ||
        this->prediction_.predictionWindow <= 0)
    {
        return 1.0;
    }

    const auto elapsed =
        this->prediction_.createdAt.toUTC().msecsTo(QDateTime::currentDateTimeUtc());
    const auto total = static_cast<double>(this->prediction_.predictionWindow) *
                       1000.0;
    return std::clamp(1.0 - static_cast<double>(elapsed) / total, 0.0, 1.0);
}

}  // namespace chatterino
