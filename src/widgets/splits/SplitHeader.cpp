// SPDX-FileCopyrightText: 2017 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "widgets/splits/SplitHeader.hpp"

#include "Application.hpp"
#include "common/network/NetworkCommon.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/hotkeys/Hotkey.hpp"
#include "controllers/hotkeys/HotkeyCategory.hpp"
#include "controllers/hotkeys/HotkeyController.hpp"
#include "controllers/notifications/NotificationController.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Resources.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/FormatTime.hpp"
#include "util/Helpers.hpp"
#include "util/LayoutHelper.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/ChatterListWidget.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/CommonTexts.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/TooltipWidget.hpp"

#include <QDrag>
#include <QDateTime>
#include <QCursor>
#include <QDesktopServices>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

namespace {

using namespace chatterino;

/// The width of the standard button.
constexpr const int BUTTON_WIDTH = 28;

/// The width of the "Add split" button.
///
/// This matches the scrollbar's full width.
constexpr const int ADD_SPLIT_BUTTON_WIDTH = 16;
constexpr const int TITLE_SETTINGS_BUTTON_WIDTH = 20;
constexpr const int TITLE_SETTINGS_BUTTON_GAP = 4;
constexpr const int TITLE_LABEL_LEFT_PADDING = 2;
constexpr const int TITLE_LABEL_RIGHT_PADDING =
    TITLE_SETTINGS_BUTTON_WIDTH + TITLE_SETTINGS_BUTTON_GAP;
constexpr const int QUEUED_SLOW_CHAT_COUNT_DIGITS = 4;
constexpr auto STREAMDATABASE_LOGIN = "streamdatabase";

class PersistentCheckMenu final : public QMenu
{
public:
    using QMenu::QMenu;

protected:
    void mouseReleaseEvent(QMouseEvent *event) override
    {
        auto *action = this->activeAction();
        if (action != nullptr && action->isEnabled() &&
            action->isCheckable())
        {
            action->setChecked(!action->isChecked());
            event->accept();
            return;
        }

        QMenu::mouseReleaseEvent(event);
    }
};

// 5 minutes
constexpr const qint64 THUMBNAIL_MAX_AGE_MS = 5LL * 60 * 1000;

struct RoomModePresentation {
    QString text;
    QString tooltip;
};

QString roomModeSentence(const QString &mode)
{
    return QStringLiteral("This chat is currently in %1 mode.").arg(mode);
}

RoomModePresentation formatRoomModes(const TwitchChannel::RoomModes &modes)
{
    QStringList labels;
    QStringList tooltips;

    if (modes.r9k)
    {
        labels.append(QStringLiteral("R9K"));
        tooltips.append(roomModeSentence(QStringLiteral("R9K")));
    }
    if (modes.slowMode > 0)
    {
        const auto duration = formatDurationExact(
            std::chrono::seconds{modes.slowMode});
        labels.append(QStringLiteral("Slow"));
        tooltips.append(QStringLiteral("This chat is currently in slow mode "
                                       "with a %1 delay.")
                            .arg(duration));
    }
    if (modes.emoteOnly)
    {
        labels.append(QStringLiteral("Emote-only"));
        tooltips.append(roomModeSentence(QStringLiteral("emote-only")));
    }
    if (modes.submode)
    {
        labels.append(QStringLiteral("Sub-only"));
        tooltips.append(roomModeSentence(QStringLiteral("subscribers-only")));
    }
    if (modes.followerOnly != -1)
    {
        if (modes.followerOnly != 0)
        {
            const auto duration = formatDurationExact(
                std::chrono::minutes{modes.followerOnly});
            labels.append(QStringLiteral("Followers"));
            tooltips.append(
                QStringLiteral("This chat is currently in followers-only mode. "
                               "You must have followed for at least %1 to chat.")
                    .arg(duration));
        }
        else
        {
            labels.append(QStringLiteral("Followers"));
            tooltips.append(roomModeSentence(QStringLiteral("followers-only")));
        }
    }

    return {
        labels.join(QStringLiteral(", ")),
        tooltips.join(QChar('\n')),
    };
}

RoomModePresentation formatRoomModes(const KickChannel::RoomModes &modes)
{
    TwitchChannel::RoomModes twitch{
        .submode = modes.subscribersMode,
        .r9k = false,
        .emoteOnly = modes.emotesMode,
        .followerOnly = -1,
        .slowMode = 0,
    };
    if (modes.followersModeDuration)
    {
        twitch.followerOnly =
            static_cast<int>(modes.followersModeDuration->count());
    }
    if (modes.slowModeDuration)
    {
        twitch.slowMode = static_cast<int>(modes.slowModeDuration->count());
    }
    return formatRoomModes(twitch);
}

void appendRoomModePresentation(RoomModePresentation &target,
                                const RoomModePresentation &source)
{
    if (source.text.isEmpty())
    {
        return;
    }

    auto targetLabels =
        target.text.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    auto targetTooltips = target.tooltip.split(QChar('\n'), Qt::SkipEmptyParts);
    const auto sourceLabels =
        source.text.split(QStringLiteral(", "), Qt::SkipEmptyParts);
    const auto sourceTooltips =
        source.tooltip.split(QChar('\n'), Qt::SkipEmptyParts);

    for (int i = 0; i < sourceLabels.size(); ++i)
    {
        const auto label = sourceLabels.at(i).trimmed();
        if (label.isEmpty() ||
            targetLabels.contains(label, Qt::CaseInsensitive))
        {
            continue;
        }

        targetLabels.append(label);
        if (i < sourceTooltips.size())
        {
            const auto tooltip = sourceTooltips.at(i).trimmed();
            if (!tooltip.isEmpty())
            {
                targetTooltips.append(tooltip);
            }
        }
    }

    target.text = targetLabels.join(QStringLiteral(", "));
    target.tooltip = targetTooltips.join(QChar('\n'));
}

auto formatTooltip(const TwitchChannel::StreamStatus &s, QString thumbnail,
                   bool limitSize = false)
{
    auto title = [&s]() -> QString {
        if (s.title.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.title.toHtmlEscaped() + "<br><br>";
    }();

    auto tooltip = [&]() -> QString {
        if (getSettings()->thumbnailSizeStream.getValue() == 0)
        {
            return QStringLiteral("");
        }

        if (thumbnail.isEmpty())
        {
            return QStringLiteral("Couldn't fetch thumbnail<br>");
        }

        QString sizeStr;
        if (limitSize)
        {
            auto height =
                std::min(getSettings()->thumbnailSizeStream.getValue(), 4) * 80;
            sizeStr =
                QStringLiteral(" height=\"") % QString::number(height) % '"';
        }

        return u"<img " % sizeStr % u" src=\"data:image/jpg;base64, " %
               thumbnail % u"\"><br>";
    }();

    auto game = [&s]() -> QString {
        if (s.game.isEmpty())
        {
            return QStringLiteral("");
        }

        return s.game.toHtmlEscaped() + "<br>";
    }();

    auto streamSummary = [&s]() -> QString {
        const auto liveStatus = QString(s.rerun ? "Vod-casting" : "Live");
        if (s.uptime.isEmpty())
        {
            return QString("%1 with %2 viewers")
                .arg(liveStatus)
                .arg(localizeNumbers(s.viewerCount));
        }

        return QString("%1 for %2 with %3 viewers")
            .arg(liveStatus)
            .arg(s.uptime)
            .arg(localizeNumbers(s.viewerCount));
    };

    auto extraStreamData = [&s, &streamSummary]() -> QString {
        if (getApp()->getStreamerMode()->isEnabled() &&
            getSettings()->streamerModeHideViewerCountAndDuration)
        {
            return QStringLiteral(
                "<span style=\"color: #808892;\">&lt;Streamer "
                "Mode&gt;</span>");
        }

        return streamSummary();
    }();

    return QString("<p style=\"text-align: center;\">" +  //
                   title +                                //
                   tooltip +                              //
                   game +                                 //
                   extraStreamData +                      //
                   "</p>"                                 //
    );
}

auto formatOfflineTooltip(const TwitchChannel::StreamStatus &s)
{
    return QString("<p style=\"text-align: center;\">Offline<br>%1</p>")
        .arg(s.title.toHtmlEscaped());
}

QString formatCompactStreamTooltip(const TwitchChannel::StreamStatus &s,
                                   const QString &thumbnail)
{
    constexpr qsizetype MAX_COMPACT_TITLE_LENGTH = 48;
    auto tooltip = QStringLiteral("<p style=\"text-align: center;\">");

    auto title = s.title.simplified();
    if (title.size() > MAX_COMPACT_TITLE_LENGTH)
    {
        title = title.left(MAX_COMPACT_TITLE_LENGTH - 3).trimmed() + "...";
    }
    if (!title.isEmpty())
    {
        tooltip += title.toHtmlEscaped() % u"<br>";
    }

    if (getSettings()->thumbnailSizeStream.getValue() != 0 &&
        !thumbnail.isEmpty())
    {
        tooltip += u"<img width=\"160\" height=\"90\" "
                   u"src=\"data:image/jpg;base64, " %
                   thumbnail % u"\"><br>";
    }

    if (!s.game.isEmpty())
    {
        tooltip += s.game.toHtmlEscaped() % u"<br>";
    }

    if (getApp()->getStreamerMode()->isEnabled() &&
        getSettings()->streamerModeHideViewerCountAndDuration)
    {
        tooltip += QStringLiteral(
            "<span style=\"color: #808892;\">&lt;Streamer Mode&gt;</span>");
    }
    else
    {
        const auto liveStatus = QString(s.rerun ? "Vod-casting" : "Live");
        tooltip += s.uptime.isEmpty()
                       ? QString("%1 with %2 viewers")
                             .arg(liveStatus)
                             .arg(localizeNumbers(s.viewerCount))
                       : QString("%1 for %2 with %3 viewers")
                             .arg(liveStatus)
                             .arg(s.uptime)
                             .arg(localizeNumbers(s.viewerCount));
    }

    tooltip += QStringLiteral("</p>");
    return tooltip;
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const QString &title, uint64_t viewerCount, const QString &uptime = {})
{
    return {
        .live = true,
        .viewerCount = static_cast<unsigned>(
            std::min<uint64_t>(viewerCount,
                               std::numeric_limits<unsigned>::max())),
        .title = title,
        .uptime = uptime,
        .streamType = QStringLiteral("live"),
    };
}

auto formatTitle(const TwitchChannel::StreamStatus &s, Settings &settings)
{
    auto title = QString();

    // live
    if (s.rerun)
    {
        title += " (rerun)";
    }
    else if (s.streamType.isEmpty())
    {
        title += " (" + s.streamType + ")";
    }
    else
    {
        title += " (live)";
    }

    // description
    if (settings.headerUptime)
    {
        title += " - " + s.uptime;
    }
    if (settings.headerGame && !s.game.isEmpty())
    {
        title += " - " + s.game;
    }
    if (settings.headerStreamTitle && !s.title.isEmpty())
    {
        title += " - " + s.title.simplified();
    }

    return title;
}

TwitchChannel::StreamStatus toTwitchStreamStatus(
    const KickChannel::StreamData &data)
{
    return {
        .live = data.isLive,
        .viewerCount = static_cast<unsigned>(data.viewerCount),
        .title = data.title,
        .game = data.category,
        .uptime = data.uptime,
        .streamType = QStringLiteral("live"),
    };
}

auto distance(QPoint a, QPoint b)
{
    auto x = std::abs(a.x() - b.x());
    auto y = std::abs(a.y() - b.y());

    return std::sqrt(x * x + y * y);
}

QPixmap tintPixmap(const QPixmap &source, const QColor &color)
{
    QPixmap tinted(source.size());
    tinted.setDevicePixelRatio(source.devicePixelRatio());
    tinted.fill(Qt::transparent);

    QPainter painter(&tinted);
    painter.drawPixmap(0, 0, source);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(tinted.rect(), color);

    return tinted;
}

void openUrlInBrowser(const QUrl &url)
{
    if (url.isValid())
    {
        QDesktopServices::openUrl(url);
    }
}

QColor viewerCountColor()
{
    return QColor(232, 232, 232);
}

QColor viewerCountIconColor()
{
    return viewerCountColor();
}

class ViewerCountIcon final : public BaseWidget
{
public:
    explicit ViewerCountIcon(BaseWidget *parent)
        : BaseWidget(parent)
    {
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        auto pen = QPen(viewerCountIconColor());
        pen.setWidthF(1.9 * this->scale());
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);

        const auto size = std::min(this->width(), this->height()) * 0.88;
        const auto centerX = this->width() / 2.0;
        const auto top =
            (this->height() - size) / 2.0 + this->scale();
        const auto headRadius = size * 0.14;
        const auto headCenter = QPointF(centerX, top + size * 0.27);
        painter.drawEllipse(headCenter, headRadius, headRadius);

        QPainterPath shoulders;
        shoulders.moveTo(centerX - size * 0.31, top + size * 0.80);
        shoulders.cubicTo(centerX - size * 0.28, top + size * 0.61,
                          centerX - size * 0.16, top + size * 0.52, centerX,
                          top + size * 0.52);
        shoulders.cubicTo(centerX + size * 0.16, top + size * 0.52,
                          centerX + size * 0.28, top + size * 0.61,
                          centerX + size * 0.31, top + size * 0.80);
        painter.drawPath(shoulders);
    }
};

struct ViewerCountEntry {
    SplitHeaderViewerCountMode mode{};
    QString platformName;
    uint64_t count = 0;
};

struct ViewerCountDisplay {
    bool visible = false;
    QString text;
    QString tooltip;
};

QString viewerNoun(uint64_t count)
{
    return count == 1 ? QStringLiteral("viewer") : QStringLiteral("viewers");
}

void addTwitchViewerCount(std::vector<ViewerCountEntry> &entries,
                          const TwitchChannel *channel)
{
    if (channel == nullptr)
    {
        return;
    }

    const auto streamStatus = channel->accessStreamStatus();
    if (!streamStatus->live)
    {
        return;
    }

    entries.push_back({
        SplitHeaderViewerCountMode::Twitch,
        QStringLiteral("Twitch"),
        streamStatus->viewerCount,
    });
}

void addKickViewerCount(std::vector<ViewerCountEntry> &entries,
                        const KickChannel *channel)
{
    if (channel == nullptr)
    {
        return;
    }

    const auto &stream = channel->streamData();
    if (!stream.isLive)
    {
        return;
    }

    entries.push_back({
        SplitHeaderViewerCountMode::Kick,
        QStringLiteral("Kick"),
        stream.viewerCount,
    });
}

void addYouTubeViewerCount(std::vector<ViewerCountEntry> &entries,
                           const YouTubeLiveChat *liveChat)
{
    if (liveChat == nullptr || !liveChat->isLive())
    {
        return;
    }

    entries.push_back({
        SplitHeaderViewerCountMode::YouTube,
        QStringLiteral("YouTube"),
        liveChat->liveViewerCount(),
    });
}

std::vector<ViewerCountEntry> viewerCountEntries(Channel *channel)
{
    std::vector<ViewerCountEntry> entries;

    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        addTwitchViewerCount(entries, twitch);
    }
    else if (auto *kick = dynamic_cast<KickChannel *>(channel))
    {
        addKickViewerCount(entries, kick);
    }
    else if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        addTwitchViewerCount(
            entries,
            dynamic_cast<TwitchChannel *>(merged->twitchChannel().get()));
        addKickViewerCount(
            entries, dynamic_cast<KickChannel *>(merged->kickChannel().get()));
        addYouTubeViewerCount(entries, merged->youtubeLiveChat());
    }

    return entries;
}

