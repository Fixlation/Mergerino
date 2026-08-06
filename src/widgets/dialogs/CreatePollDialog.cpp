// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/CreatePollDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
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
#include "widgets/Window.hpp"
#include "widgets/splits/TwitchPollsAndPredictionsBar.hpp"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCursor>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPalette>
#include <QPointer>
#include <QPolygonF>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QShowEvent>
#include <QSpinBox>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTimer>
#include <QUrl>
#include <QVariantAnimation>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <utility>

namespace chatterino {
namespace {

constexpr int MAX_POLL_TITLE_LENGTH = 60;
constexpr int MAX_POLL_RESPONSE_LENGTH = 25;
constexpr int MIN_POLL_RESPONSES = 2;
constexpr int MAX_POLL_RESPONSES = 5;
constexpr int DIALOG_WIDTH = 650;
constexpr int BASE_DIALOG_HEIGHT = 430;
constexpr int BASE_VISIBLE_RESPONSES = MIN_POLL_RESPONSES + 1;
constexpr int RESPONSE_INPUT_HEIGHT = 36;
constexpr int RESPONSE_ROW_GAP = 6;
constexpr int REMOVE_COLUMN_WIDTH = 30;
constexpr int REMOVE_BUTTON_SIZE = 24;
constexpr int REMOVE_ANIMATION_DURATION_MS = 120;
constexpr int POINTS_VALUE_WIDTH = 90;
constexpr int POINTS_ICON_SIZE = 20;
constexpr int POINTS_EXPAND_HEIGHT_DURATION_MS = 260;
constexpr int POINTS_EXPAND_OPACITY_DURATION_MS = 220;
constexpr int POINTS_COLLAPSE_HEIGHT_DURATION_MS =
    POINTS_EXPAND_HEIGHT_DURATION_MS * 105 / 100;
constexpr int POINTS_COLLAPSE_OPACITY_DURATION_MS =
    POINTS_EXPAND_OPACITY_DURATION_MS * 105 / 100;

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

class PollDurationComboBox final : public QComboBox
{
public:
    explicit PollDurationComboBox(QWidget *parent = nullptr)
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
        this->resetPopupView();
        QComboBox::showPopup();
        this->resetPopupView();
    }

