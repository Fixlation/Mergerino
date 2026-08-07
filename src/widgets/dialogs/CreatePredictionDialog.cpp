// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/CreatePredictionDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/api/TwitchModerationAuth.hpp"
#include "providers/twitch/api/TwitchWebApi.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/Clipboard.hpp"
#include "widgets/Window.hpp"
#include "widgets/dialogs/KickPredictionDialog.hpp"
#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace chatterino {
namespace {

constexpr int MAX_PREDICTION_TITLE_LENGTH = 45;
constexpr int MAX_PREDICTION_OUTCOME_LENGTH = 25;
constexpr int MIN_PREDICTION_OUTCOMES = 2;
constexpr int MAX_PREDICTION_OUTCOMES = 10;
constexpr int BADGE_SIZE = 16;
constexpr int DIALOG_WIDTH = 650;
constexpr int BASE_DIALOG_HEIGHT = 430;
constexpr int BASE_VISIBLE_OUTCOMES = MIN_PREDICTION_OUTCOMES + 1;
constexpr int OUTCOME_INPUT_HEIGHT = 36;
constexpr int OUTCOME_ROW_GAP = 6;
constexpr int REMOVE_COLUMN_WIDTH = 30;
constexpr int REMOVE_BUTTON_SIZE = 24;
constexpr int REMOVE_ANIMATION_DURATION_MS = 120;

class PredictionDurationComboBox final : public QComboBox
{
public:
    explicit PredictionDurationComboBox(QWidget *parent = nullptr)
        : QComboBox(parent)
    {
    }

    void setChromeColors(QColor separator, QColor arrow)
    {
        this->separatorColor_ = separator;
        this->arrowColor_ = arrow;
        this->update();
    }

protected:
    void showPopup() override
    {
        QComboBox::showPopup();
        this->resetPopupView();
    }

    void paintEvent(QPaintEvent *event) override
    {
        QComboBox::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        constexpr int arrowAreaWidth = 13;
        const auto separatorX = this->width() - arrowAreaWidth;
        painter.setPen(QPen(this->separatorColor_, 1));
        painter.drawLine(separatorX, 1, separatorX, this->height() - 2);

        const auto centerX = separatorX + 7;
        const auto centerY = this->height() / 2 + 1;
        QPolygonF arrow;
        arrow << QPointF(centerX - 3, centerY - 2)
              << QPointF(centerX + 3, centerY - 2)
              << QPointF(centerX, centerY + 2);
        painter.setPen(Qt::NoPen);
        painter.setBrush(this->arrowColor_);
        painter.drawPolygon(arrow);
    }

private:
    void resetPopupView()
    {
        if (this->view() == nullptr || this->model() == nullptr ||
            this->currentIndex() < 0)
        {
            return;
        }

        const auto current = this->model()->index(
            this->currentIndex(), this->modelColumn(), this->rootModelIndex());
        this->view()->setCurrentIndex(current);
        this->view()->scrollToTop();
    }

    QColor separatorColor_{QColor("#55555d")};
    QColor arrowColor_{QColor("#a0a0a8")};
};

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

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("PredictionSectionLabel"));
    return label;
}

QString badgeFileName(size_t index, int outcomeCount, bool kick)
{
    if (kick && index < MIN_PREDICTION_OUTCOMES)
    {
        return QStringLiteral("kick-outcome-%1.svg").arg(index + 1);
    }

    if (outcomeCount <= 2 && index == 1)
    {
        return QStringLiteral("pink-2.png");
    }

    return QStringLiteral("blue-%1.png").arg(index + 1);
}

QString badgeResource(size_t index, int outcomeCount, bool kick)
{
    return QStringLiteral(":/predictions/%1").arg(
        badgeFileName(index, outcomeCount, kick));
}

