// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/dialogs/SelectChannelDialog.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/tiktok/TikTokLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/Window.hpp"
#include "widgets/helper/MicroNotebook.hpp"
#include "widgets/splits/Split.hpp"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QRadioButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QVariantAnimation>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <array>

namespace chatterino::detail {

void AutoCheckedRadioButton::focusInEvent(QFocusEvent *event)
{
    this->setChecked(true);
    QRadioButton::focusInEvent(event);
}

}  // namespace chatterino::detail

namespace chatterino {

namespace {

constexpr auto STREAMDATABASE_LOGIN = "streamdatabase";

void setIndicatorModeCombo(QComboBox *combo, PlatformIndicatorMode mode)
{
    if (combo == nullptr)
    {
        return;
    }

    auto index = combo->findData(static_cast<int>(mode));
    if (index < 0)
    {
        index =
            combo->findData(static_cast<int>(PlatformIndicatorMode::LineColor));
    }
    if (index >= 0)
    {
        combo->setCurrentIndex(index);
    }
}

int indicatorModeCountIndex(PlatformIndicatorMode mode)
{
    switch (mode)
    {
        case PlatformIndicatorMode::Badge:
            return 0;
        case PlatformIndicatorMode::LineColor:
            return 1;
        case PlatformIndicatorMode::Both:
            return 2;
        case PlatformIndicatorMode::None:
            return 3;
    }
    return -1;
}

PlatformIndicatorMode indicatorModeFromCountIndex(int index)
{
    switch (index)
    {
        case 0:
            return PlatformIndicatorMode::Badge;
        case 1:
            return PlatformIndicatorMode::LineColor;
        case 2:
            return PlatformIndicatorMode::Both;
        case 3:
            return PlatformIndicatorMode::None;
        default:
            return getSettings()->mergedPlatformIndicatorMode.getEnum();
    }
}

PlatformIndicatorMode addTabDefaultIndicatorMode()
{
    const auto fallback = getSettings()->mergedPlatformIndicatorMode.getEnum();
    std::array<int, 4> counts = {0, 0, 0, 0};

    getApp()->getWindows()->getMainWindow().getNotebook().forEachSplit(
        [&](Split *split) {
            if (split == nullptr || split->isActivityPane())
            {
                return;
            }

            const auto channel = split->getChannel();
            if (!channel || channel->isEmpty())
            {
                return;
            }

            const auto index = indicatorModeCountIndex(
                split->platformIndicatorMode());
            if (index >= 0 && index < static_cast<int>(counts.size()))
            {
                ++counts[static_cast<size_t>(index)];
            }
        });

    int bestIndex = -1;
    int bestCount = 0;
    bool tied = false;
    for (int i = 0; i < static_cast<int>(counts.size()); ++i)
    {
        if (counts[static_cast<size_t>(i)] > bestCount)
        {
            bestIndex = i;
            bestCount = counts[static_cast<size_t>(i)];
            tied = false;
        }
        else if (counts[static_cast<size_t>(i)] == bestCount && bestCount > 0)
        {
            tied = true;
        }
    }

    if (bestIndex < 0 || tied)
    {
        return fallback;
    }

    return indicatorModeFromCountIndex(bestIndex);
}

void populateIndicatorModeCombo(QComboBox *combo,
                                PlatformIndicatorMode defaultMode)
{
    combo->addItem("None", static_cast<int>(PlatformIndicatorMode::None));
    combo->addItem("Highlights",
                   static_cast<int>(PlatformIndicatorMode::LineColor));
    combo->addItem("Logos", static_cast<int>(PlatformIndicatorMode::Badge));
    combo->addItem("Both", static_cast<int>(PlatformIndicatorMode::Both));
    setIndicatorModeCombo(combo, defaultMode);
}

PlatformIndicatorMode indicatorModeFromCombo(const QComboBox *combo,
                                             PlatformIndicatorMode fallback)
{
    if (combo == nullptr)
    {
        return fallback;
    }

    bool ok = false;
    const auto value = combo->currentData().toInt(&ok);
    if (!ok)
    {
        return fallback;
    }

    const auto mode = static_cast<PlatformIndicatorMode>(value);
    switch (mode)
    {
        case PlatformIndicatorMode::None:
        case PlatformIndicatorMode::LineColor:
        case PlatformIndicatorMode::Badge:
        case PlatformIndicatorMode::Both:
            return mode;
        default:
            return fallback;
    }
}

bool isLikelyYouTubeChannelId(const QString &value)
{
    static const QRegularExpression channelIdRegex("^UC[A-Za-z0-9_-]{22}$");
    return channelIdRegex.match(value.trimmed()).hasMatch();
}

bool isLikelyYouTubeVideoId(const QString &value)
{
    static const QRegularExpression videoIdRegex("^[A-Za-z0-9_-]{11}$");
    return videoIdRegex.match(value.trimmed()).hasMatch();
}

QString trimTrailingSlash(QString value)
{
    while (value.size() > 1 && value.endsWith('/'))
    {
        value.chop(1);
    }

    return value;
}

QString maybeExtractYouTubeVideoId(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    if (isLikelyYouTubeVideoId(value))
    {
        return value;
    }

    if (!value.contains("://") &&
        (value.startsWith("www.", Qt::CaseInsensitive) ||
         value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("music.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("youtu.be/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    const QUrl url(value);
    if (!url.isValid())
    {
        return {};
    }

    const auto host = url.host().toLower();
    const auto path = url.path();
    const QUrlQuery query(url);

    if (query.hasQueryItem("v"))
    {
        return query.queryItemValue("v").trimmed();
    }

    if (host.endsWith("youtu.be"))
    {
        return path.sliced(1).section('/', 0, 0).trimmed();
    }

    if (path.startsWith("/live/") || path.startsWith("/shorts/") ||
        path.startsWith("/embed/"))
    {
        return path.section('/', 2, 2).trimmed();
    }

    return {};
}

QString normalizeYouTubeSource(QString value)
{
    value = value.trimmed();
    if (value.isEmpty())
    {
        return {};
    }

    if (value.startsWith('@'))
    {
        return value.section('/', 0, 0).trimmed();
    }

    if (isLikelyYouTubeChannelId(value))
    {
        return value;
    }

    if (const auto videoId = maybeExtractYouTubeVideoId(value);
        !videoId.isEmpty())
    {
        return QString("https://www.youtube.com/watch?v=%1").arg(videoId);
    }

    if (!value.contains("://") && !value.contains('/') &&
        !value.contains('\\'))
    {
        return value;
    }

    if (!value.contains("://") &&
        (value.startsWith("www.", Qt::CaseInsensitive) ||
         value.startsWith("youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("m.youtube.com/", Qt::CaseInsensitive) ||
         value.startsWith("music.youtube.com/", Qt::CaseInsensitive)))
    {
        value.prepend("https://");
    }

    const QUrl url(value);
    if (!url.isValid())
    {
        return {};
    }

    const auto path = trimTrailingSlash(url.path());
    if (url.host().toLower().endsWith("youtube.com") && path.startsWith("/@"))
    {
        return path.section('/', 1, 1).trimmed();
    }
    if (url.host().toLower().endsWith("youtube.com") &&
        path.startsWith("/channel/"))
    {
        const auto channelId = path.section('/', 2, 2).trimmed();
        if (isLikelyYouTubeChannelId(channelId))
        {
            return channelId;
        }
    }

    if (url.host().toLower().endsWith("youtube.com"))
    {
        const auto firstSegment = path.section('/', 1, 1).trimmed();
        if (firstSegment == "c" || firstSegment == "user")
        {
            return path.section('/', 2, 2).trimmed();
        }

        if (!firstSegment.isEmpty() && firstSegment != "watch" &&
            firstSegment != "shorts" && firstSegment != "embed" &&
            firstSegment != "live" && firstSegment != "playlist" &&
            firstSegment != "results" && firstSegment != "feed")
        {
            return QUrl::fromPercentEncoding(firstSegment.toUtf8()).trimmed();
        }
    }

    return {};
}

QString normalizeTikTokSource(const QString &value)
{
    return TikTokLiveChat::normalizeSource(value);
}

constexpr qreal MIN_SLOWER_CHAT_MESSAGES_PER_SECOND = 0.25;
constexpr qreal MAX_SLOWER_CHAT_MESSAGES_PER_SECOND = 20.0;
constexpr int ADD_TAB_DIALOG_WIDTH = 440;

class InstantTooltipButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    bool event(QEvent *event) override
    {
        switch (event->type())
        {
            case QEvent::Enter:
            case QEvent::MouseMove:
                this->showInstantTooltip();
                break;
            case QEvent::ToolTip:
                return true;
            case QEvent::Leave:
            case QEvent::Hide:
            case QEvent::WindowDeactivate:
            case QEvent::MouseButtonPress:
                this->hideInstantTooltip();
                break;
            default:
                break;
        }

        return QToolButton::event(event);
    }

private:
    class HoverInfoPopup final : public QLabel
    {
    public:
        explicit HoverInfoPopup(QWidget *parent)
            : QLabel(parent, Qt::ToolTip | Qt::FramelessWindowHint |
                                 Qt::NoDropShadowWindowHint)
        {
            this->setAttribute(Qt::WA_ShowWithoutActivating);
            this->setAttribute(Qt::WA_TransparentForMouseEvents);
            this->setFocusPolicy(Qt::NoFocus);
        }

        void applyTheme(const QFont &font)
        {
            const auto *theme = getTheme();

            QColor tooltipBackground = theme->window.background;
            tooltipBackground = theme->isLightTheme()
                                    ? tooltipBackground.darker(105)
                                    : tooltipBackground.lighter(120);

            QColor tooltipBorder = theme->splits.header.border;
            if (!tooltipBorder.isValid() || tooltipBorder.alpha() == 0)
            {
                tooltipBorder = theme->window.text;
                tooltipBorder.setAlpha(90);
            }

            this->setFont(font);
            this->setStyleSheet(
                QStringLiteral("QLabel {"
                               " padding: 1px 3px;"
                               " background-color: %1;"
                               " border: 1px solid %2;"
                               " color: %3;"
                               "}")
                    .arg(tooltipBackground.name(QColor::HexArgb),
                         tooltipBorder.name(QColor::HexArgb),
                         theme->window.text.name(QColor::HexArgb)));
        }
    };

    void showInstantTooltip()
    {
        const QString text = this->toolTip().trimmed();
        if (text.isEmpty() || !this->underMouse())
        {
            this->hideInstantTooltip();
            return;
        }

        if (this->tooltipPopup_ == nullptr)
        {
            this->tooltipPopup_ = new HoverInfoPopup(this);
        }

        this->tooltipPopup_->applyTheme(QToolTip::font());
        this->tooltipPopup_->setText(text);
        this->tooltipPopup_->adjustSize();

        constexpr int tooltipVerticalGap = 3;
        const auto popupSize = this->tooltipPopup_->sizeHint();
        const QPoint tooltipTopLeft =
            this->mapToGlobal(QPoint(
                (this->width() - popupSize.width()) / 2,
                -popupSize.height() - tooltipVerticalGap));

        this->tooltipPopup_->move(tooltipTopLeft);
        this->tooltipPopup_->show();
        this->tooltipPopup_->raise();
    }

    void hideInstantTooltip()
    {
        if (this->tooltipPopup_ != nullptr)
        {
            this->tooltipPopup_->hide();
        }
    }

    HoverInfoPopup *tooltipPopup_{};
};

QString buildDialogTooltipStyleSheet()
{
    const auto *theme = getTheme();

    QColor tooltipBackground = theme->window.background;
    tooltipBackground = theme->isLightTheme()
                            ? tooltipBackground.darker(105)
                            : tooltipBackground.lighter(120);

    QColor tooltipBorder = theme->splits.header.border;
    if (!tooltipBorder.isValid() || tooltipBorder.alpha() == 0)
    {
        tooltipBorder = theme->window.text;
        tooltipBorder.setAlpha(90);
    }

    return QStringLiteral(
               "QToolTip {"
               " padding: 1px 3px;"
               " background-color: %1;"
               " border: 1px solid %2;"
               " color: %3;"
               " font-size: 11px;"
               "}")
        .arg(tooltipBackground.name(QColor::HexArgb),
             tooltipBorder.name(QColor::HexArgb),
             theme->window.text.name(QColor::HexArgb));
}

QString addTabSectionStyleSheet()
{
    return QStringLiteral(
        "QFrame#AddTabSection {"
        " border: 1px solid rgba(90, 90, 90, 75);"
        " background: transparent;"
        "}");
}

QToolButton *createInfoButton(QWidget *parent, const QString &tooltip)
{
    auto *button = new InstantTooltipButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setText("i");
    button->setFixedSize(12, 12);
    button->setToolTip(tooltip);
    button->setStyleSheet(
        "QToolButton {"
        " color: palette(mid);"
        " border: 1px solid palette(mid);"
        " border-radius: 6px;"
        " background: transparent;"
        " padding: 0px;"
        " font-size: 8px;"
        " font-weight: 600;"
        "}");
    return button;
}

QWidget *createLabelWithInfo(const QString &labelText, const QString &tooltip,
                             QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    auto *label = new QLabel(labelText, container);
    layout->addWidget(label);
    layout->addWidget(createInfoButton(container, tooltip), 0,
                      Qt::AlignVCenter);
    layout->addStretch(1);
    return container;
}

QWidget *createCheckboxRow(QCheckBox *checkbox, const QString &tooltip,
                           QWidget *parent)
{
    auto *container = new QWidget(parent);
    auto *layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    layout->addWidget(checkbox);
    layout->addWidget(createInfoButton(container, tooltip), 0,
                      Qt::AlignVCenter);
    layout->addStretch(1);
    return container;
}

void applyAnimatedRowProgress(QWidget *widget, qreal progress)
{
    if (widget == nullptr)
    {
        return;
    }

    progress = std::clamp(progress, 0.0, 1.0);
    auto *effect =
        qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());

    if (progress >= 1.0)
    {
        if (effect != nullptr)
        {
            widget->setGraphicsEffect(nullptr);
        }
        widget->setMaximumHeight(QWIDGETSIZE_MAX);
        widget->show();
        return;
    }

    if (effect == nullptr)
    {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }

    effect->setOpacity(progress);
    widget->setMaximumHeight(QWIDGETSIZE_MAX);

    if (progress <= 0.0)
    {
        widget->hide();
        return;
    }

    widget->show();
}

}  // namespace

SelectChannelDialog::SelectChannelDialog(bool showSpecialPage, QWidget *parent)
    : SelectChannelDialog(showSpecialPage, false, parent)
{
}

SelectChannelDialog::SelectChannelDialog(bool showSpecialPage,
                                         bool allowFolderSelection,
                                         QWidget *parent)
    : BaseWindow(
          {
              BaseWindow::Flags::EnableCustomFrame,
              BaseWindow::Flags::Dialog,
              BaseWindow::DisableLayoutSave,
              BaseWindow::BoundsCheckOnShow,
          },
          parent)
    , selectedChannel_(Channel::getEmpty())
{
    this->setWindowTitle("Select a channel to join");

    auto &ui = this->ui_;
    auto *rootLayout = new QVBoxLayout(this->getLayoutContainer());
    rootLayout->setContentsMargins({});
    rootLayout->setSpacing(4);

    ui.notebook = new MicroNotebook(this->getLayoutContainer());
    ui.notebook->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    rootLayout->addWidget(ui.notebook);

    ui.mergedPage = new QWidget;
    ui.mergedPage->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto *mergedLayout = new QVBoxLayout(ui.mergedPage);
    mergedLayout->setContentsMargins(6, 6, 6, 0);
    mergedLayout->setAlignment(Qt::AlignTop);
    mergedLayout->setSpacing(5);

    auto *intro = new QLabel(
        "By default, Twitch and Kick use the tab name as the channel name. "
        "Override only when names differ, and add a YouTube @handle/video "
        "link or TikTok @username/live URL for read-only chat.");
    intro->setWordWrap(true);
    mergedLayout->addWidget(intro);

    auto *baseGroup = new QFrame;
    baseGroup->setObjectName(QStringLiteral("AddTabSection"));
    baseGroup->setFrameShape(QFrame::NoFrame);
    baseGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    baseGroup->setStyleSheet(addTabSectionStyleSheet());
    auto *baseLayout = new QFormLayout(baseGroup);
    baseLayout->setContentsMargins(8, 8, 8, 8);
    baseLayout->setHorizontalSpacing(6);
    baseLayout->setVerticalSpacing(4);

    ui.tabName = new QLineEdit();
    ui.tabName->setPlaceholderText("Streamer / tab name");
    baseLayout->addRow("Tab name", ui.tabName);
    mergedLayout->addWidget(baseGroup);

    auto *platformGroup = new QFrame;
    platformGroup->setObjectName(QStringLiteral("AddTabSection"));
    platformGroup->setFrameShape(QFrame::NoFrame);
    platformGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    platformGroup->setStyleSheet(addTabSectionStyleSheet());
    auto *platformLayout = new QFormLayout(platformGroup);
    platformLayout->setContentsMargins(8, 8, 8, 8);
    platformLayout->setHorizontalSpacing(6);
    platformLayout->setVerticalSpacing(4);

    ui.enableTwitch = new QCheckBox("Enable Twitch");
    ui.enableTwitch->setChecked(true);
    platformLayout->addRow(ui.enableTwitch);
    ui.twitchName = new QLineEdit();
    platformLayout->addRow("Twitch channel", ui.twitchName);

    ui.enableKick = new QCheckBox("Enable Kick");
    ui.enableKick->setChecked(true);
    platformLayout->addRow(ui.enableKick);
    ui.kickName = new QLineEdit();
    platformLayout->addRow("Kick channel", ui.kickName);

    ui.enableYouTube = new QCheckBox("Enable YouTube");
    platformLayout->addRow(ui.enableYouTube);
    ui.youtubeUrl = new QLineEdit();
    ui.youtubeUrl->setPlaceholderText("@handle or any YouTube video link");
    platformLayout->addRow("YouTube", ui.youtubeUrl);

    ui.enableTikTok = new QCheckBox("Enable TikTok");
    platformLayout->addRow(ui.enableTikTok);
    ui.tiktokSource = new QLineEdit();
    ui.tiktokSource->setPlaceholderText("@username or TikTok live URL");
    platformLayout->addRow("TikTok", ui.tiktokSource);
    ui.tiktokShowJoinMessages = new QCheckBox("Show users joining");
    ui.tiktokShowJoinMessages->setChecked(true);
    const auto tiktokJoinMessagesTooltip = QStringLiteral(
        "Show a chat message when a viewer joins the TikTok LIVE.");
    platformLayout->addRow(createCheckboxRow(ui.tiktokShowJoinMessages,
                                             tiktokJoinMessagesTooltip, this));

    ui.indicatorMode = new QComboBox();
    populateIndicatorModeCombo(ui.indicatorMode,
                               getSettings()->mergedPlatformIndicatorMode
                                   .getEnum());
    const auto platformStyleTooltip = QStringLiteral(
        "Show no platform indicator, platform color, logo, or both.");
    platformLayout->addRow(
        createLabelWithInfo("Platform style", platformStyleTooltip, this),
        ui.indicatorMode);

    ui.enableActivity = new QCheckBox("Enable activity tab");
    const auto activityTabTooltip = QStringLiteral(
        "Open a linked activity tab for this merged tab.");
    platformLayout->addRow(
        createCheckboxRow(ui.enableActivity, activityTabTooltip, this));
    ui.filterActivity = new QCheckBox("Filter activity");
    const auto filterActivityTooltip = QStringLiteral(
        "Hide activity alerts from main chat.");
    platformLayout->addRow(
        createCheckboxRow(ui.filterActivity, filterActivityTooltip, this));

    ui.slowerChat = new QCheckBox("Slower chat");
    const auto slowerChatTooltip = QStringLiteral(
        "Queue messages and release them at a fixed rate. (This is not "
        "slow mode and does not affect viewers.)");
    platformLayout->addRow(
        createCheckboxRow(ui.slowerChat, slowerChatTooltip, this));

    ui.slowerChatRate = new QDoubleSpinBox();
    ui.slowerChatRate->setDecimals(0);
    ui.slowerChatRate->setRange(1.0, MAX_SLOWER_CHAT_MESSAGES_PER_SECOND);
    ui.slowerChatRate->setSingleStep(1.0);
    ui.slowerChatRate->setButtonSymbols(QAbstractSpinBox::NoButtons);
    ui.slowerChatRate->setFixedWidth(32);
    const auto slowerChatRateTooltip = QStringLiteral(
        "How many queued messages to show each second.");
    ui.slowerChatRateLabel = createLabelWithInfo("Messages per second",
                                                 slowerChatRateTooltip, this);
    ui.slowerChatRateField = ui.slowerChatRate;
    platformLayout->addRow(ui.slowerChatRateLabel, ui.slowerChatRateField);

    ui.viewerCount = new QCheckBox("Viewer count");
    const auto viewerCountTooltip = QStringLiteral(
        "Show the viewer count in this tab header.");
    platformLayout->addRow(
        createCheckboxRow(ui.viewerCount, viewerCountTooltip, this));

    ui.streamDatabaseBadgeFeed = new QCheckBox("Badge feed");
    const auto streamDatabaseBadgeFeedTooltip = QStringLiteral(
        "Show the StreamDatabase badge feed on StreamDatabase tabs.");
    ui.streamDatabaseBadgeFeedRow = createCheckboxRow(
        ui.streamDatabaseBadgeFeed, streamDatabaseBadgeFeedTooltip, this);
    platformLayout->addRow(ui.streamDatabaseBadgeFeedRow);

    mergedLayout->addWidget(platformGroup);

    QObject::connect(ui.enableTwitch, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableKick, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableYouTube, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.enableTikTok, &QCheckBox::toggled, this,
                     [this](bool) { this->syncMergedFieldState(); });
    QObject::connect(ui.slowerChat, &QCheckBox::toggled, this,
                     [this](bool) { this->updateSlowerChatVisibility(); });
    QObject::connect(ui.tabName, &QLineEdit::textChanged, this,
                     [this] {
                         this->updatePlatformOverridePlaceholders();
                         this->updateStreamDatabaseBadgeFeedVisibility();
                     });
    QObject::connect(ui.twitchName, &QLineEdit::textChanged, this,
                     [this] { this->updateStreamDatabaseBadgeFeedVisibility(); });
    QObject::connect(ui.enableActivity, &QCheckBox::toggled, this,
                     [this](bool enabled) {
                         if (enabled && this->ui_.filterActivity != nullptr &&
                             !this->ui_.filterActivity->isChecked())
                         {
                             this->ui_.filterActivity->setChecked(true);
                         }
                     });

    ui.notebook->addPage(ui.mergedPage, "Merged");

    if (showSpecialPage)
    {
        ui.specialPage = new QWidget;
        auto *specialLayout = new QVBoxLayout(ui.specialPage);
        specialLayout->setContentsMargins(6, 6, 6, 0);
        specialLayout->setAlignment(Qt::AlignTop);
        specialLayout->setSpacing(5);
        auto *specialIntro = new QLabel(
            "Utility tabs are not tied to a specific streamer tab. Mentions "
            "include platform mentions and Twitch/Kick username keywords.");
        specialIntro->setWordWrap(true);
        specialIntro->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        specialIntro->setSizePolicy(QSizePolicy::Preferred,
                                    QSizePolicy::Maximum);
        specialLayout->addWidget(specialIntro);

        ui.whispers = new detail::AutoCheckedRadioButton("Whispers");
        ui.mentions = new detail::AutoCheckedRadioButton("Mentions");
        ui.watching = new detail::AutoCheckedRadioButton("Watching");
        ui.live = new detail::AutoCheckedRadioButton("Live");
        ui.automod = new detail::AutoCheckedRadioButton("AutoMod");

        if (allowFolderSelection)
        {
            ui.folder = new detail::AutoCheckedRadioButton("Folder");
            specialLayout->addWidget(ui.folder);
        }
        specialLayout->addWidget(ui.whispers);
        specialLayout->addWidget(ui.mentions);
        specialLayout->addWidget(ui.watching);
        specialLayout->addWidget(ui.live);
        specialLayout->addWidget(ui.automod);

        ui.notebook->addPage(ui.specialPage, "More");
    }
    else
    {
        ui.notebook->setShowHeader(false);
    }

    auto *buttonBox =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    buttonBox->setContentsMargins({6, 0, 6, 6});
    rootLayout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, this, [this] {
        this->ok();
    });
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, this, [this] {
        this->close();
    });