    void hidePopup() override
    {
        QComboBox::hidePopup();
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

class PollPointsSpinBox final : public QSpinBox
{
public:
    explicit PollPointsSpinBox(QWidget *parent = nullptr)
        : QSpinBox(parent)
    {
        this->lineEdit()->setMaxLength(6);
    }

    void setChromeColors(QColor separator, QColor arrow, QColor disabledArrow)
    {
        this->separatorColor_ = separator;
        this->arrowColor_ = arrow;
        this->disabledArrowColor_ = disabledArrow;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QSpinBox::paintEvent(event);

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        constexpr int buttonWidth = 18;
        const auto separatorX = this->width() - buttonWidth;
        const auto middleY = this->height() / 2;
        painter.setPen(QPen(this->separatorColor_, 1));
        painter.drawLine(separatorX, 1, separatorX, this->height() - 2);
        painter.drawLine(separatorX + 1, middleY, this->width() - 2, middleY);

        const auto arrowColor =
            this->isEnabled() ? this->arrowColor_ : this->disabledArrowColor_;
        painter.setPen(Qt::NoPen);
        painter.setBrush(arrowColor);

        const auto centerX = separatorX + buttonWidth / 2;
        const auto upperCenterY = this->height() / 4 + 1;
        QPolygonF upArrow;
        upArrow << QPointF(centerX - 3, upperCenterY + 2)
                << QPointF(centerX + 3, upperCenterY + 2)
                << QPointF(centerX, upperCenterY - 2);
        painter.drawPolygon(upArrow);

        const auto lowerCenterY = this->height() * 3 / 4;
        QPolygonF downArrow;
        downArrow << QPointF(centerX - 3, lowerCenterY - 2)
                  << QPointF(centerX + 3, lowerCenterY - 2)
                  << QPointF(centerX, lowerCenterY + 2);
        painter.drawPolygon(downArrow);
    }

private:
    QColor separatorColor_{QColor("#55555d")};
    QColor arrowColor_{QColor("#a0a0a8")};
    QColor disabledArrowColor_{QColor("#77777f")};
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

void drawChannelPointsIcon(QPainter &painter, const QRect &rect,
                           const QColor &color)
{
    QPainterPath ring;
    ring.setFillRule(Qt::OddEvenFill);
    ring.addEllipse(QRectF{1, 1, 22, 22});
    ring.addEllipse(QRectF{3, 3, 18, 18});

    QPainterPath innerArc;
    innerArc.moveTo(12, 5);
    innerArc.lineTo(12, 7);
    innerArc.arcTo(QRectF{7, 7, 10, 10}, 90, -90);
    innerArc.lineTo(19, 12);
    innerArc.arcTo(QRectF{5, 5, 14, 14}, 0, 90);
    innerArc.closeSubpath();

    painter.save();
    painter.translate(rect.x(), rect.y());
    painter.scale(rect.width() / 24.0, rect.height() / 24.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(ring);
    painter.drawPath(innerArc);
    painter.restore();
}

QPixmap fallbackChannelPointsIcon(const QColor &color)
{
    QPixmap pixmap(POINTS_ICON_SIZE, POINTS_ICON_SIZE);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    drawChannelPointsIcon(painter, pixmap.rect(), color);

    return pixmap;
}

QPixmap tintedPixmap(const QPixmap &source, const QColor &color)
{
    if (source.isNull())
    {
        return {};
    }

    QPixmap tinted(source.size());
    tinted.fill(Qt::transparent);

    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);

    return tinted;
}

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("PollSectionLabel"));
    return label;
}

QString bestImageUrl(const QJsonObject &image)
{
    const auto exactUrl = image.value(QStringLiteral("url")).toString();
    if (!exactUrl.isEmpty())
    {
        return exactUrl;
    }

    const auto url4x = image.value(QStringLiteral("url_4x")).toString();
    if (!url4x.isEmpty())
    {
        return url4x;
    }

    const auto url2x = image.value(QStringLiteral("url_2x")).toString();
    if (!url2x.isEmpty())
    {
        return url2x;
    }

    return image.value(QStringLiteral("url_1x")).toString();
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

}  // namespace

void CreatePollDialog::showDialog(ChannelPtr channel,
                                  const TwitchChannel &twitchChannel)
{
    auto *dialog = new CreatePollDialog(
        std::move(channel), twitchChannel.roomId(), twitchChannel.getName(),
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())));
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

void CreatePollDialog::showKickDialog(ChannelPtr channel,
                                      const KickChannel &kickChannel)
{
    auto *dialog = new CreatePollDialog(
        std::move(channel), QString{},
        kickChannel.slug().trimmed().isEmpty() ? kickChannel.getName()
                                              : kickChannel.slug(),
        static_cast<QWidget *>(&(getApp()->getWindows()->getMainWindow())),
        true);
    dialog->setAttribute(Qt::WA_DeleteOnClose, true);
    dialog->show();
    dialog->activateWindow();
    dialog->raise();
}

CreatePollDialog::CreatePollDialog(ChannelPtr channel, QString broadcasterID,
                                   QString channelLogin, QWidget *parent,
                                   bool kick)
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
    , channelLogin_(std::move(channelLogin))
    , kick_(kick)
{
    this->setWindowTitle(this->kick_ ? QStringLiteral("Create a Kick Poll")
                                     : QStringLiteral("Create a New Poll"));
    this->setScaleIndependentSize(DIALOG_WIDTH, BASE_DIALOG_HEIGHT);
    this->setAutoFillBackground(true);
    this->getLayoutContainer()->setObjectName(QStringLiteral("CreatePollRoot"));
    this->getLayoutContainer()->setAutoFillBackground(true);

    auto *root = new QVBoxLayout(this->getLayoutContainer());
    root->setContentsMargins(18, 16, 18, 14);
    root->setSpacing(9);
    root->setAlignment(Qt::AlignTop);

    root->addWidget(sectionLabel(QStringLiteral("Question"), this));
    this->question_ = new QLineEdit(this);
    this->question_->setMaxLength(MAX_POLL_TITLE_LENGTH);
    root->addWidget(this->question_);

    root->addWidget(sectionLabel(QStringLiteral("Responses (Minimum 2)"),
                                 this));
    this->responsesLayout_ = new QVBoxLayout;
    this->responsesLayout_->setContentsMargins(0, 0, 0, 0);
    this->responsesLayout_->setSpacing(0);
    root->addLayout(this->responsesLayout_);

    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        auto &row = this->responseRows_[i];
        row.container = new QWidget(this);
        row.container->setObjectName(QStringLiteral("PollResponseRow"));
        row.container->setSizePolicy(QSizePolicy::Expanding,
                                     QSizePolicy::Fixed);
        row.container->setMinimumHeight(0);
        row.container->setMaximumHeight(0);
        row.topGap = i == 0 ? 0 : RESPONSE_ROW_GAP;

        auto *rowLayout = new QHBoxLayout(row.container);
        rowLayout->setContentsMargins(0, row.topGap, 6, 0);
        rowLayout->setSpacing(0);

        row.input = new QLineEdit(row.container);
        row.input->setFixedHeight(RESPONSE_INPUT_HEIGHT);
        row.input->setMaxLength(MAX_POLL_RESPONSE_LENGTH);
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
        row.removeButton->setObjectName(QStringLiteral("PollRemoveButton"));
        row.removeButton->setFixedSize(REMOVE_BUTTON_SIZE, REMOVE_BUTTON_SIZE);
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

        this->responsesLayout_->addWidget(row.container);

        QObject::connect(row.input, &QLineEdit::textChanged, this, [this] {
            this->updateResponseRows();
            this->updateStartButton();
        });
        QObject::connect(row.removeButton, &QPushButton::clicked, this,
                         [this, i] {
                             this->removeResponse(i);
                         });
    }