bool viewerCountModeIncludes(SplitHeaderViewerCountMode selected,
                             const ViewerCountEntry &entry)
{
    return selected == SplitHeaderViewerCountMode::Total ||
           selected == entry.mode;
}

QString viewerCountTooltipLine(const ViewerCountEntry &entry)
{
    return QStringLiteral("%1: %2 %3")
        .arg(entry.platformName)
        .arg(localizeNumbers(entry.count))
        .arg(viewerNoun(entry.count));
}

ViewerCountDisplay viewerCountDisplay(Channel *channel, const Split *split)
{
    auto *settings = getSettings();
    if ((split != nullptr && !split->viewerCountEnabled()) ||
        (getApp()->getStreamerMode()->isEnabled() &&
         settings->streamerModeHideViewerCountAndDuration))
    {
        return {};
    }

    const auto selectedMode = headerViewerCountModeSetting().getEnum();
    const auto entries = viewerCountEntries(channel);

    uint64_t total = 0;
    QStringList tooltipLines;
    for (const auto &entry : entries)
    {
        if (!viewerCountModeIncludes(selectedMode, entry))
        {
            continue;
        }

        total += entry.count;
        tooltipLines.append(viewerCountTooltipLine(entry));
    }

    if (tooltipLines.isEmpty())
    {
        return {};
    }

    return {
        .visible = true,
        .text = localizeNumbers(total),
        .tooltip = tooltipLines.join(QStringLiteral("\n")),
    };
}

const TwitchChannel *viewerListTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

const TwitchChannel *streamDatabaseTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

TwitchChannel *roomModeTwitchChannel(Channel *channel)
{
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel))
    {
        return twitch;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<TwitchChannel *>(merged->twitchChannel().get());
    }

    return nullptr;
}

KickChannel *roomModeKickChannel(Channel *channel)
{
    if (auto *kick = dynamic_cast<KickChannel *>(channel))
    {
        return kick;
    }

    if (auto *merged = dynamic_cast<MergedChannel *>(channel))
    {
        return dynamic_cast<KickChannel *>(merged->kickChannel().get());
    }

    return nullptr;
}

bool splitIsStreamDatabase(const Split *split)
{
    if (split == nullptr)
    {
        return false;
    }

    auto *twitch = streamDatabaseTwitchChannel(split->getChannel().get());
    return twitch != nullptr &&
           twitch->getName().compare(QString::fromLatin1(STREAMDATABASE_LOGIN),
                                     Qt::CaseInsensitive) == 0;
}

