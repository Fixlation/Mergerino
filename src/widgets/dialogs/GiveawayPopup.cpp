// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/GiveawayPopup.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/MessageElement.hpp"
#include "providers/bttv/BttvEmotes.hpp"
#include "providers/ffz/FfzEmotes.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/seventv/SeventvEmotes.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchBadge.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"

#include <QAbstractAnimation>
#include <QCheckBox>
#include <QCompleter>
#include <QDateTime>
#include <QEasingCurve>
#include <QEvent>
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
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QStringListModel>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTimer>
#include <QToolButton>
#include <QVariantAnimation>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
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
constexpr int WHEEL_AREA_HEIGHT = 228;
constexpr int KEYWORD_WINDOW_HEIGHT = 625;
constexpr int WHEEL_WINDOW_HEIGHT =
    KEYWORD_WINDOW_HEIGHT + WHEEL_AREA_HEIGHT - ENTRANT_VIEWPORT_HEIGHT;
constexpr int NUMBER_WINDOW_HEIGHT = 455;
constexpr int EMOTE_WINDOW_HEIGHT = 486;
constexpr int REEL_RESULT_MS = 3500;
constexpr int MAX_EMOTE_HINT = 3;

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

class GiveawayPlatformButton final : public QToolButton
{
public:
    explicit GiveawayPlatformButton(MessagePlatform platform,
                                    QWidget *parent = nullptr)
        : QToolButton(parent)
        , icon_(giveawayPlatformIconPath(platform))
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

class CenteredRemoveButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);

        QStyleOptionToolButton option;
        this->initStyleOption(&option);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(option.palette.color(QPalette::ButtonText), 1.5,
                            Qt::SolidLine, Qt::RoundCap));
        const auto center = QRectF(this->rect()).center();
        constexpr qreal HALF_STROKE = 3.0;
        painter.drawLine(center + QPointF(-HALF_STROKE, -HALF_STROKE),
                         center + QPointF(HALF_STROKE, HALF_STROKE));
        painter.drawLine(center + QPointF(HALF_STROKE, -HALF_STROKE),
                         center + QPointF(-HALF_STROKE, HALF_STROKE));
    }
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