    auto *settingsLabel = sectionLabel(QStringLiteral("Settings"), this);
    settingsLabel->setVisible(!this->kick_);
    root->addWidget(settingsLabel);

    auto *settingsContainer = new QWidget(this);
    auto *settingsLayout = new QVBoxLayout(settingsContainer);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(0);

    this->allowAdditionalVotes_ =
        new QCheckBox(QStringLiteral("Allow Additional Votes"),
                      settingsContainer);
    this->allowAdditionalVotes_->setMouseTracking(true);
    this->allowAdditionalVotes_->installEventFilter(this);
    settingsLayout->addWidget(this->allowAdditionalVotes_);

    this->pointsContainer_ = new QWidget(settingsContainer);
    this->pointsContainer_->setObjectName(QStringLiteral("PollPointsContainer"));
    this->pointsContainer_->setSizePolicy(QSizePolicy::Expanding,
                                          QSizePolicy::Fixed);
    auto *pointsRow = new QHBoxLayout(this->pointsContainer_);
    pointsRow->setContentsMargins(18, 8, 0, 0);
    pointsRow->setSpacing(8);

    this->pointsPerVote_ = new PollPointsSpinBox(this);
    this->pointsPerVote_->setRange(1, 999999);
    this->pointsPerVote_->setValue(1);
    this->pointsPerVote_->setEnabled(false);
    this->pointsPerVote_->setFixedWidth(POINTS_VALUE_WIDTH);

    this->channelPointsIcon_ = new QLabel(this);
    this->channelPointsIcon_->setFixedSize(POINTS_ICON_SIZE, POINTS_ICON_SIZE);
    this->channelPointsIcon_->setScaledContents(true);

    this->channelPointsLabel_ =
        new QLabel(QStringLiteral("Channel Points per Vote"), this);

    pointsRow->addWidget(this->pointsPerVote_, 0);
    pointsRow->addWidget(this->channelPointsIcon_, 0, Qt::AlignVCenter);
    pointsRow->addWidget(this->channelPointsLabel_, 0, Qt::AlignVCenter);
    pointsRow->addStretch(1);
    settingsLayout->addWidget(this->pointsContainer_);
    settingsContainer->setVisible(!this->kick_);
    root->addWidget(settingsContainer);

    this->pointsOpacityEffect_ =
        new QGraphicsOpacityEffect(this->pointsContainer_);
    this->pointsOpacityEffect_->setOpacity(0.0);
    this->pointsContainer_->setGraphicsEffect(this->pointsOpacityEffect_);
    this->pointsHeightAnimation_ =
        new QPropertyAnimation(this->pointsContainer_, "maximumHeight", this);
    this->pointsHeightAnimation_->setDuration(POINTS_EXPAND_HEIGHT_DURATION_MS);
    this->pointsHeightAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    this->pointsOpacityAnimation_ =
        new QPropertyAnimation(this->pointsOpacityEffect_, "opacity", this);
    this->pointsOpacityAnimation_->setDuration(
        POINTS_EXPAND_OPACITY_DURATION_MS);
    this->pointsOpacityAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(this->pointsHeightAnimation_, &QPropertyAnimation::finished,
                     this, [this] {
                         if (this->allowAdditionalVotes_->isChecked())
                         {
                             this->pointsContainer_->setMaximumHeight(
                                 this->pointsContainer_->sizeHint().height());
                         }
                     });
    this->dialogHeightAnimation_ = new QVariantAnimation(this);
    this->dialogHeightAnimation_->setDuration(
        POINTS_EXPAND_HEIGHT_DURATION_MS);
    this->dialogHeightAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(this->dialogHeightAnimation_, &QVariantAnimation::valueChanged,
                     this, [this](const QVariant &value) {
                         const auto height = value.toInt();
                         this->setMinimumHeight(height);
                         this->setMaximumHeight(height);
                     });
    QObject::connect(this->dialogHeightAnimation_, &QVariantAnimation::finished,
                     this, [this] {
                         const auto finalHeight =
                             this->dialogHeightAnimation_->endValue().toInt();
                         this->setScaleIndependentSize(
                             DIALOG_WIDTH,
                             qRound(finalHeight / this->scale()));
                     });

    root->addWidget(sectionLabel(QStringLiteral("Duration"), this));
    this->duration_ = new PollDurationComboBox(this);
    this->duration_->setCursor(Qt::PointingHandCursor);
    this->duration_->setFixedWidth(58);
    if (this->kick_)
    {
        this->duration_->addItem(QStringLiteral("30s"), 30);
        this->duration_->addItem(QStringLiteral("2m"), 120);
        this->duration_->addItem(QStringLiteral("3m"), 180);
        this->duration_->addItem(QStringLiteral("4m"), 240);
        this->duration_->addItem(QStringLiteral("5m"), 300);
    }
    else
    {
        this->duration_->addItem(QStringLiteral("1m"), 60);
        this->duration_->addItem(QStringLiteral("2m"), 120);
        this->duration_->addItem(QStringLiteral("3m"), 180);
        this->duration_->addItem(QStringLiteral("5m"), 300);
        this->duration_->addItem(QStringLiteral("10m"), 600);
    }
    this->duration_->view()->setCursor(Qt::ArrowCursor);
    this->duration_->view()->viewport()->setCursor(Qt::ArrowCursor);
    root->addWidget(this->duration_, 0, Qt::AlignLeft);