class ClickableSubmenuActionFilter final : public QObject
{
public:
    ClickableSubmenuActionFilter(QMenu *menu, QAction *action,
                                 std::function<void()> onClick)
        : QObject(menu)
        , menu_(menu)
        , action_(action)
        , onClick_(std::move(onClick))
    {
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched != this->menu_ ||
            event->type() != QEvent::MouseButtonRelease ||
            this->menu_->activeAction() != this->action_)
        {
            return QObject::eventFilter(watched, event);
        }

        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() != Qt::LeftButton ||
            !this->menu_->actionGeometry(this->action_).contains(
                mouseEvent->pos()))
        {
            return QObject::eventFilter(watched, event);
        }

        this->onClick_();
        this->menu_->close();
        return true;
    }

private:
    QMenu *menu_{};
    QAction *action_{};
    std::function<void()> onClick_;
};

class TitleSettingsButton final : public SvgButton
{
public:
    TitleSettingsButton(Src source, BaseWidget *parent = nullptr,
                        QSize padding = {6, 3})
        : SvgButton(std::move(source), parent, padding)
    {
        this->updateIconColor();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPixmap iconLayer(this->size() * this->devicePixelRatio());
        iconLayer.setDevicePixelRatio(this->devicePixelRatio());
        iconLayer.fill(Qt::transparent);

        {
            QPainter iconPainter(&iconLayer);
            this->paintContent(iconPainter);
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(0.0, -0.5 * this->scale());
        painter.drawPixmap(this->rect(), iconLayer, {{}, iconLayer.size()});
    }

    void themeChangedEvent() override
    {
        SvgButton::themeChangedEvent();
        this->updateIconColor();
    }

    void mouseOverUpdated() override
    {
        this->updateIconColor();
    }

private:
    void updateIconColor()
    {
        if (this->mouseOver())
        {
            this->setColor(this->theme->isLightTheme() ? QColor(24, 24, 24)
                                                       : QColor(255, 255, 255));
            return;
        }

        auto color = this->theme->splits.header.text;
        color.setAlpha(this->theme->isLightTheme() ? 120 : 150);
        this->setColor(color);
    }
};

void splitHeaderTrace(const QString &message)
{
    QFile file(QStringLiteral("mergerino-layout-trace.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
    {
        return;
    }

    QTextStream stream(&file);
    stream << message << '\n';
}

Split *traceSplitHeaderSplit(Split *split)
{
    splitHeaderTrace(QStringLiteral("SplitHeader after BaseWidget"));
    return split;
}

TooltipWidget *createSplitHeaderTooltip(SplitHeader *header)
{
    splitHeaderTrace(QStringLiteral("SplitHeader before TooltipWidget"));
    auto *tooltip = new TooltipWidget(header);
    splitHeaderTrace(QStringLiteral("SplitHeader after TooltipWidget"));
    return tooltip;
}

}  // namespace

namespace chatterino {

SplitHeader::SplitHeader(Split *split)
    : BaseWidget(split)
    , split_(traceSplitHeaderSplit(split))
    , tooltipWidget_(createSplitHeaderTooltip(this))
{
    splitHeaderTrace(QStringLiteral("SplitHeader constructor body start"));
    splitHeaderTrace(QStringLiteral("SplitHeader before initializeLayout"));
    this->initializeLayout();
    splitHeaderTrace(QStringLiteral("SplitHeader after initializeLayout"));

    this->setMouseTracking(true);
    splitHeaderTrace(QStringLiteral("SplitHeader before updateChannelText"));
    this->updateChannelText();
    splitHeaderTrace(QStringLiteral("SplitHeader after updateChannelText"));
    splitHeaderTrace(QStringLiteral("SplitHeader before handleChannelChanged"));
    this->handleChannelChanged();
    splitHeaderTrace(QStringLiteral("SplitHeader after handleChannelChanged"));

    // The lifetime of these signals are tied to the lifetime of the Split.
    // Since the SplitHeader is owned by the Split, they will always be destroyed
    // at the same time.
    std::ignore = this->split_->focused.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->focusLost.connect([this]() {
        this->themeChangedEvent();
    });
    std::ignore = this->split_->channelChanged.connect([this]() {
        this->handleChannelChanged();
    });

    this->bSignals_.emplace_back(
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            this->updateIcons();
        }));

    auto _ = [this](const auto &, const auto &) {
        this->updateChannelText();
    };
    getSettings()->headerViewerCount.connect(_, this->managedConnections_);
    headerViewerCountModeSetting().connect(_, this->managedConnections_);
    getSettings()->headerStreamTitle.connect(_, this->managedConnections_);
    getSettings()->headerGame.connect(_, this->managedConnections_);
    getSettings()->headerUptime.connect(_, this->managedConnections_);
    getSettings()->headerChatModeIndicator.connect(
        [this](const auto &, const auto &) {
            this->updateRoomModes();
        },
        this->managedConnections_);

    auto *window = dynamic_cast<BaseWindow *>(this->window());
    if (window)
    {
        // Hack: In some cases Qt doesn't send the leaveEvent the "actual" last mouse receiver.
        // This can happen when quickly moving the mouse out of the window and right clicking.
        // To prevent the tooltip from getting stuck, we use the window's leaveEvent.
        this->managedConnections_.managedConnect(window->leaving, [this] {
            if (this->tooltipWidget_->isVisible())
            {
                this->tooltipWidget_->hide();
            }
        });
    }

    this->scaleChangedEvent(this->scale());
}

void SplitHeader::setSlowChatQueueIndicatorReady(bool value)
{
    this->slowChatQueueIndicatorReady_ = value;
    this->updateIcons();
}

void SplitHeader::initializeLayout()
{
    assert(this->layout() == nullptr);

    this->moderationButton_ = new SvgButton(
        {
            .dark = ":/buttons/moderationDisabled-darkMode.svg",
            .light = ":/buttons/moderationDisabled-lightMode.svg",
        },
        this, {5, 5});

    this->chattersButton_ = new SvgButton(
        {
            .dark = ":/buttons/chatters-darkMode.svg",
            .light = ":/buttons/chatters-lightMode.svg",
        },
        this, {4, 4});

    this->alertsButton_ = new SvgButton(
        {
            .dark = ":/buttons/alertsPane-darkMode.svg",
            .light = ":/buttons/alertsPane-lightMode.svg",
        },
        this, {4, 4});

    this->addButton_ = new DrawnButton(DrawnButton::Symbol::Plus,
                                       {
                                           .padding = 3,
                                           .thickness = 1,
                                       },
                                       this);

    this->dropdownButton_ =
        new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);

    this->viewerCountContainer_ = new BaseWidget(this);
    this->viewerCountContainer_->setSizePolicy(QSizePolicy::Fixed,
                                               QSizePolicy::Preferred);
    this->viewerCountContainer_->hide();

    this->viewerCountIcon_ = new ViewerCountIcon(this->viewerCountContainer_);
    this->viewerCountIcon_->setAttribute(Qt::WA_TransparentForMouseEvents);

    this->viewerCountLabel_ = new Label(this->viewerCountContainer_);
    this->viewerCountLabel_->setSizePolicy(QSizePolicy::Fixed,
                                           QSizePolicy::Preferred);
    this->viewerCountLabel_->setCentered(true);
    this->viewerCountLabel_->setPadding(QMargins(2, 0, 4, 0));
    this->viewerCountLabel_->setAttribute(Qt::WA_TransparentForMouseEvents);

    auto *viewerCountLayout = new QHBoxLayout(this->viewerCountContainer_);
    viewerCountLayout->setContentsMargins(0, 0, 0, 0);
    viewerCountLayout->setSpacing(0);
    viewerCountLayout->addWidget(this->viewerCountIcon_);
    viewerCountLayout->addWidget(this->viewerCountLabel_);

    this->clearActivityButton_ = new SvgButton(
        {
            .dark = ":/buttons/trash-darkMode.svg",
            .light = ":/buttons/trash-lightMode.svg",
        },
        this, {0, 0});
    this->clearActivityButton_->setContentSize(QSize{14, 14});
    this->clearActivityButton_->setScaleIndependentSize(BUTTON_WIDTH, 24);
    this->clearActivityButton_->hide();

    this->titleSettingsButton_ = new TitleSettingsButton(
        {
            .dark = ":/buttons/settings-darkMode.svg",
            .light = ":/buttons/settings-lightMode.svg",
        },
        this, {3, 3});
    this->titleSettingsButton_->setContentSize(QSize{13, 13});
    this->titleSettingsButton_->setScaleIndependentSize(
        TITLE_SETTINGS_BUTTON_WIDTH, BUTTON_WIDTH);
    this->titleSettingsButton_->setToolTip("Tab settings");
    this->titleSettingsButton_->hide();

    /// XXX: this never gets disconnected
    QObject::connect(this->dropdownButton_, &Button::leftMousePress, this,
                     [this] {
                          this->dropdownButton_->setMenu(this->createMainMenu());
                     });

