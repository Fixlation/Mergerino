// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/GiveawayPopup.hpp"

#include "common/Channel.hpp"
#include "messages/MessageElement.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "singletons/Theme.hpp"

#include <QAbstractAnimation>
#include <QDateTime>
#include <QEasingCurve>
#include <QFont>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollBar>
#include <QScrollArea>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QStringList>
#include <QTextBrowser>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>
#include <ranges>
#include <utility>

namespace chatterino {

namespace {

constexpr int MAX_CAPTURED_MESSAGES_PER_PARTICIPANT = 40;
constexpr int MAX_NUMBER = 999999999;
constexpr int MAX_VISIBLE_ENTRANTS = 6;
constexpr int ENTRANT_ROW_HEIGHT = 24;
constexpr int ENTRANT_VIEWPORT_HEIGHT =
    MAX_VISIBLE_ENTRANTS * ENTRANT_ROW_HEIGHT + 4;
constexpr int KEYWORD_WINDOW_HEIGHT = 600;
constexpr int NUMBER_WINDOW_HEIGHT = 455;

class JumpToClickSlider final : public QSlider
{
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton)
        {
            QStyleOptionSlider option;
            this->initStyleOption(&option);
            const auto groove = this->style()->subControlRect(
                QStyle::CC_Slider, &option, QStyle::SC_SliderGroove, this);
            const auto handle = this->style()->subControlRect(
                QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, this);
            const auto position = event->position().toPoint();

            if (!handle.contains(position))
            {
                const int handleLength =
                    this->orientation() == Qt::Horizontal ? handle.width()
                                                          : handle.height();
                const int sliderMinimum =
                    this->orientation() == Qt::Horizontal ? groove.x()
                                                          : groove.y();
                const int sliderMaximum =
                    (this->orientation() == Qt::Horizontal ? groove.right()
                                                            : groove.bottom()) -
                    handleLength + 1;
                const int mousePosition =
                    this->orientation() == Qt::Horizontal ? position.x()
                                                          : position.y();

                this->setValue(QStyle::sliderValueFromPosition(
                    this->minimum(), this->maximum(),
                    mousePosition - sliderMinimum - handleLength / 2,
                    sliderMaximum - sliderMinimum, option.upsideDown));
            }
        }

        QSlider::mousePressEvent(event);
    }
};

QString platformName(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral("Kick");
        case MessagePlatform::YouTube:
            return QStringLiteral("YouTube");
        case MessagePlatform::TikTok:
            return QStringLiteral("TikTok");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral("Twitch");
    }
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QStringLiteral(":/platforms/kick.svg");
        case MessagePlatform::YouTube:
            return QStringLiteral(":/platforms/youtube.svg");
        case MessagePlatform::TikTok:
            return QStringLiteral(":/platforms/tiktok.svg");
        case MessagePlatform::AnyOrTwitch:
        default:
            return QStringLiteral(":/platforms/twitch.svg");
    }
}

class GiveawayPlatformButton final : public QToolButton
{
public:
    explicit GiveawayPlatformButton(MessagePlatform platform,
                                    QWidget *parent = nullptr)
        : QToolButton(parent)
        , icon_(platformIconPath(platform))
        , brightness_(platform == MessagePlatform::AnyOrTwitch ? 0.82 : 1.0)
    {
    }

    qreal desaturation() const
    {
        return this->desaturation_;
    }

    void setDesaturation(qreal desaturation)
    {
        const auto clamped = std::clamp(desaturation, 0.0, 1.0);
        if (qFuzzyCompare(this->desaturation_, clamped))
        {
            return;
        }
        this->desaturation_ = clamped;
        this->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        (void)event;
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        this->initStyleOption(&option);
        option.icon = {};
        painter.drawComplexControl(QStyle::CC_ToolButton, option);

        auto image = this->icon_.pixmap(this->iconSize())
                         .toImage()
                         .convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); ++y)
        {
            auto *scanLine = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x)
            {
                const auto pixel = scanLine[x];
                const int red = qRound(qRed(pixel) * this->brightness_);
                const int green = qRound(qGreen(pixel) * this->brightness_);
                const int blue = qRound(qBlue(pixel) * this->brightness_);
                const int gray = qGray(red, green, blue);
                scanLine[x] = qRgba(
                    qRound(red + (gray - red) * this->desaturation_),
                    qRound(green + (gray - green) * this->desaturation_),
                    qRound(blue + (gray - blue) * this->desaturation_),
                    qAlpha(pixel));
            }
        }

        const QRect iconRect(
            (this->width() - this->iconSize().width()) / 2,
            (this->height() - this->iconSize().height()) / 2,
            this->iconSize().width(), this->iconSize().height());
        painter.drawImage(iconRect, image);
    }

private:
    QIcon icon_;
    qreal brightness_ = 1.0;
    qreal desaturation_ = 0.0;
};

QLabel *sectionLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("GiveawaySectionLabel"));
    return label;
}

bool containsPlatform(const std::vector<MessagePlatform> &platforms,
                      MessagePlatform platform)
{
    return std::ranges::find(platforms, platform) != platforms.end();
}

}  // namespace