// Drops the invisible characters chat clients append to get around duplicate
// message checks, so "Kappa" with a trailing tag still counts as "Kappa"
QString withoutInvisibleCharacters(QString token)
{
    static const auto tagSpace = QStringLiteral("\U000E0000");
    token.remove(tagSpace);
    token.removeIf([](QChar character) {
        const auto code = character.unicode();
        return code == 0x034F || (code >= 0x200B && code <= 0x200D) ||
               code == 0x2060 || code == 0xFEFF || code == 0x2800;
    });
    return token;
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
    this->entrantRefreshTimer_.setSingleShot(true);
    this->entrantRefreshTimer_.setInterval(100);
    QObject::connect(&this->entrantRefreshTimer_, &QTimer::timeout, this,
                     [this] {
                         this->updateEntrantList();
                     });
    this->reelResultTimer_.setSingleShot(true);
    this->reelResultTimer_.setInterval(REEL_RESULT_MS);
    QObject::connect(&this->reelResultTimer_, &QTimer::timeout, this, [this] {
        this->showDrawResult(false);
    });
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
    const auto channelName =
        channel != nullptr ? channel->getName().trimmed() : QString{};
    this->setWindowTitle(
        channelName.isEmpty()
            ? QStringLiteral("Giveaway")
            : QStringLiteral("Giveaway - %1").arg(channelName));

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
        if (this->mode_ == Mode::Emote)
        {
            this->rebuildEmotePool();
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
    const auto addModeButton = [this, modeRow](const QString &text,
                                               const QString &objectName,
                                               const QString &toolTip,
                                               Mode mode) {
        auto *button = new QPushButton(text, this);
        button->setObjectName(objectName);
        button->setCheckable(true);
        button->setToolTip(toolTip);
        modeRow->addWidget(button, 1);
        QObject::connect(button, &QPushButton::clicked, this, [this, mode] {
            this->setMode(mode);
        });
        return button;
    };
    this->keywordModeButton_ = addModeButton(
        QStringLiteral("Default"), QStringLiteral("GiveawayModeLeft"),
        QStringLiteral("Viewers enter with a keyword. The winner is drawn "
                       "with a case-opening reel."),
        Mode::Keyword);
    this->wheelModeButton_ = addModeButton(
        QStringLiteral("Wheel"), QStringLiteral("GiveawayModeWheel"),
        QStringLiteral("Viewers join a prize wheel with a keyword, then you "
                       "spin it."),
        Mode::Wheel);
    this->emoteModeButton_ = addModeButton(
        QStringLiteral("Emote guess"), QStringLiteral("GiveawayModeEmote"),
        QStringLiteral("The first viewer to send the hidden emote wins."),
        Mode::Emote);
    this->numberModeButton_ = addModeButton(
        QStringLiteral("First number"), QStringLiteral("GiveawayModeRight"),
        QStringLiteral("The first viewer to send the hidden number wins."),
        Mode::Number);
    rootLayout->addLayout(modeRow);

    this->modeStack_ = new QStackedWidget(this);
    this->modeStack_->setObjectName(QStringLiteral("GiveawayModeStack"));

    auto *keywordPage = new QWidget(this->modeStack_);
    auto *keywordLayout = new QVBoxLayout(keywordPage);
    keywordLayout->setContentsMargins(0, 2, 0, 2);
    keywordLayout->setSpacing(5);
    this->keywordHelp_ = new QLabel(keywordPage);
    this->keywordHelp_->setObjectName(QStringLiteral("GiveawayHelp"));
    this->keywordHelp_->setWordWrap(true);
    keywordLayout->addWidget(this->keywordHelp_);
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
    this->excludePreviousWinners_ = new QCheckBox(
        QStringLiteral("Exclude previous winners from future rolls"),
        keywordPage);
    this->excludePreviousWinners_->setObjectName(
        QStringLiteral("GiveawayExcludePreviousWinners"));
    this->excludePreviousWinners_->setChecked(true);
    this->excludePreviousWinners_->setToolTip(QStringLiteral(
        "Previous winners remain listed but cannot win again while enabled."));
    keywordLayout->addWidget(this->excludePreviousWinners_);
    QObject::connect(this->excludePreviousWinners_, &QCheckBox::toggled, this,
                     [this] { this->updateRoundControls(); });
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

    auto *emotePage = new QWidget(this->modeStack_);
    auto *emoteLayout = new QVBoxLayout(emotePage);
    emoteLayout->setContentsMargins(0, 2, 0, 2);
    emoteLayout->setSpacing(7);
    emoteLayout->setAlignment(Qt::AlignTop);
    auto *emoteHelp = new QLabel(
        QStringLiteral("First viewer to send the hidden emote on its own wins."),
        emotePage);
    emoteHelp->setObjectName(QStringLiteral("GiveawayHelp"));
    emoteHelp->setWordWrap(true);
    emoteLayout->addWidget(emoteHelp);

    auto *sourceRow = new QHBoxLayout;
    sourceRow->setContentsMargins(0, 0, 0, 0);
    sourceRow->setSpacing(5);
    sourceRow->addWidget(new QLabel(QStringLiteral("Emotes"), emotePage));
    sourceRow->addSpacing(2);
    const std::array<std::pair<EmoteSource, QString>, 4> emoteSources{{
        {EmoteSource::SevenTv, QStringLiteral("7TV")},
        {EmoteSource::Bttv, QStringLiteral("BTTV")},
        {EmoteSource::Ffz, QStringLiteral("FFZ")},
        {EmoteSource::Twitch, QStringLiteral("Twitch")},
    }};
    for (const auto &[source, label] : emoteSources)
    {
        auto *button = new QPushButton(label, emotePage);
        button->setObjectName(QStringLiteral("GiveawayEmoteSource"));
        button->setCheckable(true);
        button->setChecked(true);
        button->setCursor(Qt::PointingHandCursor);
        QObject::connect(button, &QPushButton::toggled, this, [this] {
            this->rebuildEmotePool();
        });
        this->emoteSourceButtons_.insert(source, button);
        sourceRow->addWidget(button);
    }
    sourceRow->addStretch(1);
    this->emoteIncludeGlobals_ =
        new QCheckBox(QStringLiteral("Include global emotes"), emotePage);
    this->emoteIncludeGlobals_->setObjectName(
        QStringLiteral("GiveawayEmoteGlobals"));
    this->emoteIncludeGlobals_->setChecked(true);
    this->emoteIncludeGlobals_->setToolTip(
        QStringLiteral("Also use the global 7TV, BTTV and FFZ emotes. Twitch "
                       "only ever adds the global emotes everyone has."));
    QObject::connect(this->emoteIncludeGlobals_, &QCheckBox::toggled, this,
                     [this] {
                         this->rebuildEmotePool();
                     });
    sourceRow->addWidget(this->emoteIncludeGlobals_);
    emoteLayout->addLayout(sourceRow);

    auto *emoteTargetRow = new QHBoxLayout;
    emoteTargetRow->setContentsMargins(0, 0, 0, 0);
    emoteTargetRow->setSpacing(7);
    this->emoteTile_ = new GiveawayEmoteTile(emotePage);
    emoteTargetRow->addWidget(this->emoteTile_);
    this->emoteInput_ = new GiveawaySecretLineEdit(emotePage);
    this->emoteInput_->setObjectName(QStringLiteral("GiveawayEmoteInput"));
    this->emoteInput_->setPlaceholderText(QStringLiteral("Emote name"));
    this->emoteInput_->setMaxLength(100);
    this->emoteNames_ = new QStringListModel(this);
    this->emoteCompleter_ = new QCompleter(this->emoteNames_, this);
    this->emoteCompleter_->setCaseSensitivity(Qt::CaseInsensitive);
    this->emoteCompleter_->setFilterMode(Qt::MatchContains);
    this->emoteCompleter_->setMaxVisibleItems(8);
    this->emoteInput_->setCompleter(this->emoteCompleter_);
    emoteTargetRow->addWidget(this->emoteInput_, 1);
    this->emoteRandomButton_ =
        new QPushButton(QStringLiteral("Random"), emotePage);
    this->emoteRandomButton_->setCursor(Qt::PointingHandCursor);
    this->emoteRandomButton_->setToolTip(
        QStringLiteral("Pick a random emote from the pool."));
    emoteTargetRow->addWidget(this->emoteRandomButton_);
    this->emoteHintButton_ = new QPushButton(QStringLiteral("Hint"), emotePage);
    this->emoteHintButton_->setObjectName(QStringLiteral("GiveawayEmoteHint"));
    this->emoteHintButton_->setCursor(Qt::PointingHandCursor);
    this->emoteHintButton_->setToolTip(
        QStringLiteral("Reveal a little more about the emote: where it is "
                       "from, its length, then its first letter."));
    // room for the longest label so the row never shifts
    this->emoteHintButton_->setMinimumWidth(
        this->emoteHintButton_->fontMetrics().horizontalAdvance(
            QStringLiteral("Hint %1/%1").arg(MAX_EMOTE_HINT)) +
        28);
    emoteTargetRow->addWidget(this->emoteHintButton_);
    this->emoteVisibilityButton_ = new QPushButton(emotePage);
    this->emoteVisibilityButton_->setObjectName(
        QStringLiteral("GiveawayVisibilityButton"));
    this->emoteVisibilityButton_->setFixedSize(QSize(32, 30));
    this->emoteVisibilityButton_->setIconSize(QSize(16, 16));
    this->emoteVisibilityButton_->setCursor(Qt::PointingHandCursor);
    emoteTargetRow->addWidget(this->emoteVisibilityButton_);
    emoteLayout->addLayout(emoteTargetRow);
    this->emoteHintLabel_ = new QLabel(emotePage);
    this->emoteHintLabel_->setObjectName(QStringLiteral("GiveawayEmoteHintText"));
    emoteLayout->addWidget(this->emoteHintLabel_);
    this->modeStack_->addWidget(emotePage);
    rootLayout->addWidget(this->modeStack_);

    this->emoteVisibilityOpacity_ =
        new QGraphicsOpacityEffect(this->emoteVisibilityButton_);
    this->emoteVisibilityOpacity_->setOpacity(1.0);
    this->emoteVisibilityButton_->setGraphicsEffect(
        this->emoteVisibilityOpacity_);
    QObject::connect(this->emoteInput_, &QLineEdit::textChanged, this, [this] {
        this->emoteHintLevel_ = 0;
        this->wonEmote_.clear();
        this->refreshEmoteDecoys();
        this->updateEmoteTarget();
        this->updateRoundControls();
    });
    QObject::connect(this->emoteRandomButton_, &QPushButton::clicked, this,
                     [this] {
                         this->randomizeEmote();
                     });
    QObject::connect(this->emoteHintButton_, &QPushButton::clicked, this,
                     [this] {
                         this->revealEmoteHint();
                     });
    QObject::connect(this->emoteVisibilityButton_, &QPushButton::clicked, this,
                     [this] {
                         this->toggleNumberVisibility();
                     });

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
            this->emoteVisibilityOpacity_->setOpacity(opacity);
        });
    QObject::connect(this->numberVisibilityAnimation_,
                     &QVariantAnimation::finished, this, [this] {
                         this->pendingNumberVisibility_.reset();
                         this->numberVisibilityOpacity_->setOpacity(1.0);
                         this->emoteVisibilityOpacity_->setOpacity(1.0);
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

    this->drawArea_ = new QWidget(roundCard);
    this->drawArea_->setObjectName(QStringLiteral("GiveawayDrawArea"));
    auto *drawLayout = new QHBoxLayout(this->drawArea_);
    drawLayout->setContentsMargins(0, 0, 0, 0);
    drawLayout->setSpacing(10);
    this->entrantScrollArea_ = new QScrollArea(this->drawArea_);
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
    this->entrantEmptyLabel_ =
        new QLabel(QStringLiteral("No entrants yet"), this->entrantListWidget_);
    this->entrantEmptyLabel_->setObjectName(
        QStringLiteral("GiveawayEntrantEmpty"));
    this->entrantEmptyLabel_->setAlignment(Qt::AlignCenter);
    this->entrantListWidget_->installEventFilter(this);
    QObject::connect(this->entrantScrollArea_->verticalScrollBar(),
                     &QScrollBar::valueChanged, this, [this] {
                         this->renderEntrantRows();
                     });
    this->entrantScrollArea_->setWidget(this->entrantListWidget_);
    this->entrantScrollArea_->setFixedHeight(ENTRANT_VIEWPORT_HEIGHT);
    this->entrantScrollArea_->verticalScrollBar()->setSingleStep(
        ENTRANT_ROW_HEIGHT);
    drawLayout->addWidget(this->entrantScrollArea_, 1);

    this->reel_ = new GiveawayReel(this->drawArea_);
    this->reel_->setFixedHeight(ENTRANT_VIEWPORT_HEIGHT);
    this->reel_->hide();
    this->reel_->onLanded = [this] {
        this->handleDrawLanded();
    };
    this->reel_->onDismissed = [this] {
        this->reelResultTimer_.stop();
        this->showDrawResult(false);
    };
    drawLayout->addWidget(this->reel_, 1);

    this->wheel_ = new GiveawayWheel(this->drawArea_);
    this->wheel_->setFixedSize(220, WHEEL_AREA_HEIGHT);
    this->wheel_->hide();
    this->wheel_->onLanded = [this] {
        this->handleDrawLanded();
    };
    drawLayout->addWidget(this->wheel_);
    roundLayout->addWidget(this->drawArea_);

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
        this->rollWinner();
    });
}