    auto *layout = makeLayout<QHBoxLayout>({
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // title
        this->titleLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::MinimumExpanding,
                             QSizePolicy::Preferred);
            w->setCentered(true);
            w->setShouldElide(true);
            w->setPadding(QMargins(TITLE_LABEL_LEFT_PADDING, -1,
                                   TITLE_LABEL_RIGHT_PADDING, 1));
        }),
        // space
        makeWidget<BaseWidget>([](auto w) {
            w->setScaleIndependentSize(8, 4);
        }),
        // viewer count
        this->viewerCountContainer_,
        // mode
        this->modeButton_ = makeWidget<LabelButton>([&](auto w) {
            w->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Minimum);
            w->hide();
            w->setMenu(this->createChatModeMenu());
        }),
        // slower chat queued count
        this->queuedSlowChatCountLabel_ = makeWidget<Label>([](auto w) {
            w->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
            w->setCentered(true);
            w->setFontStyle(FontStyle::UiMediumBold);
            w->setPadding(QMargins(5, 0, 5, 0));
            w->setText(QString());
        }),
        // alerts
        this->alertsButton_,
        // moderator
        this->moderationButton_,
        // viewer list
        this->chattersButton_,
        // activity clear
        this->clearActivityButton_,
        // dropdown
        this->dropdownButton_,
        // add split
        this->addButton_,
    });

    QObject::connect(
        this->moderationButton_, &Button::clicked, this,
        [this](Qt::MouseButton button) mutable {
            switch (button)
            {
                case Qt::LeftButton:
                    if (getSettings()->moderationActions.empty())
                    {
                        getApp()->getWindows()->showSettingsDialog(
                            this, SettingsDialogPreference::ModerationActions);
                        this->split_->setModerationMode(true);
                    }
                    else
                    {
                        auto moderationMode = this->split_->getModerationMode();

                        this->split_->setModerationMode(!moderationMode);
                        // w->setDim(moderationMode ? DimButton::Dim::Some
                        //                          : DimButton::Dim::None);
                    }
                    break;

                case Qt::RightButton:
                case Qt::MiddleButton:
                    getApp()->getWindows()->showSettingsDialog(
                        this, SettingsDialogPreference::ModerationActions);
                    break;

                default:
                    break;
            }
        });

    QObject::connect(this->chattersButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->openChatterList();
                     });

    QObject::connect(this->alertsButton_, &Button::leftClicked, this, [this]() {
        this->split_->openAlertsPane();
    });

    QObject::connect(this->clearActivityButton_, &Button::leftClicked, this,
                     [this]() {
                         this->split_->clear();
                     });

    QObject::connect(this->addButton_, &Button::leftClicked, this, [this]() {
        this->split_->addSibling();
    });
    QObject::connect(this->titleSettingsButton_, &Button::leftClicked, this,
                     [this] {
                         this->hideHoverTooltip();
                         this->openTabSettings();
                     });

    getSettings()->customURIScheme.connect(
        [this] {
            if (auto *const drop = this->dropdownButton_)
            {
                drop->setMenu(this->createMainMenu());
            }
        },
        this->managedConnections_);

    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    this->setLayout(layout);

    this->titleLabel_->installEventFilter(this);
    this->titleSettingsButton_->installEventFilter(this);
    this->viewerCountContainer_->installEventFilter(this);
    this->modeButton_->installEventFilter(this);
    this->queuedSlowChatCountLabel_->installEventFilter(this);
    this->alertsButton_->installEventFilter(this);
    this->moderationButton_->installEventFilter(this);

    this->setAddButtonVisible(false);
}

bool SplitHeader::eventFilter(QObject *watched, QEvent *event)
{
    const auto eventType = event->type();
    auto *target = qobject_cast<QWidget *>(watched);

    if (target != nullptr)
    {
        if (watched == this->titleLabel_ &&
            eventType == QEvent::MouseButtonPress)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::RightButton)
            {
                this->hideHoverTooltip();
                this->popupMainMenu(
                    target->mapToGlobal(mouseEvent->pos() + QPoint(0, 4)));
                return true;
            }
        }

        if (watched == this->titleSettingsButton_ &&
            eventType == QEvent::MouseButtonPress)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::RightButton)
            {
                this->hideHoverTooltip();
                this->popupTitleSettingsButtonMenu(
                    target->mapToGlobal(mouseEvent->pos()));
                return true;
            }
        }

        if (watched == this->modeButton_ &&
            eventType == QEvent::MouseButtonPress)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::RightButton)
            {
                this->hideHoverTooltip();
                if (auto *menu = this->modeButton_->menu())
                {
                    menu->popup(target->mapToGlobal(mouseEvent->pos()));
                }
                return true;
            }
        }

        if (eventType == QEvent::Enter)
        {
            if (watched == this->titleLabel_)
            {
                this->setTitleSettingsButtonVisible(true);
                this->updateChannelText();
                if (!this->tooltipText_.isEmpty())
                {
                    this->showHoverTooltip(target, this->tooltipText_, true);
                }
            }
            else if (watched == this->titleSettingsButton_)
            {
                this->setTitleSettingsButtonVisible(true);
                this->hideHoverTooltip();
            }
            else if (watched == this->viewerCountContainer_ &&
                     this->viewerCountContainer_->isVisible() &&
                     !this->viewerCountTooltipText_.isEmpty())
            {
                this->showHoverTooltip(target, this->viewerCountTooltipText_,
                                       false);
            }
            else if (watched == this->modeButton_ &&
                     this->modeButton_->isVisible() &&
                     !this->modeTooltipText_.isEmpty())
            {
                this->showHoverTooltip(target, this->modeTooltipText_, true);
            }
            else if (watched == this->queuedSlowChatCountLabel_ &&
                     !this->queuedSlowChatCountLabel_->getText().isEmpty())
            {
                this->showHoverTooltip(target,
                                       "Queued messages waiting for slower chat.",
                                       false);
            }
            else if (watched == this->alertsButton_ &&
                     this->alertsButton_->isVisible())
            {
                this->showHoverTooltip(target, "Toggle the activity tab.",
                                       false);
            }
            else if (watched == this->moderationButton_ &&
                     this->moderationButton_->isVisible())
            {
                this->showHoverTooltip(target, "Toggle moderation mode.", false);
            }
        }
        else if ((watched == this->titleLabel_ ||
                  watched == this->titleSettingsButton_) &&
                 eventType == QEvent::Leave)
        {
            QTimer::singleShot(0, this, [this] {
                this->setTitleSettingsButtonVisible(
                    this->isTitleSettingsButtonHoverArea());
            });
        }
        else if (eventType == QEvent::Hide || eventType == QEvent::MouseButtonPress)
        {
            this->hideHoverTooltip();
        }
    }

    return BaseWidget::eventFilter(watched, event);
}

void SplitHeader::showHoverTooltip(QWidget *target, const QString &text,
                                   bool wordWrap)
{
    if (target == nullptr || text.isEmpty())
    {
        this->hideHoverTooltip();
        return;
    }

    this->tooltipWidget_->setOne({nullptr, text});
    this->tooltipWidget_->setWordWrap(wordWrap);
    this->tooltipWidget_->adjustSize();

#ifdef Q_OS_WIN
    this->tooltipWidget_->show();
#endif

    auto pos = target->mapToGlobal(target->rect().bottomLeft()) +
               QPoint((target->width() - this->tooltipWidget_->width()) / 2, 1);

    this->tooltipWidget_->moveTo(pos, widgets::BoundsChecking::CursorPosition);

#ifndef Q_OS_WIN
    this->tooltipWidget_->show();
#endif
}

void SplitHeader::hideHoverTooltip()
{
    this->tooltipWidget_->hide();
}

void SplitHeader::updateThumbnail(const QString &url, bool followRedirects)
{
    if (url.isEmpty())
    {
        this->thumbnailUrl_.clear();
        this->thumbnail_.clear();
        this->lastThumbnail_.invalidate();
        return;
    }

    if (this->thumbnailUrl_ != url)
    {
        this->thumbnailUrl_ = url;
        this->thumbnail_.clear();
        this->lastThumbnail_.invalidate();
    }

    if (this->lastThumbnail_.isValid() &&
        this->lastThumbnail_.elapsed() <= THUMBNAIL_MAX_AGE_MS)
    {
        return;
    }

    auto request = NetworkRequest(url, NetworkRequestType::Get)
                       .caller(this)
                       .onSuccess([this, url](const auto &result) {
                           assert(!isAppAboutToQuit());

                           if (this->thumbnailUrl_ != url)
                           {
                               return;
                           }
                           if (result.status() == 200)
                           {
                               this->thumbnail_ = QString::fromLatin1(
                                   result.getData().toBase64());
                           }
                           else
                           {
                               this->thumbnail_.clear();
                           }
                           this->updateChannelText();
                       });
    if (followRedirects)
    {
        request = std::move(request).followRedirects(true);
    }
    std::move(request).execute();
    this->lastThumbnail_.restart();
}