    this->setMergedDefaults();
    this->syncMergedFieldState();
    ui.notebook->onCurrentChanged(this, [this] {
        this->updateDialogSize();
    });
    this->updateSlowerChatVisibility(false);
    this->addShortcuts();
    this->themeChangedEvent();
    this->updateDialogSize();
}

void SelectChannelDialog::ok()
{
    const auto built = this->ui_.notebook->isSelected(this->ui_.specialPage)
                           ? this->buildSpecialSelection()
                           : this->buildMergedSelection();
    if (!built)
    {
        return;
    }

    this->hasSelectedChannel_ = true;
    this->close();
}

void SelectChannelDialog::setMergedDefaults()
{
    this->ui_.notebook->select(this->ui_.mergedPage);
    this->ui_.tabName->clear();
    this->ui_.enableActivity->setChecked(false);
    this->ui_.filterActivity->setChecked(false);
    this->ui_.enableTwitch->setChecked(true);
    this->ui_.twitchName->clear();
    this->ui_.enableKick->setChecked(true);
    this->ui_.kickName->clear();
    this->ui_.enableYouTube->setChecked(false);
    this->ui_.youtubeUrl->clear();
    this->ui_.enableTikTok->setChecked(false);
    this->ui_.tiktokSource->clear();
    this->ui_.tiktokShowJoinMessages->setChecked(true);
    this->ui_.slowerChat->setChecked(false);
    this->ui_.slowerChatRate->setValue(5.0);
    this->ui_.viewerCount->setChecked(
        getSettings()->headerViewerCount.getValue());
    this->ui_.streamDatabaseBadgeFeed->setChecked(true);
    setIndicatorModeCombo(this->ui_.indicatorMode, addTabDefaultIndicatorMode());
    this->updatePlatformOverridePlaceholders();
}

void SelectChannelDialog::loadMergedDefaultsFromChannel(
    const IndirectChannel &indirectChannel)
{
    const auto &channel = indirectChannel.get();
    if (!channel)
    {
        this->setMergedDefaults();
        return;
    }

    this->setMergedDefaults();
    this->ui_.notebook->select(this->ui_.mergedPage);

    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        const auto &config = merged->config();
        this->ui_.tabName->setText(config.tabName);
        this->ui_.enableTwitch->setChecked(config.twitchEnabled);
        this->ui_.twitchName->setText(config.twitchChannelName);
        this->ui_.enableKick->setChecked(config.kickEnabled);
        this->ui_.kickName->setText(config.kickChannelName);
        this->ui_.enableYouTube->setChecked(config.youtubeEnabled);
        this->ui_.youtubeUrl->setText(
            normalizeYouTubeSource(config.youtubeStreamUrl).isEmpty()
                ? config.youtubeStreamUrl
                : normalizeYouTubeSource(config.youtubeStreamUrl));
        this->ui_.enableTikTok->setChecked(config.tiktokEnabled);
        this->ui_.tiktokSource->setText(
            normalizeTikTokSource(config.tiktokSource).isEmpty()
                ? config.tiktokSource
                : normalizeTikTokSource(config.tiktokSource));
        this->ui_.tiktokShowJoinMessages->setChecked(
            config.tiktokShowJoinMessages);
    }
    else if (indirectChannel.getType() == Channel::Type::Twitch)
    {
        this->ui_.tabName->setText(channel->getName());
        this->ui_.enableTwitch->setChecked(true);
        this->ui_.enableKick->setChecked(false);
    }
    else if (indirectChannel.getType() == Channel::Type::Kick)
    {
        this->ui_.tabName->setText(channel->getName());
        this->ui_.enableTwitch->setChecked(false);
        this->ui_.enableKick->setChecked(true);
    }