void GiveawayPopup::resetParticipants()
{
    this->entrantRefreshTimer_.stop();
    this->reelResultTimer_.stop();
    this->drawing_ = false;
    this->drawWinnerKey_.clear();
    this->emoteGuessCount_ = 0;
    if (this->reel_ != nullptr)
    {
        this->reel_->stop();
        this->showDrawResult(false);
    }
    if (this->wheel_ != nullptr)
    {
        this->wheel_->stop();
    }
    for (auto &row : this->entrantRows_)
        row.key.clear();
    this->sortedParticipantKeys_.clear();
    this->pendingParticipantKeys_.clear();
    this->participants_.clear();
    this->participantOrder_.clear();
    this->previousWinnerKeys_.clear();
    this->removedParticipantKeys_.clear();
    this->updateEntrantList();
}

void GiveawayPopup::removeParticipant(const QString &participantKey)
{
    const auto participant = this->participants_.find(participantKey);
    if (this->drawing_ || participant == this->participants_.end())
    {
        return;
    }

    this->participants_.erase(participant);
    this->participantOrder_.removeAll(participantKey);
    this->sortedParticipantKeys_.removeAll(participantKey);
    this->pendingParticipantKeys_.removeAll(participantKey);
    this->previousWinnerKeys_.remove(participantKey);
    this->removedParticipantKeys_.insert(participantKey);
    if (this->winnerKey_ == participantKey)
    {
        this->clearWinner();
    }
    this->updateEntrantList();
    this->updateRoundControls();
}

void GiveawayPopup::scheduleEntrantRefresh()
{
    if (!this->entrantRefreshTimer_.isActive())
    {
        this->entrantRefreshTimer_.start();
    }
}

void GiveawayPopup::updateEntrantControls()
{
    if (!this->rollButton_)
        return;  // Initial UI construction.
    const auto eligible =
        this->participants_.size() - (this->excludePreviousWinners_->isChecked()
                                          ? this->previousWinnerKeys_.size()
                                          : 0);
    this->rollButton_->setEnabled(this->drawing_ || eligible > 0);
    this->rollButton_->setText(this->drawing_ ? QStringLiteral("Skip")
                               : this->mode_ == Mode::Wheel
                                   ? QStringLiteral("Spin wheel")
                                   : QStringLiteral("Roll winner"));
    if (this->mode_ == Mode::Emote)
    {
        this->entrantCountLabel_->setText(
            QStringLiteral("%1 guess%2")
                .arg(this->emoteGuessCount_)
                .arg(this->emoteGuessCount_ == 1 ? QString()
                                                 : QStringLiteral("es")));
    }
    else
    {
        this->entrantCountLabel_->setText(
            QStringLiteral("%1 entrant%2")
                .arg(this->participants_.size())
                .arg(this->participants_.size() == 1 ? QString()
                                                     : QStringLiteral("s")));
    }
    if (this->mode_ == Mode::Wheel)
    {
        this->wheel_->setEntries(this->eligibleDrawEntries());
    }
}