QString SplitHeader::mergedStreamPreviewTooltip(MergedChannel *mergedChannel)
{
    if (!mergedChannel || !mergedChannel->isLive())
    {
        return {};
    }

    if (auto *twitch = dynamic_cast<TwitchChannel *>(
            mergedChannel->twitchChannel().get());
        twitch && twitch->isLive())
    {
        const auto streamStatus = twitch->accessStreamStatus();
        QString thumbnailUrl =
            "https://static-cdn.jtvnw.net/previews-ttv/live_user_" +
            twitch->getName().toLower();
        switch (getSettings()->thumbnailSizeStream.getValue())
        {
            case 1:
                thumbnailUrl.append("-80x45.jpg");
                break;
            case 2:
                thumbnailUrl.append("-160x90.jpg");
                break;
            case 3:
                thumbnailUrl.append("-360x203.jpg");
                break;
            default:
                thumbnailUrl.clear();
                break;
        }
        this->updateThumbnail(thumbnailUrl, false);
        return formatCompactStreamTooltip(*streamStatus, this->thumbnail_);
    }

    if (auto *kick = dynamic_cast<KickChannel *>(
            mergedChannel->kickChannel().get());
        kick && kick->isLive())
    {
        const auto &stream = kick->streamData();
        this->updateThumbnail(stream.thumbnailUrl, true);
        return formatCompactStreamTooltip(toTwitchStreamStatus(stream),
                                          this->thumbnail_);
    }

    if (auto *youtube = mergedChannel->youtubeLiveChat();
        youtube && youtube->isLive())
    {
        this->updateThumbnail(youtube->previewThumbnailUrl(), true);
        return formatCompactStreamTooltip(
            toTwitchStreamStatus(youtube->liveTitle(),
                                 youtube->liveViewerCount(),
                                 youtube->liveUptime()),
            this->thumbnail_);
    }

    return {};
}

std::unique_ptr<QMenu> SplitHeader::createMainMenu()
{
    // top level menu
    const auto &h = getApp()->getHotkeys();
    auto menu = std::make_unique<QMenu>();
    menu->addAction(
        "Change channel",
        h->getDisplaySequence(HotkeyCategory::Split, "changeChannel"),
        this->split_, &Split::changeChannel);
    menu->addAction("Close",
                    h->getDisplaySequence(HotkeyCategory::Split, "delete"),
                    this->split_, &Split::deleteFromContainer);
    menu->addSeparator();
    if (auto *mergedChannel =
            dynamic_cast<MergedChannel *>(this->split_->getChannel().get()))
    {
        auto browserUrls = mergedChannel->liveStreamBrowserUrls();
        const auto hasLiveStreams = !browserUrls.empty();
        if (!hasLiveStreams)
        {
            browserUrls = mergedChannel->channelBrowserUrls();
        }

        if (!browserUrls.empty())
        {
            const auto actionText = hasLiveStreams ? "Open stream in browser"
                                                   : "Open channel in browser";
            if (browserUrls.size() == 1)
            {
                const auto url = browserUrls.front().url;
                menu->addAction(actionText, this, [url] {
                    openUrlInBrowser(url);
                });
            }
            else
            {
                auto *streamMenu = new QMenu(actionText, menu.get());
                for (const auto &browserUrl : browserUrls)
                {
                    streamMenu->addAction(browserUrl.platformName, this,
                                          [url = browserUrl.url] {
                                              openUrlInBrowser(url);
                                          });
                }

                auto *streamAction = menu->addMenu(streamMenu);
                const auto defaultUrl = browserUrls.front().url;
                auto openDefault = [defaultUrl] {
                    openUrlInBrowser(defaultUrl);
                };
                QObject::connect(streamAction, &QAction::triggered, this,
                                 openDefault);
                menu->installEventFilter(new ClickableSubmenuActionFilter(
                    menu.get(), streamAction, std::move(openDefault)));
            }
            menu->addSeparator();
        }
    }
    menu->addAction(
        "Popup",
        h->getDisplaySequence(HotkeyCategory::Window, "popup", {{"split"}}),
        this->split_, &Split::popup);
    menu->addAction(
        "Popup overlay",
        h->getDisplaySequence(HotkeyCategory::Split, "popupOverlay"),
        this->split_, &Split::showOverlayWindow);
    menu->addAction("Search",
                    h->getDisplaySequence(HotkeyCategory::Split, "showSearch"),
                    this->split_, [this] {
                        this->split_->showSearch(true);
                    });
    menu->addAction("Export chat", this, &SplitHeader::exportChat);
    if (this->split_->isActivityPane())
    {
        menu->addAction("Settings", this->split_, &Split::showSettingsDialog);
    }
    else
    {
        menu->addAction("Settings", this->split_, &Split::changeChannel);
    }
    menu->addAction(
        "Filters",
        h->getDisplaySequence(HotkeyCategory::Split, "pickFilters"),
        this->split_, &Split::setFiltersDialog);
    auto *unhidePinnedMessageAction =
        menu->addAction("Unhide pinned message", this->split_,
                        &Split::unhidePinnedMessages);
    unhidePinnedMessageAction->setVisible(false);
    QObject::connect(
        menu.get(), &QMenu::aboutToShow, this,
        [this, unhidePinnedMessageAction] {
            unhidePinnedMessageAction->setVisible(
                this->split_->hasHiddenPinnedMessage());
        });
    if (!this->split_->titleSettingsButtonVisible())
    {
        menu->addAction("Show settings cog", this, [this] {
            this->split_->setTitleSettingsButtonVisible(true);
            this->setTitleSettingsButtonVisible(
                this->isTitleSettingsButtonHoverArea());
        });
    }
    if (!this->split_->isActivityPane())
    {
        auto *chatboxButtonsMenu = new PersistentCheckMenu(menu.get());
        chatboxButtonsMenu->setTitle(QStringLiteral("Chatbox buttons"));
        menu->addMenu(chatboxButtonsMenu);
        auto addVisibilityAction =
            [this, chatboxButtonsMenu](const QString &label,
                                       BoolSetting &setting) {
                auto *action = chatboxButtonsMenu->addAction(label);
                action->setCheckable(true);
                action->setChecked(setting.getValue());
                QObject::connect(action, &QAction::toggled, this,
                                 [&setting](bool checked) {
                                     setting.setValue(checked);
                                 });
            };
        addVisibilityAction(QStringLiteral("7TV"),
                            showSeventvChatButtonSetting());
        addVisibilityAction(QStringLiteral("Predictions"),
                            showPredictionChatButtonSetting());
        addVisibilityAction(QStringLiteral("Polls"),
                            showPollChatButtonSetting());
        addVisibilityAction(QStringLiteral("Giveaway"),
                            showGiveawayChatButtonSetting());
    }
    if (!this->split_->isActivityPane())
    {
        auto *modeIndicatorAction = menu->addAction("Chat mode indicator");
        modeIndicatorAction->setCheckable(true);
        modeIndicatorAction->setChecked(
            getSettings()->headerChatModeIndicator &&
            this->split_->chatModeIndicatorVisible());
        modeIndicatorAction->setEnabled(getSettings()->headerChatModeIndicator);
        QObject::connect(modeIndicatorAction, &QAction::toggled, this,
                         [this](bool checked) {
                             this->split_->setChatModeIndicatorVisible(checked);
                         });
    }
    if (!this->split_->isActivityPane() && splitIsStreamDatabase(this->split_))
    {
        auto *badgeFeedAction = menu->addAction("Badge feed");
        badgeFeedAction->setCheckable(true);
        badgeFeedAction->setChecked(
            this->split_->streamDatabaseBadgeFeedVisible());
        QObject::connect(badgeFeedAction, &QAction::toggled, this,
                         [this](bool checked) {
                             this->split_->setStreamDatabaseBadgeFeedVisible(
                                 checked);
                         });
    }
    menu->addSeparator();

    auto *twitchChannel =
        dynamic_cast<TwitchChannel *>(this->split_->getChannel().get());
    auto *kickChannel =
        dynamic_cast<KickChannel *>(this->split_->getChannel().get());

    if (twitchChannel || kickChannel)
    {
        menu->addAction(
            OPEN_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openInBrowser);
        if (twitchChannel)
        {
            menu->addAction(OPEN_PLAYER_IN_BROWSER,
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openPlayerInBrowser"),
                            this->split_, &Split::openBrowserPlayer);
        }
        menu->addAction(
            OPEN_IN_STREAMLINK,
            h->getDisplaySequence(HotkeyCategory::Split, "openInStreamlink"),
            this->split_, &Split::openInStreamlink);

        if (!getSettings()->customURIScheme.getValue().isEmpty())
        {
            menu->addAction("Open in custom player",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openInCustomPlayer"),
                            this->split_, &Split::openWithCustomScheme);
        }

        if (this->split_->getChannel()->hasModRights())
        {
            menu->addAction(
                OPEN_MOD_VIEW_IN_BROWSER,
                h->getDisplaySequence(HotkeyCategory::Split, "openModView"),
                this->split_, &Split::openModViewInBrowser);
        }

        if (twitchChannel)
        {
            menu->addAction(
                    "Create a clip",
                    h->getDisplaySequence(HotkeyCategory::Split, "createClip"),
                    this->split_,
                    [twitchChannel] {
                        twitchChannel->createClip({}, {});
                    })
                ->setVisible(twitchChannel->isLive());
        }

        if (this->split_->getIndirectChannel().getType() ==
            Channel::Type::TwitchWatching)
        {
            menu->addAction("Reset /watching", this->split_, [] {
                if (!getApp()
                         ->getTwitch()
                         ->getWatchingChannel()
                         .get()
                         ->isEmpty())
                {
                    getApp()->getTwitch()->setWatchingChannel(
                        Channel::getEmpty());
                }
            });
        }

        menu->addSeparator();
    }

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchWhispers)
    {
        menu->addAction(
            OPEN_WHISPERS_IN_BROWSER,
            h->getDisplaySequence(HotkeyCategory::Split, "openInBrowser"),
            this->split_, &Split::openWhispersInBrowser);
        menu->addSeparator();
    }

    // reload / reconnect
    if (this->split_->getChannel()->canReconnect())
    {
        menu->addAction(
            "Reconnect",
            h->getDisplaySequence(HotkeyCategory::Split, "reconnect"), this,
            &SplitHeader::reconnect);
    }

    if (twitchChannel || kickChannel)
    {
        auto bothSeq = h->getDisplaySequence(
            HotkeyCategory::Split, "reloadEmotes", {std::vector<QString>()});
        auto channelSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                "reloadEmotes", {{"channel"}});
        auto subSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "reloadEmotes", {{"subscriber"}});
        menu->addAction("Reload channel emotes",
                        channelSeq.isEmpty() ? bothSeq : channelSeq, this,
                        &SplitHeader::reloadChannelEmotes);
        if (twitchChannel)
        {
            menu->addAction("Reload subscriber emotes",
                            subSeq.isEmpty() ? bothSeq : subSeq, this,
                            &SplitHeader::reloadSubscriberEmotes);
        }
    }

    menu->addSeparator();

    {
        // "How to..." sub menu
        auto *subMenu = new QMenu("How to...", this);
        subMenu->addAction("move split", this->split_, &Split::explainMoving);
        subMenu->addAction("add/split", this->split_, &Split::explainSplitting);
        menu->addMenu(subMenu);
    }

    menu->addSeparator();

    // sub menu
    auto *moreMenu = new QMenu("More", this);

    auto modModeSeq = h->getDisplaySequence(HotkeyCategory::Split,
                                            "setModerationMode", {{"toggle"}});
    if (modModeSeq.isEmpty())
    {
        modModeSeq =
            h->getDisplaySequence(HotkeyCategory::Split, "setModerationMode",
                                  {std::vector<QString>()});
        // this makes a full std::optional<> with an empty vector inside
    }
    moreMenu->addAction(
        "Toggle moderation mode", modModeSeq, this->split_, [this]() {
            this->split_->setModerationMode(!this->split_->getModerationMode());
        });

    if (this->split_->getChannel()->getType() == Channel::Type::TwitchMentions)
    {
        auto *action = new QAction(this);
        action->setText("Enable /mention tab highlights");
        action->setCheckable(true);

        QObject::connect(moreMenu, &QMenu::aboutToShow, this, [action]() {
            action->setChecked(getSettings()->highlightMentions);
        });
        QObject::connect(action, &QAction::triggered, this, []() {
            getSettings()->highlightMentions =
                !getSettings()->highlightMentions;
        });

        moreMenu->addAction(action);
    }

    if (auto *viewerListChannel =
            viewerListTwitchChannel(this->split_->getChannel().get());
        !this->split_->isActivityPane() && viewerListChannel &&
        viewerListChannel->hasModRights())
    {
        moreMenu->addAction(
            "Show viewer list",
            h->getDisplaySequence(HotkeyCategory::Split, "openViewerList"),
            this->split_, &Split::openChatterList);
    }

    if (twitchChannel)
    {
        moreMenu->addAction("Subscribe",
                            h->getDisplaySequence(HotkeyCategory::Split,
                                                  "openSubscriptionPage"),
                            this->split_, &Split::openSubPage);

        {
            auto *action = new QAction(this);
            action->setText("Notify when live");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setChannelNotification", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setChannelNotification",
                                                  {std::vector<QString>()});
                // this makes a full std::optional<> with an empty vector inside
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(
                        getApp()->getNotifications()->isChannelNotified(
                            this->split_->getChannel()->getName(),
                            Platform::Twitch));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getApp()->getNotifications()->updateChannelNotification(
                    this->split_->getChannel()->getName(), Platform::Twitch);
            });

            moreMenu->addAction(action);
        }

        {
            auto *action = new QAction(this);
            action->setText("Mute highlight sounds");
            action->setCheckable(true);

            auto notifySeq = h->getDisplaySequence(
                HotkeyCategory::Split, "setHighlightSounds", {{"toggle"}});
            if (notifySeq.isEmpty())
            {
                notifySeq = h->getDisplaySequence(HotkeyCategory::Split,
                                                  "setHighlightSounds",
                                                  {std::vector<QString>()});
            }
            action->setShortcut(notifySeq);

            QObject::connect(
                moreMenu, &QMenu::aboutToShow, this, [action, this]() {
                    action->setChecked(getSettings()->isMutedChannel(
                        this->split_->getChannel()->getName()));
                });
            QObject::connect(action, &QAction::triggered, this, [this]() {
                getSettings()->toggleMutedChannel(
                    this->split_->getChannel()->getName());
            });

            moreMenu->addAction(action);
        }
    }

    moreMenu->addSeparator();
    moreMenu->addAction(
        "Clear messages",
        h->getDisplaySequence(HotkeyCategory::Split, "clearMessages"),
        this->split_, &Split::clear);
    //    moreMenu->addSeparator();
    //    moreMenu->addAction("Show changelog", this,
    //    SLOT(moreMenuShowChangelog()));
    menu->addMenu(moreMenu);

    return menu;
}