QPixmap loadBadgePixmap(size_t index, int outcomeCount, bool kick)
{
    QPixmap badge(badgeResource(index, outcomeCount, kick));
    if (!badge.isNull())
    {
        return badge;
    }

    const auto fileName = badgeFileName(index, outcomeCount, kick);
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sourcePath = QDir::cleanPath(appDir.filePath(
        QStringLiteral("../../MergerinoSource/resources/predictions/%1").arg(
            fileName)));

    badge.load(sourcePath);
    return badge;
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

std::shared_ptr<KickChannel> resolveKickChannel(const ChannelPtr &channel)
{
    if (channel == nullptr)
    {
        return nullptr;
    }

    if (auto kick = std::dynamic_pointer_cast<KickChannel>(channel))
    {
        return kick;
    }

    if (auto merged = std::dynamic_pointer_cast<MergedChannel>(channel))
    {
        return std::dynamic_pointer_cast<KickChannel>(merged->kickChannel());
    }

    return nullptr;
}

bool kickPredictionAlreadyExistsError(const QString &error)
{
    const auto normalized = error.trimmed();
    return normalized.contains(QStringLiteral("PREDICTION_ALREADY_EXISTS"),
                               Qt::CaseInsensitive) ||
           normalized.compare(QStringLiteral("Invalid request"),
                              Qt::CaseInsensitive) == 0;
}

QString kickPredictionErrorText(const QString &error, bool broadcaster)
{
    if (error.contains(QStringLiteral("CHANNEL_POINTS_DISABLED"),
                       Qt::CaseInsensitive))
    {
        constexpr auto channelPointsUrl =
            "https://dashboard.kick.com/community/chat/channel-points";
        const auto link =
            QStringLiteral("<a style=\"color:#ffffff; text-decoration:underline;\" "
                           "href=\"%1\">%2</a>");
        return broadcaster
                   ? QStringLiteral(
                         "You need to enable channel points before starting a "
                         "prediction. %1")
                         .arg(link.arg(QString::fromLatin1(channelPointsUrl),
                                       QStringLiteral("Enable channel points")))
                   : QStringLiteral(
                         "Channel points need to be enabled by the channel "
                         "owner before you can start a prediction. They can "
                         "enable them %1.")
                         .arg(link.arg(QString::fromLatin1(channelPointsUrl),
                                       QStringLiteral("here")));
    }

    if (kickPredictionAlreadyExistsError(error))
    {
        return QStringLiteral(
            "You already have a Kick prediction running. Open it from the "
            "prediction banner to manage it.");
    }

    return QStringLiteral("Failed to create Kick prediction - %1").arg(error);
}

void notifyPollsAndPredictionsChanged(const ChannelPtr &channel)
{
    if (auto twitch = resolveTwitchChannel(channel))
    {
        twitch->streamStatusChanged.invoke();
    }
}

}  // namespace