    this->syncMergedFieldState();
}

void SelectChannelDialog::setSelectedChannel(
    std::optional<IndirectChannel> channel_)
{
    this->hasSelectedChannel_ = false;

    if (!channel_.has_value())
    {
        this->setMergedDefaults();
        return;
    }

    const auto &indirectChannel = channel_.value();
    assert(indirectChannel.get());

    this->selectedChannel_ = indirectChannel;

    switch (indirectChannel.getType())
    {
        case Channel::Type::TwitchWhispers:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.whispers->setChecked(true);
            break;
        case Channel::Type::TwitchMentions:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.mentions->setChecked(true);
            break;
        case Channel::Type::TwitchWatching:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.watching->setChecked(true);
            break;
        case Channel::Type::TwitchLive:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.live->setChecked(true);
            break;
        case Channel::Type::TwitchAutomod:
            if (this->ui_.specialPage == nullptr)
            {
                this->setMergedDefaults();
                break;
            }
            this->ui_.notebook->select(this->ui_.specialPage);
            this->ui_.automod->setChecked(true);
            break;
        default:
            this->loadMergedDefaultsFromChannel(indirectChannel);
            break;
    }
}

void SelectChannelDialog::setActivityPaneEnabled(bool enabled)
{
    if (this->ui_.enableActivity)
    {
        this->ui_.enableActivity->setChecked(enabled);
    }
}