void SplitHeader::exportChat()
{
    const auto channel = this->split_->getChannel();
    if (!channel || channel->isEmpty())
    {
        return;
    }

    auto fileNameBase = channel->getDisplayName().trimmed();
    if (fileNameBase.isEmpty())
    {
        fileNameBase = QStringLiteral("chat");
    }
    static const QRegularExpression invalidFileChars(
        QStringLiteral(R"([\\/:*?"<>|])"));
    fileNameBase.replace(invalidFileChars, QStringLiteral("_"));

    const auto defaultFileName =
        QStringLiteral("%1-%2.txt")
            .arg(fileNameBase,
                 QDateTime::currentDateTime().toString(
                     QStringLiteral("yyyyMMdd-HHmmss")));

    const auto path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export chat"), defaultFileName,
        QStringLiteral("Text files (*.txt);;All files (*)"));
    if (path.isEmpty())
    {
        return;
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    {
        QMessageBox::warning(
            this, QStringLiteral("Export chat"),
            QStringLiteral("Couldn't write the chat export file."));
        return;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);

    const auto snapshot = channel->getMessageSnapshot();
    for (const auto &message : snapshot)
    {
        if (!message)
        {
            continue;
        }

        const auto timestamp =
            message->serverReceivedTime.isValid()
                ? message->serverReceivedTime.toString(
                      QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                : message->parseTime.toString(QStringLiteral("HH:mm:ss"));
        auto author = message->displayName.trimmed();
        if (author.isEmpty())
        {
            author = message->loginName.trimmed();
        }

        QString platformCode;
        switch (message->platform)
        {
            case MessagePlatform::Kick:
                platformCode = QStringLiteral("K");
                break;
            case MessagePlatform::YouTube:
                platformCode = QStringLiteral("YT");
                break;
            case MessagePlatform::TikTok:
                platformCode = QStringLiteral("TT");
                break;
            case MessagePlatform::AnyOrTwitch:
            default:
                platformCode = QStringLiteral("T");
                break;
        }

        stream << '[' << platformCode << '-' << timestamp << "] ";
        if (!author.isEmpty())
        {
            stream << author << ": ";
        }
        stream << message->messageText << '\n';
    }

    if (!file.commit())
    {
        QMessageBox::warning(
            this, QStringLiteral("Export chat"),
            QStringLiteral("Couldn't finish writing the chat export file."));
    }
}

std::unique_ptr<QMenu> SplitHeader::createChatModeMenu()
{
    auto menu = std::make_unique<QMenu>();

    menu->addAction("Hide", this, [this] {
        this->split_->setChatModeIndicatorVisible(false);
    });

    return menu;
}

void SplitHeader::popupMainMenu(const QPoint &globalPosition)
{
    auto *menu = this->createMainMenu().release();
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(globalPosition);
}

void SplitHeader::openTabSettings()
{
    QPointer<Split> split = this->split_;
    const bool isActivityPane = this->split_->isActivityPane();
    QTimer::singleShot(0, this, [split, isActivityPane] {
        if (split == nullptr)
        {
            return;
        }

        if (isActivityPane)
        {
            split->showSettingsDialog();
        }
        else
        {
            split->changeChannel();
        }
    });
}

void SplitHeader::popupTitleSettingsButtonMenu(const QPoint &globalPosition)
{
    QMenu menu(this);
    menu.addAction("Hide settings cog", this, [this] {
        this->split_->setTitleSettingsButtonVisible(false);
        this->setTitleSettingsButtonVisible(false);
    });
    menu.exec(globalPosition);
}

void SplitHeader::setTitleSettingsButtonVisible(bool visible)
{
    if (this->titleSettingsButton_ == nullptr)
    {
        return;
    }

    if (visible && this->split_->titleSettingsButtonVisible())
    {
        this->positionTitleSettingsButton();
        this->titleSettingsButton_->show();
        this->titleSettingsButton_->raise();
        return;
    }

    this->titleSettingsButton_->hide();
}

void SplitHeader::positionTitleSettingsButton()
{
    if (this->titleLabel_ == nullptr || this->titleSettingsButton_ == nullptr)
    {
        return;
    }

    const auto labelRect = this->titleLabel_->geometry();
    const int leftPadding = static_cast<int>(
        std::round(TITLE_LABEL_LEFT_PADDING * this->scale()));
    const int gap =
        static_cast<int>(std::round(TITLE_SETTINGS_BUTTON_GAP * this->scale()));
    const int buttonWidth = this->titleSettingsButton_->width();
    const int textAreaWidth =
        std::max(0, labelRect.width() - leftPadding - buttonWidth - gap);
    const auto text = this->titleLabel_->getText();
    const auto metrics = getApp()->getFonts()->getFontMetrics(
        this->titleLabel_->getFontStyle(), this->titleLabel_->scale());
    const auto visibleText =
        metrics.elidedText(text, Qt::ElideRight, textAreaWidth);
    const int visibleTextWidth =
        static_cast<int>(std::ceil(metrics.horizontalAdvance(visibleText)));
    int textLeft = labelRect.x() + leftPadding;
    if (visibleTextWidth < textAreaWidth)
    {
        textLeft += (textAreaWidth - visibleTextWidth) / 2;
    }

    const int minX = labelRect.x() + leftPadding;
    const int maxX = std::max(minX, labelRect.right() - buttonWidth + 1);
    int x = textLeft + visibleTextWidth + gap;
    if (this->split_->isActivityPane())
    {
        x += static_cast<int>(std::round(9 * this->scale()));
    }
    x = std::clamp(x, minX, maxX);
    const int y = labelRect.y() +
                  (labelRect.height() - this->titleSettingsButton_->height()) /
                      2;
    this->titleSettingsButton_->move(x, y);
}

bool SplitHeader::isTitleSettingsButtonHoverArea() const
{
    const auto globalCursor = QCursor::pos();
    if (this->titleLabel_ != nullptr &&
        this->titleLabel_->rect().contains(
            this->titleLabel_->mapFromGlobal(globalCursor)))
    {
        return true;
    }

    return this->titleSettingsButton_ != nullptr &&
           this->titleSettingsButton_->rect().contains(
               this->titleSettingsButton_->mapFromGlobal(globalCursor));
}

void SplitHeader::updateRoomModes()
{
    assert(this->modeButton_ != nullptr);

    auto hideModeButton = [this] {
        this->modeTooltipText_.clear();
        this->modeButton_->hide();
        if (this->modeButton_->underMouse() && this->tooltipWidget_->isVisible())
        {
            this->hideHoverTooltip();
        }
    };

    auto applyPresentation = [this,
                              &hideModeButton](const RoomModePresentation
                                                   &presentation) {
        this->modeTooltipText_ = presentation.tooltip;

        if (!presentation.text.isEmpty())
        {
            this->modeButton_->setEnabled(true);
            this->modeButton_->setText(presentation.text);
            this->modeButton_->show();

            if (this->modeButton_->underMouse() &&
                this->tooltipWidget_->isVisible() &&
                !this->modeTooltipText_.isEmpty())
            {
                this->showHoverTooltip(this->modeButton_,
                                       this->modeTooltipText_, true);
            }
        }
        else
        {
            hideModeButton();
        }
    };

    if (this->split_->isActivityPane() ||
        !getSettings()->headerChatModeIndicator ||
        !this->split_->chatModeIndicatorVisible())
    {
        hideModeButton();
        return;
    }

    auto *channel = this->split_->getChannel().get();
    auto *twitchChannel = roomModeTwitchChannel(channel);
    auto *kickChannel = roomModeKickChannel(channel);

    RoomModePresentation presentation;

    if (twitchChannel != nullptr)
    {
        auto roomModes = twitchChannel->accessRoomModes();
        appendRoomModePresentation(presentation, formatRoomModes(*roomModes));
    }

    if (kickChannel != nullptr)
    {
        appendRoomModePresentation(presentation,
                                   formatRoomModes(kickChannel->roomModes()));
    }

    applyPresentation(presentation);
}

void SplitHeader::resetThumbnail()
{
    this->lastThumbnail_.invalidate();
    this->thumbnail_.clear();
    this->thumbnailUrl_.clear();
}

void SplitHeader::handleChannelChanged()
{
    this->resetThumbnail();

    this->updateChannelText();

    this->channelConnections_.clear();

    auto channel = this->split_->getChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            twitchChannel->streamStatusChanged, [this]() {
                this->updateChannelText();
            });
    }
    else if (auto *kickChannel = dynamic_cast<KickChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(kickChannel->streamDataChanged,
                                                 [this]() {
                                                     this->updateChannelText();
                                                 });
    }
    else if (auto *mergedChannel = dynamic_cast<MergedChannel *>(channel.get()))
    {
        this->channelConnections_.managedConnect(
            mergedChannel->streamStatusChanged, [this]() {
                this->updateChannelText();
            });
    }
}