void CreatePredictionDialog::showDialog(ChannelPtr channel,
                                        const TwitchChannel &twitchChannel)
{
    auto *dialog = new CreatePredictionDialog(
        std::move(channel), twitchChannel.roomId(),
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

void CreatePredictionDialog::showKickDialog(ChannelPtr channel,
                                            const KickChannel &kickChannel)
{
    auto *dialog = new CreatePredictionDialog(
        std::move(channel), QString{},
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())),
        true,
        kickChannel.slug().trimmed().isEmpty() ? kickChannel.getName()
                                              : kickChannel.slug());
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

CreatePredictionDialog::CreatePredictionDialog(ChannelPtr channel,
                                               QString broadcasterID,
                                               QWidget *parent, bool kick,
                                               QString kickChannelSlug)
    : BasePopup(
          {
              BaseWindow::EnableCustomFrame,
              BaseWindow::CloseButtonOnly,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , channel_(std::move(channel))
    , broadcasterID_(std::move(broadcasterID))
    , kick_(kick)
    , kickChannelSlug_(std::move(kickChannelSlug))
{
    this->setWindowTitle(this->kick_
                             ? QStringLiteral("Start a Kick Prediction")
                             : QStringLiteral("Start a Prediction"));
    this->setScaleIndependentSize(DIALOG_WIDTH, BASE_DIALOG_HEIGHT);
    this->setAutoFillBackground(true);
    this->getLayoutContainer()->setObjectName(
        QStringLiteral("CreatePredictionRoot"));
    this->getLayoutContainer()->setAutoFillBackground(true);

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(16, 12, 16, 12);
    root->setSpacing(7);

    auto *titleRow = new QHBoxLayout;
    titleRow->setContentsMargins(0, 0, 0, 0);
    titleRow->setSpacing(8);
    titleRow->addWidget(sectionLabel(QStringLiteral("Name the prediction"),
                                     this));
    titleRow->addStretch(1);
    this->titleCount_ = new QLabel(this);
    this->titleCount_->setObjectName(QStringLiteral("PredictionTitleCount"));
    titleRow->addWidget(this->titleCount_);
    root->addLayout(titleRow);

    this->title_ = new QLineEdit(this);
    this->title_->setMaxLength(MAX_PREDICTION_TITLE_LENGTH);
    this->title_->setPlaceholderText(QStringLiteral(
        "What viewers will predict, like \"Will I win five games in a row?\""));
    root->addWidget(this->title_);

    root->addSpacing(2);
    root->addWidget(sectionLabel(QStringLiteral("Possible Outcomes"), this));

    this->outcomesLayout_ = new QVBoxLayout;
    this->outcomesLayout_->setContentsMargins(0, 0, 0, 0);
    this->outcomesLayout_->setSpacing(0);
    root->addLayout(this->outcomesLayout_);

    for (size_t i = 0; i < this->outcomeRows_.size(); ++i)
    {
        auto &row = this->outcomeRows_[i];
        row.container = new QWidget(this);
        row.container->setObjectName(QStringLiteral("PredictionOutcomeRow"));
        row.container->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        row.container->setMinimumHeight(0);
        row.container->setMaximumHeight(0);
        row.topGap = i == 0 ? 0 : OUTCOME_ROW_GAP;

        auto *rowLayout = new QHBoxLayout(row.container);
        rowLayout->setContentsMargins(0, row.topGap, 6, 0);
        rowLayout->setSpacing(0);

        row.badge = new QLabel(row.container);
        row.badge->setFixedSize(BADGE_SIZE, BADGE_SIZE);
        row.badge->setScaledContents(true);
        rowLayout->addWidget(row.badge, 0, Qt::AlignVCenter);
        rowLayout->addSpacing(8);

        row.input = new QLineEdit(row.container);
        row.input->setFixedHeight(OUTCOME_INPUT_HEIGHT);
        row.input->setMaxLength(MAX_PREDICTION_OUTCOME_LENGTH);
        row.input->setPlaceholderText(
            i == 0
                ? QStringLiteral("Outcome 1, like \"Yes\"")
                : (i == 1 ? QStringLiteral("Outcome 2, like \"No\"")
                          : QString{}));
        rowLayout->addWidget(row.input, 1);

        row.removeContainer = new QWidget(row.container);
        row.removeContainer->setMinimumWidth(0);
        row.removeContainer->setMaximumWidth(0);
        row.removeContainer->setFixedHeight(REMOVE_BUTTON_SIZE);
        auto *removeLayout = new QHBoxLayout(row.removeContainer);
        removeLayout->setContentsMargins(4, 0, 2, 0);
        removeLayout->setSpacing(0);

        row.removeButton =
            new QPushButton(QString(QChar(0x00D7)), row.removeContainer);
        row.removeButton->setObjectName(
            QStringLiteral("PredictionRemoveButton"));
        row.removeButton->setFixedSize(REMOVE_BUTTON_SIZE,
                                       REMOVE_BUTTON_SIZE);
        removeLayout->addWidget(row.removeButton, 0, Qt::AlignTop);
        rowLayout->addWidget(row.removeContainer, 0, Qt::AlignTop);

        row.removeOpacityEffect = new QGraphicsOpacityEffect(row.removeButton);
        row.removeOpacityEffect->setOpacity(0.0);
        row.removeButton->setGraphicsEffect(row.removeOpacityEffect);
        row.removeWidthAnimation =
            new QPropertyAnimation(row.removeContainer, "maximumWidth", this);
        row.removeWidthAnimation->setDuration(REMOVE_ANIMATION_DURATION_MS);
        row.removeWidthAnimation->setEasingCurve(QEasingCurve::OutCubic);
        row.removeOpacityAnimation =
            new QPropertyAnimation(row.removeOpacityEffect, "opacity", this);
        row.removeOpacityAnimation->setDuration(REMOVE_ANIMATION_DURATION_MS);
        row.removeOpacityAnimation->setEasingCurve(QEasingCurve::OutCubic);
        auto *rowPtr = &row;
        QObject::connect(row.removeWidthAnimation, &QPropertyAnimation::finished,
                         this, [rowPtr] {
                             const auto targetWidth =
                                 rowPtr->removeVisible ? REMOVE_COLUMN_WIDTH
                                                       : 0;
                             rowPtr->removeContainer->setMinimumWidth(
                                 targetWidth);
                             rowPtr->removeContainer->setMaximumWidth(
                                 targetWidth);
                         });

        this->outcomesLayout_->addWidget(row.container);

        QObject::connect(row.input, &QLineEdit::textChanged, this, [this] {
            this->updateOutcomeRows();
            this->updateStartButton();
        });
        QObject::connect(row.removeButton, &QPushButton::clicked, this,
                         [this, i] {
                             this->removeOutcome(i);
                         });
    }

    root->addSpacing(2);
    root->addWidget(sectionLabel(QStringLiteral("Submission Period"), this));
    this->duration_ = new PredictionDurationComboBox(this);
    this->duration_->setCursor(Qt::PointingHandCursor);
    this->duration_->setFixedWidth(58);
    if (this->kick_)
    {
        this->duration_->addItem(QStringLiteral("1m"), 60);
        this->duration_->addItem(QStringLiteral("5m"), 300);
        this->duration_->addItem(QStringLiteral("10m"), 600);
        this->duration_->addItem(QStringLiteral("30m"), 1800);
    }
    else
    {
        this->duration_->addItem(QStringLiteral("30s"), 30);
        this->duration_->addItem(QStringLiteral("1m"), 60);
        this->duration_->addItem(QStringLiteral("2m"), 120);
        this->duration_->addItem(QStringLiteral("5m"), 300);
        this->duration_->addItem(QStringLiteral("10m"), 600);
        this->duration_->addItem(QStringLiteral("15m"), 900);
        this->duration_->addItem(QStringLiteral("20m"), 1200);
        this->duration_->addItem(QStringLiteral("30m"), 1800);
    }
    this->duration_->view()->setCursor(Qt::ArrowCursor);
    this->duration_->view()->viewport()->setCursor(Qt::ArrowCursor);
    root->addWidget(this->duration_, 0, Qt::AlignLeft);

    this->errorLabel_ = new QLabel(this);
    this->errorLabel_->setObjectName(QStringLiteral("PredictionErrorLabel"));
    this->errorLabel_->setWordWrap(true);
    this->errorLabel_->setOpenExternalLinks(true);
    this->errorLabel_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    this->errorLabel_->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(
        this->errorLabel_, &QWidget::customContextMenuRequested, this,
        [this](const QPoint &position) {
            const auto channelPointsUrl = QStringLiteral(
                "https://dashboard.kick.com/community/chat/channel-points");
            if (!this->errorLabel_->text().contains(channelPointsUrl))
            {
                return;
            }

            QMenu menu(this->errorLabel_);
            auto *copyLink = menu.addAction(QStringLiteral("Copy link"));
            if (menu.exec(this->errorLabel_->mapToGlobal(position)) == copyLink)
            {
                crossPlatformCopy(channelPointsUrl);
            }
        });
    this->errorLabel_->hide();
    root->addWidget(this->errorLabel_);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 14, 0, 0);
    buttonRow->setSpacing(8);

    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("PredictionCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    this->startButton_ =
        new QPushButton(QStringLiteral("Start Prediction"), this);
    this->startButton_->setObjectName(QStringLiteral("PredictionStartButton"));
    this->startButton_->setCursor(Qt::PointingHandCursor);

    buttonRow->addStretch(1);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(this->startButton_);
    root->addLayout(buttonRow);

    QObject::connect(cancelButton, &QPushButton::clicked, this, [this] {
        this->close();
    });
    QObject::connect(this->startButton_, &QPushButton::clicked, this, [this] {
        this->submit();
    });
    QObject::connect(this->title_, &QLineEdit::textChanged, this, [this] {
        this->updateTitleCount();
        this->updateStartButton();
    });

    this->themeChangedEvent();
    this->updateTitleCount();
    this->updateOutcomeRows(false);
    this->updateStartButton();
}

void CreatePredictionDialog::showEvent(QShowEvent *event)
{
    this->title_->setFocus(Qt::ActiveWindowFocusReason);
    BasePopup::showEvent(event);
}

void CreatePredictionDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->applyTheme();
    this->updateOutcomeRows(false);
}

void CreatePredictionDialog::applyTheme()
{
    const auto *theme = getTheme();
    if (theme == nullptr)
    {
        return;
    }

    const auto background = theme->isLightTheme() ? QColor("#f7f7f8")
                                                  : QColor("#18181b");
    const auto field = theme->isLightTheme() ? QColor("#ffffff")
                                             : QColor("#18181b");
    const auto disabledField = theme->isLightTheme() ? QColor("#eeeeF1")
                                                     : QColor("#2a2a2f");
    const auto text = theme->isLightTheme() ? QColor("#1f1f23")
                                            : QColor("#efeff1");
    const auto mutedText = withAlpha(text, 190);
    const auto disabledText = withAlpha(text, 135);
    const auto border = theme->isLightTheme() ? QColor("#c9c9d0")
                                              : QColor("#d5d5dd");
    const auto subtleBorder = theme->isLightTheme() ? QColor("#dedee3")
                                                    : QColor("#24242a");
    const auto selection = theme->isLightTheme() ? QColor("#dedee5")
                                                 : QColor("#3a3a40");
    const auto comboSeparator = theme->isLightTheme() ? QColor("#c3c3ca")
                                                      : QColor("#56565f");
    const auto comboArrow = theme->isLightTheme() ? QColor("#4f4f56")
                                                  : QColor("#9a9aa3");
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
    static_cast<PredictionDurationComboBox *>(this->duration_)
        ->setChromeColors(comboSeparator, comboArrow);

    this->getLayoutContainer()->setStyleSheet(QStringLiteral(R"(
QWidget#CreatePredictionRoot {
    background: %1;
    color: %4;
}
QLabel {
    color: %4;
}
QLabel#PredictionSectionLabel {
    font-weight: 700;
}
QLabel#PredictionTitleCount {
    color: %5;
}
QLineEdit {
    background: %2;
    color: %4;
    border: 1px solid %7;
    border-radius: 6px;
    padding: 4px 10px;
    min-height: 25px;
    selection-background-color: %9;
}
QComboBox {
    background: %2;
    color: %4;
    border: 1px solid %6;
    border-radius: 4px;
    padding: 3px 12px 3px 8px;
    min-height: 25px;
    selection-background-color: %9;
}
QComboBox::drop-down {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 13px;
    border: none;
    background: transparent;
}
QComboBox::down-arrow {
    image: none;
    width: 0px;
    height: 0px;
}
QLineEdit:disabled {
    background: %3;
    color: %6;
    border-color: %8;
}
QComboBox QAbstractItemView {
    background: %2;
    color: %4;
    border: 1px solid %8;
    outline: none;
    selection-background-color: %9;
    selection-color: %4;
}
QComboBox QAbstractItemView::item {
    min-height: 24px;
    padding: 3px 6px;
}
QComboBox QAbstractItemView::item:selected {
    background: %9;
    color: %4;
}
QPushButton#PredictionRemoveButton {
    background: transparent;
    color: %4;
    border: none;
    font-family: "Segoe UI";
    font-weight: 400;
    font-size: 22px;
    padding: 2px 2px 1px 0px;
}
QPushButton#PredictionRemoveButton:disabled {
    color: %6;
}
QPushButton#PredictionRemoveButton:enabled:hover {
    background: transparent;
    color: %4;
}
QPushButton#PredictionCancelButton {
    background: %3;
    color: %4;
    border: none;
    border-radius: 4px;
    padding: 7px 16px;
    font-weight: 700;
}
QPushButton#PredictionCancelButton:hover {
    background: %8;
}
QPushButton#PredictionStartButton {
    background: %10;
    color: %4;
    border: none;
    border-radius: 4px;
    padding: 7px 18px;
    font-weight: 700;
}
QPushButton#PredictionStartButton:hover {
    background: %11;
}
QPushButton#PredictionStartButton:disabled {
    background: %3;
    color: %6;
}
QLabel#PredictionErrorLabel {
    color: #ff6b6b;
}
)")
                                                 .arg(rgba(background),
                                                      rgba(field),
                                                      rgba(disabledField),
                                                      rgba(text),
                                                      rgba(mutedText),
                                                      rgba(disabledText),
                                                      rgba(border),
                                                      rgba(subtleBorder),
                                                      rgba(selection),
                                                      rgba(neutralButton),
                                                      rgba(neutralButtonHover)));
}