void SelectChannelDialog::setPlatformIndicatorMode(PlatformIndicatorMode mode)
{
    if (this->ui_.indicatorMode)
    {
        setIndicatorModeCombo(this->ui_.indicatorMode, mode);
    }
}

void SelectChannelDialog::setFilterActivity(bool enabled)
{
    if (this->ui_.filterActivity)
    {
        this->ui_.filterActivity->setChecked(enabled);
    }
}

void SelectChannelDialog::setSlowerChatEnabled(bool enabled)
{
    if (this->ui_.slowerChat)
    {
        this->ui_.slowerChat->setChecked(enabled);
        this->updateSlowerChatVisibility(false);
    }
}

void SelectChannelDialog::setSlowerChatMessagesPerSecond(qreal value)
{
    if (this->ui_.slowerChatRate)
    {
        this->ui_.slowerChatRate->setValue(value);
    }
}

void SelectChannelDialog::setViewerCountEnabled(bool enabled)
{
    if (this->ui_.viewerCount)
    {
        this->ui_.viewerCount->setChecked(enabled);
    }
}

void SelectChannelDialog::setStreamDatabaseBadgeFeedVisible(bool visible)
{
    if (this->ui_.streamDatabaseBadgeFeed)
    {
        this->ui_.streamDatabaseBadgeFeed->setChecked(visible);
    }
}