void GiveawayPopup::updateEntrantList()
{
    this->entrantRefreshTimer_.stop();
    auto *scroll = this->entrantScrollArea_->verticalScrollBar();
    const int first = scroll->value() / ENTRANT_ROW_HEIGHT;
    const auto anchor = first < this->sortedParticipantKeys_.size()
                            ? this->sortedParticipantKeys_[first]
                            : QString{};
    const int offset = scroll->value() % ENTRANT_ROW_HEIGHT;
    if (!this->pendingParticipantKeys_.isEmpty())
    {
        const auto less = [this](const QString &left, const QString &right) {
            const auto l = this->participants_.constFind(left);
            const auto r = this->participants_.constFind(right);
            const auto byName =
                QString::localeAwareCompare(l->sortName, r->sortName);
            return byName == 0 ? left < right : byName < 0;
        };
        std::sort(this->pendingParticipantKeys_.begin(),
                  this->pendingParticipantKeys_.end(), less);
        const auto oldSize = this->sortedParticipantKeys_.size();
        this->sortedParticipantKeys_.append(this->pendingParticipantKeys_);
        this->pendingParticipantKeys_.clear();
        std::inplace_merge(this->sortedParticipantKeys_.begin(),
                           this->sortedParticipantKeys_.begin() + oldSize,
                           this->sortedParticipantKeys_.end(), less);
    }
    this->entrantListWidget_->setMinimumHeight(
        std::max<int>(this->sortedParticipantKeys_.size(),
                      MAX_VISIBLE_ENTRANTS) *
            ENTRANT_ROW_HEIGHT +
        4);
    if (first > 0 && !anchor.isEmpty())
    {
        const auto position = this->sortedParticipantKeys_.indexOf(anchor);
        if (position >= 0)
            scroll->setValue(position * ENTRANT_ROW_HEIGHT + offset);
    }
    this->renderEntrantRows();
    this->updateEntrantControls();
}

bool GiveawayPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == this->entrantListWidget_ && event->type() == QEvent::Resize)
    {
        this->renderEntrantRows();
    }
    return BasePopup::eventFilter(watched, event);
}

void GiveawayPopup::renderEntrantRows()
{
    if (!this->entrantListWidget_ || !this->entrantEmptyLabel_)
        return;
    const int viewportRows =
        std::max(MAX_VISIBLE_ENTRANTS,
                 this->entrantScrollArea_->maximumHeight() / ENTRANT_ROW_HEIGHT);
    this->entrantEmptyLabel_->setGeometry(0, 2,
                                          this->entrantListWidget_->width(),
                                          viewportRows * ENTRANT_ROW_HEIGHT);
    this->entrantEmptyLabel_->setVisible(
        this->sortedParticipantKeys_.isEmpty());
    const int first = this->entrantScrollArea_->verticalScrollBar()->value() /
                      ENTRANT_ROW_HEIGHT;
    const int count = std::min<int>(
        viewportRows + 2,
        std::max<int>(0, this->sortedParticipantKeys_.size() - first));
    while (this->entrantRows_.size() < count)
    {
        EntrantRow row;
        row.widget = new QWidget(this->entrantListWidget_);
        row.widget->setObjectName(QStringLiteral("GiveawayEntrantRow"));
        auto *layout = new QHBoxLayout(row.widget);
        layout->setContentsMargins(6, 0, 8, 0);
        layout->setSpacing(8);
        row.platform = new QLabel(row.widget);
        row.platform->setObjectName(
            QStringLiteral("GiveawayEntrantPlatformIcon"));
        row.platform->setFixedSize(QSize(18, ENTRANT_ROW_HEIGHT));
        row.platform->setAlignment(Qt::AlignCenter);
        row.name = new QLabel(row.widget);
        row.name->setObjectName(QStringLiteral("GiveawayEntrantName"));
        row.name->setTextFormat(Qt::PlainText);
        row.entries = new QLabel(row.widget);
        row.entries->setObjectName(QStringLiteral("GiveawayEntrantEntries"));
        row.entries->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        row.remove = new CenteredRemoveButton(row.widget);
        row.remove->setObjectName(QStringLiteral("GiveawayRemoveEntrant"));
        row.remove->setAutoRaise(true);
        row.remove->setCursor(Qt::PointingHandCursor);
        row.remove->setFixedSize(20, 20);
        QObject::connect(
            row.remove, &QToolButton::pressed, this, [button = row.remove] {
                button->setProperty("pressedParticipantKey",
                                    button->property("participantKey"));
            });
        QObject::connect(
            row.remove, &QToolButton::clicked, this,
            [this, button = row.remove] {
                const auto key =
                    button->property("pressedParticipantKey").toString();
                QTimer::singleShot(0, this, [this, key] {
                    this->removeParticipant(key);
                });
            });
        layout->addWidget(row.platform);
        layout->addWidget(row.name, 1);
        layout->addWidget(row.entries);
        layout->addWidget(row.remove);
        this->entrantRows_.append(row);
    }
    for (int i = 0; i < this->entrantRows_.size(); ++i)
    {
        auto &row = this->entrantRows_[i];
        if (i >= count)
        {
            row.widget->hide();
            continue;
        }
        const auto &key = this->sortedParticipantKeys_[first + i];
        const auto participant = this->participants_.constFind(key);
        if (participant == this->participants_.cend())
        {
            row.widget->hide();
            continue;
        }
        if (row.key != key)
        {
            row.key = key;
            row.name->setText(participant->author);
            row.platform->setPixmap(
                QIcon(giveawayPlatformIconPath(participant->platform))
                    .pixmap(QSize(16, 16)));
            row.platform->setToolTip(platformName(participant->platform));
            row.remove->setProperty("participantKey", key);
            row.remove->setAccessibleName(
                QStringLiteral("Remove %1 from giveaway")
                    .arg(participant->author));
            row.remove->setToolTip(
                QStringLiteral("Remove %1 from this giveaway")
                    .arg(participant->author));
        }
        auto entryText =
            QStringLiteral("%1 entr%2")
                .arg(participant->entryCount)
                .arg(participant->entryCount == 1 ? QStringLiteral("y")
                                                  : QStringLiteral("ies"));
        if (this->previousWinnerKeys_.contains(key))
            entryText.prepend(QStringLiteral("Won • "));
        row.entries->setText(entryText);
        row.widget->setGeometry(0, 2 + (first + i) * ENTRANT_ROW_HEIGHT,
                                this->entrantListWidget_->width(),
                                ENTRANT_ROW_HEIGHT);
        row.widget->show();
    }
}