    this->errorLabel_ = new QLabel(this);
    this->errorLabel_->setObjectName(QStringLiteral("PollErrorLabel"));
    this->errorLabel_->setWordWrap(true);
    this->errorLabel_->hide();
    root->addWidget(this->errorLabel_);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 2, 0, 0);
    buttonRow->setSpacing(8);

    auto *cancelButton = new QPushButton(QStringLiteral("Cancel"), this);
    cancelButton->setObjectName(QStringLiteral("PollCancelButton"));
    cancelButton->setCursor(Qt::PointingHandCursor);
    this->startButton_ = new QPushButton(QStringLiteral("Start Poll"), this);
    this->startButton_->setObjectName(QStringLiteral("PollStartButton"));
    this->startButton_->setCursor(Qt::PointingHandCursor);

    buttonRow->addStretch(1);
    buttonRow->addWidget(cancelButton);
    buttonRow->addWidget(this->startButton_);
    root->addLayout(buttonRow);

    QObject::connect(cancelButton, &QPushButton::clicked, this, [this] {
        this->close();
    });
    QObject::connect(this->startButton_, &QPushButton::clicked, this,
                     [this] { this->submit(); });
    QObject::connect(this->question_, &QLineEdit::textChanged, this, [this] {
        this->updateStartButton();
    });
    QObject::connect(this->allowAdditionalVotes_, &QCheckBox::toggled, this,
                     [this] {
                         this->updatePointsControls();
                         this->updateStartButton();
                     });
    QObject::connect(this->pointsPerVote_,
                     QOverload<int>::of(&QSpinBox::valueChanged), this,
                     [this] { this->updateStartButton(); });

    this->setChannelPointsName(QStringLiteral("Channel Points"));
    this->updatePointsControls(false);
    this->updateResponseRows(false);
    this->updateStartButton();
    this->themeChangedEvent();
    if (!this->kick_)
    {
        this->loadChannelPointsMetadata();
    }
}

bool CreatePollDialog::eventFilter(QObject *object, QEvent *event)
{
    if (object == this->allowAdditionalVotes_)
    {
        switch (event->type())
        {
            case QEvent::Enter:
                this->updateAdditionalVotesCursor(
                    this->allowAdditionalVotes_->mapFromGlobal(
                        QCursor::pos()));
                break;

            case QEvent::MouseMove:
                this->updateAdditionalVotesCursor(
                    static_cast<QMouseEvent *>(event)->pos());
                break;

            case QEvent::Leave:
                this->allowAdditionalVotes_->unsetCursor();
                break;

            default:
                break;
        }
    }

    return BasePopup::eventFilter(object, event);
}

void CreatePollDialog::updateAdditionalVotesCursor(const QPoint &position)
{
    if (this->allowAdditionalVotes_ == nullptr)
    {
        return;
    }

    QStyleOptionButton option;
    option.initFrom(this->allowAdditionalVotes_);
    option.text = this->allowAdditionalVotes_->text();
    option.state.setFlag(QStyle::State_On,
                         this->allowAdditionalVotes_->isChecked());
    option.state.setFlag(QStyle::State_Off,
                         !this->allowAdditionalVotes_->isChecked());

    const auto indicatorRect = this->allowAdditionalVotes_->style()
                                   ->subElementRect(QStyle::SE_CheckBoxIndicator,
                                                    &option,
                                                    this->allowAdditionalVotes_);

    this->allowAdditionalVotes_->setCursor(
        indicatorRect.contains(position) ? Qt::PointingHandCursor
                                         : Qt::ArrowCursor);
}

void CreatePollDialog::showEvent(QShowEvent *event)
{
    this->question_->setFocus(Qt::ActiveWindowFocusReason);
    BasePopup::showEvent(event);
}

void CreatePollDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->applyTheme();
    this->updateResponseRows(false);
}