IndirectChannel SelectChannelDialog::getSelectedChannel() const
{
    return this->selectedChannel_;
}

bool SelectChannelDialog::activityPaneEnabled() const
{
    return this->ui_.enableActivity != nullptr &&
           this->ui_.enableActivity->isChecked();
}

bool SelectChannelDialog::filterActivity() const
{
    return this->ui_.filterActivity != nullptr &&
           this->ui_.filterActivity->isChecked();
}

PlatformIndicatorMode SelectChannelDialog::platformIndicatorMode() const
{
    if (this->ui_.indicatorMode == nullptr)
    {
        return getSettings()->mergedPlatformIndicatorMode.getEnum();
    }

    return indicatorModeFromCombo(
        this->ui_.indicatorMode,
        getSettings()->mergedPlatformIndicatorMode.getEnum());
}

bool SelectChannelDialog::slowerChatEnabled() const
{
    return this->ui_.slowerChat != nullptr && this->ui_.slowerChat->isChecked();
}

qreal SelectChannelDialog::slowerChatMessagesPerSecond() const
{
    return this->ui_.slowerChatRate != nullptr ? this->ui_.slowerChatRate->value()
                                               : 5.0;
}

bool SelectChannelDialog::viewerCountEnabled() const
{
    return this->ui_.viewerCount != nullptr &&
           this->ui_.viewerCount->isChecked();
}