void GiveawayPopup::setMode(Mode mode)
{
    if (this->running_ || this->drawing_ || this->mode_ == mode)
    {
        // Clicking the active mode would otherwise uncheck its button
        this->updateModeControls();
        return;
    }
    this->mode_ = mode;
    this->resetParticipants();
    this->clearWinner();
    if (mode == Mode::Emote)
    {
        this->rebuildEmotePool();
    }
    this->setStatus(QStringLiteral("Ready on %1.")
                        .arg(this->selectedPlatformsText()));
    this->updateModeControls();
    this->updateRoundControls();
}

void GiveawayPopup::updateModeControls()
{
    const bool keyword = this->mode_ == Mode::Keyword;
    const bool wheel = this->mode_ == Mode::Wheel;
    const bool entries = keyword || wheel;
    const bool locked = this->running_ || this->drawing_;
    for (const auto &[button, mode] :
         {std::pair{this->keywordModeButton_, Mode::Keyword},
          std::pair{this->wheelModeButton_, Mode::Wheel},
          std::pair{this->emoteModeButton_, Mode::Emote},
          std::pair{this->numberModeButton_, Mode::Number}})
    {
        button->setChecked(this->mode_ == mode);
        button->setEnabled(!locked);
    }
    this->modeStack_->setCurrentIndex(
        entries ? 0 : (this->mode_ == Mode::Number ? 1 : 2));
    this->keywordHelp_->setText(
        wheel ? QStringLiteral("Each viewer joins the wheel once by sending the "
                               "exact keyword.")
              : QStringLiteral(
                    "Each viewer enters once by sending the exact keyword."));
    this->rollButton_->setVisible(entries);
    this->entrantCountLabel_->setVisible(entries ||
                                         this->mode_ == Mode::Emote);
    this->drawArea_->setVisible(entries);
    this->entrantScrollArea_->setFixedHeight(
        wheel ? WHEEL_AREA_HEIGHT : ENTRANT_VIEWPORT_HEIGHT);
    this->wheel_->setVisible(wheel);
    if (!keyword)
    {
        this->reelResultTimer_.stop();
        this->reel_->stop();
    }
    const bool reelShown =
        keyword && (this->reel_->spinning() || this->reel_->landed());
    this->reel_->setVisible(reelShown);
    this->entrantScrollArea_->setVisible(entries && !reelShown);
    this->renderEntrantRows();

    this->modeStack_->setFixedHeight(
        this->modeStack_->currentWidget()->sizeHint().height());
    if (auto *layout = this->root_->layout())
    {
        layout->invalidate();
        layout->activate();
    }
    switch (this->mode_)
    {
        case Mode::Wheel:
            this->setScaleIndependentHeight(WHEEL_WINDOW_HEIGHT);
            break;
        case Mode::Emote:
            this->setScaleIndependentHeight(EMOTE_WINDOW_HEIGHT);
            break;
        case Mode::Number:
            this->setScaleIndependentHeight(NUMBER_WINDOW_HEIGHT);
            break;
        case Mode::Keyword:
        default:
            this->setScaleIndependentHeight(KEYWORD_WINDOW_HEIGHT);
            break;
    }
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
    const bool emoteReady =
        this->emotePool_.contains(this->emoteInput_->text().trimmed());
    bool modeReady = keywordReady;
    QString startText = QStringLiteral("Start giveaway");
    if (this->mode_ == Mode::Emote)
    {
        modeReady = emoteReady;
        startText = QStringLiteral("Start emote hunt");
    }
    else if (this->mode_ == Mode::Number)
    {
        modeReady = numberReady;
        startText = QStringLiteral("Start number hunt");
    }
    const bool canStart = selectedCount > 0 && modeReady;
    this->startButton_->setEnabled(!this->drawing_ &&
                                   (this->running_ || canStart));
    this->startButton_->setText(this->running_ ? QStringLiteral("Stop")
                                               : startText);
    this->updateEntrantControls();

    const bool fieldsEnabled = !this->running_ && !this->drawing_;
    for (auto *button : std::as_const(this->emoteSourceButtons_))
    {
        button->setEnabled(fieldsEnabled);
    }
    this->emoteIncludeGlobals_->setEnabled(fieldsEnabled);
    this->emoteInput_->setEnabled(fieldsEnabled);
    this->emoteRandomButton_->setEnabled(fieldsEnabled);
    this->emoteVisibilityButton_->setEnabled(fieldsEnabled);
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

    if (this->usesKeywordEntries())
    {
        this->activeSubscriberMultiplier_ = this->subscriberLuck_->value();
        this->activeKeyword_ = this->keywordInput_->text().trimmed();
        if (this->activeKeyword_.isEmpty())
        {
            this->setStatus(QStringLiteral("Enter a keyword first."), true);
            return;
        }
    }
    else if (this->mode_ == Mode::Emote)
    {
        const auto target = this->emoteInput_->text().trimmed();
        if (!this->emotePool_.contains(target))
        {
            this->setStatus(
                QStringLiteral("Pick an emote from the pool first."), true);
            return;
        }
        this->activeEmote_ = target;
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
    this->wonEmote_.clear();
    this->updateEmoteTarget();
    this->running_ = true;
    if (this->mode_ == Mode::Keyword)
    {
        this->setStatus(
            QStringLiteral("Entries open on %1 • keyword: %2")
                .arg(this->selectedPlatformsText(), this->activeKeyword_));
    }
    else if (this->mode_ == Mode::Wheel)
    {
        this->setStatus(
            QStringLiteral("Wheel open on %1 • keyword: %2")
                .arg(this->selectedPlatformsText(), this->activeKeyword_));
    }
    else if (this->mode_ == Mode::Emote)
    {
        this->setStatus(
            QStringLiteral("Emote hunt live on %1 • target: %2")
                .arg(this->selectedPlatformsText(),
                     this->numberVisibility_ == NumberVisibility::Visible
                         ? this->activeEmote_
                         : QStringLiteral("hidden")));
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
    this->updateEntrantList();
    if (this->usesKeywordEntries())
    {
        this->setStatus(
            QStringLiteral("Entries closed • %1 entrant%2")
                .arg(this->participants_.size())
                .arg(this->participants_.size() == 1 ? QString()
                                                     : QStringLiteral("s")));
    }
    else if (this->mode_ == Mode::Emote)
    {
        this->setStatus(QStringLiteral("Emote hunt stopped."));
    }
    else
    {
        this->setStatus(QStringLiteral("Number hunt stopped."));
    }
    this->updateRoundControls();
}

bool GiveawayPopup::usesKeywordEntries() const
{
    return this->mode_ == Mode::Keyword || this->mode_ == Mode::Wheel;
}

std::vector<GiveawayDrawEntry> GiveawayPopup::eligibleDrawEntries() const
{
    std::vector<GiveawayDrawEntry> entries;
    entries.reserve(static_cast<size_t>(this->participantOrder_.size()));
    const bool exclude = this->excludePreviousWinners_->isChecked();
    for (const auto &key : this->participantOrder_)
    {
        if (exclude && this->previousWinnerKeys_.contains(key))
        {
            continue;
        }
        const auto participant = this->participants_.constFind(key);
        if (participant == this->participants_.cend())
        {
            continue;
        }
        entries.push_back({
            .key = participant->key,
            .name = participant->author,
            .platform = participant->platform,
            .weight = std::max(1, participant->entryCount),
        });
    }
    return entries;
}

void GiveawayPopup::rollWinner()
{
    if (this->drawing_)
    {
        this->finishDraw();
        return;
    }
    if (!this->usesKeywordEntries() || this->participants_.isEmpty())
    {
        return;
    }

    this->updateEntrantList();
    const auto entries = this->eligibleDrawEntries();
    const auto winner = pickWeightedGiveawayEntry(entries);
    if (winner < 0)
    {
        this->setStatus(QStringLiteral("All entrants have already won."));
        this->updateRoundControls();
        return;
    }

    const auto &key = entries[static_cast<size_t>(winner)].key;
    if (!this->isVisible())
    {
        // Nobody is watching, so skip the show
        this->chooseWinner(key);
        return;
    }

    this->drawing_ = true;
    this->drawWinnerKey_ = key;
    this->reelResultTimer_.stop();
    if (this->mode_ == Mode::Keyword)
    {
        this->showDrawResult(true);
        this->reel_->spin(entries, winner);
        this->setStatus(QStringLiteral("Rolling…"));
    }
    else
    {
        this->wheel_->stop();
        this->wheel_->setEntries(entries);
        this->wheel_->spin(winner);
        this->setStatus(QStringLiteral("Spinning the wheel…"));
    }
    this->updateRoundControls();
}

void GiveawayPopup::finishDraw()
{
    if (this->mode_ == Mode::Keyword)
    {
        this->reel_->finish();
    }
    else
    {
        this->wheel_->finish();
    }
}

void GiveawayPopup::handleDrawLanded()
{
    if (!this->drawing_)
    {
        return;
    }
    this->drawing_ = false;
    const auto key = std::exchange(this->drawWinnerKey_, {});
    if (this->mode_ == Mode::Keyword)
    {
        this->reelResultTimer_.start();
    }
    if (this->participants_.contains(key))
    {
        this->chooseWinner(key);
    }
    else
    {
        this->updateRoundControls();
    }
}

void GiveawayPopup::showDrawResult(bool visible)
{
    const bool reel = visible && this->mode_ == Mode::Keyword;
    if (!reel && !this->reel_->spinning())
    {
        this->reel_->stop();
    }
    this->reel_->setVisible(reel);
    this->entrantScrollArea_->setVisible(!reel);
    // Lay the swap out now so the reel never spins at a stale size
    this->drawArea_->layout()->activate();
    if (!reel)
    {
        this->renderEntrantRows();
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

    this->emoteInput_->setSecret(!visible);
    // Suggestions would give a hidden target away
    this->emoteInput_->setCompleter(visible ? this->emoteCompleter_ : nullptr);
    this->emoteVisibilityButton_->setIcon(QIcon(iconPath));
    this->emoteVisibilityButton_->setAccessibleName(
        visible ? QStringLiteral("Hide target emote")
                : QStringLiteral("Show target emote"));
    this->emoteVisibilityButton_->setToolTip(
        this->numberVisibilityButton_->toolTip());
    this->updateEmoteTarget();
}

void GiveawayPopup::rebuildEmotePool()
{
    if (this->running_)
    {
        return;  // the target has to stay guessable
    }

    this->emotePool_.clear();
    QHash<EmoteSource, int> counts;
    const auto enabled = [this](EmoteSource source) {
        const auto *button = this->emoteSourceButtons_.value(source, nullptr);
        return button != nullptr && button->isChecked();
    };
    const auto add = [this, &counts](const std::shared_ptr<const EmoteMap> &map,
                                     EmoteSource source) {
        if (map == nullptr)
        {
            return;
        }
        for (const auto &[name, emote] : *map)
        {
            if (emote == nullptr || emote->zeroWidth ||
                name.string.isEmpty() || this->emotePool_.contains(name.string))
            {
                continue;
            }
            this->emotePool_.insert(name.string, {emote, source});
            ++counts[source];
        }
    };

    std::shared_ptr<TwitchChannel> twitch;
    std::shared_ptr<KickChannel> kick;
    if (const auto merged =
            std::dynamic_pointer_cast<MergedChannel>(this->channel_))
    {
        twitch = std::dynamic_pointer_cast<TwitchChannel>(
            merged->twitchChannel());
        kick = std::dynamic_pointer_cast<KickChannel>(merged->kickChannel());
    }
    else
    {
        twitch = std::dynamic_pointer_cast<TwitchChannel>(this->channel_);
        kick = std::dynamic_pointer_cast<KickChannel>(this->channel_);
    }

    // Channel emotes first so they win over globals with the same name
    auto *app = getApp();
    const bool globals = this->emoteIncludeGlobals_->isChecked();
    if (enabled(EmoteSource::SevenTv))
    {
        if (twitch != nullptr)
        {
            add(twitch->seventvEmotes(), EmoteSource::SevenTv);
        }
        if (kick != nullptr)
        {
            add(kick->seventvEmotes(), EmoteSource::SevenTv);
        }
    }
    if (twitch != nullptr && enabled(EmoteSource::Bttv))
    {
        add(twitch->bttvEmotes(), EmoteSource::Bttv);
    }
    if (twitch != nullptr && enabled(EmoteSource::Ffz))
    {
        add(twitch->ffzEmotes(), EmoteSource::Ffz);
    }
    if (globals && enabled(EmoteSource::SevenTv))
    {
        add(app->getSeventvEmotes()->globalEmotes(), EmoteSource::SevenTv);
    }
    if (globals && enabled(EmoteSource::Bttv))
    {
        add(app->getBttvEmotes()->emotes(), EmoteSource::Bttv);
    }
    if (globals && enabled(EmoteSource::Ffz))
    {
        add(app->getFfzEmotes()->emotes(), EmoteSource::Ffz);
    }
    if (enabled(EmoteSource::Twitch))
    {
        if (const auto account = app->getAccounts()->twitch.getCurrent())
        {
            const std::shared_ptr<const EmoteMap> twitchGlobals =
                *account->accessGlobalEmotes();
            add(twitchGlobals, EmoteSource::Twitch);
        }
    }

    for (auto it = this->emoteSourceButtons_.cbegin();
         it != this->emoteSourceButtons_.cend(); ++it)
    {
        const auto count = counts.value(it.key());
        if (!it.value()->isChecked())
        {
            it.value()->setToolTip(QStringLiteral("Not in the pool. Click to "
                                                  "add these emotes."));
        }
        else if (count == 0 && it.key() == EmoteSource::Twitch)
        {
            it.value()->setToolTip(
                QStringLiteral("Twitch's global emotes load once you are "
                               "logged in to Twitch."));
        }
        else
        {
            it.value()->setToolTip(
                QStringLiteral("%1 emote%2 in the pool")
                    .arg(count)
                    .arg(count == 1 ? QString() : QStringLiteral("s")));
        }
    }

    auto names = this->emotePool_.keys();
    std::ranges::sort(names, [](const QString &left, const QString &right) {
        const auto order = left.compare(right, Qt::CaseInsensitive);
        return order == 0 ? left < right : order < 0;
    });
    this->emoteNames_->setStringList(names);
    this->refreshEmoteDecoys();
    this->updateEmoteTarget();
    this->updateRoundControls();
}

void GiveawayPopup::refreshEmoteDecoys()
{
    // A dozen random emotes for the hidden reel, never the target itself
    constexpr qsizetype DECOYS = 12;
    const auto target = this->emoteInput_->text().trimmed();
    auto names = this->emoteNames_->stringList();
    names.removeAll(target);
    std::vector<EmotePtr> decoys;
    while (!names.isEmpty() &&
           static_cast<qsizetype>(decoys.size()) < DECOYS)
    {
        const auto index = static_cast<qsizetype>(
            QRandomGenerator::global()->bounded(
                static_cast<quint32>(names.size())));
        decoys.push_back(this->emotePool_.value(names.takeAt(index)).emote);
    }
    this->emoteTile_->setDecoys(std::move(decoys));
}

void GiveawayPopup::randomizeEmote()
{
    if (this->emotePool_.isEmpty())
    {
        this->rebuildEmotePool();
    }
    const auto names = this->emoteNames_->stringList();
    if (names.isEmpty())
    {
        this->setStatus(
            QStringLiteral("No emotes are loaded for this channel yet."), true);
        return;
    }

    const auto current = this->emoteInput_->text().trimmed();
    auto pick = current;
    for (int attempt = 0; attempt < 8 && (pick == current); ++attempt)
    {
        pick = names.at(static_cast<qsizetype>(QRandomGenerator::global()->bounded(
            static_cast<quint32>(names.size()))));
    }
    this->emoteInput_->setText(pick);
}

void GiveawayPopup::updateEmoteTarget()
{
    const auto name = this->emoteInput_->text().trimmed();
    const auto candidate = this->emotePool_.constFind(name);
    const bool found = candidate != this->emotePool_.cend();
    const bool revealed =
        this->numberVisibility_ == NumberVisibility::Visible ||
        (!this->wonEmote_.isEmpty() && this->wonEmote_ == name);
    const bool wonWhileHidden =
        revealed && this->numberVisibility_ != NumberVisibility::Visible;
    this->emoteTile_->setEmote(found ? candidate->emote : nullptr);
    this->emoteTile_->setRevealed(revealed, wonWhileHidden);
    this->emoteTile_->setToolTip(found && revealed ? name : QString());
    this->emoteHintButton_->setEnabled(found &&
                                       this->emoteHintLevel_ < MAX_EMOTE_HINT);
    this->emoteHintButton_->setText(
        this->emoteHintLevel_ == 0
            ? QStringLiteral("Hint")
            : QStringLiteral("Hint %1/%2")
                  .arg(this->emoteHintLevel_)
                  .arg(MAX_EMOTE_HINT));
    this->emoteHintLabel_->setText(this->emoteHintText());
}

void GiveawayPopup::revealEmoteHint()
{
    this->emoteHintLevel_ = std::min(this->emoteHintLevel_ + 1, MAX_EMOTE_HINT);
    this->updateEmoteTarget();
}

QString GiveawayPopup::emoteHintText() const
{
    const auto name = this->emoteInput_->text().trimmed();
    const auto candidate = this->emotePool_.constFind(name);
    const auto poolSize = this->emotePool_.size();
    if (candidate == this->emotePool_.cend() || this->emoteHintLevel_ == 0)
    {
        if (poolSize == 0)
        {
            return QStringLiteral("No emotes loaded for this channel yet.");
        }
        return QStringLiteral("%1 emote%2 in the pool")
            .arg(poolSize)
            .arg(poolSize == 1 ? QString() : QStringLiteral("s"));
    }

    QString source;
    switch (candidate->source)
    {
        case EmoteSource::Bttv:
            source = QStringLiteral("BTTV");
            break;
        case EmoteSource::Ffz:
            source = QStringLiteral("FFZ");
            break;
        case EmoteSource::Twitch:
            source = QStringLiteral("Twitch global");
            break;
        case EmoteSource::SevenTv:
        default:
            source = QStringLiteral("7TV");
            break;
    }
    QStringList hints{QStringLiteral("%1 emote").arg(source)};
    if (this->emoteHintLevel_ >= 2)
    {
        hints.append(QStringLiteral("%1 letters").arg(name.size()));
    }
    if (this->emoteHintLevel_ >= 3)
    {
        hints.append(
            QStringLiteral("starts with \u201C%1\u201D").arg(name.left(1)));
    }
    return QStringLiteral("Hint: ") + hints.join(QStringLiteral(" • "));
}

QString GiveawayPopup::emoteGuess(const QString &text) const
{
    // A guess is a message made of one emote, repeated or not; anything
    // else would let viewers paste the whole emote list at once
    QString guess;
    const auto tokens =
        text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const auto &rawToken : tokens)
    {
        const auto token = withoutInvisibleCharacters(rawToken);
        if (token.isEmpty())
        {
            continue;
        }
        if (guess.isEmpty())
        {
            guess = token;
        }
        else if (token != guess)
        {
            return {};
        }
    }
    return this->emotePool_.contains(guess) ? guess : QString();
}

void GiveawayPopup::handleMessage(const MessagePtr &message)
{
    if (message == nullptr || !this->acceptsPlatform(message->platform) ||
        !this->isEligibleMessage(*message))
    {
        return;
    }

    const auto key = this->participantKey(*message);
    if (this->removedParticipantKeys_.contains(key))
    {
        return;
    }
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

    if (this->usesKeywordEntries())
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
            this->pendingParticipantKeys_.append(key);
            this->scheduleEntrantRefresh();
        }
        return;
    }

    if (this->mode_ == Mode::Emote)
    {
        const auto guess = this->emoteGuess(message->messageText);
        if (guess.isEmpty())
        {
            return;
        }
        ++this->emoteGuessCount_;
        if (guess != this->activeEmote_)
        {
            this->scheduleEntrantRefresh();
            return;
        }
        if (existing == this->participants_.end())
        {
            auto participant = this->participantFromMessage(*message);
            this->appendParticipantMessage(participant, *message);
            this->participants_.insert(key, std::move(participant));
            this->participantOrder_.append(key);
            this->pendingParticipantKeys_.append(key);
        }
        this->running_ = false;
        this->wonEmote_ = this->activeEmote_;
        this->chooseWinner(key,
                           QStringLiteral("guessed %1").arg(this->activeEmote_));
        this->updateEmoteTarget();
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
        this->pendingParticipantKeys_.append(key);
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
    participant.sortName = participant.author.toCaseFolded();
    participant.platform = message.platform;
    participant.entryCount =
        this->usesKeywordEntries() && this->messageIsSubscriber(message)
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
    this->previousWinnerKeys_.insert(participantKey);
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
    this->updateEntrantList();
    this->updateRoundControls();
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
    this->winnerMessages_->document()->setMaximumBlockCount(40);
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
    const GiveawayDrawColors drawColors{
        .background = background,
        .panel = panel,
        .text = text,
        .muted = muted,
        .border = border,
    };
    this->reel_->setColors(drawColors);
    this->wheel_->setColors(drawColors);
    this->emoteTile_->setColors(drawColors);
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
QLabel#GiveawayEntrantCount, QLabel#GiveawayEmoteHintText { color: %5; }
QLabel#GiveawayWinnerLabel { font-weight: 600; }
QLabel#GiveawaySubscriberLuckValue { font-weight: 600; }
QCheckBox#GiveawayExcludePreviousWinners,
QCheckBox#GiveawayEmoteGlobals { color: %5; spacing: 6px; }
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
QToolButton#GiveawayRemoveEntrant {
    background: transparent;
    color: %5;
    border: none;
    border-radius: 3px;
    padding: 0;
}

QToolButton#GiveawayRemoveEntrant:hover {
    background: %7;
    color: #ff6b6b;
}
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
QPushButton#GiveawayModeWheel, QPushButton#GiveawayModeEmote {
    border-left: 1px solid %5;
    border-right: none;
    border-radius: 0;
}
QPushButton#GiveawayModeRight {
    border-left: 1px solid %5;
    border-top-left-radius: 0;
    border-bottom-left-radius: 0;
}
QPushButton#GiveawayEmoteSource { padding: 3px 9px; font-weight: 600; }
QPushButton#GiveawayEmoteSource:!checked { color: %5; }
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
    if (this->mode_ == Mode::Emote)
    {
        // Emote sets load in the background, so pick up anything new
        this->rebuildEmotePool();
    }
    this->updateRoundControls();
    if (this->usesKeywordEntries() && !this->running_)
    {
        this->keywordInput_->setFocus(Qt::PopupFocusReason);
    }
}

}  // namespace chatterino