GiveawayPopup::GiveawayPopup(QWidget *parent)
    : BasePopup(
          {
              BaseWindow::EnableCustomFrame,
              BaseWindow::CloseButtonOnly,
              BaseWindow::FixedSizeCustomFrame,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
{
    this->setWindowTitle(QStringLiteral("Giveaway"));
    this->setScaleIndependentSize(520, KEYWORD_WINDOW_HEIGHT);
    this->setAutoFillBackground(true);
    this->buildUi();
    this->applyTheme();
    this->randomizeTarget();
    this->updateModeControls();
    this->updatePlatformControls();
    this->updateRoundControls();
}

void GiveawayPopup::setContext(
    ChannelPtr channel, std::vector<MessagePlatform> availablePlatforms)
{
    std::ranges::sort(availablePlatforms,
                      [](MessagePlatform left, MessagePlatform right) {
                          return static_cast<int>(left) <
                                 static_cast<int>(right);
                      });
    availablePlatforms.erase(
        std::unique(availablePlatforms.begin(), availablePlatforms.end()),
        availablePlatforms.end());

    const bool channelChanged = this->channel_ != channel;
    const bool platformsChanged = this->availablePlatforms_ != availablePlatforms;
    if (!channelChanged && !platformsChanged)
    {
        return;
    }

    if (channelChanged)
    {
        this->channelConnections_.clear();
        this->channel_ = std::move(channel);
        this->running_ = false;
        this->resetParticipants();
        this->clearWinner();
        if (this->channel_ != nullptr)
        {
            this->channelConnections_.managedConnect(
                this->channel_->messageAppended,
                [this](MessagePtr &message,
                       std::optional<MessageFlags> /*overridingFlags*/) {
                    this->handleMessage(message);
                });
        }
    }

    this->availablePlatforms_ = std::move(availablePlatforms);
    for (auto it = this->platformButtons_.begin();
         it != this->platformButtons_.end(); ++it)
    {
        const bool available =
            containsPlatform(this->availablePlatforms_, it.key());
        it.value()->setVisible(available);
        if (available && (channelChanged || platformsChanged))
        {
            it.value()->setChecked(true);
        }
    }
    this->updatePlatformControls();
    this->updateRoundControls();
    if (this->availablePlatforms_.empty())
    {
        this->setStatus(
            QStringLiteral("No supported chat platforms are enabled on this tab."),
            true);
    }
    else
    {
        this->setStatus(QStringLiteral("Ready on %1.")
                            .arg(this->selectedPlatformsText()));
    }
}

void GiveawayPopup::buildUi()
{
    this->root_ = this->getLayoutContainer();
    this->root_->setObjectName(QStringLiteral("GiveawayRoot"));
    this->root_->setAutoFillBackground(true);

    auto *rootLayout = new QVBoxLayout(this->root_);
    rootLayout->setContentsMargins(16, 10, 16, 10);
    rootLayout->setSpacing(8);

    rootLayout->addWidget(sectionLabel(QStringLiteral("Platforms"), this));
    auto *platformRow = new QHBoxLayout;
    platformRow->setContentsMargins(0, 0, 0, 0);
    platformRow->setSpacing(6);
    for (const auto platform : {MessagePlatform::AnyOrTwitch,
                                MessagePlatform::Kick,
                                MessagePlatform::YouTube,
                                MessagePlatform::TikTok})
    {
        auto *button = new GiveawayPlatformButton(platform, this);
        button->setObjectName(QStringLiteral("GiveawayPlatformButton"));
        button->setIconSize(QSize(20, 20));
        button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        button->setFixedSize(QSize(38, 34));
        button->setAccessibleName(platformName(platform));
        button->setCheckable(true);
        button->setChecked(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setToolTip(
            QStringLiteral("Include %1 chat in this giveaway.")
                .arg(platformName(platform)));

        auto *selectionAnimation = new QVariantAnimation(button);
        selectionAnimation->setDuration(360);
        selectionAnimation->setEasingCurve(QEasingCurve::InOutSine);
        QObject::connect(
            button, &QToolButton::toggled, this,
            [this, button, selectionAnimation](bool selected) {
                selectionAnimation->stop();
                selectionAnimation->setStartValue(button->desaturation());
                selectionAnimation->setEndValue(selected ? 0.0 : 1.0);
                selectionAnimation->start();
                this->updatePlatformControls();
                this->updateRoundControls();
            });
        QObject::connect(
            selectionAnimation, &QVariantAnimation::valueChanged, button,
            [button](const QVariant &value) {
                button->setDesaturation(value.toReal());
            });
        this->platformButtons_.insert(platform, button);
        platformRow->addWidget(button);
    }
    platformRow->addStretch(1);
    rootLayout->addLayout(platformRow);

    rootLayout->addWidget(sectionLabel(QStringLiteral("Giveaway style"), this));
    auto *modeRow = new QHBoxLayout;
    modeRow->setContentsMargins(0, 0, 0, 0);
    modeRow->setSpacing(0);
    this->keywordModeButton_ =
        new QPushButton(QStringLiteral("Keyword"), this);
    this->keywordModeButton_->setObjectName(
        QStringLiteral("GiveawayModeLeft"));
    this->keywordModeButton_->setCheckable(true);
    this->numberModeButton_ =
        new QPushButton(QStringLiteral("First number"), this);
    this->numberModeButton_->setObjectName(
        QStringLiteral("GiveawayModeRight"));
    this->numberModeButton_->setCheckable(true);
    modeRow->addWidget(this->keywordModeButton_, 1);
    modeRow->addWidget(this->numberModeButton_, 1);
    rootLayout->addLayout(modeRow);
    QObject::connect(this->keywordModeButton_, &QPushButton::clicked, this,
                     [this] {
                         this->setMode(Mode::Keyword);
                     });
    QObject::connect(this->numberModeButton_, &QPushButton::clicked, this,
                     [this] {
                         this->setMode(Mode::Number);
                     });

    this->modeStack_ = new QStackedWidget(this);
    this->modeStack_->setObjectName(QStringLiteral("GiveawayModeStack"));

    auto *keywordPage = new QWidget(this->modeStack_);
    auto *keywordLayout = new QVBoxLayout(keywordPage);
    keywordLayout->setContentsMargins(0, 2, 0, 2);
    keywordLayout->setSpacing(5);
    auto *keywordHelp = new QLabel(
        QStringLiteral("Each viewer enters once by sending the exact keyword."),
        keywordPage);
    keywordHelp->setObjectName(QStringLiteral("GiveawayHelp"));
    keywordHelp->setWordWrap(true);
    keywordLayout->addWidget(keywordHelp);
    this->keywordInput_ = new QLineEdit(keywordPage);
    this->keywordInput_->setPlaceholderText(QStringLiteral("Entry keyword"));
    this->keywordInput_->setMaxLength(100);
    keywordLayout->addWidget(this->keywordInput_);

    auto *subLuckRow = new QHBoxLayout;
    subLuckRow->setContentsMargins(0, 0, 0, 0);
    subLuckRow->setSpacing(9);
    subLuckRow->addWidget(new QLabel(QStringLiteral("Subscriber luck"),
                                     keywordPage));
    this->subscriberLuck_ =
        new JumpToClickSlider(Qt::Horizontal, keywordPage);
    this->subscriberLuck_->setObjectName(
        QStringLiteral("GiveawaySubscriberLuck"));
    this->subscriberLuck_->setRange(1, 100);
    this->subscriberLuck_->setValue(1);
    this->subscriberLuck_->setSingleStep(1);
    this->subscriberLuck_->setPageStep(10);
    this->subscriberLuck_->setCursor(Qt::PointingHandCursor);
    this->subscriberLuck_->setToolTip(
        QStringLiteral("Number of weighted entries given to subscribers."));
    subLuckRow->addWidget(this->subscriberLuck_, 1);
    this->subscriberLuckValue_ = new QLabel(QStringLiteral("1x"), keywordPage);
    this->subscriberLuckValue_->setObjectName(
        QStringLiteral("GiveawaySubscriberLuckValue"));
    this->subscriberLuckValue_->setAlignment(Qt::AlignRight |
                                             Qt::AlignVCenter);
    this->subscriberLuckValue_->setFixedWidth(34);
    subLuckRow->addWidget(this->subscriberLuckValue_);
    keywordLayout->addLayout(subLuckRow);
    QObject::connect(this->subscriberLuck_, &QSlider::valueChanged, this,
                     [this](int value) {
                         this->subscriberLuckValue_->setText(
                             QStringLiteral("%1x").arg(value));
                     });
    this->modeStack_->addWidget(keywordPage);

    auto *numberPage = new QWidget(this->modeStack_);
    auto *numberLayout = new QVBoxLayout(numberPage);
    numberLayout->setContentsMargins(0, 2, 0, 2);
    numberLayout->setSpacing(7);
    numberLayout->setAlignment(Qt::AlignTop);
    auto *numberHelp = new QLabel(
        QStringLiteral("The first viewer to send the exact target number wins."),
        numberPage);
    numberHelp->setObjectName(QStringLiteral("GiveawayHelp"));
    numberHelp->setWordWrap(true);
    numberLayout->addWidget(numberHelp);

    auto *rangeRow = new QHBoxLayout;
    rangeRow->setContentsMargins(0, 0, 0, 0);
    rangeRow->setSpacing(7);
    rangeRow->addWidget(new QLabel(QStringLiteral("Range"), numberPage));
    this->rangeMinimum_ = new QSpinBox(numberPage);
    this->rangeMinimum_->setRange(0, MAX_NUMBER);
    this->rangeMinimum_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    this->rangeMinimum_->setValue(1);
    this->rangeMinimum_->setToolTip(QStringLiteral("Smallest valid number"));
    this->rangeMaximum_ = new QSpinBox(numberPage);
    this->rangeMaximum_->setRange(0, MAX_NUMBER);
    this->rangeMaximum_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    this->rangeMaximum_->setValue(1000);
    this->rangeMaximum_->setToolTip(QStringLiteral("Largest valid number"));
    auto *rangeSeparator = new QLabel(QStringLiteral("to"), numberPage);
    rangeRow->addWidget(this->rangeMinimum_, 1);
    rangeRow->addWidget(rangeSeparator);
    rangeRow->addWidget(this->rangeMaximum_, 1);
    numberLayout->addLayout(rangeRow);

    auto *targetRow = new QHBoxLayout;
    targetRow->setContentsMargins(0, 0, 0, 0);
    targetRow->setSpacing(7);
    targetRow->addWidget(new QLabel(QStringLiteral("Target"), numberPage));
    this->targetInput_ = new QLineEdit(numberPage);
    this->targetInput_->setValidator(
        new QRegularExpressionValidator(QRegularExpression(QStringLiteral(
            "^(0|[1-9][0-9]{0,8})$")), this->targetInput_));
    targetRow->addWidget(this->targetInput_, 1);
    this->randomizeButton_ =
        new QPushButton(QStringLiteral("Randomize"), numberPage);
    this->randomizeButton_->setCursor(Qt::PointingHandCursor);
    this->randomizeButton_->setToolTip(
        QStringLiteral("Choose a random target inside the selected range."));
    targetRow->addWidget(this->randomizeButton_);
    this->numberVisibilityButton_ = new QPushButton(numberPage);
    this->numberVisibilityButton_->setObjectName(
        QStringLiteral("GiveawayVisibilityButton"));
    this->numberVisibilityButton_->setFixedSize(QSize(32, 30));
    this->numberVisibilityButton_->setIconSize(QSize(16, 16));
    this->numberVisibilityButton_->setAccessibleName(
        QStringLiteral("Hide target number"));
    this->numberVisibilityButton_->setCursor(Qt::PointingHandCursor);
    targetRow->addWidget(this->numberVisibilityButton_);
    numberLayout->addLayout(targetRow);
    this->modeStack_->addWidget(numberPage);
    rootLayout->addWidget(this->modeStack_);

    this->numberVisibilityOpacity_ =
        new QGraphicsOpacityEffect(this->numberVisibilityButton_);
    this->numberVisibilityOpacity_->setOpacity(1.0);
    this->numberVisibilityButton_->setGraphicsEffect(
        this->numberVisibilityOpacity_);
    this->numberVisibilityAnimation_ = new QVariantAnimation(this);
    this->numberVisibilityAnimation_->setDuration(170);
    this->numberVisibilityAnimation_->setStartValue(0.0);
    this->numberVisibilityAnimation_->setEndValue(1.0);
    this->numberVisibilityAnimation_->setEasingCurve(QEasingCurve::InOutCubic);
    QObject::connect(
        this->numberVisibilityAnimation_, &QVariantAnimation::valueChanged,
        this, [this](const QVariant &value) {
            const auto progress = value.toReal();
            if (progress >= 0.5 && !this->numberVisibilitySwapped_ &&
                this->pendingNumberVisibility_.has_value())
            {
                this->numberVisibilitySwapped_ = true;
                this->applyNumberVisibility(*this->pendingNumberVisibility_);
            }
            const auto opacity =
                progress < 0.5 ? 1.0 - progress * 2.0
                               : (progress - 0.5) * 2.0;
            this->numberVisibilityOpacity_->setOpacity(opacity);
        });
    QObject::connect(this->numberVisibilityAnimation_,
                     &QVariantAnimation::finished, this, [this] {
                         this->pendingNumberVisibility_.reset();
                         this->numberVisibilityOpacity_->setOpacity(1.0);
                     });

    QObject::connect(this->randomizeButton_, &QPushButton::clicked, this,
                     [this] {
                         this->randomizeTarget();
                     });
    QObject::connect(this->numberVisibilityButton_, &QPushButton::clicked,
                     this, [this] {
                         this->toggleNumberVisibility();
                     });
    QObject::connect(this->rangeMinimum_,
                     qOverload<int>(&QSpinBox::valueChanged), this,
                     [this](int value) {
                         if (value > this->rangeMaximum_->value())
                         {
                             this->rangeMaximum_->setValue(value);
                         }
                         this->updateRoundControls();
                     });
    QObject::connect(this->rangeMaximum_,
                     qOverload<int>(&QSpinBox::valueChanged), this,
                     [this](int value) {
                         if (value < this->rangeMinimum_->value())
                         {
                             this->rangeMinimum_->setValue(value);
                         }
                         this->updateRoundControls();
                     });
    QObject::connect(this->keywordInput_, &QLineEdit::textChanged, this,
                     [this] {
                         this->updateRoundControls();
                     });
    QObject::connect(this->targetInput_, &QLineEdit::textChanged, this,
                     [this] {
                         this->updateRoundControls();
                     });

    auto *roundCard = new QWidget(this);
    roundCard->setObjectName(QStringLiteral("GiveawayRoundCard"));
    roundCard->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *roundLayout = new QVBoxLayout(roundCard);
    roundLayout->setContentsMargins(11, 7, 11, 7);
    roundLayout->setSpacing(4);
    auto *roundTop = new QHBoxLayout;
    roundTop->setContentsMargins(0, 0, 0, 0);
    this->statusLabel_ = new QLabel(roundCard);
    this->statusLabel_->setObjectName(QStringLiteral("GiveawayStatus"));
    this->statusLabel_->setWordWrap(true);
    this->entrantCountLabel_ = new QLabel(roundCard);
    this->entrantCountLabel_->setObjectName(
        QStringLiteral("GiveawayEntrantCount"));
    this->entrantCountLabel_->setAlignment(Qt::AlignRight | Qt::AlignTop);
    roundTop->addWidget(this->statusLabel_, 1);
    roundTop->addWidget(this->entrantCountLabel_, 0, Qt::AlignTop);
    roundLayout->addLayout(roundTop);

    this->entrantScrollArea_ = new QScrollArea(roundCard);
    this->entrantScrollArea_->setObjectName(
        QStringLiteral("GiveawayEntrantScrollArea"));
    this->entrantScrollArea_->setFrameShape(QFrame::NoFrame);
    this->entrantScrollArea_->setWidgetResizable(true);
    this->entrantScrollArea_->setHorizontalScrollBarPolicy(
        Qt::ScrollBarAlwaysOff);
    this->entrantScrollArea_->setVerticalScrollBarPolicy(
        Qt::ScrollBarAsNeeded);
    this->entrantScrollArea_->setSizePolicy(QSizePolicy::Expanding,
                                            QSizePolicy::Fixed);
    this->entrantListWidget_ = new QWidget(this->entrantScrollArea_);
    this->entrantListWidget_->setObjectName(
        QStringLiteral("GiveawayEntrantList"));
    this->entrantListLayout_ = new QVBoxLayout(this->entrantListWidget_);
    this->entrantListLayout_->setContentsMargins(0, 2, 0, 2);
    this->entrantListLayout_->setSpacing(0);
    this->entrantListLayout_->setAlignment(Qt::AlignTop);
    this->entrantScrollArea_->setWidget(this->entrantListWidget_);
    this->entrantScrollArea_->setFixedHeight(ENTRANT_VIEWPORT_HEIGHT);
    this->entrantScrollArea_->verticalScrollBar()->setSingleStep(
        ENTRANT_ROW_HEIGHT);
    roundLayout->addWidget(this->entrantScrollArea_);

    this->updateEntrantList();
    rootLayout->addWidget(roundCard);

    auto *winnerHeader = new QHBoxLayout;
    winnerHeader->setContentsMargins(0, 0, 0, 0);
    winnerHeader->addWidget(
        sectionLabel(QStringLiteral("Winner messages"), this));
    this->winnerLabel_ = new QLabel(QStringLiteral("No winner yet"), this);
    this->winnerLabel_->setObjectName(QStringLiteral("GiveawayWinnerLabel"));
    this->winnerLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    winnerHeader->addWidget(this->winnerLabel_, 1);
    rootLayout->addLayout(winnerHeader);

    this->winnerMessages_ = new QTextBrowser(this);
    this->winnerMessages_->setObjectName(
        QStringLiteral("GiveawayWinnerMessages"));
    this->winnerMessages_->setReadOnly(true);
    this->winnerMessages_->setOpenExternalLinks(false);
    this->winnerMessages_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    this->winnerMessages_->setPlaceholderText(QStringLiteral(
        "The winner's messages will stay here until the next roll."));
    this->winnerMessages_->setFixedHeight(72);
    rootLayout->addWidget(this->winnerMessages_);

    auto *actionRow = new QHBoxLayout;
    actionRow->setContentsMargins(0, 0, 0, 0);
    actionRow->setSpacing(8);
    this->startButton_ = new QPushButton(this);
    this->startButton_->setObjectName(QStringLiteral("GiveawayStartButton"));
    this->startButton_->setCursor(Qt::PointingHandCursor);
    this->rollButton_ = new QPushButton(QStringLiteral("Roll winner"), this);
    this->rollButton_->setObjectName(QStringLiteral("GiveawayRollButton"));
    this->rollButton_->setCursor(Qt::PointingHandCursor);
    actionRow->addStretch(1);
    actionRow->addWidget(this->startButton_);
    actionRow->addWidget(this->rollButton_);
    rootLayout->addLayout(actionRow);
    QObject::connect(this->startButton_, &QPushButton::clicked, this, [this] {
        this->startOrStopRound();
    });
    QObject::connect(this->rollButton_, &QPushButton::clicked, this, [this] {
        this->rollKeywordWinner();
    });
}

void GiveawayPopup::resetParticipants()
{
    this->participants_.clear();
    this->participantOrder_.clear();
    this->updateEntrantList();
}

void GiveawayPopup::updateEntrantList()
{
    while (auto *item = this->entrantListLayout_->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    int rowCount = 0;
    if (this->participants_.isEmpty())
    {
        auto *emptyLabel =
            new QLabel(QStringLiteral("No entrants yet"),
                       this->entrantListWidget_);
        emptyLabel->setObjectName(QStringLiteral("GiveawayEntrantEmpty"));
        emptyLabel->setAlignment(Qt::AlignCenter);
        emptyLabel->setFixedHeight(MAX_VISIBLE_ENTRANTS * ENTRANT_ROW_HEIGHT);
        this->entrantListLayout_->addWidget(emptyLabel);
    }
    else
    {
        auto sortedKeys = this->participantOrder_;
        std::ranges::stable_sort(
            sortedKeys, [this](const QString &leftKey,
                               const QString &rightKey) {
                const auto left = this->participants_.constFind(leftKey);
                const auto right = this->participants_.constFind(rightKey);
                if (left == this->participants_.cend())
                {
                    return false;
                }
                if (right == this->participants_.cend())
                {
                    return true;
                }

                const auto byName = QString::localeAwareCompare(
                    left->author.toCaseFolded(),
                    right->author.toCaseFolded());
                return byName == 0 ? leftKey < rightKey : byName < 0;
            });

        for (const auto &key : sortedKeys)
        {
            const auto participant = this->participants_.constFind(key);
            if (participant == this->participants_.cend())
            {
                continue;
            }

            auto *row = new QWidget(this->entrantListWidget_);
            row->setObjectName(QStringLiteral("GiveawayEntrantRow"));
            row->setFixedHeight(ENTRANT_ROW_HEIGHT);
            auto *layout = new QHBoxLayout(row);
            layout->setContentsMargins(6, 0, 8, 0);
            layout->setSpacing(8);

            auto *platformIcon = new QLabel(row);
            platformIcon->setObjectName(
                QStringLiteral("GiveawayEntrantPlatformIcon"));
            platformIcon->setPixmap(
                QIcon(platformIconPath(participant->platform))
                    .pixmap(QSize(16, 16)));
            platformIcon->setFixedSize(QSize(18, ENTRANT_ROW_HEIGHT));
            platformIcon->setAlignment(Qt::AlignCenter);
            platformIcon->setToolTip(platformName(participant->platform));

            auto *name = new QLabel(participant->author, row);
            name->setObjectName(QStringLiteral("GiveawayEntrantName"));
            auto *count = new QLabel(
                QStringLiteral("%1 entr%2")
                    .arg(participant->entryCount)
                    .arg(participant->entryCount == 1
                             ? QStringLiteral("y")
                             : QStringLiteral("ies")),
                row);
            count->setObjectName(QStringLiteral("GiveawayEntrantEntries"));
            count->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            layout->addWidget(platformIcon);
            layout->addWidget(name, 1);
            layout->addWidget(count);
            this->entrantListLayout_->addWidget(row);
            ++rowCount;
        }
    }

    const int fullHeight =
        std::max(rowCount, MAX_VISIBLE_ENTRANTS) * ENTRANT_ROW_HEIGHT + 4;
    this->entrantListWidget_->setMinimumHeight(fullHeight);
}

void GiveawayPopup::setMode(Mode mode)
{
    if (this->running_ || this->mode_ == mode)
    {
        return;
    }
    this->mode_ = mode;
    this->resetParticipants();
    this->clearWinner();
    this->setStatus(QStringLiteral("Ready on %1.")
                        .arg(this->selectedPlatformsText()));
    this->updateModeControls();
    this->updateRoundControls();
}

void GiveawayPopup::updateModeControls()
{
    const bool keyword = this->mode_ == Mode::Keyword;
    this->keywordModeButton_->setChecked(keyword);
    this->numberModeButton_->setChecked(!keyword);
    this->modeStack_->setCurrentIndex(keyword ? 0 : 1);
    this->keywordModeButton_->setEnabled(!this->running_);
    this->numberModeButton_->setEnabled(!this->running_);
    this->rollButton_->setVisible(keyword);
    this->entrantCountLabel_->setVisible(keyword);
    this->entrantScrollArea_->setVisible(keyword);

    this->modeStack_->setFixedHeight(
        this->modeStack_->currentWidget()->sizeHint().height());
    if (auto *layout = this->root_->layout())
    {
        layout->invalidate();
        layout->activate();
    }
    this->setScaleIndependentHeight(keyword ? KEYWORD_WINDOW_HEIGHT
                                            : NUMBER_WINDOW_HEIGHT);
}

void GiveawayPopup::updatePlatformControls()
{
    int selectedCount = 0;
    for (auto it = this->platformButtons_.begin();
         it != this->platformButtons_.end(); ++it)
    {
        const bool available =
            containsPlatform(this->availablePlatforms_, it.key());
        it.value()->setVisible(available);
        it.value()->setEnabled(available && !this->running_);
        if (available && it.value()->isChecked())
        {
            ++selectedCount;
        }
    }
    if (selectedCount == 0 && !this->running_)
    {
        this->setStatus(QStringLiteral("Select at least one platform."), true);
    }
    else if (selectedCount > 0 && !this->running_ &&
             this->statusLabel_->text() ==
                 QStringLiteral("Select at least one platform."))
    {
        this->setStatus(
            QStringLiteral("Ready on %1.").arg(this->selectedPlatformsText()));
    }
}

void GiveawayPopup::updateRoundControls()
{
    int selectedCount = 0;
    for (auto it = this->platformButtons_.cbegin();
         it != this->platformButtons_.cend(); ++it)
    {
        if (it.value()->isVisible() && it.value()->isChecked())
        {
            ++selectedCount;
        }
    }

    const bool keywordReady = !this->keywordInput_->text().trimmed().isEmpty();
    bool targetOk = false;
    const auto target = this->targetInput_->text().toInt(&targetOk);
    const bool numberReady = targetOk && target >= this->rangeMinimum_->value() &&
                             target <= this->rangeMaximum_->value();
    const bool canStart = selectedCount > 0 &&
                          (this->mode_ == Mode::Keyword ? keywordReady
                                                       : numberReady);
    this->startButton_->setEnabled(this->running_ || canStart);
    this->startButton_->setText(
        this->running_
            ? QStringLiteral("Stop")
            : (this->mode_ == Mode::Keyword
                   ? QStringLiteral("Start giveaway")
                   : QStringLiteral("Start number hunt")));
    this->rollButton_->setEnabled(!this->participants_.isEmpty());
    this->entrantCountLabel_->setText(
        QStringLiteral("%1 entrant%2")
            .arg(this->participants_.size())
            .arg(this->participants_.size() == 1 ? QString()
                                                 : QStringLiteral("s")));

    const bool fieldsEnabled = !this->running_;
    this->subscriberLuck_->setEnabled(fieldsEnabled);
    this->keywordInput_->setEnabled(fieldsEnabled);
    this->rangeMinimum_->setEnabled(fieldsEnabled);
    this->rangeMaximum_->setEnabled(fieldsEnabled);
    this->targetInput_->setEnabled(fieldsEnabled);
    this->randomizeButton_->setEnabled(fieldsEnabled);
    this->numberVisibilityButton_->setEnabled(fieldsEnabled);
    this->updateModeControls();
    this->updatePlatformControls();
}

void GiveawayPopup::startOrStopRound()
{
    if (this->running_)
    {
        this->stopRound();
    }
    else
    {
        this->startRound();
    }
}

void GiveawayPopup::startRound()
{
    if (this->channel_ == nullptr || this->availablePlatforms_.empty())
    {
        this->setStatus(
            QStringLiteral("No supported chat is available on this tab."), true);
        return;
    }

    if (this->mode_ == Mode::Keyword)
    {
        this->activeSubscriberMultiplier_ = this->subscriberLuck_->value();
        this->activeKeyword_ = this->keywordInput_->text().trimmed();
        if (this->activeKeyword_.isEmpty())
        {
            this->setStatus(QStringLiteral("Enter a keyword first."), true);
            return;
        }
    }
    else
    {
        bool ok = false;
        const auto target = this->targetInput_->text().toInt(&ok);
        if (!ok || target < this->rangeMinimum_->value() ||
            target > this->rangeMaximum_->value())
        {
            this->setStatus(
                QStringLiteral("The target must be inside the selected range."),
                true);
            return;
        }
        this->activeTarget_ = target;
    }

    bool anyPlatform = false;
    for (auto it = this->platformButtons_.cbegin();
         it != this->platformButtons_.cend(); ++it)
    {
        anyPlatform = anyPlatform ||
                      (it.value()->isVisible() && it.value()->isChecked());
    }
    if (!anyPlatform)
    {
        this->setStatus(QStringLiteral("Select at least one platform."), true);
        return;
    }

    this->resetParticipants();
    this->clearWinner();
    this->running_ = true;
    if (this->mode_ == Mode::Keyword)
    {
        this->setStatus(
            QStringLiteral("Entries open on %1 • keyword: %2")
                .arg(this->selectedPlatformsText(), this->activeKeyword_));
    }
    else
    {
        const auto targetDescription =
            this->numberVisibility_ == NumberVisibility::Visible
                ? QString::number(this->activeTarget_)
                : QStringLiteral("hidden");
        this->setStatus(
            QStringLiteral("Number hunt live on %1 • target: %2")
                .arg(this->selectedPlatformsText(), targetDescription));
    }
    this->updateRoundControls();
}

void GiveawayPopup::stopRound()
{
    this->running_ = false;
    if (this->mode_ == Mode::Keyword)
    {
        this->setStatus(
            QStringLiteral("Entries closed • %1 entrant%2")
                .arg(this->participants_.size())
                .arg(this->participants_.size() == 1 ? QString()
                                                     : QStringLiteral("s")));
    }
    else
    {
        this->setStatus(QStringLiteral("Number hunt stopped."));
    }
    this->updateRoundControls();
}

void GiveawayPopup::rollKeywordWinner()
{
    if (this->mode_ != Mode::Keyword || this->participants_.isEmpty())
    {
        return;
    }

    quint64 totalEntries = 0;
    for (const auto &key : this->participantOrder_)
    {
        const auto participant = this->participants_.constFind(key);
        if (participant != this->participants_.cend())
        {
            totalEntries += static_cast<quint64>(
                std::max(1, participant->entryCount));
        }
    }
    if (totalEntries == 0)
    {
        return;
    }

    const auto rejectionThreshold =
        (std::numeric_limits<quint64>::max() - totalEntries + 1) %
        totalEntries;
    quint64 randomValue = 0;
    do
    {
        randomValue = QRandomGenerator::global()->generate64();
    } while (randomValue < rejectionThreshold);
    quint64 ticket = randomValue % totalEntries;

    for (const auto &key : this->participantOrder_)
    {
        const auto entries = static_cast<quint64>(
            std::max(1, this->participants_.value(key).entryCount));
        if (ticket < entries)
        {
            this->chooseWinner(key);
            return;
        }
        ticket -= entries;
    }
}

void GiveawayPopup::randomizeTarget()
{
    const auto minimum = this->rangeMinimum_->value();
    const auto maximum = this->rangeMaximum_->value();
    const auto span = static_cast<quint32>(maximum - minimum + 1);
    const auto target = minimum +
                        static_cast<int>(QRandomGenerator::global()->bounded(span));
    this->targetInput_->setText(QString::number(target));
}

void GiveawayPopup::toggleNumberVisibility()
{
    if (this->numberVisibilityAnimation_->state() ==
        QAbstractAnimation::Running)
    {
        return;
    }
    this->pendingNumberVisibility_ =
        this->numberVisibility_ == NumberVisibility::Visible
            ? NumberVisibility::Hidden
            : NumberVisibility::Visible;
    this->numberVisibilitySwapped_ = false;
    this->numberVisibilityAnimation_->start();
}

void GiveawayPopup::applyNumberVisibility(NumberVisibility visibility)
{
    this->numberVisibility_ = visibility;
    const bool visible = visibility == NumberVisibility::Visible;
    this->targetInput_->setEchoMode(visible ? QLineEdit::Normal
                                            : QLineEdit::NoEcho);
    this->numberVisibilityButton_->setText(QString());
    const bool light = getTheme() != nullptr && getTheme()->isLightTheme();
    const auto iconPath =
        visible ? (light ? QStringLiteral(":/buttons/eyeDark.svg")
                         : QStringLiteral(":/buttons/eye.svg"))
                : (light ? QStringLiteral(":/buttons/eyeClosedDark.svg")
                         : QStringLiteral(":/buttons/eyeClosed.svg"));
    this->numberVisibilityButton_->setIcon(QIcon(iconPath));
    this->numberVisibilityButton_->setAccessibleName(
        visible ? QStringLiteral("Hide target number")
                : QStringLiteral("Show target number"));
    this->numberVisibilityButton_->setToolTip(
        visible ? QStringLiteral("Target is visible. Click to hide it.")
                : QStringLiteral("Target is hidden. Click to show it."));
}

void GiveawayPopup::handleMessage(const MessagePtr &message)
{
    if (message == nullptr || !this->acceptsPlatform(message->platform) ||
        !this->isEligibleMessage(*message))
    {
        return;
    }

    const auto key = this->participantKey(*message);
    auto existing = this->participants_.find(key);
    if (existing != this->participants_.end())
    {
        this->appendParticipantMessage(existing.value(), *message);
        if (this->winnerKey_ == key)
        {
            this->appendWinnerMessage(existing->messages.constLast());
        }
    }

    if (!this->running_)
    {
        return;
    }

    if (this->mode_ == Mode::Keyword)
    {
        if (message->messageText.trimmed().compare(this->activeKeyword_,
                                                   Qt::CaseInsensitive) != 0)
        {
            return;
        }
        if (existing == this->participants_.end())
        {
            auto participant = this->participantFromMessage(*message);
            this->appendParticipantMessage(participant, *message);
            this->participants_.insert(key, std::move(participant));
            this->participantOrder_.append(key);
            this->updateEntrantList();
            this->setStatus(
                QStringLiteral("Entries open on %1 • keyword: %2")
                    .arg(this->selectedPlatformsText(), this->activeKeyword_));
            this->updateRoundControls();
        }
        return;
    }

    bool numberOk = false;
    const auto messageNumber = message->messageText.trimmed().toInt(&numberOk);
    if (!numberOk || messageNumber != this->activeTarget_)
    {
        return;
    }

    if (existing == this->participants_.end())
    {
        auto participant = this->participantFromMessage(*message);
        this->appendParticipantMessage(participant, *message);
        this->participants_.insert(key, std::move(participant));
        this->participantOrder_.append(key);
        this->updateEntrantList();
    }
    this->running_ = false;
    this->chooseWinner(key, QStringLiteral("found %1 first")
                                .arg(this->activeTarget_));
    this->updateRoundControls();
}

bool GiveawayPopup::acceptsPlatform(MessagePlatform platform) const
{
    const auto button = this->platformButtons_.value(platform, nullptr);
    return button != nullptr && button->isVisible() && button->isChecked();
}

bool GiveawayPopup::isEligibleMessage(const Message &message) const
{
    return !message.loginName.trimmed().isEmpty() &&
           !message.messageText.trimmed().isEmpty() &&
           !message.flags.hasAny(
               {MessageFlag::System, MessageFlag::Subscription,
                MessageFlag::Whisper, MessageFlag::ModerationAction,
                MessageFlag::Timeout, MessageFlag::TikTokJoinMessage});
}

bool GiveawayPopup::messageIsSubscriber(const Message &message) const
{
    for (const auto &badge : message.twitchBadges)
    {
        if (badge.flag_ == MessageElementFlag::BadgeSubscription)
        {
            return true;
        }
    }
    return std::ranges::any_of(
        message.elements, [](const auto &element) {
            return element != nullptr &&
                   element->getFlags().has(
                       MessageElementFlag::BadgeSubscription);
        });
}

QString GiveawayPopup::participantKey(const Message &message) const
{
    return message.loginName.trimmed()
        .normalized(QString::NormalizationForm_KC)
        .toCaseFolded();
}

GiveawayPopup::Participant GiveawayPopup::participantFromMessage(
    const Message &message) const
{
    Participant participant;
    participant.key = this->participantKey(message);
    participant.author = message.displayName.trimmed();
    if (participant.author.isEmpty())
    {
        participant.author = message.loginName.trimmed();
    }
    participant.platform = message.platform;
    participant.entryCount =
        this->mode_ == Mode::Keyword && this->messageIsSubscriber(message)
            ? this->activeSubscriberMultiplier_ : 1;
    return participant;
}

GiveawayPopup::CapturedMessage GiveawayPopup::capturedMessage(
    const Message &message) const
{
    CapturedMessage captured;
    captured.text = message.messageText;
    captured.author = message.displayName.trimmed();
    if (captured.author.isEmpty())
    {
        captured.author = message.loginName.trimmed();
    }
    captured.platform = message.platform;
    const auto time = message.serverReceivedTime.isValid()
                          ? message.serverReceivedTime.toLocalTime()
                          : QDateTime::currentDateTime();
    captured.timestamp = time.toString(QStringLiteral("HH:mm"));
    return captured;
}

void GiveawayPopup::appendParticipantMessage(Participant &participant,
                                              const Message &message)
{
    participant.messages.append(this->capturedMessage(message));
    while (participant.messages.size() >
           MAX_CAPTURED_MESSAGES_PER_PARTICIPANT)
    {
        participant.messages.removeFirst();
    }
}

void GiveawayPopup::chooseWinner(const QString &participantKey,
                                 const QString &reason)
{
    const auto participant = this->participants_.constFind(participantKey);
    if (participant == this->participants_.cend())
    {
        return;
    }
    this->winnerKey_ = participantKey;
    this->winnerLabel_->setText(
        QStringLiteral("%1 • %2")
            .arg(participant->author, platformName(participant->platform)));
    this->renderWinnerMessages();
    if (reason.isEmpty())
    {
        this->setStatus(QStringLiteral("Winner: %1 on %2")
                            .arg(participant->author,
                                 platformName(participant->platform)));
    }
    else
    {
        this->setStatus(QStringLiteral("Winner: %1 on %2 • %3")
                            .arg(participant->author,
                                 platformName(participant->platform), reason));
    }
}

void GiveawayPopup::clearWinner()
{
    this->winnerKey_.clear();
    if (this->winnerLabel_ != nullptr)
    {
        this->winnerLabel_->setText(QStringLiteral("No winner yet"));
    }
    if (this->winnerMessages_ != nullptr)
    {
        this->winnerMessages_->clear();
    }
}

void GiveawayPopup::renderWinnerMessages()
{
    this->winnerMessages_->clear();
    const auto winner = this->participants_.constFind(this->winnerKey_);
    if (winner == this->participants_.cend())
    {
        return;
    }
    for (const auto &message : winner->messages)
    {
        this->appendWinnerMessage(message);
    }
}

void GiveawayPopup::appendWinnerMessage(const CapturedMessage &message)
{
    const auto timestamp = message.timestamp.toHtmlEscaped();
    const auto author = message.author.toHtmlEscaped();
    const auto body = message.text.toHtmlEscaped().replace(
        QLatin1Char('\n'), QStringLiteral("<br>"));
    this->winnerMessages_->append(
        QStringLiteral(
            "<div style='margin:1px 0 5px 0'><span style='opacity:.58'>%1"
            "</span>&nbsp; <b>%2:</b> %3</div>")
            .arg(timestamp, author, body));
    this->winnerMessages_->verticalScrollBar()->setValue(
        this->winnerMessages_->verticalScrollBar()->maximum());
}

void GiveawayPopup::setStatus(const QString &text, bool error)
{
    this->statusLabel_->setText(text);
    this->statusLabel_->setProperty("error", error);
    this->statusLabel_->style()->unpolish(this->statusLabel_);
    this->statusLabel_->style()->polish(this->statusLabel_);
}

QString GiveawayPopup::selectedPlatformsText() const
{
    QStringList platforms;
    for (const auto platform : {MessagePlatform::AnyOrTwitch,
                                MessagePlatform::Kick,
                                MessagePlatform::YouTube,
                                MessagePlatform::TikTok})
    {
        const auto button = this->platformButtons_.value(platform, nullptr);
        if (button != nullptr && button->isVisible() && button->isChecked())
        {
            platforms.append(platformName(platform));
        }
    }
    return platforms.isEmpty() ? QStringLiteral("no platforms")
                               : platforms.join(QStringLiteral(" + "));
}

void GiveawayPopup::applyTheme()
{
    const auto *theme = getTheme();
    if (theme == nullptr || this->root_ == nullptr)
    {
        return;
    }

    const bool light = theme->isLightTheme();
    const auto colorOr = [](const QColor &color, const QColor &fallback) {
        return color.isValid() ? color : fallback;
    };
    const auto background =
        colorOr(theme->window.background,
                light ? QColor(QStringLiteral("#f4f4f6"))
                      : QColor(QStringLiteral("#18181b")));
    const auto panel =
        colorOr(theme->splits.header.background,
                light ? QColor(QStringLiteral("#ffffff"))
                      : QColor(QStringLiteral("#202024")));
    const auto field =
        colorOr(theme->splits.input.background,
                light ? QColor(QStringLiteral("#ffffff"))
                      : QColor(QStringLiteral("#121214")));
    const auto text =
        colorOr(theme->window.text,
                light ? QColor(QStringLiteral("#202024"))
                      : QColor(QStringLiteral("#efeff1")));
    const auto muted =
        colorOr(theme->tabs.regular.text,
                light ? QColor(QStringLiteral("#62626c"))
                      : QColor(QStringLiteral("#a8a8b2")));
    const auto border =
        colorOr(theme->splits.header.border,
                light ? QColor(QStringLiteral("#cfd0d6"))
                      : QColor(QStringLiteral("#34343b")));
    const auto hover =
        colorOr(theme->tabs.regular.backgrounds.hover,
                light ? QColor(QStringLiteral("#e5e5e9"))
                      : QColor(QStringLiteral("#2c2c32")));
    const auto selected =
        colorOr(theme->splits.header.focusedBackground,
                light ? QColor(QStringLiteral("#d9d9df"))
                      : QColor(QStringLiteral("#3a3a42")));

    this->overrideBackgroundColor_ = background;
    auto palette = this->palette();
    palette.setColor(QPalette::Window, background);
    palette.setColor(QPalette::Base, field);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::PlaceholderText, muted);
    palette.setColor(QPalette::Highlight, selected);
    palette.setColor(QPalette::Mid, border);
    this->setPalette(palette);
    this->root_->setPalette(palette);

    this->root_->setStyleSheet(QStringLiteral(R"(
QWidget#GiveawayRoot { background: %1; color: %4; }
QLabel { color: %4; }
QLabel#GiveawaySectionLabel { font-weight: 600; }
QLabel#GiveawayHelp, QLabel#GiveawayEntrantEmpty,
QLabel#GiveawayEntrantEntries, QLabel#GiveawayWinnerLabel,
QLabel#GiveawayEntrantCount { color: %5; }
QLabel#GiveawayWinnerLabel { font-weight: 600; }
QLabel#GiveawaySubscriberLuckValue { font-weight: 600; }
QLabel#GiveawayStatus[error="true"] { color: #ff6b6b; }
QWidget#GiveawayRoundCard {
    background: %2;
    border: 1px solid %6;
    border-radius: 6px;
}
QScrollArea#GiveawayEntrantScrollArea,
QWidget#GiveawayEntrantList {
    background: transparent;
    border: none;
}
QWidget#GiveawayEntrantRow {
    background: transparent;
    border-top: 1px solid %6;
}
QScrollBar:vertical {
    background: transparent;
    width: 8px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: %6;
    border-radius: 4px;
    min-height: 22px;
}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QLineEdit, QSpinBox, QTextBrowser {
    background: %3;
    color: %4;
    border: 1px solid %6;
    border-radius: 5px;
    padding: 5px 7px;
    selection-background-color: %8;
}
QSpinBox { padding-right: 7px; }
QSlider#GiveawaySubscriberLuck { min-height: 20px; }
QSlider#GiveawaySubscriberLuck::groove:horizontal {
    height: 4px;
    background: %6;
    border-radius: 2px;
}
QSlider#GiveawaySubscriberLuck::sub-page:horizontal {
    background: %8;
    border-radius: 2px;
}
QSlider#GiveawaySubscriberLuck::add-page:horizontal {
    background: %6;
    border-radius: 2px;
}
QSlider#GiveawaySubscriberLuck::handle:horizontal {
    width: 14px;
    margin: -5px 0;
    background: %4;
    border: 1px solid %6;
    border-radius: 7px;
}
QSlider#GiveawaySubscriberLuck::handle:horizontal:hover {
    border-color: %5;
}
QTextBrowser#GiveawayWinnerMessages { padding: 7px; }
QPushButton, QToolButton {
    background: %2;
    color: %4;
    border: 1px solid %6;
    border-radius: 5px;
    padding: 6px 10px;
}
QToolButton#GiveawayPlatformButton { padding: 5px; }
QPushButton#GiveawayVisibilityButton { padding: 0; }
QPushButton:hover, QToolButton:hover { background: %7; }
QPushButton:checked, QToolButton:checked {
    background: %8;
    border-color: %5;
    font-weight: 600;
}
QPushButton:disabled, QToolButton:disabled {
    color: %5;
    background: %2;
}
QPushButton#GiveawayModeLeft {
    border-right: none;
    border-top-right-radius: 0;
    border-bottom-right-radius: 0;
}
QPushButton#GiveawayModeRight {
    border-left: 1px solid %5;
    border-top-left-radius: 0;
    border-bottom-left-radius: 0;
}
QPushButton#GiveawayStartButton,
QPushButton#GiveawayRollButton { font-weight: 600; min-width: 104px; }
)")
                                  .arg(background.name(), panel.name(),
                                       field.name(), text.name(), muted.name(),
                                       border.name(), hover.name(),
                                       selected.name()));

    this->updateEntrantList();
    this->applyNumberVisibility(this->numberVisibility_);
}

void GiveawayPopup::themeChangedEvent()
{
    BasePopup::themeChangedEvent();
    this->applyTheme();
}

void GiveawayPopup::showEvent(QShowEvent *event)
{
    BasePopup::showEvent(event);
    this->updateRoundControls();
    if (this->mode_ == Mode::Keyword && !this->running_)
    {
        this->keywordInput_->setFocus(Qt::PopupFocusReason);
    }
}

}  // namespace chatterino