void CreatePollDialog::applyTheme()
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
    const auto text = theme->isLightTheme() ? QColor("#1f1f23")
                                            : QColor("#efeff1");
    const auto border = theme->isLightTheme() ? QColor("#c9c9d0")
                                              : QColor("#d5d5dd");
    const auto subtleBorder = theme->isLightTheme() ? QColor("#dedee3")
                                                    : QColor("#24242a");
    const auto disabledField = theme->isLightTheme() ? QColor("#eeeeF1")
                                                     : QColor("#2a2a2f");
    const auto disabledText = withAlpha(text, 135);
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
    if (this->duration_ != nullptr)
    {
        static_cast<PollDurationComboBox *>(this->duration_)
            ->setChromeColors(comboSeparator, comboArrow);
    }
    if (this->pointsPerVote_ != nullptr)
    {
        static_cast<PollPointsSpinBox *>(this->pointsPerVote_)
            ->setChromeColors(comboSeparator, comboArrow, disabledText);
    }

    this->getLayoutContainer()->setStyleSheet(QStringLiteral(R"(
QWidget#CreatePollRoot {
    background: %1;
    color: %3;
}
QLabel {
    color: %3;
}
QLabel#PollSectionLabel {
    font-weight: 600;
}
QLineEdit, QComboBox, QSpinBox {
    background: %2;
    color: %3;
    border: 1px solid %4;
    border-radius: 6px;
    padding: 4px 8px;
    min-height: 25px;
    selection-background-color: %8;
}
QComboBox {
    background: %2;
    color: %3;
    border: 1px solid %4;
    border-radius: 4px;
    padding: 3px 12px 3px 8px;
    min-height: 25px;
    selection-background-color: %8;
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
QSpinBox {
    padding: 4px 22px 4px 8px;
}
QSpinBox::up-button {
    subcontrol-origin: border;
    subcontrol-position: top right;
    width: 18px;
    border: none;
    background: transparent;
}
QSpinBox::down-button {
    subcontrol-origin: border;
    subcontrol-position: bottom right;
    width: 18px;
    border: none;
    background: transparent;
}
QSpinBox::up-arrow, QSpinBox::down-arrow {
    image: none;
    width: 0px;
    height: 0px;
}
QComboBox QAbstractItemView {
    background: %2;
    color: %3;
    border: 1px solid %5;
    outline: none;
    selection-background-color: %8;
    selection-color: %3;
}
QComboBox QAbstractItemView::item {
    min-height: 24px;
    padding: 3px 6px;
}
QComboBox QAbstractItemView::item:selected {
    background: %8;
    color: %3;
}
QSpinBox:disabled {
    background: %6;
    color: %7;
    border-color: %5;
}
QCheckBox {
    color: %3;
    spacing: 8px;
}
QCheckBox::indicator {
    width: 16px;
    height: 16px;
}
QPushButton#PollRemoveButton {
    background: transparent;
    color: %3;
    border: none;
    font-family: "Segoe UI";
    font-weight: 400;
    font-size: 22px;
    padding: 2px 2px 1px 0px;
}
QPushButton#PollRemoveButton:disabled {
    color: %7;
}
QPushButton#PollRemoveButton:enabled:hover {
    background: transparent;
    color: %3;
}
QPushButton#PollCancelButton {
    background: %6;
    color: %3;
    border: none;
    border-radius: 4px;
    padding: 7px 16px;
    font-weight: 600;
}
QPushButton#PollCancelButton:hover {
    background: %5;
}
QPushButton#PollStartButton {
    background: %9;
    color: %3;
    border: none;
    border-radius: 4px;
    padding: 7px 18px;
    font-weight: 700;
}
QPushButton#PollStartButton:hover {
    background: %10;
}
QPushButton#PollStartButton:disabled {
    background: %6;
    color: %7;
}
QLabel#PollErrorLabel {
    color: #ff6b6b;
}
)")
                                                 .arg(rgba(background),
                                                      rgba(field),
                                                      rgba(text),
                                                      rgba(border),
                                                      rgba(subtleBorder),
                                                      rgba(disabledField),
                                                      rgba(disabledText),
                                                      rgba(selection),
                                                      rgba(neutralButton),
                                                      rgba(neutralButtonHover)));

    if (this->channelPointsIcon_ != nullptr &&
        this->channelPointsIcon_->pixmap(Qt::ReturnByValue).isNull())
    {
        this->setChannelPointsIcon(fallbackChannelPointsIcon(Qt::white));
    }
}

void CreatePollDialog::loadChannelPointsMetadata()
{
    auto currentUser = getApp()->getAccounts()->twitch.getCurrent();
    if (currentUser->isAnon() || this->channelLogin_.isEmpty())
    {
        return;
    }

    const QJsonObject payload{
        {QStringLiteral("operationName"),
         QStringLiteral("MergerinoChannelPointsMetadata")},
        {QStringLiteral("variables"),
         QJsonObject{{QStringLiteral("channelLogin"), this->channelLogin_}}},
        {QStringLiteral("query"),
         QStringLiteral(
             "query MergerinoChannelPointsMetadata($channelLogin: String!) { "
             "channel(name: $channelLogin) { communityPointsSettings { name "
             "image { url } defaultImage { url } } } }")},
    };

    NetworkRequest(QUrl(QStringLiteral("https://gql.twitch.tv/gql")),
                   NetworkRequestType::Post)
        .header("Client-ID", currentUser->getOAuthClient())
        .header("Authorization",
                QStringLiteral("OAuth ") + currentUser->getOAuthToken())
        .json(payload)
        .timeout(10000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            const auto root = result.parseJson();
            const auto settings =
                root.value(QStringLiteral("data"))
                    .toObject()
                    .value(QStringLiteral("channel"))
                    .toObject()
                    .value(QStringLiteral("communityPointsSettings"))
                    .toObject();

            const auto name = settings.value(QStringLiteral("name")).toString();
            if (!name.isEmpty())
            {
                this->setChannelPointsName(name);
            }

            auto imageUrl =
                bestImageUrl(settings.value(QStringLiteral("image")).toObject());
            if (imageUrl.isEmpty())
            {
                imageUrl = bestImageUrl(
                    settings.value(QStringLiteral("defaultImage")).toObject());
            }
            if (!imageUrl.isEmpty())
            {
                this->loadChannelPointsIcon(imageUrl);
            }
        })
        .onError([](const NetworkResult &) {})
        .execute();
}