void SplitHeader::scaleChangedEvent(float scale)
{
    int w = int(BUTTON_WIDTH * scale);
    int addSplitWidth = int(ADD_SPLIT_BUTTON_WIDTH * scale);

    this->setFixedHeight(w);
    this->dropdownButton_->setFixedWidth(w);
    this->titleSettingsButton_->setFixedSize(
        int(TITLE_SETTINGS_BUTTON_WIDTH * scale), w);
    this->alertsButton_->setFixedWidth(w);
    this->moderationButton_->setFixedWidth(w);
    this->chattersButton_->setFixedWidth(w);
    this->viewerCountContainer_->setFixedHeight(w);
    this->viewerCountIcon_->setFixedSize(static_cast<int>(17 * scale), w);
    this->viewerCountLabel_->setFixedHeight(w);
    const auto queuedCountFont =
        getApp()->getFonts()->getFont(FontStyle::UiMediumBold, scale);
    const auto queuedCountMetrics = QFontMetrics(queuedCountFont);
    const auto queuedCountWidth =
        queuedCountMetrics.horizontalAdvance(
            QString(QUEUED_SLOW_CHAT_COUNT_DIGITS, QChar(u'9'))) +
        static_cast<int>(std::round(12 * scale));
    this->queuedSlowChatCountLabel_->setFixedWidth(queuedCountWidth);

    this->addButton_->setFixedWidth(addSplitWidth);
    this->positionTitleSettingsButton();
}

void SplitHeader::setAddButtonVisible(bool value)
{
    this->addButton_->setVisible(value && !this->split_->isActivityPane());
}