void CreatePredictionDialog::updateTitleCount()
{
    this->titleCount_->setText(
        QStringLiteral("%1/%2")
            .arg(this->title_->text().size())
            .arg(MAX_PREDICTION_TITLE_LENGTH));
}

void CreatePredictionDialog::updateOutcomeRows(bool animated)
{
    int outcomeCount = 0;
    int lastFilled = -1;
    for (size_t i = 0; i < this->outcomeRows_.size(); ++i)
    {
        if (!this->outcomeRows_[i].input->text().trimmed().isEmpty())
        {
            ++outcomeCount;
            lastFilled = static_cast<int>(i);
        }
    }

    const auto firstTwoComplete =
        !this->outcomeRows_[0].input->text().trimmed().isEmpty() &&
        !this->outcomeRows_[1].input->text().trimmed().isEmpty();

    auto visibleCount =
        this->kick_ ? MIN_PREDICTION_OUTCOMES : MIN_PREDICTION_OUTCOMES + 1;
    if (!this->kick_ && firstTwoComplete)
    {
        visibleCount = std::max(visibleCount, lastFilled + 2);
    }
    visibleCount = std::clamp(
        visibleCount, 0,
        this->kick_ ? MIN_PREDICTION_OUTCOMES : MAX_PREDICTION_OUTCOMES);

    this->updateDialogHeight(visibleCount, animated);

    for (size_t i = 0; i < this->outcomeRows_.size(); ++i)
    {
        auto &row = this->outcomeRows_[i];
        const auto shouldShow = static_cast<int>(i) < visibleCount;
        const auto unlocked = i < MIN_PREDICTION_OUTCOMES || firstTwoComplete;
        const auto hasText = !row.input->text().trimmed().isEmpty();
        const auto removable = shouldShow && hasText && outcomeCount > 2 &&
                               !this->submitting_;

        this->updateOutcomeRowVisibility(row, shouldShow, animated);
        row.input->setEnabled(shouldShow && unlocked && !this->submitting_);
        this->updateRemoveButton(row, removable, animated && shouldShow);
        row.removeButton->setEnabled(removable);
        row.removeButton->setCursor(removable ? Qt::PointingHandCursor
                                              : Qt::ArrowCursor);

        if (shouldShow && (i < MIN_PREDICTION_OUTCOMES || hasText))
        {
            const auto badge = loadBadgePixmap(i, outcomeCount, this->kick_);
            row.badge->setPixmap(
                badge.scaled(BADGE_SIZE, BADGE_SIZE, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation));
        }
        else
        {
            row.badge->clear();
        }
    }

    this->adjustSize();
}