void CreatePollDialog::loadChannelPointsIcon(const QString &url)
{
    NetworkRequest(QUrl(url), NetworkRequestType::Get)
        .timeout(10000)
        .caller(this)
        .onSuccess([this](const NetworkResult &result) {
            QPixmap pixmap;
            if (pixmap.loadFromData(result.getData()))
            {
                this->setChannelPointsIcon(pixmap);
            }
        })
        .onError([](const NetworkResult &) {})
        .execute();
}

void CreatePollDialog::setChannelPointsName(const QString &name)
{
    this->channelPointsLabel_->setText(
        QStringLiteral("%1 per Vote").arg(name.trimmed()));
}

void CreatePollDialog::setChannelPointsIcon(const QPixmap &pixmap)
{
    const auto scaled = pixmap.scaled(POINTS_ICON_SIZE, POINTS_ICON_SIZE,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation);
    this->channelPointsIcon_->setPixmap(tintedPixmap(scaled, Qt::white));
}

void CreatePollDialog::updatePointsControls(bool animated)
{
    const auto enabled = this->allowAdditionalVotes_->isChecked();
    this->pointsPerVote_->setEnabled(enabled);

    if (this->pointsContainer_ == nullptr ||
        this->pointsHeightAnimation_ == nullptr ||
        this->pointsOpacityAnimation_ == nullptr ||
        this->pointsOpacityEffect_ == nullptr)
    {
        return;
    }

    const auto targetHeight =
        enabled ? this->pointsContainer_->sizeHint().height() : 0;

    if (!animated)
    {
        this->pointsHeightAnimation_->stop();
        this->pointsOpacityAnimation_->stop();
        this->pointsContainer_->setMaximumHeight(targetHeight);
        this->pointsOpacityEffect_->setOpacity(enabled ? 1.0 : 0.0);
        this->pointsContainer_->setVisible(enabled);
        this->updateDialogHeight(this->visibleResponseCount(), false);
        return;
    }

    this->pointsHeightAnimation_->stop();
    this->pointsOpacityAnimation_->stop();

    if (enabled)
    {
        this->pointsContainer_->show();
    }

    this->pointsHeightAnimation_->setStartValue(
        this->pointsContainer_->maximumHeight());
    this->pointsHeightAnimation_->setEndValue(targetHeight);
    this->pointsHeightAnimation_->setDuration(
        enabled ? POINTS_EXPAND_HEIGHT_DURATION_MS
                : POINTS_COLLAPSE_HEIGHT_DURATION_MS);
    this->pointsHeightAnimation_->setEasingCurve(QEasingCurve::OutCubic);

    this->pointsOpacityAnimation_->setStartValue(
        this->pointsOpacityEffect_->opacity());
    this->pointsOpacityAnimation_->setEndValue(enabled ? 1.0 : 0.0);
    this->pointsOpacityAnimation_->setDuration(
        enabled ? POINTS_EXPAND_OPACITY_DURATION_MS
                : POINTS_COLLAPSE_OPACITY_DURATION_MS);
    this->pointsOpacityAnimation_->setEasingCurve(QEasingCurve::OutCubic);

    this->updateDialogHeight(this->visibleResponseCount(), true);
    this->pointsHeightAnimation_->start();
    this->pointsOpacityAnimation_->start();
}

void CreatePollDialog::updateResponseRows(bool animated)
{
    int responseCount = 0;
    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        if (!this->responseRows_[i].input->text().trimmed().isEmpty())
        {
            ++responseCount;
        }
    }

    const auto firstTwoComplete =
        !this->responseRows_[0].input->text().trimmed().isEmpty() &&
        !this->responseRows_[1].input->text().trimmed().isEmpty();
    const auto visibleCount = this->visibleResponseCount();
    this->updateDialogHeight(visibleCount, false);

    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        auto &row = this->responseRows_[i];
        const auto shouldShow = static_cast<int>(i) < visibleCount;
        const auto unlocked = i < MIN_POLL_RESPONSES || firstTwoComplete;
        const auto hasText = !row.input->text().trimmed().isEmpty();
        const auto removable = shouldShow && hasText &&
                               responseCount > MIN_POLL_RESPONSES &&
                               !this->submitting_;

        this->updateResponseRowVisibility(row, shouldShow, animated);
        row.input->setEnabled(shouldShow && unlocked && !this->submitting_);
        this->updateRemoveButton(row, removable, animated && shouldShow);
        row.removeButton->setEnabled(removable);
        row.removeButton->setCursor(removable ? Qt::PointingHandCursor
                                              : Qt::ArrowCursor);
    }

    this->adjustSize();
}