void SplitHeader::updateChannelText()
{
    auto indirectChannel = this->split_->getIndirectChannel();
    auto channel = this->split_->getChannel();
    this->titleLabel_->setPadding(
        this->split_->isActivityPane()
            ? QMargins(TITLE_LABEL_LEFT_PADDING + 22, -1,
                       TITLE_LABEL_RIGHT_PADDING, 1)
            : QMargins(TITLE_LABEL_LEFT_PADDING, -1,
                       TITLE_LABEL_RIGHT_PADDING, 1));
    this->isLive_ = false;
    this->tooltipText_ = QString();
    this->viewerCountTooltipText_ = QString();

    auto title = channel->getLocalizedName();

    if (indirectChannel.getType() == Channel::Type::TwitchWatching)
    {
        title = "watching: " + (title.isEmpty() ? "none" : title);
    }

    if (this->split_->isActivityPane())
    {
        this->resetThumbnail();
        this->viewerCountContainer_->hide();
        this->viewerCountLabel_->setText(QString());
        this->titleLabel_->setText(this->split_->activityPaneTitle());
        this->positionTitleSettingsButton();
        return;
    }

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        const auto streamStatus = twitchChannel->accessStreamStatus();

        if (streamStatus->live)
        {
            this->isLive_ = true;
            // XXX: This URL format can be figured out from the Helix Get Streams API which we parse in TwitchChannel::parseLiveStatus
            QString url = "https://static-cdn.jtvnw.net/"
                          "previews-ttv/live_user_" +
                          channel->getName().toLower();
            switch (getSettings()->thumbnailSizeStream.getValue())
            {
                case 1:
                    url.append("-80x45.jpg");
                    break;
                case 2:
                    url.append("-160x90.jpg");
                    break;
                case 3:
                    url.append("-360x203.jpg");
                    break;
                default:
                    url = "";
            }
            this->updateThumbnail(url, false);
            this->tooltipText_ = formatTooltip(*streamStatus, this->thumbnail_);
            title += formatTitle(*streamStatus, *getSettings());
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(*streamStatus);
        }
    }
    else if (auto *kickChannel = dynamic_cast<KickChannel *>(channel.get()))
    {
        const auto &stream = kickChannel->streamData();
        auto twitch = toTwitchStreamStatus(stream);
        if (stream.isLive)
        {
            this->isLive_ = true;
            this->updateThumbnail(stream.thumbnailUrl, true);
            this->tooltipText_ = formatTooltip(twitch, this->thumbnail_, true);
            title += formatTitle(twitch, *getSettings());
        }
        else
        {
            this->tooltipText_ = formatOfflineTooltip(twitch);
        }
    }
    else if (auto *mergedChannel = dynamic_cast<MergedChannel *>(channel.get()))
    {
        this->isLive_ = mergedChannel->isLive();
        this->tooltipText_ = this->mergedStreamPreviewTooltip(mergedChannel);
        if (this->tooltipText_.isEmpty())
        {
            this->tooltipText_ = mergedChannel->tooltipText();
        }
        if (this->isLive_)
        {
            title += mergedChannel->statusSuffix();
        }
    }

    if (!title.isEmpty() && !this->split_->getFilters().empty())
    {
        title += " - filtered";
    }

    const auto viewerCount = viewerCountDisplay(channel.get(), this->split_);
    const auto refreshViewerCountTooltip =
        viewerCount.visible && this->viewerCountContainer_->isVisible() &&
        this->viewerCountContainer_->underMouse() &&
        this->tooltipWidget_->isVisible();

    this->viewerCountContainer_->setVisible(viewerCount.visible);
    this->viewerCountLabel_->setText(viewerCount.text);
    this->viewerCountTooltipText_ = viewerCount.tooltip;
    if (refreshViewerCountTooltip && !this->viewerCountTooltipText_.isEmpty())
    {
        this->showHoverTooltip(this->viewerCountContainer_,
                               this->viewerCountTooltipText_, false);
    }

    this->titleLabel_->setText(title.isEmpty() ? "<empty>" : title);
    this->positionTitleSettingsButton();
}

void SplitHeader::updateIcons()
{
    auto channel = this->split_->getChannel();
    auto activityInactive = this->theme->splits.header.text;
    activityInactive.setAlpha(this->theme->isLightTheme() ? 120 : 150);
    const auto activityActive = this->theme->isLightTheme()
                                    ? QColor(24, 24, 24)
                                    : QColor(255, 255, 255);

    if (this->split_->isActivityPane())
    {
        this->viewerCountContainer_->hide();
        this->viewerCountLabel_->setText(QString());
        this->queuedSlowChatCountLabel_->hide();
        this->queuedSlowChatCountLabel_->setText(QString());
        this->alertsButton_->hide();
        this->moderationButton_->hide();
        this->chattersButton_->hide();
        this->clearActivityButton_->show();

        return;
    }
    this->clearActivityButton_->hide();

    QString queuedSlowChatText;
    if (this->slowChatQueueIndicatorReady_)
    {
        const int queuedSlowChatMessages =
            this->split_->getChannelView().pendingSlowChatMessageCount();
        if (this->split_->slowerChatEnabled() && queuedSlowChatMessages > 0)
        {
            queuedSlowChatText = localizeNumbers(queuedSlowChatMessages);
        }
    }
    this->queuedSlowChatCountLabel_->setText(queuedSlowChatText);
    this->queuedSlowChatCountLabel_->setVisible(!queuedSlowChatText.isEmpty());

    this->alertsButton_->show();
    this->alertsButton_->setColor(this->split_->hasLinkedActivityPane()
                                      ? std::optional<QColor>(activityActive)
                                      : std::optional<QColor>(activityInactive));

    if (channel->isTwitchOrKickChannel() || channel->isMergedChannel())
    {
        auto moderationMode = this->split_->getModerationMode() &&
                              !getSettings()->moderationActions.empty();

        if (moderationMode)
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationEnabled-darkMode.svg",
                .light = ":/buttons/moderationEnabled-lightMode.svg",
            });
        }
        else
        {
            this->moderationButton_->setSource({
                .dark = ":/buttons/moderationDisabled-darkMode.svg",
                .light = ":/buttons/moderationDisabled-lightMode.svg",
            });
        }

        if (channel->hasModRights() || moderationMode)
        {
            this->moderationButton_->show();
        }
        else
        {
            this->moderationButton_->hide();
        }

        if (auto *viewerListChannel = viewerListTwitchChannel(channel.get());
            viewerListChannel && viewerListChannel->hasModRights())
        {
            const auto viewerListOpen =
                !this->split_->findChildren<ChatterListWidget *>().isEmpty();
            this->chattersButton_->setColor(
                viewerListOpen ? std::optional<QColor>(activityActive)
                               : std::optional<QColor>(activityInactive));
            this->chattersButton_->show();
        }
        else
        {
            this->chattersButton_->setColor(
                std::optional<QColor>(activityInactive));
            this->chattersButton_->hide();
        }
    }
    else
    {
        this->moderationButton_->hide();
        this->chattersButton_->setColor(std::optional<QColor>(activityInactive));
        this->chattersButton_->hide();
    }
}

void SplitHeader::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);

    QColor background = this->theme->splits.header.background;
    QColor border = this->theme->splits.header.border;

    if (this->split_->hasFocus())
    {
        background = this->theme->splits.header.focusedBackground;
        border = this->theme->splits.header.focusedBorder;
    }

    painter.fillRect(this->rect(), background);
    painter.setPen(border);
    painter.drawRect(0, 0, this->width() - 1, this->height() - 2);
    painter.fillRect(0, this->height() - 1, this->width(), 1, background);
}

void SplitHeader::mousePressEvent(QMouseEvent *event)
{
    switch (event->button())
    {
        case Qt::LeftButton: {
            this->split_->setFocus(Qt::MouseFocusReason);

            this->dragging_ = true;

            this->dragStart_ = event->pos();
        }
        break;

        case Qt::RightButton: {
            this->popupMainMenu(this->mapToGlobal(event->pos() + QPoint(0, 4)));
        }
        break;

        case Qt::MiddleButton: {
            this->split_->openInBrowser();
        }
        break;

        default: {
        }
        break;
    }

    this->doubleClicked_ = false;
}

void SplitHeader::mouseReleaseEvent(QMouseEvent * /*event*/)
{
    this->dragging_ = false;
}

void SplitHeader::mouseMoveEvent(QMouseEvent *event)
{
    if (this->dragging_)
    {
        if (distance(this->dragStart_, event->pos()) > 15 * this->scale())
        {
            this->split_->drag();
            this->dragging_ = false;
        }
    }
}

void SplitHeader::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
    {
        this->dragging_ = false;
        this->hideHoverTooltip();
        this->openTabSettings();
        event->accept();
    }
    this->doubleClicked_ = true;
}

void SplitHeader::enterEvent(QEnterEvent *event)
{
    BaseWidget::enterEvent(event);
}

void SplitHeader::leaveEvent(QEvent *event)
{
    this->hideHoverTooltip();
    this->setTitleSettingsButtonVisible(false);

    BaseWidget::leaveEvent(event);
}

void SplitHeader::themeChangedEvent()
{
    auto palette = QPalette();

    if (this->split_->hasFocus())
    {
        palette.setColor(QPalette::WindowText,
                         this->theme->splits.header.focusedText);
    }
    else
    {
        palette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    }
    this->titleLabel_->setPalette(palette);
    palette.setColor(QPalette::WindowText, viewerCountColor());
    this->viewerCountLabel_->setPalette(palette);

    auto queuedPalette = QPalette();
    queuedPalette.setColor(QPalette::WindowText, this->theme->splits.header.text);
    this->queuedSlowChatCountLabel_->setPalette(queuedPalette);

    auto bg = this->theme->splits.header.background;
    this->addButton_->setOptions({
        .background = bg,
        .backgroundHover = bg,
    });

    this->updateIcons();
    this->update();
}

void SplitHeader::reloadChannelEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedChannelEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedChannelEmotes_ = now;

    auto channel = this->split_->getChannel();

    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshFFZChannelEmotes(true);
        twitchChannel->refreshBTTVChannelEmotes(true);
        twitchChannel->refreshSevenTVChannelEmotes(true);
    }
    else if (auto *kc = dynamic_cast<KickChannel *>(channel.get()))
    {
        kc->reloadSeventvEmotes(true);
    }
}

void SplitHeader::reloadSubscriberEmotes()
{
    using namespace std::chrono_literals;

    auto now = std::chrono::steady_clock::now();
    if (this->lastReloadedSubEmotes_ + 30s > now)
    {
        return;
    }
    this->lastReloadedSubEmotes_ = now;

    auto channel = this->split_->getChannel();
    if (auto *twitchChannel = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        twitchChannel->refreshTwitchChannelEmotes(true);
    }
}

void SplitHeader::reconnect()
{
    this->split_->getChannel()->reconnect();
}

}  // namespace chatterino