void CreatePredictionDialog::updateOutcomeRowVisibility(OutcomeRow &row,
                                                        bool visible,
                                                        bool animated)
{
    (void)animated;

    if (row.container == nullptr)
    {
        return;
    }

    const auto targetHeight =
        visible ? OUTCOME_INPUT_HEIGHT + row.topGap : 0;
    row.visible = visible;
    row.container->setMinimumHeight(targetHeight);
    row.container->setMaximumHeight(targetHeight);
    row.container->setVisible(visible);
}

void CreatePredictionDialog::updateRemoveButton(OutcomeRow &row, bool visible,
                                                bool animated)
{
    if (row.removeContainer == nullptr || row.removeButton == nullptr ||
        row.removeWidthAnimation == nullptr ||
        row.removeOpacityAnimation == nullptr ||
        row.removeOpacityEffect == nullptr)
    {
        return;
    }

    const auto targetWidth = visible ? REMOVE_COLUMN_WIDTH : 0;
    const auto targetOpacity = visible ? 1.0 : 0.0;

    if (!animated)
    {
        row.removeWidthAnimation->stop();
        row.removeOpacityAnimation->stop();
        row.removeContainer->setMinimumWidth(targetWidth);
        row.removeContainer->setMaximumWidth(targetWidth);
        row.removeOpacityEffect->setOpacity(targetOpacity);
        row.removeVisible = visible;
        return;
    }

    if (row.removeVisible == visible &&
        row.removeContainer->maximumWidth() == targetWidth)
    {
        return;
    }

    row.removeWidthAnimation->stop();
    row.removeOpacityAnimation->stop();

    row.removeContainer->setMinimumWidth(0);
    row.removeWidthAnimation->setStartValue(
        row.removeContainer->maximumWidth());
    row.removeWidthAnimation->setEndValue(targetWidth);
    row.removeWidthAnimation->setDuration(REMOVE_ANIMATION_DURATION_MS);
    row.removeWidthAnimation->setEasingCurve(QEasingCurve::OutCubic);

    row.removeOpacityAnimation->setStartValue(
        row.removeOpacityEffect->opacity());
    row.removeOpacityAnimation->setEndValue(targetOpacity);
    row.removeOpacityAnimation->setDuration(REMOVE_ANIMATION_DURATION_MS);
    row.removeOpacityAnimation->setEasingCurve(QEasingCurve::OutCubic);

    row.removeVisible = visible;
    row.removeWidthAnimation->start();
    row.removeOpacityAnimation->start();
}