bool SelectChannelDialog::streamDatabaseBadgeFeedVisible() const
{
    return this->ui_.streamDatabaseBadgeFeed == nullptr ||
           this->ui_.streamDatabaseBadgeFeed->isChecked();
}

bool SelectChannelDialog::hasSeletedChannel() const
{
    return this->hasSelectedChannel_;
}

bool SelectChannelDialog::selectedFolder() const
{
    return this->selectedFolder_;
}

void SelectChannelDialog::updateDialogSize()
{
    this->setFixedWidth(ADD_TAB_DIALOG_WIDTH);
    this->setMinimumHeight(0);
    this->setMaximumHeight(QWIDGETSIZE_MAX);

    if (this->ui_.notebook != nullptr)
    {
        this->ui_.notebook->setMinimumHeight(0);
        this->ui_.notebook->setMaximumHeight(QWIDGETSIZE_MAX);
        this->ui_.notebook->updateGeometry();
        this->ui_.notebook->setFixedHeight(
            this->ui_.notebook->sizeHint().height());
    }

    if (auto *layout = this->getLayoutContainer()->layout())
    {
        layout->invalidate();
        layout->activate();
    }

    const auto targetHeight =
        this->layout()->sizeHint().expandedTo(this->layout()->minimumSize())
            .height();
    this->setFixedSize(ADD_TAB_DIALOG_WIDTH, targetHeight);
}