void CreatePollDialog::updateResponseRowVisibility(ResponseRow &row,
                                                   bool visible,
                                                   bool animated)
{
    (void)animated;

    if (row.container == nullptr)
    {
        return;
    }

    const auto targetHeight =
        visible ? RESPONSE_INPUT_HEIGHT + row.topGap : 0;
    row.visible = visible;
    row.container->setMinimumHeight(targetHeight);
    row.container->setMaximumHeight(targetHeight);
    row.container->setVisible(visible);
}

void CreatePollDialog::updateRemoveButton(ResponseRow &row, bool visible,
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

int CreatePollDialog::visibleResponseCount() const
{
    int lastFilled = -1;
    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        if (!this->responseRows_[i].input->text().trimmed().isEmpty())
        {
            lastFilled = static_cast<int>(i);
        }
    }

    const auto firstTwoComplete =
        !this->responseRows_[0].input->text().trimmed().isEmpty() &&
        !this->responseRows_[1].input->text().trimmed().isEmpty();

    auto visibleCount = MIN_POLL_RESPONSES + 1;
    if (firstTwoComplete)
    {
        visibleCount = std::max(visibleCount, lastFilled + 2);
    }

    return std::clamp(visibleCount, 0, MAX_POLL_RESPONSES);
}

int CreatePollDialog::dialogHeightForVisibleResponses(int visibleCount) const
{
    const auto extraRows =
        std::max(0, visibleCount - BASE_VISIBLE_RESPONSES);
    auto height = BASE_DIALOG_HEIGHT - (this->kick_ ? 58 : 0) +
                  extraRows * (RESPONSE_INPUT_HEIGHT + RESPONSE_ROW_GAP);

    if (this->allowAdditionalVotes_ != nullptr &&
        this->allowAdditionalVotes_->isChecked() &&
        this->pointsContainer_ != nullptr)
    {
        height += this->pointsContainer_->sizeHint().height();
    }

    height += this->errorLabelHeight();

    return height;
}

int CreatePollDialog::errorLabelHeight() const
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

void CreatePollDialog::updateDialogHeight(int visibleCount, bool animated)
{
    const auto targetHeight =
        this->dialogHeightForVisibleResponses(visibleCount);

    if (!animated || this->dialogHeightAnimation_ == nullptr)
    {
        if (this->dialogHeightAnimation_ != nullptr)
        {
            this->dialogHeightAnimation_->stop();
        }
        this->setScaleIndependentSize(DIALOG_WIDTH, targetHeight);
        return;
    }

    const auto targetPixelHeight = qRound(targetHeight * this->scale());
    if (this->height() == targetPixelHeight)
    {
        this->setScaleIndependentSize(DIALOG_WIDTH, targetHeight);
        return;
    }

    this->dialogHeightAnimation_->stop();
    this->dialogHeightAnimation_->setStartValue(this->height());
    this->dialogHeightAnimation_->setEndValue(targetPixelHeight);
    this->dialogHeightAnimation_->setDuration(
        targetPixelHeight > this->height() ? POINTS_EXPAND_HEIGHT_DURATION_MS
                                           : POINTS_COLLAPSE_HEIGHT_DURATION_MS);
    this->dialogHeightAnimation_->setEasingCurve(QEasingCurve::OutCubic);
    this->dialogHeightAnimation_->start();
}

void CreatePollDialog::updateStartButton()
{
    const auto hasQuestion = !this->question_->text().trimmed().isEmpty();
    const auto hasResponses = this->responses().size() >= 2;
    const auto validPoints = !this->allowAdditionalVotes_->isChecked() ||
                             this->pointsPerVote_->value() > 0;

    this->startButton_->setEnabled(!this->submitting_ && hasQuestion &&
                                   hasResponses && validPoints);
}

void CreatePollDialog::removeResponse(size_t index)
{
    QStringList remaining;
    remaining.reserve(MAX_POLL_RESPONSES);
    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        const auto text = this->responseRows_[i].input->text().trimmed();
        if (i != index && !text.isEmpty())
        {
            remaining.push_back(text);
        }
    }

    for (size_t i = 0; i < this->responseRows_.size(); ++i)
    {
        QSignalBlocker blocker(this->responseRows_[i].input);
        this->responseRows_[i].input->setText(
            i < static_cast<size_t>(remaining.size()) ? remaining.at(i)
                                                      : QString{});
    }

    this->updateResponseRows();
    this->updateStartButton();
}