int CreatePredictionDialog::visibleOutcomeCount() const
{
    int visibleCount = 0;
    for (const auto &row : this->outcomeRows_)
    {
        if (row.visible)
        {
            ++visibleCount;
        }
    }

    return visibleCount;
}

int CreatePredictionDialog::dialogHeightForVisibleOutcomes(
    int visibleCount) const
{
    const auto extraRows =
        std::max(0, visibleCount - BASE_VISIBLE_OUTCOMES);
    return BASE_DIALOG_HEIGHT -
           (this->kick_ ? OUTCOME_INPUT_HEIGHT + OUTCOME_ROW_GAP : 0) +
           extraRows * (OUTCOME_INPUT_HEIGHT + OUTCOME_ROW_GAP) +
           this->errorLabelHeight();
}

int CreatePredictionDialog::errorLabelHeight() const
{
    if (this->errorLabel_ == nullptr || !this->errorLabel_->isVisible())
    {
        return 0;
    }

    const auto labelWidth = DIALOG_WIDTH - 32;
    const auto wrappedHeight = this->errorLabel_->heightForWidth(labelWidth);
    const auto labelHeight =
        wrappedHeight > 0 ? wrappedHeight : this->errorLabel_->sizeHint().height();

    return labelHeight + 8;
}

void CreatePredictionDialog::updateDialogHeight(int visibleCount,
                                                bool animated)
{
    (void)animated;

    const auto targetHeight =
        this->dialogHeightForVisibleOutcomes(visibleCount);

    this->setScaleIndependentSize(DIALOG_WIDTH, targetHeight);
}