void SelectChannelDialog::syncMergedFieldState()
{
    this->ui_.twitchName->setEnabled(this->ui_.enableTwitch->isChecked());
    this->ui_.kickName->setEnabled(this->ui_.enableKick->isChecked());
    this->ui_.youtubeUrl->setEnabled(this->ui_.enableYouTube->isChecked());
    this->ui_.tiktokSource->setEnabled(this->ui_.enableTikTok->isChecked());
    this->ui_.tiktokShowJoinMessages->setEnabled(
        this->ui_.enableTikTok->isChecked());
    this->updateStreamDatabaseBadgeFeedVisibility();
}

void SelectChannelDialog::updatePlatformOverridePlaceholders()
{
    auto placeholder = this->ui_.tabName->text().trimmed();
    if (placeholder.isEmpty())
    {
        placeholder = QStringLiteral("Same as tab name");
    }

    this->ui_.twitchName->setPlaceholderText(placeholder);
    this->ui_.kickName->setPlaceholderText(placeholder);
}

void SelectChannelDialog::updateStreamDatabaseBadgeFeedVisibility()
{
    if (this->ui_.streamDatabaseBadgeFeedRow == nullptr)
    {
        return;
    }

    const auto tabName = this->ui_.tabName->text().trimmed();
    const auto twitchName = this->ui_.twitchName->text().trimmed();
    const auto effectiveTwitchName = twitchName.isEmpty() ? tabName : twitchName;
    const bool visible =
        this->ui_.enableTwitch->isChecked() &&
        effectiveTwitchName.compare(QString::fromLatin1(STREAMDATABASE_LOGIN),
                                    Qt::CaseInsensitive) == 0;

    const bool rowShown = !this->ui_.streamDatabaseBadgeFeedRow->isHidden();
    if (rowShown == visible)
    {
        return;
    }

    if (!rowShown && visible)
    {
        setIndicatorModeCombo(this->ui_.indicatorMode,
                              PlatformIndicatorMode::LineColor);
    }
    this->ui_.streamDatabaseBadgeFeedRow->setVisible(visible);
    this->updateDialogSize();
}

void SelectChannelDialog::applySlowerChatRateVisibilityProgress(qreal progress)
{
    this->ui_.slowerChatRateVisibilityProgress = progress;
    applyAnimatedRowProgress(this->ui_.slowerChatRateLabel, progress);
    applyAnimatedRowProgress(this->ui_.slowerChatRateField, progress);

    if (this->isVisible())
    {
        this->updateDialogSize();
    }
}

void SelectChannelDialog::updateSlowerChatVisibility(bool animate)
{
    const bool visible =
        this->ui_.slowerChat != nullptr && this->ui_.slowerChat->isChecked();

    if (this->ui_.slowerChatRateLabel == nullptr ||
        this->ui_.slowerChatRateField == nullptr)
    {
        return;
    }

    const qreal targetProgress = visible ? 1.0 : 0.0;
    if (this->ui_.slowerChatRateAnimation == nullptr)
    {
        this->ui_.slowerChatRateAnimation = new QVariantAnimation(this);
        this->ui_.slowerChatRateAnimation->setDuration(160);
        this->ui_.slowerChatRateAnimation->setEasingCurve(
            QEasingCurve::InOutCubic);
        QObject::connect(this->ui_.slowerChatRateAnimation,
                         &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
                             this->applySlowerChatRateVisibilityProgress(
                                 value.toReal());
                         });
    }

    if (!animate || !this->isVisible())
    {
        this->ui_.slowerChatRateAnimation->stop();
        this->applySlowerChatRateVisibilityProgress(targetProgress);
        return;
    }

    if (qFuzzyCompare(this->ui_.slowerChatRateVisibilityProgress, targetProgress))
    {
        this->applySlowerChatRateVisibilityProgress(targetProgress);
        return;
    }

    this->ui_.slowerChatRateAnimation->stop();
    this->ui_.slowerChatRateAnimation->setStartValue(
        this->ui_.slowerChatRateVisibilityProgress);
    this->ui_.slowerChatRateAnimation->setEndValue(targetProgress);
    this->ui_.slowerChatRateAnimation->start();
}