void CreatePollDialog::submit()
{
    if (this->submitting_)
    {
        return;
    }

    const auto title = this->question_->text().trimmed();
    const auto choices = this->responses();
    if (title.isEmpty())
    {
        this->showError(QStringLiteral("Question is required."));
        return;
    }
    if (choices.size() < 2)
    {
        this->showError(
            QStringLiteral("At least two responses are required."));
        return;
    }

    this->clearError();
    this->submitting_ = true;
    this->updateResponseRows(false);
    this->updateStartButton();

    const auto durationSeconds = this->duration_->currentData().toInt();
    const auto points = this->allowAdditionalVotes_->isChecked()
                            ? std::optional<int>{this->pointsPerVote_->value()}
                            : std::nullopt;
    QPointer<CreatePollDialog> self(this);
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
            scheduleFailure(
                QStringLiteral("You must be logged in to Kick to create a poll."));
            return;
        }
        const auto websiteToken = kickAccount->chatIdentityToken().trimmed();
        if (websiteToken.isEmpty())
        {
            scheduleFailure(QStringLiteral(
                "Connect Kick’s website session under Settings > Accounts "
                "before creating polls."));
            return;
        }

        const auto kickChannel = resolveKickChannel(this->channel_);
        const std::weak_ptr<KickChannel> kickChannelWeak = kickChannel;
        auto requestSlug = this->channelLogin_.trimmed();
        if (kickChannel != nullptr)
        {
            const auto liveSlug = kickChannel->slug().trimmed();
            requestSlug = liveSlug.isEmpty()
                              ? kickChannel->getName().trimmed()
                              : liveSlug;
        }
        getKickApi()->createPoll(
            requestSlug, websiteToken, title, choices, durationSeconds, 15,
            [channel = this->channel_, title, choices, durationSeconds,
             kickChannelWeak, self](ExpectedStr<void> result) {
                if (self == nullptr)
                {
                    return;
                }
                QTimer::singleShot(
                    0, self.data(),
                    [channel, title, choices, durationSeconds, kickChannelWeak,
                     self, result = std::move(result)] {
                        if (self == nullptr)
                        {
                            return;
                        }
                        if (!result)
                        {
                            self->finishSubmitFailure(
                                "Failed to create Kick poll - " +
                                result.error());
                            return;
                        }
                        if (auto kickChannel = kickChannelWeak.lock();
                            kickChannel != nullptr &&
                            (!kickChannel->activePoll() ||
                             kickChannel->activePoll()->title != title))
                        {
                            const auto now = QDateTime::currentDateTimeUtc();
                            KickPoll poll{
                                .title = title,
                                .state = QStringLiteral("ACTIVE"),
                                .createdAt = now,
                                .endsAt = now.addSecs(durationSeconds),
                                .hideAt = now.addSecs(durationSeconds + 15),
                            };
                            poll.options.reserve(choices.size());
                            for (const auto &choice : choices)
                            {
                                poll.options.push_back({.title = choice});
                            }
                            kickChannel->updatePoll(std::move(poll));
                        }
                        if (channel != nullptr)
                        {
                            channel->addSystemMessage(
                                QStringLiteral("Created Kick poll: '%1'")
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
        scheduleFailure(QStringLiteral("You must be logged in to create a poll."));
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
                channel->addSystemMessage(
                    QStringLiteral("Created poll: '%1'").arg(title));
                notifyPollsAndPredictionsChanged(channel);
            }
            self->close();
        });
    };
    std::function<void(const QString &)> failureCallback =
        [scheduleFailure](const QString &error) {
        scheduleFailure("Failed to create poll - " + error);
    };

    if (currentUser->getUserId() == this->broadcasterID_)
    {
        getHelix()->createPoll(this->broadcasterID_, title, choices,
                               std::chrono::seconds(durationSeconds),
                               points.value_or(0), std::move(successCallback),
                               std::move(failureCallback));
        return;
    }

    QString authError;
    const auto moderationAccount =
        TwitchModerationAuth::resolveForCurrentUser(currentUser->getUserId(),
                                                    &authError);
    if (!moderationAccount.isValid())
    {
        scheduleFailure("Failed to create poll - " + authError);
        return;
    }

    TwitchWebApi::startPoll(
        this->broadcasterID_, title, choices, durationSeconds, points,
        moderationAccount.clientId, moderationAccount.oauthToken,
        [successCallback = std::move(successCallback),
         broadcasterID = this->broadcasterID_, title, choices,
         durationSeconds]() mutable {
            TwitchPollsAndPredictionsBar::rememberLocalPoll(
                broadcasterID, title, choices, durationSeconds);
            successCallback();
        },
        std::move(failureCallback));
}

void CreatePollDialog::finishSubmitFailure(const QString &message)
{
    this->submitting_ = false;
    this->updateResponseRows(false);
    this->updateStartButton();
    this->showError(message);
}

void CreatePollDialog::showError(const QString &message)
{
    this->errorLabel_->setText(message);
    this->errorLabel_->show();
    this->errorLabel_->updateGeometry();
    this->updateDialogHeight(this->visibleResponseCount(), false);
}

void CreatePollDialog::clearError()
{
    this->errorLabel_->clear();
    this->errorLabel_->hide();
    this->updateDialogHeight(this->visibleResponseCount(), false);
}

QStringList CreatePollDialog::responses() const
{
    QStringList choices;
    for (const auto &row : this->responseRows_)
    {
        const auto text = row.input->text().trimmed();
        if (!text.isEmpty())
        {
            choices.push_back(text);
        }
    }

    return choices;
}

}  // namespace chatterino