void CreatePredictionDialog::updateStartButton()
{
    const auto hasTitle = !this->title_->text().trimmed().isEmpty();
    const auto hasRequiredOutcomes =
        !this->outcomeRows_[0].input->text().trimmed().isEmpty() &&
        !this->outcomeRows_[1].input->text().trimmed().isEmpty();

    this->startButton_->setEnabled(!this->submitting_ && hasTitle &&
                                   hasRequiredOutcomes);
}

void CreatePredictionDialog::removeOutcome(size_t index)
{
    QStringList remaining;
    remaining.reserve(MAX_PREDICTION_OUTCOMES);
    for (size_t i = 0; i < this->outcomeRows_.size(); ++i)
    {
        const auto text = this->outcomeRows_[i].input->text().trimmed();
        if (i != index && !text.isEmpty())
        {
            remaining.push_back(text);
        }
    }

    for (size_t i = 0; i < this->outcomeRows_.size(); ++i)
    {
        QSignalBlocker blocker(this->outcomeRows_[i].input);
        this->outcomeRows_[i].input->setText(
            i < static_cast<size_t>(remaining.size()) ? remaining.at(i)
                                                      : QString{});
    }

    this->updateOutcomeRows();
    this->updateStartButton();
}

void CreatePredictionDialog::submit()
{
    if (this->submitting_)
    {
        return;
    }

    const auto title = this->title_->text().trimmed();
    const auto outcomeTitles = this->outcomes();
    if (title.isEmpty())
    {
        this->showError(QStringLiteral("Prediction name is required."));
        return;
    }
    if (outcomeTitles.size() < MIN_PREDICTION_OUTCOMES)
    {
        this->showError(
            QStringLiteral("At least two outcomes are required."));
        return;
    }
    if (outcomeTitles.at(0).compare(outcomeTitles.at(1),
                                    Qt::CaseInsensitive) == 0)
    {
        this->showError(QStringLiteral(
            "The two prediction outcomes must have different names."));
        return;
    }

    this->clearError();
    this->submitting_ = true;
    this->updateOutcomeRows(false);
    this->updateStartButton();

    const auto durationSeconds = this->duration_->currentData().toInt();
    QPointer<CreatePredictionDialog> self(this);
    const auto scheduleFailure = [self](const QString &message) {
        if (self == nullptr)
        {
            return;
        }

        QTimer::singleShot(0, self.data(), [self, message] {
            if (self != nullptr)
            {
                self->finishSubmitFailure(message);
            }
        });
    };

    if (this->kick_)
    {
        auto kickAccount = getApp()->getAccounts()->kick.current();
        if (kickAccount == nullptr || kickAccount->isAnonymous())
        {
            scheduleFailure(QStringLiteral(
                "You must be logged in to Kick to create a prediction."));
            return;
        }
        const auto websiteToken = kickAccount->chatIdentityToken().trimmed();
        if (websiteToken.isEmpty())
        {
            scheduleFailure(QStringLiteral(
                "Connect Kick’s website session under Settings > Accounts "
                "before creating predictions."));
            return;
        }

        const auto kickChannel = resolveKickChannel(this->channel_);
        const bool kickBroadcaster =
            kickChannel != nullptr && kickChannel->isBroadcaster();
        const std::weak_ptr<KickChannel> kickChannelWeak = kickChannel;
        auto requestSlug = this->kickChannelSlug_.trimmed();
        if (kickChannel != nullptr)
        {
            const auto liveSlug = kickChannel->slug().trimmed();
            requestSlug = liveSlug.isEmpty()
                              ? kickChannel->getName().trimmed()
                              : liveSlug;
        }
        getKickApi()->createPrediction(
            requestSlug, websiteToken, title, outcomeTitles,
            durationSeconds,
            [channel = this->channel_, title, outcomeTitles, durationSeconds,
             self, kickBroadcaster,
             kickChannelWeak](ExpectedStr<void> result) {
                if (self == nullptr)
                {
                    return;
                }
                QTimer::singleShot(
                    0, self.data(),
                    [channel, title, outcomeTitles, durationSeconds, self,
                     kickBroadcaster, kickChannelWeak,
                     result = std::move(result)] {
                        if (self == nullptr)
                        {
                            return;
                        }
                        if (!result)
                        {
                            if (kickPredictionAlreadyExistsError(
                                    result.error()))
                            {
                                if (auto kickChannel = kickChannelWeak.lock();
                                    kickChannel != nullptr &&
                                    kickChannel->activePrediction())
                                {
                                    self->close();
                                    KickPredictionDialog::showDialog(
                                        kickChannel, channel);
                                    return;
                                }
                            }

                            self->finishSubmitFailure(kickPredictionErrorText(
                                result.error(), kickBroadcaster));
                            return;
                        }
                        if (auto kickChannel = kickChannelWeak.lock();
                            kickChannel != nullptr &&
                            (!kickChannel->activePrediction() ||
                             kickChannel->activePrediction()->title != title))
                        {
                            KickPrediction prediction{
                                .title = title,
                                .state = QStringLiteral("ACTIVE"),
                                .durationSeconds = durationSeconds,
                                .createdAt = QDateTime::currentDateTimeUtc(),
                                .updatedAt = QDateTime::currentDateTimeUtc(),
                            };
                            prediction.outcomes.reserve(outcomeTitles.size());
                            for (const auto &outcomeTitle : outcomeTitles)
                            {
                                prediction.outcomes.push_back(
                                    {.title = outcomeTitle});
                            }
                            kickChannel->updatePrediction(std::move(prediction));
                        }
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                QStringLiteral("Created Kick prediction: '%1'")
                                    .arg(title));
                        }
                        self->close();
                    });
            });
        return;
    }

    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser == nullptr || currentUser->isAnon() ||
        currentUser->getOAuthToken().isEmpty())
    {
        scheduleFailure(QStringLiteral(
            "You must be logged in to create a prediction."));
        return;
    }

    std::function<void()> successCallback = [channel = this->channel_, title,
                                             self] {
        if (self == nullptr)
        {
            return;
        }

        QTimer::singleShot(0, self.data(), [channel, title, self] {
            if (self == nullptr)
            {
                return;
            }

            if (channel != nullptr)
            {
                channel->addSystemMessage(QStringLiteral(
                    "Created Twitch prediction: '%1'").arg(title));
                notifyPollsAndPredictionsChanged(channel);
            }
            self->close();
        });
    };
    std::function<void(const QString &)> failureCallback =
        [scheduleFailure](const QString &error) {
        scheduleFailure("Failed to create prediction - " + error);
    };

    if (currentUser->getUserId() == this->broadcasterID_)
    {
        getHelix()->createPrediction(this->broadcasterID_, title, outcomeTitles,
                                     std::chrono::seconds(durationSeconds),
                                     std::move(successCallback),
                                     std::move(failureCallback));
        return;
    }

    QString authError;
    const auto moderationAccount =
        TwitchModerationAuth::resolveForCurrentUser(currentUser->getUserId(),
                                                    &authError);
    if (!moderationAccount.isValid())
    {
        scheduleFailure("Failed to create prediction - " + authError);
        return;
    }

    TwitchWebApi::startPrediction(
        this->broadcasterID_, title, outcomeTitles, durationSeconds,
        moderationAccount.clientId, moderationAccount.oauthToken,
        [successCallback = std::move(successCallback),
         broadcasterID = this->broadcasterID_, title, outcomeTitles,
         durationSeconds](const QString &predictionID,
                          const QJsonObject &predictionObject) mutable {
            TwitchPollsAndPredictionsBar::rememberLocalPrediction(
                broadcasterID, title, outcomeTitles, durationSeconds,
                predictionID, predictionObject);
            successCallback();
        },
        std::move(failureCallback));
}

void CreatePredictionDialog::finishSubmitFailure(const QString &message)
{
    this->submitting_ = false;
    this->updateOutcomeRows(false);
    this->updateStartButton();
    this->showError(message);
}

void CreatePredictionDialog::showError(const QString &message)
{
    this->errorLabel_->setText(message);
    this->errorLabel_->show();
    this->errorLabel_->updateGeometry();
    this->updateDialogHeight(this->visibleOutcomeCount(), false);
}

void CreatePredictionDialog::clearError()
{
    this->errorLabel_->clear();
    this->errorLabel_->hide();
    this->updateDialogHeight(this->visibleOutcomeCount(), false);
}

QStringList CreatePredictionDialog::outcomes() const
{
    QStringList result;
    for (const auto &row : this->outcomeRows_)
    {
        const auto text = row.input->text().trimmed();
        if (!text.isEmpty())
        {
            result.push_back(text);
        }
    }

    return result;
}

}  // namespace chatterino