bool SelectChannelDialog::buildMergedSelection()
{
    this->selectedFolder_ = false;

    const auto youtubeChannel = normalizeYouTubeSource(this->ui_.youtubeUrl->text());
    const auto tiktokSource = normalizeTikTokSource(this->ui_.tiktokSource->text());
    MergedChannelConfig config{
        .tabName = this->ui_.tabName->text().trimmed(),
        .twitchEnabled = this->ui_.enableTwitch->isChecked(),
        .twitchChannelName = this->ui_.twitchName->text().trimmed(),
        .kickEnabled = this->ui_.enableKick->isChecked(),
        .kickChannelName = this->ui_.kickName->text().trimmed(),
        .youtubeEnabled = this->ui_.enableYouTube->isChecked(),
        .youtubeStreamUrl = youtubeChannel,
        .tiktokEnabled = this->ui_.enableTikTok->isChecked(),
        .tiktokSource = tiktokSource,
        .tiktokShowJoinMessages =
            this->ui_.tiktokShowJoinMessages->isChecked(),
    };

    if (!config.twitchEnabled && !config.kickEnabled && !config.youtubeEnabled &&
        !config.tiktokEnabled)
    {
        QMessageBox::warning(this, "Select a platform",
                             "Enable at least one platform for this merged tab.");
        return false;
    }

    if (config.twitchEnabled && config.effectiveTwitchChannelName().isEmpty())
    {
        QMessageBox::warning(
            this, "Missing Twitch channel",
            "Enter a tab name or Twitch override before enabling Twitch.");
        return false;
    }

    if (config.kickEnabled && config.effectiveKickChannelName().isEmpty())
    {
        QMessageBox::warning(
            this, "Missing Kick channel",
            "Enter a tab name or Kick override before enabling Kick.");
        return false;
    }

    if (config.youtubeEnabled && config.youtubeStreamUrl.isEmpty())
    {
        QMessageBox::warning(
            this, "Missing YouTube source",
            "Enter the streamer's YouTube @handle or any video link from the "
            "desired channel before enabling YouTube.");
        return false;
    }

    if (config.tiktokEnabled && config.tiktokSource.isEmpty())
    {
        QMessageBox::warning(
            this, "Missing TikTok source",
            "Enter the streamer's TikTok @username or live URL before "
            "enabling TikTok.");
        return false;
    }

    if (auto existing =
            std::dynamic_pointer_cast<MergedChannel>(this->selectedChannel_.get()))
    {
        auto existingConfig = existing->config();
        existingConfig.tiktokShowJoinMessages =
            config.tiktokShowJoinMessages;
        if (existingConfig == config)
        {
            existing->setTikTokShowJoinMessages(
                config.tiktokShowJoinMessages);
            return true;
        }
    }

    this->selectedChannel_ = IndirectChannel(
        std::make_shared<MergedChannel>(std::move(config)));
    return true;
}

bool SelectChannelDialog::buildSpecialSelection()
{
    this->selectedFolder_ = false;

    if (this->ui_.folder != nullptr && this->ui_.folder->isChecked())
    {
        this->selectedFolder_ = true;
    }
    else if (this->ui_.watching->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getWatchingChannel();
    }
    else if (this->ui_.mentions->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getMentionsChannel();
    }
    else if (this->ui_.whispers->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getWhispersChannel();
    }
    else if (this->ui_.live->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getLiveChannel();
    }
    else if (this->ui_.automod->isChecked())
    {
        this->selectedChannel_ = getApp()->getTwitch()->getAutomodChannel();
    }
    else
    {
        return false;
    }

    return true;
}

void SelectChannelDialog::closeEvent(QCloseEvent *event)
{
    BaseWindow::closeEvent(event);
    this->closed.invoke();
}

void SelectChannelDialog::themeChangedEvent()
{
    BaseWindow::themeChangedEvent();
    this->setPalette(getTheme()->palette);
    this->setStyleSheet(buildDialogTooltipStyleSheet());
}

void SelectChannelDialog::scaleChangedEvent(float newScale)
{
    BaseWindow::scaleChangedEvent(newScale);

    auto uiFont =
        getApp()->getFonts()->getFont(FontStyle::UiMedium, this->scale());
    this->ui_.tabName->setFont(uiFont);
    this->ui_.twitchName->setFont(uiFont);
    this->ui_.kickName->setFont(uiFont);
    this->ui_.youtubeUrl->setFont(uiFont);
    this->ui_.tiktokSource->setFont(uiFont);
    if (this->ui_.slowerChat)
    {
        this->ui_.slowerChat->setFont(uiFont);
    }
    if (this->ui_.slowerChatRate)
    {
        this->ui_.slowerChatRate->setFont(uiFont);
    }
    if (this->ui_.viewerCount)
    {
        this->ui_.viewerCount->setFont(uiFont);
    }
    if (this->ui_.streamDatabaseBadgeFeed)
    {
        this->ui_.streamDatabaseBadgeFeed->setFont(uiFont);
    }

    this->applySlowerChatRateVisibilityProgress(
        this->ui_.slowerChatRateVisibilityProgress);
}

void SelectChannelDialog::addShortcuts()
{
    HotkeyController::HotkeyMap actions{
        {"accept",
         [this](const std::vector<QString> &) -> QString {
             this->ok();
             return "";
         }},
        {"reject",
         [this](const std::vector<QString> &) -> QString {
             this->close();
             return "";
         }},
        {"scrollPage", nullptr},
        {"search", nullptr},
        {"delete", nullptr},
        {"openTab", nullptr},
    };

    this->shortcuts_ = getApp()->getHotkeys()->shortcutsForCategory(
        HotkeyCategory::PopupWindow, actions, this);
}

}  // namespace chatterino
