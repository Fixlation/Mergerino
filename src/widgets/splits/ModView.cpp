// SPDX-License-Identifier: MIT
#include "widgets/splits/ModView.hpp"

#include "Application.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/builtin/MassModeration.hpp"
#include "controllers/commands/CommandController.hpp"
#include "controllers/moderation/ModerationProfiles.hpp"
#include "controllers/userdata/UserDataController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeLiveChat.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "singletons/WindowManager.hpp"
#include "util/PostToThread.hpp"
#include "widgets/buttons/DrawnButton.hpp"
#include "widgets/buttons/LabelButton.hpp"
#include "widgets/buttons/SvgButton.hpp"
#include "widgets/dialogs/EditUserNotesDialog.hpp"
#include "widgets/dialogs/SettingsDialog.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/Label.hpp"
#include "widgets/splits/ModViewButton.hpp"
#include "widgets/splits/ModViewRules.hpp"
#include "widgets/splits/Split.hpp"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTextLayout>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>
#include <mutex>

namespace chatterino {
using moderation::ModerationProfiles;
using moderation::ModerationReview;
using moderation::ReviewInput;
using moderation::ReviewRules;
using moderation::ReviewUser;
using moderation::RuleAction;

namespace {
constexpr int HEADER_HEIGHT = 28;
constexpr int SMALL_BUTTON_SIZE = 24;
constexpr int WIDE_LAYOUT_WIDTH = 640;
constexpr int COMPACT_IDENTITY_WIDTH = 340;
constexpr int TAB_REVIEW = 0;
constexpr int TAB_TRACKING = 1;
constexpr int TAB_RULES = 2;
constexpr int TAB_LOG = 3;
constexpr int FEEDBACK_DURATION_MS = 6000;
constexpr size_t MAX_LOG = 500;
constexpr size_t MAX_QUEUED_ACTIONS = 500;
constexpr int MAX_AGE_LOOKUPS = 1000;
constexpr int BULK_TIMEOUT = 600;
constexpr qint64 MAX_IMPORT_BYTES = 10 * 1024 * 1024;
constexpr auto KEY_HINT = "Keys: J/K move · D dismiss · F flag · T timeout · "
                          "B ban · Enter confirm · Esc cancel";

// Qt::UserRole holds the user key or message ID, UserRole + 1 the revision.
enum ItemRole {
    DetailRole = Qt::UserRole + 2,
    ColorRole,
    IconRole,
    StampRole,
    TimeRole,
    DeletedRole,
};

QLabel *plainLabel(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    return label;
}

QLabel *mutedLabel(const QString &text, QWidget *parent)
{
    auto *label = plainLabel(text, parent);
    label->setProperty("muted", true);
    return label;
}

QWidget *emptyState(QLabel *&title, QLabel *&hint, QWidget *parent)
{
    auto *box = new QWidget(parent);
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->setSpacing(4);
    title = plainLabel({}, box);
    title->setProperty("strong", true);
    title->setAlignment(Qt::AlignHCenter);
    hint = mutedLabel({}, box);
    hint->setAlignment(Qt::AlignHCenter);
    for (auto *label : {title, hint})
    {
        auto policy = label->sizePolicy();
        policy.setHorizontalPolicy(QSizePolicy::Ignored);
        label->setSizePolicy(policy);
    }
    box->setMinimumWidth(0);
    layout->addStretch(2);
    layout->addWidget(title);
    layout->addWidget(hint);
    layout->addStretch(3);
    return box;
}

QListWidget *userList(const QString &name, QWidget *parent)
{
    auto *list = new QListWidget(parent);
    list->setObjectName(name);
    list->setFrameShape(QFrame::NoFrame);
    list->setSelectionMode(QAbstractItemView::SingleSelection);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setMouseTracking(true);
    list->viewport()->setAttribute(Qt::WA_Hover);
    return list;
}

QString platformIconPath(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return ":/platforms/kick.svg";
        case MessagePlatform::YouTube:
            return ":/platforms/youtube.svg";
        case MessagePlatform::TikTok:
            return ":/platforms/tiktok.svg";
        case MessagePlatform::AnyOrTwitch:
        default:
            return ":/platforms/twitch.svg";
    }
}

QPixmap platformPixmap(const QString &path, int size, qreal ratio)
{
    static QHash<QString, QPixmap> cache;
    const auto key = QString("%1:%2:%3").arg(path).arg(size).arg(ratio);
    auto it = cache.find(key);
    if (it == cache.end())
    {
        QPixmap pixmap(qRound(size * ratio), qRound(size * ratio));
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        QSvgRenderer(path).render(&painter);
        painter.end();
        pixmap.setDevicePixelRatio(ratio);
        it = cache.insert(key, pixmap);
    }
    return *it;
}

QString ageText(qint64 elapsedMs)
{
    const auto seconds = std::max<qint64>(elapsedMs, 0) / 1000;
    if (seconds < 5)
        return "now";
    if (seconds < 60)
        return QString("%1s").arg(seconds);
    if (seconds < 3600)
        return QString("%1m").arg(seconds / 60);
    return QString("%1h").arg(seconds / 3600);
}

QString chatTimestamp(const QDateTime &timestamp)
{
    static QLocale locale("en_US");
    return timestamp.isValid()
               ? locale.toString(timestamp.toLocalTime().time(),
                                 getSettings()->timestampFormat.getValue())
               : QString();
}

int timeoutSeconds(const TimeoutButton &timeout)
{
    static const QMap<QString, int> durations{
        {"s", 1}, {"m", 60}, {"h", 3600}, {"d", 86400}, {"w", 604800},
    };
    return timeout.second * durations.value(timeout.first, 1);
}

QString watchingSummary(const ReviewRules &rules)
{
    QStringList parts;
    if (rules.enabled)
    {
        const std::pair<bool, const char *> checks[] = {
            {rules.bursts.enabled, "bursts"},
            {rules.repeats.enabled, "repeats"},
            {rules.links.enabled, "links"},
            {rules.coordinated.enabled, "copied text"},
            {rules.caps.enabled, "caps"},
            {rules.symbols.enabled, "symbol spam"},
            {rules.emotes.enabled, "emote spam"},
            {rules.walls.enabled, "text walls"},
            {rules.mentions.enabled, "mass mentions"},
            {rules.unusual.enabled, "unusual characters"},
            {rules.platformAlerts.enabled, "platform flags"},
            {rules.firstMessages.enabled, "first-time chatters"},
            {rules.newAccounts.enabled, "new accounts"},
            {rules.names.enabled, "name patterns"},
            {rules.returning.enabled, "returning offenders"},
        };
        for (const auto &[on, name] : checks)
            if (on)
                parts.append(name);
        int lists = 0;
        for (const auto &list : rules.lists)
            lists += list.enabled && !list.terms.isEmpty() ? 1 : 0;
        if (lists > 0)
            parts.append(lists == 1 ? QString("1 word list")
                                    : QString("%1 word lists").arg(lists));
    }
    if (parts.isEmpty())
        return "Automatic flags are off. Right-click a message to review "
               "someone.";
    if (parts.size() > 4)
    {
        const auto more = parts.size() - 3;
        parts = parts.mid(0, 3);
        parts.append(QString("%1 more").arg(more));
    }
    const auto last = parts.takeLast();
    return QString("Watching for %1.")
        .arg(parts.isEmpty() ? last : parts.join(", ") + " and " + last);
}

bool hasChat(const ChannelPtr &channel)
{
    return channel && !channel->isEmpty();
}

/// Bulk and rule actions never touch mods, VIPs, owners or you.
bool unattended(const QString &source)
{
    return source == "Bulk" || source.startsWith("Auto");
}

QString csvField(QString value)
{
    // Spreadsheets run cells starting with these as formulas
    if (!value.isEmpty() && QString("=+-@\t").contains(value.front()))
        value.prepend('\'');
    if (value.contains(',') || value.contains('"') || value.contains('\n') ||
        value.contains('\r'))
        value = QString("\"%1\"").arg(value.replace('"', "\"\""));
    return value;
}

QString safeFileName(QString name)
{
    name.replace(QRegularExpression(R"([\\/:*?"<>|]+)"), " ");
    name = name.simplified();
    return name.isEmpty() ? QString("Mod View profile") : name.left(60);
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
           file.commit();
}

QJsonObject readJsonFile(const QString &path, QString &error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        error = QString("Couldn't open %1.").arg(QFileInfo(path).fileName());
        return {};
    }
    if (file.size() > MAX_IMPORT_BYTES)
    {
        error = "That file is too large to be a Mod View profile.";
        return {};
    }
    QJsonParseError parse{};
    const auto document = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject())
    {
        error = "That file isn't a Mod View profile.";
        return {};
    }
    return document.object();
}

QColor blended(const QColor &base, const QColor &over, qreal amount)
{
    return QColor::fromRgbF(
        float(base.redF() + (over.redF() - base.redF()) * amount),
        float(base.greenF() + (over.greenF() - base.greenF()) * amount),
        float(base.blueF() + (over.blueF() - base.blueF()) * amount));
}

void paintRowBackground(QPainter &painter, const QStyleOptionViewItem &option,
                        const ModViewStyle &style)
{
    if (option.state.testFlag(QStyle::State_Selected))
    {
        painter.fillRect(option.rect, style.selected);
        painter.fillRect(option.rect.x(), option.rect.y(), int(2 * style.scale),
                         option.rect.height(), style.accent);
    }
    else if (option.state.testFlag(QStyle::State_MouseOver))
    {
        painter.fillRect(option.rect, style.hover);
    }
}

QJsonObject preferences()
{
    return QJsonDocument::fromJson(
               getSettings()->modViewPreferences.getValue().toUtf8())
        .object();
}

QString scopeFor(const ChannelPtr &channel)
{
    QString source = QString::number(int(channel->getType())) + ':';
    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        const auto &config = merged->config();
        source += QString::fromUtf8(
            QJsonDocument(
                QJsonObject{
                    {"twitch",
                     config.twitchEnabled
                         ? config.effectiveTwitchChannelName().toLower()
                         : QString()},
                    {"kick", config.kickEnabled
                                 ? config.effectiveKickChannelName().toLower()
                                 : QString()},
                    {"youtube", config.youtubeEnabled ? config.youtubeStreamUrl
                                                      : QString()},
                    {"tiktok", config.tiktokEnabled
                                   ? config.tiktokSource.toLower()
                                   : QString()}})
                .toJson(QJsonDocument::Compact));
    }
    else
    {
        source += channel->getName().toCaseFolded();
    }
    return QString::fromLatin1(
        QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}
}  // namespace

// Two-line user row: chat-colored name and age above a muted detail line.
class ModViewUserDelegate final : public QStyledItemDelegate
{
public:
    ModViewUserDelegate(const ModViewStyle &style, std::function<qint64()> now,
                        QObject *parent)
        : QStyledItemDelegate(parent)
        , style_(style)
        , now_(std::move(now))
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex & /*index*/) const override
    {
        return {0, QFontMetrics(option.font).height() * 2 + this->scaled(11)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::TextAntialiasing);
        paintRowBackground(*painter, option, this->style_);

        auto nameFont = option.font;
        nameFont.setWeight(QFont::DemiBold);
        const QFontMetrics metrics(option.font);
        const QFontMetrics nameMetrics(nameFont);
        auto body = option.rect.adjusted(this->scaled(10), this->scaled(5),
                                         -this->scaled(10), -this->scaled(5));
        const auto icon = index.data(IconRole).toString();
        if (!icon.isEmpty())
        {
            const auto size = this->scaled(14);
            painter->drawPixmap(
                body.left(), body.top() + (nameMetrics.height() - size) / 2,
                platformPixmap(icon, size,
                               painter->device()->devicePixelRatioF()));
            body.setLeft(body.left() + size + this->scaled(6));
        }

        auto nameRight = body.right();
        const auto stamp = index.data(StampRole).toLongLong();
        if (stamp >= 0)
        {
            const auto age = ageText(this->now_() - stamp);
            const auto width = metrics.horizontalAdvance(age);
            painter->setFont(option.font);
            painter->setPen(this->style_.muted);
            painter->drawText(QRect(body.right() - width, body.top(), width,
                                    nameMetrics.height()),
                              Qt::AlignRight | Qt::AlignVCenter, age);
            nameRight -= width + this->scaled(8);
        }

        auto color = index.data(ColorRole).value<QColor>();
        if (color.isValid())
            getApp()->getThemes()->normalizeColor(color);
        else
            color = this->style_.text;
        const QRect nameRect(body.left(), body.top(), nameRight - body.left(),
                             nameMetrics.height());
        painter->setFont(nameFont);
        painter->setPen(color);
        painter->drawText(
            nameRect, Qt::AlignLeft | Qt::AlignVCenter,
            nameMetrics.elidedText(index.data().toString(), Qt::ElideRight,
                                   nameRect.width()));

        const QRect detailRect(body.left(), nameRect.bottom() + 1, body.width(),
                               metrics.height());
        painter->setFont(option.font);
        painter->setPen(this->style_.muted);
        painter->drawText(
            detailRect, Qt::AlignLeft | Qt::AlignVCenter,
            metrics.elidedText(index.data(DetailRole).toString(),
                               Qt::ElideRight, detailRect.width()));
        painter->restore();
    }

private:
    int scaled(int value) const
    {
        return int(value * this->style_.scale);
    }

    const ModViewStyle &style_;
    std::function<qint64()> now_;
};

// Reads like a chat line: a muted timestamp followed by text that wraps back to
// the left edge. Deleted messages stay visible but are struck through.
class ModViewHistoryDelegate final : public QStyledItemDelegate
{
public:
    ModViewHistoryDelegate(const ModViewStyle &style, QObject *parent)
        : QStyledItemDelegate(parent)
        , style_(style)
    {
    }

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override
    {
        QTextLayout layout;
        const auto height = this->layoutRow(layout, option, index);
        return {0, int(std::ceil(height)) + this->scaled(6)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        painter->save();
        paintRowBackground(*painter, option, this->style_);
        QTextLayout layout;
        this->layoutRow(layout, option, index);
        painter->setPen(index.data(DeletedRole).toBool() ? this->style_.muted
                                                         : this->style_.text);
        layout.draw(painter, QPointF(option.rect.x() + this->scaled(10),
                                     option.rect.y() + this->scaled(3)));
        painter->restore();
    }

private:
    int scaled(int value) const
    {
        return int(value * this->style_.scale);
    }

    qreal layoutRow(QTextLayout &layout, const QStyleOptionViewItem &option,
                    const QModelIndex &index) const
    {
        const auto time = index.data(TimeRole).toString();
        const auto message = index.data().toString();
        const auto prefix = time.isEmpty() ? QString() : time + "  ";
        layout.setText(prefix + message);
        layout.setFont(option.font);
        QTextOption wrapping;
        wrapping.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        layout.setTextOption(wrapping);

        QList<QTextLayout::FormatRange> formats;
        QTextCharFormat timeFormat;
        timeFormat.setForeground(this->style_.muted);
        formats.append(
            QTextLayout::FormatRange{0, int(prefix.size()), timeFormat});
        if (index.data(DeletedRole).toBool())
        {
            QTextCharFormat deleted;
            deleted.setFontStrikeOut(true);
            formats.append(QTextLayout::FormatRange{
                int(prefix.size()), int(message.size()), deleted});
        }
        layout.setFormats(formats);

        // Measured from the whole list so that row heights don't depend on
        // whether the scrollbar is showing; it fits in the right margin.
        const qreal width = std::max(
            1, (option.widget ? option.widget->width() : option.rect.width()) -
                   this->scaled(20));
        qreal height = 0;
        layout.beginLayout();
        for (auto line = layout.createLine(); line.isValid();
             line = layout.createLine())
        {
            line.setLineWidth(width);
            line.setPosition({0, height});
            height += line.height();
        }
        layout.endLayout();
        return height;
    }

    const ModViewStyle &style_;
};

struct ModView::Inbox {
    struct Event {
        MessagePtr message;
        bool update = false;
        bool removed = false;
    };
    std::mutex mutex;
    std::deque<Event> pending;
    bool active = false;
    quint64 skipped = 0;

    void push(const MessagePtr &message, bool update = false,
              bool removed = false)
    {
        std::lock_guard lock(this->mutex);
        if (!this->active || !message)
            return;
        if (this->pending.size() >= 512)
        {
            this->pending.pop_front();
            ++this->skipped;
        }
        this->pending.push_back({message, update, removed});
    }
};

ModView::ModView(Split *split, QWidget *parent)
    : BaseWidget(parent)
    , split_(split)
    , inbox_(std::make_shared<Inbox>())
{
    this->clock_.start();
    this->setObjectName("ModView");
    this->setMinimumWidth(0);
    this->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    this->setAutoFillBackground(true);
    auto *root = new QVBoxLayout(this);
    // The native split supplies the surrounding divider.
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(this->buildHeader());
    root->addWidget(this->buildLockdownBar());

    this->content_ = new QSplitter(Qt::Vertical, this);
    this->content_->setObjectName("ModViewContent");
    this->content_->setChildrenCollapsible(false);
    this->content_->setHandleWidth(1);
    this->pages_ = new QStackedWidget(this->content_);
    this->pages_->setMinimumWidth(0);
    this->pages_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
    this->pages_->addWidget(this->buildReviewPage());
    this->pages_->addWidget(this->buildTrackingPage());
    this->pages_->addWidget(this->buildRulesPage());
    this->pages_->addWidget(this->buildLogPage());
    this->content_->addWidget(this->pages_);
    this->content_->addWidget(this->buildDetails());
    root->addWidget(this->content_, 1);
    root->addWidget(this->buildBulkBar());

    this->feedback_ = plainLabel({}, this);
    this->feedback_->setObjectName("ModViewStatus");
    this->feedback_->hide();
    root->addWidget(this->feedback_);

    this->setFocusPolicy(Qt::StrongFocus);
    this->setFocusProxy(this->queue_);
    QObject::connect(qApp, &QApplication::focusChanged, this,
                     [this](QWidget *, QWidget *current) {
                         if (this->split_ && this->split_->isModViewPane() &&
                             current &&
                             (current == this || this->isAncestorOf(current)))
                             this->split_->focused.invoke();
                     });
    this->connectControls();
    this->loadRulesEditor();
    this->refreshLists(true);
    this->scaleChangedEvent(this->scale());
}

QWidget *ModView::buildHeader()
{
    this->header_ = new QFrame(this);
    this->header_->setObjectName("ModViewHeader");
    auto *layout = new QHBoxLayout(this->header_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->tabs_ = new QTabBar(this->header_);
    this->tabs_->setObjectName("ModViewTabs");
    this->tabs_->setDrawBase(false);
    this->tabs_->setExpanding(false);
    this->tabs_->setUsesScrollButtons(true);
    this->tabs_->setMinimumWidth(0);
    this->tabs_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    this->tabs_->setElideMode(Qt::ElideRight);
    this->tabs_->setFocusPolicy(Qt::NoFocus);
    this->tabs_->addTab("Review");
    this->tabs_->addTab("Tracking");
    this->tabs_->addTab("Rules");
    this->tabs_->addTab("Log");
    this->tabs_->setTabToolTip(TAB_REVIEW, KEY_HINT);
    this->tabs_->setTabToolTip(TAB_TRACKING, KEY_HINT);
    this->tabs_->setTabToolTip(TAB_LOG, "Actions sent from Mod View");
    layout->addWidget(this->tabs_);
    layout->addStretch();

    this->tools_ = new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);
    this->tools_->setObjectName("ModViewTools");
    this->tools_->setToolTip("Moderation tools");
    this->tools_->setAccessibleName("Moderation tools");
    layout->addWidget(this->tools_);

    this->close_ = new SvgButton(
        {
            .dark = ":/buttons/x-darkMode.svg",
            .light = ":/buttons/x-lightMode.svg",
        },
        this, {7, 7});
    this->close_->setObjectName("ModViewClose");
    this->close_->setToolTip("Close Mod View");
    this->close_->setAccessibleName("Close Mod View");
    layout->addWidget(this->close_);
    return this->header_;
}

QWidget *ModView::buildReviewPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->reviewTools_ = new QWidget(page);
    auto *tools = new QHBoxLayout(this->reviewTools_);
    tools->setContentsMargins(8, 6, 6, 5);
    tools->setSpacing(4);
    this->reviewSearch_ = new QLineEdit(this->reviewTools_);
    this->reviewSearch_->setPlaceholderText("Filter flagged users");
    this->reviewSearch_->setClearButtonEnabled(true);
    tools->addWidget(this->reviewSearch_, 1);
    this->dismissAll_ = new ModViewButton("Dismiss all", this);
    this->dismissAll_->setToolTip("Dismiss current flags; tracking is kept");
    tools->addWidget(this->dismissAll_);
    layout->addWidget(this->reviewTools_);

    this->emptyQueue_ =
        emptyState(this->emptyQueueTitle_, this->emptyQueueHint_, page);
    layout->addWidget(this->emptyQueue_, 1);
    this->queue_ = userList("ModViewReviewQueue", page);
    this->queue_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->queue_->setItemDelegate(new ModViewUserDelegate(
        this->style_,
        [this] {
            return this->now();
        },
        this->queue_));
    layout->addWidget(this->queue_, 1);
    return page;
}

QWidget *ModView::buildTrackingPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(8, 6, 8, 5);
    tools->setSpacing(8);
    this->userSearch_ = new QLineEdit(page);
    this->userSearch_->setPlaceholderText("Find a user seen in this chat");
    this->userSearch_->setClearButtonEnabled(true);
    tools->addWidget(this->userSearch_, 1);
    this->searchMode_ = new ModViewComboBox(page);
    this->searchMode_->setObjectName("ModViewSearchMode");
    this->searchMode_->addItems({"Names", "Messages"});
    this->searchMode_->setToolTip("Search names, or what people said");
    this->searchMode_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    tools->addWidget(this->searchMode_);
    this->showRecent_ = new ModViewSwitch("Recent", page);
    this->showRecent_->setToolTip("Also list everyone seen recently");
    tools->addWidget(this->showRecent_);
    layout->addLayout(tools);

    this->emptyTracked_ =
        emptyState(this->emptyTrackedTitle_, this->emptyTrackedHint_, page);
    layout->addWidget(this->emptyTracked_, 1);
    this->tracked_ = userList("ModViewTrackedUsers", page);
    this->tracked_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    this->tracked_->setItemDelegate(new ModViewUserDelegate(
        this->style_,
        [this] {
            return this->now();
        },
        this->tracked_));
    layout->addWidget(this->tracked_, 1);
    return page;
}

QWidget *ModView::buildRulesPage()
{
    this->rulesEditor_ = new ModViewRulesEditor(this, this);
    auto *editor = this->rulesEditor_;
    editor->changed = [this] {
        if (!this->loadingRules_)
            this->rulesTimer_.start();
    };
    editor->profileSelected = [this](const QString &id) {
        this->selectProfile(id);
    };
    editor->profileCommand = [this](const QString &command) {
        this->runProfileCommand(command);
    };
    editor->platformToggled = [this](MessagePlatform platform, bool active) {
        this->setPlatformActive(platform, active);
    };
    editor->inlineActionsToggled = [this](bool enabled) {
        setInlineActionsPreference(this->channel_, enabled);
    };
    editor->testRequested = [this] {
        this->testRules();
    };
    editor->lockdownToggled = [this] {
        this->toggleLockdown();
    };
    return editor;
}

QWidget *ModView::buildLogPage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    auto *tools = new QHBoxLayout;
    tools->setContentsMargins(10, 6, 6, 5);
    tools->setSpacing(4);
    auto *title = mutedLabel("Sent this session", page);
    title->setWordWrap(false);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    tools->addWidget(title, 1);
    this->undo_ = new ModViewButton("Undo", this);
    this->undo_->setObjectName("ModViewUndo");
    this->undo_->setToolTip("Lift the selected timeout or ban");
    this->undo_->setEnabled(false);
    tools->addWidget(this->undo_);
    this->exportLog_ = new ModViewButton("Export", this);
    this->exportLog_->setObjectName("ModViewExportLog");
    this->exportLog_->setToolTip("Save the log as a CSV file");
    this->exportLog_->setEnabled(false);
    tools->addWidget(this->exportLog_);
    layout->addLayout(tools);

    this->emptyLog_ = emptyState(this->emptyLogTitle_, this->emptyLogHint_, page);
    this->emptyLogTitle_->setText("No actions yet");
    this->emptyLogHint_->setText(
        "Timeouts, bans, deletions and warnings sent from Mod View, by you or "
        "by your rules, are listed here.");
    layout->addWidget(this->emptyLog_, 1);
    this->logList_ = userList("ModViewLog", page);
    this->logList_->setItemDelegate(new ModViewUserDelegate(
        this->style_,
        [this] {
            return this->now();
        },
        this->logList_));
    this->logList_->hide();
    layout->addWidget(this->logList_, 1);
    return page;
}

QWidget *ModView::buildBulkBar()
{
    this->bulkBar_ = new QFrame(this);
    this->bulkBar_->setObjectName("ModViewBulk");
    this->bulkBar_->hide();
    auto *layout = new QVBoxLayout(this->bulkBar_);
    layout->setContentsMargins(8, 5, 8, 7);
    layout->setSpacing(5);

    auto *top = new QHBoxLayout;
    top->setSpacing(4);
    this->bulkLabel_ = plainLabel({}, this->bulkBar_);
    this->bulkLabel_->setObjectName("ModViewBulkLabel");
    top->addWidget(this->bulkLabel_, 1);
    this->bulkCancel_ = new ModViewButton("Clear", this, {8, 3});
    this->bulkCancel_->setObjectName("ModViewBulkCancel");
    top->addWidget(this->bulkCancel_, 0, Qt::AlignTop);
    layout->addLayout(top);

    auto *actions = new QHBoxLayout;
    actions->setSpacing(4);
    this->bulkDismiss_ = new ModViewButton("Dismiss", this, {10, 4});
    this->bulkDismiss_->setObjectName("ModViewBulkDismiss");
    this->bulkTimeout_ = new ModViewButton("Timeout 10m", this, {10, 4});
    this->bulkTimeout_->setObjectName("ModViewBulkTimeout");
    this->bulkTimeout_->setToolTip(
        "Time out everyone selected for 10 minutes, each on their own "
        "platform");
    this->bulkBan_ = new ModViewButton("Ban", this, {10, 4});
    this->bulkBan_->setObjectName("ModViewBulkBan");
    this->bulkConfirm_ = new ModViewButton("Ban all", this, {12, 4});
    this->bulkConfirm_->setObjectName("ModViewBulkConfirm");
    this->bulkConfirm_->hide();
    actions->addWidget(this->bulkDismiss_);
    actions->addWidget(this->bulkTimeout_);
    actions->addWidget(this->bulkBan_);
    actions->addStretch();
    actions->addWidget(this->bulkConfirm_);
    layout->addLayout(actions);
    return this->bulkBar_;
}

QWidget *ModView::buildLockdownBar()
{
    this->lockdownBar_ = new QFrame(this);
    this->lockdownBar_->setObjectName("ModViewLockdownBar");
    this->lockdownBar_->hide();
    auto *layout = new QHBoxLayout(this->lockdownBar_);
    layout->setContentsMargins(10, 4, 6, 4);
    layout->setSpacing(4);
    this->lockdownLabel_ = plainLabel({}, this->lockdownBar_);
    this->lockdownLabel_->setObjectName("ModViewLockdownLabel");
    layout->addWidget(this->lockdownLabel_, 1);
    this->endLockdown_ = new ModViewButton("End", this, {8, 3});
    this->endLockdown_->setObjectName("ModViewEndLockdown");
    this->endLockdown_->setToolTip("Undo what the lockdown turned on");
    layout->addWidget(this->endLockdown_, 0, Qt::AlignVCenter);
    return this->lockdownBar_;
}

QWidget *ModView::buildDetails()
{
    this->details_ = new QWidget(this);
    this->details_->setObjectName("ModViewDetails");
    this->details_->hide();
    auto *layout = new QVBoxLayout(this->details_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    this->identityRow_ = new QHBoxLayout;
    auto *identity = this->identityRow_;
    identity->setContentsMargins(10, 7, 4, 6);
    identity->setSpacing(0);
    this->platformIcon_ = new QLabel(this->details_);
    this->platformIcon_->setAlignment(Qt::AlignTop);
    this->platformIcon_->hide();
    identity->addWidget(this->platformIcon_);
    auto *names = new QVBoxLayout;
    names->setSpacing(1);
    this->userLabel_ =
        new Label(this, "Select a user to review", FontStyle::UiMediumBold);
    this->userLabel_->setObjectName("ModViewSelectedUser");
    this->userLabel_->setShouldElide(true);
    this->userLabel_->setPadding(QMargins(0, 0, 0, 0));
    this->userLabel_->setSizePolicy(QSizePolicy::Ignored,
                                    QSizePolicy::Preferred);
    names->addWidget(this->userLabel_);
    this->identityLabel_ = mutedLabel({}, this->details_);
    this->identityLabel_->setWordWrap(false);
    this->identityLabel_->setSizePolicy(QSizePolicy::Ignored,
                                        QSizePolicy::Preferred);
    names->addWidget(this->identityLabel_);
    identity->addLayout(names, 1);

    this->track_ = new ModViewButton("Track", this, {6, 2});
    this->track_->setObjectName("ModViewTrack");
    this->notes_ = new ModViewButton("Notes", this, {6, 2});
    this->notes_->setObjectName("ModViewNotes");
    this->userCard_ = new ModViewButton("Usercard", this, {6, 2});
    this->userCard_->setObjectName("ModViewUserCard");
    this->identityActions_ = new QWidget(this->details_);
    auto *identityActions = new QHBoxLayout(this->identityActions_);
    identityActions->setContentsMargins(0, 0, 0, 0);
    identityActions->setSpacing(0);
    for (auto *button : {this->track_, this->notes_, this->userCard_})
        identityActions->addWidget(button);
    identity->addWidget(this->identityActions_, 0, Qt::AlignTop);
    this->closeUser_ = new SvgButton(
        {
            .dark = ":/buttons/x-darkMode.svg",
            .light = ":/buttons/x-lightMode.svg",
        },
        this, {6, 6});
    this->closeUser_->setObjectName("ModViewCloseUser");
    this->closeUser_->setToolTip("Close this user");
    this->closeUser_->setAccessibleName("Close this user");
    identity->addWidget(this->closeUser_, 0, Qt::AlignTop);
    layout->addLayout(identity);
    this->wrappedActions_ = new QHBoxLayout;
    this->wrappedActions_->setContentsMargins(4, 0, 4, 4);
    this->wrappedActions_->setAlignment(Qt::AlignLeft);
    layout->addLayout(this->wrappedActions_);

    auto *reasons = new QVBoxLayout;
    reasons->setContentsMargins(10, 0, 10, 7);
    this->reasons_ = plainLabel({}, this->details_);
    this->reasons_->setObjectName("ModViewReasons");
    reasons->addWidget(this->reasons_);
    layout->addLayout(reasons);

    this->history_ = userList("ModViewUserHistory", this->details_);
    this->history_->setMinimumHeight(48);
    this->history_->setResizeMode(QListView::Adjust);
    this->history_->setItemDelegate(
        new ModViewHistoryDelegate(this->style_, this->history_));
    layout->addWidget(this->history_, 1);
    this->emptyHistory_ =
        mutedLabel("No messages seen this session.", this->details_);
    this->emptyHistory_->setAlignment(Qt::AlignCenter);
    this->emptyHistory_->hide();
    layout->addWidget(this->emptyHistory_, 1);

    this->actions_ = new QFrame(this->details_);
    this->actions_->setObjectName("ModViewActions");
    auto *band = new QVBoxLayout(this->actions_);
    band->setContentsMargins(8, 6, 8, 8);
    band->setSpacing(6);

    this->timeouts_ = new QWidget(this->actions_);
    this->timeouts_->setObjectName("ModViewTimeouts");
    auto *timeouts = new QHBoxLayout(this->timeouts_);
    timeouts->setContentsMargins(0, 0, 0, 0);
    timeouts->setSpacing(1);
    band->addWidget(this->timeouts_);

    this->accessLabel_ = mutedLabel({}, this->actions_);
    this->accessLabel_->setObjectName("ModViewAccess");
    band->addWidget(this->accessLabel_);

    this->primaryActions_ = new QWidget(this->actions_);
    auto *primary = new QHBoxLayout(this->primaryActions_);
    primary->setContentsMargins(0, 0, 0, 0);
    primary->setSpacing(4);
    this->dismiss_ = new ModViewButton("Dismiss", this, {12, 4});
    this->dismiss_->setObjectName("ModViewDismiss");
    this->mark_ = new ModViewButton("Flag for review", this, {12, 4});
    this->mark_->setObjectName("ModViewMark");
    primary->addWidget(this->dismiss_);
    primary->addWidget(this->mark_);
    primary->addStretch();
    this->more_ = new DrawnButton(DrawnButton::Symbol::Kebab, {}, this);
    this->more_->setObjectName("ModViewMore");
    this->more_->setToolTip("More actions");
    this->more_->setAccessibleName("More actions");
    primary->addWidget(this->more_);
    this->ban_ = new ModViewButton("Ban", this, {12, 4});
    this->ban_->setObjectName("ModViewBan");
    primary->addWidget(this->ban_);
    band->addWidget(this->primaryActions_);

    this->confirmation_ = new QWidget(this->actions_);
    auto *confirmation = new QHBoxLayout(this->confirmation_);
    confirmation->setContentsMargins(0, 0, 0, 0);
    confirmation->setSpacing(4);
    this->confirmationLabel_ = plainLabel({}, this->confirmation_);
    confirmation->addWidget(this->confirmationLabel_, 1);
    this->duration_ = new QSpinBox(this->confirmation_);
    this->duration_->setObjectName("ModViewTimeoutDuration");
    this->duration_->setRange(1, 1209600);
    this->duration_->setValue(300);
    this->duration_->setSuffix(" s");
    this->duration_->setButtonSymbols(QAbstractSpinBox::NoButtons);
    this->duration_->setAlignment(Qt::AlignCenter);
    this->duration_->setToolTip("Timeout duration in seconds");
    this->duration_->hide();
    confirmation->addWidget(this->duration_);
    this->cancel_ = new ModViewButton("Cancel", this, {10, 4});
    confirmation->addWidget(this->cancel_);
    this->confirm_ = new ModViewButton("Confirm", this, {12, 4});
    this->confirm_->setObjectName("ModViewConfirmAction");
    confirmation->addWidget(this->confirm_);
    this->confirmation_->hide();
    band->addWidget(this->confirmation_);
    layout->addWidget(this->actions_);
    return this->details_;
}

void ModView::connectControls()
{
    this->rulesTimer_.setInterval(350);
    this->rulesTimer_.setSingleShot(true);
    QObject::connect(&this->rulesTimer_, &QTimer::timeout, this,
                     &ModView::saveRulesEditor);
    this->refreshTimer_.setInterval(100);
    QObject::connect(&this->refreshTimer_, &QTimer::timeout, this,
                     &ModView::flushMessages);
    this->feedbackTimer_.setInterval(FEEDBACK_DURATION_MS);
    this->feedbackTimer_.setSingleShot(true);
    QObject::connect(&this->feedbackTimer_, &QTimer::timeout, this, [this] {
        this->feedback({});
    });
    // Queued actions go out a few per second so a raid doesn't hit rate
    // limits.
    this->actionTimer_.setInterval(300);
    QObject::connect(&this->actionTimer_, &QTimer::timeout, this,
                     &ModView::dispatchAction);
    this->ageTimer_.setInterval(1500);
    this->ageTimer_.setSingleShot(true);
    QObject::connect(&this->ageTimer_, &QTimer::timeout, this,
                     &ModView::lookUpAccountAges);
    this->duration_->installEventFilter(this);
    this->duration_->setFocusPolicy(Qt::StrongFocus);
    QObject::connect(this->reviewSearch_, &QLineEdit::textChanged, this,
                     [this] {
                         this->refreshLists(true);
                     });
    const auto searchChanged = [this] {
        // Rows show what matched, so they are described again
        for (int i = 0; i < this->tracked_->count(); ++i)
            this->tracked_->item(i)->setData(Qt::UserRole + 1, 0);
        this->userSearch_->setPlaceholderText(
            this->searchMode_->currentIndex() == 1
                ? "Find what someone said"
                : "Find a user seen in this chat");
        this->refreshLists(true);
    };
    QObject::connect(this->userSearch_, &QLineEdit::textChanged, this,
                     searchChanged);
    QObject::connect(this->searchMode_, &QComboBox::currentIndexChanged, this,
                     searchChanged);
    QObject::connect(this->showRecent_, &QCheckBox::toggled, this, [this] {
        this->refreshLists(true);
    });
    for (auto *list : {this->queue_, this->tracked_})
    {
        QObject::connect(
            list, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *item) {
                if (item)
                    this->selectUser(item->data(Qt::UserRole).toString());
            });
        QObject::connect(list, &QListWidget::itemSelectionChanged, this,
                         &ModView::updateBulkBar);
        list->installEventFilter(this);
    }
    QObject::connect(this->logList_, &QListWidget::currentItemChanged, this,
                     [this](QListWidgetItem *item) {
                         bool undoable = false;
                         const auto id =
                             item ? item->data(Qt::UserRole).toULongLong() : 0;
                         for (const auto &entry : this->log_)
                             if (entry.id == id)
                                 undoable =
                                     entry.done && !entry.failed &&
                                     (entry.request.action == Action::Timeout ||
                                      entry.request.action == Action::Ban);
                         this->undo_->setEnabled(undoable);
                     });
    QObject::connect(this->undo_, &Button::leftClicked, this,
                     &ModView::undoLogEntry);
    QObject::connect(this->exportLog_, &Button::leftClicked, this,
                     &ModView::exportLog);
    QObject::connect(this->tabs_, &QTabBar::currentChanged, this,
                     [this](int index) {
                         this->pages_->setCurrentIndex(index);
                         this->confirmingBulk_ = false;
                         this->updateDetailsVisibility();
                         this->updateBulkBar();
                         if (index == TAB_LOG)
                             this->refreshLog();
                     });
    QObject::connect(this->close_, &Button::leftClicked, this, [this] {
        this->closeRequested.invoke();
    });
    QObject::connect(this->tools_, &Button::leftClicked, this,
                     &ModView::showTools);
    QObject::connect(this->dismissAll_, &Button::leftClicked, this, [this] {
        this->review_.dismissAll(this->now());
        this->refreshLists(true);
        if (this->tabs_->currentIndex() == TAB_REVIEW)
            this->selectUser({});
    });
    QObject::connect(this->track_, &Button::leftClicked, this, [this] {
        if (const auto *user = this->review_.find(this->selectedKey_))
        {
            if (!this->review_.track(user->key, !user->tracked))
                this->feedback("Tracking is limited to 200 users per chat. "
                               "Untrack a user first.");
            this->savePreferences();
            this->refreshLists(true);
        }
    });
    QObject::connect(this->mark_, &Button::leftClicked, this, [this] {
        this->review_.mark(this->selectedKey_, this->now());
        this->refreshLists(true);
    });
    QObject::connect(this->dismiss_, &Button::leftClicked, this, [this] {
        const auto row = this->queue_->currentRow();
        this->review_.dismiss(this->selectedKey_, this->now());
        this->refreshLists(true);
        if (this->tabs_->currentIndex() == TAB_REVIEW)
            this->advanceQueue(row);
    });
    QObject::connect(this->userCard_, &Button::leftClicked, this, [this] {
        if (const auto *user = this->review_.find(this->selectedKey_);
            user && this->split_)
            this->split_->getChannelView().showUserInfoPopup(
                user->login, user->platform, {}, user->userID);
    });
    QObject::connect(this->notes_, &Button::leftClicked, this,
                     &ModView::editNotes);
    QObject::connect(this->closeUser_, &Button::leftClicked, this, [this] {
        this->selectUser({});
    });
    QObject::connect(this->ban_, &Button::leftClicked, this, [this] {
        this->requestAction(Action::Ban);
    });
    QObject::connect(this->more_, &Button::leftClicked, this,
                     &ModView::showMoreActions);
    QObject::connect(this->cancel_, &Button::leftClicked, this,
                     &ModView::clearConfirmation);
    QObject::connect(this->confirm_, &Button::leftClicked, this, [this] {
        const auto key = this->confirmationKey_;
        this->clearConfirmation();
        if (key != this->selectedKey_)
            return;
        this->executeAction(this->confirmationAction_, key,
                            this->confirmationMessageID_,
                            this->confirmationActor_, this->duration_->value());
    });
    QObject::connect(this->bulkTimeout_, &Button::leftClicked, this, [this] {
        this->bulkAction(Action::Timeout, BULK_TIMEOUT);
    });
    QObject::connect(this->bulkBan_, &Button::leftClicked, this, [this] {
        this->confirmingBulk_ = true;
        this->updateBulkBar();
    });
    QObject::connect(this->bulkConfirm_, &Button::leftClicked, this, [this] {
        if (this->confirmingBulk_)
            this->bulkAction(Action::Ban);
    });
    QObject::connect(this->bulkCancel_, &Button::leftClicked, this, [this] {
        if (this->confirmingBulk_)
        {
            this->confirmingBulk_ = false;
            this->updateBulkBar();
            return;
        }
        auto *list = this->tabs_->currentIndex() == TAB_REVIEW
                         ? this->queue_
                         : this->tracked_;
        list->clearSelection();
        this->syncSelection(list);
    });
    QObject::connect(this->bulkDismiss_, &Button::leftClicked, this, [this] {
        const bool review = this->tabs_->currentIndex() == TAB_REVIEW;
        const auto keys = this->bulkKeys();
        for (const auto &key : keys)
        {
            if (review)
                this->review_.dismiss(key, this->now());
            else
                this->review_.track(key, false);
        }
        if (!review)
            this->savePreferences();
        (review ? this->queue_ : this->tracked_)->clearSelection();
        this->refreshLists(true);
        this->feedback(QString(review ? "Dismissed %1 users."
                                      : "Stopped tracking %1 users.")
                           .arg(keys.size()));
    });
    QObject::connect(this->endLockdown_, &Button::leftClicked, this,
                     &ModView::toggleLockdown);
    getSettings()->timeoutButtons.connect(
        [this](auto, auto) {
            this->rebuildTimeouts();
        },
        this->signalHolder_);
    // Other chats can change a shared profile
    getSettings()->modViewPreferences.connect(
        [this](const auto &, const auto &) {
            this->rulesEditor_->setInlineActions(this->inlineActionsEnabled());
            this->profilesChanged();
        },
        this->signalHolder_, false);
    this->signalHolder_.managedConnect(getApp()->getFonts()->fontChanged,
                                       [this] {
                                           this->updateFonts();
                                       });
}

void ModView::rebuildTimeouts()
{
    for (const auto &preset : this->timeoutPresets_)
        delete preset.button;
    this->timeoutPresets_.clear();
    for (const auto &timeout : getSettings()->timeoutButtons.getValueCopy())
    {
        auto *button = new ModViewButton(
            QString::number(timeout.second) + timeout.first, this, {0, 4});
        button->setFill(this->presetFill());
        const auto seconds = timeoutSeconds(timeout);
        this->timeoutPresets_.push_back({button, seconds});
        qobject_cast<QHBoxLayout *>(this->timeouts_->layout())
            ->addWidget(button, 1);
        QObject::connect(button, &Button::leftClicked, this, [this, seconds] {
            this->requestAction(Action::Timeout, seconds);
        });
    }
    this->updateCapabilities();
}

QColor ModView::presetFill() const
{
    return blended(this->theme->splits.input.background,
                   this->theme->messages.textColors.regular, 0.05);
}

void ModView::updateFonts()
{
    auto *fonts = getApp()->getFonts();
    this->setFont(fonts->getFont(FontStyle::UiMedium, this->scale()));
    this->history_->setFont(
        fonts->getFont(FontStyle::ChatMedium, this->scale()));
}

ModView::~ModView()
{
    this->connections_.clear();
    std::lock_guard lock(this->inbox_->mutex);
    this->inbox_->active = false;
    this->inbox_->pending.clear();
}

qint64 ModView::now() const
{
    return this->clock_.elapsed();
}
bool ModView::inlineActionsEnabled() const
{
    return inlineActionsPreference(this->channel_).value_or(true);
}

std::optional<bool> ModView::inlineActionsPreference(const ChannelPtr &channel)
{
    if (!hasChat(channel))
    {
        return std::nullopt;
    }
    const auto chat = ModerationProfiles(preferences()).chat(scopeFor(channel));
    const auto enabled = chat.value("inlineActions");
    if (enabled.isBool())
    {
        return enabled.toBool();
    }
    const auto legacy = chat.value("rules").toObject().value("inlineActions");
    return legacy.isBool() ? std::optional<bool>(legacy.toBool()) : std::nullopt;
}

void ModView::setInlineActionsPreference(const ChannelPtr &channel,
                                          bool enabled)
{
    if (!hasChat(channel))
    {
        return;
    }
    const auto scope = scopeFor(channel);
    ModerationProfiles store(preferences());
    auto chat = store.chat(scope);
    if (chat.value("inlineActions") == QJsonValue(enabled))
    {
        return;
    }
    chat.insert("inlineActions", enabled);
    store.setChat(scope, chat);
    getSettings()->modViewPreferences = QString::fromUtf8(
        QJsonDocument(store.toJson()).toJson(QJsonDocument::Compact));
    getSettings()->requestSave();
}
int ModView::preferredWidth() const
{
    return this->preferredWidth_;
}
void ModView::setPreferredWidth(int width)
{
    this->preferredWidth_ = std::clamp(width, 260, 800);
    this->savePreferences();
}

bool ModView::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::Wheel)
    {
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched);
            spin && !spin->hasFocus())
        {
            event->ignore();
            return true;
        }
    }
    if (event->type() == QEvent::KeyPress &&
        (watched == this->queue_ || watched == this->tracked_) &&
        this->handleListKey(static_cast<QListWidget *>(watched),
                            static_cast<QKeyEvent *>(event)))
        return true;
    return BaseWidget::eventFilter(watched, event);
}

bool ModView::handleListKey(QListWidget *list, QKeyEvent *event)
{
    if ((event->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
        return false;
    const bool bulk = this->bulkKeys().size() > 1;
    const bool confirming = !this->confirmationKey_.isEmpty();
    const auto click = [](Button *button) {
        if (!button->isHidden() && button->isEnabled())
            Q_EMIT button->leftClicked();
    };
    switch (event->key())
    {
        case Qt::Key_J:
        case Qt::Key_K:
            if (list->count() > 0)
                list->setCurrentRow(std::clamp(
                    list->currentRow() + (event->key() == Qt::Key_J ? 1 : -1),
                    0, list->count() - 1));
            return true;
        case Qt::Key_D:
            click(bulk ? this->bulkDismiss_ : this->dismiss_);
            return true;
        case Qt::Key_F:
            if (!bulk)
                click(this->mark_);
            return true;
        case Qt::Key_T:
            if (bulk)
                click(this->bulkTimeout_);
            else if (!confirming && !this->timeouts_->isHidden())
                for (const auto &preset : this->timeoutPresets_)
                    if (!preset.button->isHidden())
                    {
                        this->requestAction(Action::Timeout, preset.seconds);
                        break;
                    }
            return true;
        case Qt::Key_B:
            click(bulk ? this->bulkBan_ : this->ban_);
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (this->confirmingBulk_)
                click(this->bulkConfirm_);
            else if (confirming)
                click(this->confirm_);
            else
                return false;
            return true;
        case Qt::Key_Escape:
            if (this->confirmingBulk_ || bulk)
                click(this->bulkCancel_);
            else if (confirming)
                this->clearConfirmation();
            else
                return false;
            return true;
        default:
            return false;
    }
}

void ModView::showEvent(QShowEvent *event)
{
    BaseWidget::showEvent(event);
    this->refreshLists(true);
}

void ModView::paintEvent(QPaintEvent *event)
{
    BaseWidget::paintEvent(event);
    if (this->split_ && !this->split_->isModViewPane() &&
        this->parentWidget() == this->split_.data())
    {
        QPainter painter(this);
        painter.fillRect(0, 0, 1, this->height(),
                         this->theme->splits.header.border);
    }
}

void ModView::resizeEvent(QResizeEvent *event)
{
    BaseWidget::resizeEvent(event);
    const auto orientation =
        this->width() >= int(WIDE_LAYOUT_WIDTH * this->scale()) ? Qt::Horizontal
                                                                : Qt::Vertical;
    if (this->content_->orientation() != orientation)
    {
        this->content_->setOrientation(orientation);
        this->detailSizes_.clear();
        this->content_->setSizes({2000, 3000});
    }

    const bool compact =
        this->width() < int(COMPACT_IDENTITY_WIDTH * this->scale());
    if (compact != this->compactIdentity_)
    {
        this->compactIdentity_ = compact;
        if (compact)
            this->wrappedActions_->addWidget(this->identityActions_);
        else
            this->identityRow_->insertWidget(
                this->identityRow_->indexOf(this->closeUser_),
                this->identityActions_, 0, Qt::AlignTop);
    }
}

void ModView::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    const auto headerSize = int(HEADER_HEIGHT * scale);
    const auto smallSize = int(SMALL_BUTTON_SIZE * scale);
    this->header_->setFixedHeight(headerSize);
    this->tools_->setFixedSize(headerSize, headerSize);
    this->close_->setFixedSize(headerSize, headerSize);
    this->more_->setFixedSize(smallSize, smallSize);
    this->closeUser_->setFixedSize(smallSize, smallSize);
    this->duration_->setFixedWidth(int(70 * scale));
    this->rulesEditor_->applyScale(scale);
    this->updateFonts();
    this->themeChangedEvent();
}

void ModView::themeChangedEvent()
{
    const auto &splits = this->theme->splits;
    const auto scale = this->scale();
    const auto bg = splits.background;
    const auto border = splits.header.border;
    this->style_ = {
        .text = this->theme->messages.textColors.regular,
        .muted = this->theme->messages.textColors.system,
        .hover = blended(bg, this->theme->messages.textColors.regular, 0.05),
        .selected = blended(bg, this->theme->messages.textColors.regular, 0.11),
        .accent = this->theme->tabs.selected.line.regular,
        .attention = this->theme->tabs.highlighted.line.regular,
        .scale = scale,
    };

    // A stylesheet restores the palette it first saw when it repolishes, so it
    // has to be cleared for a new palette to survive a theme change.
    this->setStyleSheet({});
    auto palette = this->theme->palette;
    palette.setColor(QPalette::Window, bg);
    palette.setColor(QPalette::Base, bg);
    palette.setColor(QPalette::Text, this->style_.text);
    palette.setColor(QPalette::WindowText, this->style_.text);
    palette.setColor(QPalette::PlaceholderText, this->style_.muted);
    palette.setColor(QPalette::Disabled, QPalette::PlaceholderText,
                     this->style_.muted);
    palette.setColor(QPalette::Highlight, this->style_.selected);
    palette.setColor(QPalette::HighlightedText, this->style_.text);
    palette.setColor(QPalette::Disabled, QPalette::Text, this->style_.muted);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     this->style_.muted);
    this->setPalette(palette);

    const auto primaryFill =
        blended(splits.input.background, this->style_.text, 0.12);
    this->dismiss_->setFill(primaryFill);
    this->mark_->setFill(primaryFill);
    for (const auto &preset : this->timeoutPresets_)
        preset.button->setFill(this->presetFill());
    auto outline = this->style_.attention;
    outline.setAlpha(150);
    this->ban_->setBorderColor(outline);
    this->confirm_->setBorderColor(outline);
    auto confirmFill = this->style_.attention;
    confirmFill.setAlpha(45);
    this->confirm_->setFill(confirmFill);
    this->bulkBan_->setBorderColor(outline);
    this->bulkConfirm_->setBorderColor(outline);
    this->bulkConfirm_->setFill(confirmFill);
    this->bulkDismiss_->setFill(primaryFill);
    this->bulkTimeout_->setFill(this->presetFill());
    this->rulesEditor_->applyColors(this->presetFill(), primaryFill);
    auto lockdownFill = this->style_.attention;
    lockdownFill.setAlpha(34);

    QString sheet = R"(
QWidget#ModView QLabel, QWidget#ModView QCheckBox {
    color: @text; background: transparent; }
QWidget#ModView QCheckBox:disabled, QWidget#ModView QLabel:disabled {
    color: @muted; }
QWidget#ModView QLabel[muted="true"] { color: @muted; }
QWidget#ModView QLabel[strong="true"] { font-weight: 600; }
QWidget#ModView QLabel[section="true"] { font-weight: 600; }
QWidget#ModView QLabel#ModViewReasons {
    border-left: @barpx solid @attention; padding-left: @gappx; }
QWidget#ModView QLabel#ModViewStatus {
    color: @muted; background: @field; border-top: 1px solid @border;
    padding: 4px 10px; }
QWidget#ModViewBan QLabel, QWidget#ModViewConfirmAction QLabel,
QWidget#ModViewBulkBan QLabel, QWidget#ModViewBulkConfirm QLabel {
    color: @attention; }
QLabel#ModViewBulkLabel, QLabel#ModViewLockdownLabel { font-weight: 600; }

QFrame#ModViewHeader {
    background: transparent; border: 0; border-bottom: 1px solid @border; }
QFrame#ModViewActions, QFrame#ModViewBulk {
    background: @field; border: 0; border-top: 1px solid @border; }
QFrame#ModViewLockdownBar {
    background: @lockdown; border: 0; border-bottom: 1px solid @border; }

QWidget#ModView QTabBar { background: transparent; }
QWidget#ModView QTabBar::tab {
    color: @muted; background: transparent; border: 0;
    border-bottom: @barpx solid transparent;
    height: @tabheightpx; padding: 0 @tabpaddingpx; }
QWidget#ModView QTabBar::tab:hover { color: @text; }
QWidget#ModView QTabBar::tab:selected {
    color: @text; border-bottom-color: @accent; }

QWidget#ModView QLineEdit, QWidget#ModView QPlainTextEdit,
QWidget#ModView QSpinBox {
    color: @text; background: @field; border: 1px solid @border;
    border-radius: 3px; padding: @gappx; selection-background-color: @selected;
    selection-color: @text; }
QWidget#ModView QLineEdit { padding: @barpx @gappx; }
QWidget#ModView QSpinBox { padding: @barpx 0; }
QWidget#ModView QLineEdit:focus, QWidget#ModView QPlainTextEdit:focus,
QWidget#ModView QSpinBox:focus { border-color: @accent; }
QWidget#ModView QSpinBox:disabled { color: @muted; background: transparent; }

QWidget#ModView QComboBox {
    color: @text; background: @field; border: 1px solid @border;
    border-radius: 3px; padding: @barpx @arrowpx @barpx @gappx;
    selection-background-color: @selected; selection-color: @text; }
QWidget#ModView QComboBox:focus { border-color: @accent; }
QWidget#ModView QComboBox:disabled { color: @muted; background: transparent; }
QWidget#ModView QComboBox::drop-down {
    subcontrol-origin: border; subcontrol-position: top right;
    width: @arrowpx; border: none; background: transparent; }
QWidget#ModView QComboBox::down-arrow { image: none; width: 0px; height: 0px; }
QWidget#ModView QComboBox QAbstractItemView {
    color: @text; background: @field; border: 1px solid @border;
    selection-background-color: @selected; selection-color: @text;
    outline: none; }

QWidget#ModView QListWidget { background: transparent; border: 0; outline: none; }
QWidget#ModView QSplitter::handle { background: @border; }
QWidget#ModView QScrollArea { background: transparent; border: 0; }
QWidget#ModViewRulesPage, QWidget#ModViewRulesViewport {
    background: transparent; }

QWidget#ModView QScrollBar:vertical {
    background: transparent; width: @scrollbarpx; margin: 0; }
QWidget#ModView QScrollBar::handle:vertical {
    background: @thumb; min-height: 24px; }
QWidget#ModView QScrollBar::handle:vertical:hover { background: @grip; }
QWidget#ModView QScrollBar::add-line:vertical,
QWidget#ModView QScrollBar::sub-line:vertical { height: 0; }
QWidget#ModView QScrollBar::add-page:vertical,
QWidget#ModView QScrollBar::sub-page:vertical { background: transparent; }
)";
    const std::pair<QString, QString> tokens[] = {
        {"@text", this->style_.text.name(QColor::HexArgb)},
        {"@muted", this->style_.muted.name(QColor::HexArgb)},
        {"@selected", this->style_.selected.name(QColor::HexArgb)},
        {"@accent", this->style_.accent.name(QColor::HexArgb)},
        {"@attention", this->style_.attention.name(QColor::HexArgb)},
        {"@border", border.name(QColor::HexArgb)},
        {"@field", splits.input.background.name(QColor::HexArgb)},
        {"@thumb", this->theme->scrollbars.thumb.name(QColor::HexArgb)},
        {"@grip", this->theme->scrollbars.thumbSelected.name(QColor::HexArgb)},
        {"@tabheight", QString::number(int(HEADER_HEIGHT * scale) - 3)},
        {"@tabpadding", QString::number(int(10 * scale))},
        {"@scrollbar", QString::number(int(8 * scale))},
        {"@bar", QString::number(int(2 * scale))},
        {"@gap", QString::number(int(5 * scale))},
        {"@arrow", QString::number(int(16 * scale))},
        {"@lockdown", lockdownFill.name(QColor::HexArgb)},
    };
    for (const auto &[token, value] : tokens)
        sheet.replace(token, value);
    this->setStyleSheet(sheet);

    this->queue_->viewport()->update();
    this->tracked_->viewport()->update();
    this->history_->viewport()->update();
    this->logList_->viewport()->update();
    this->refreshDetails();
}

void ModView::feedback(const QString &text)
{
    this->feedback_->setText(text);
    this->feedback_->setVisible(!text.isEmpty());
    if (text.isEmpty())
        this->feedbackTimer_.stop();
    else
        this->feedbackTimer_.start();
}

void ModView::setChannel(ChannelPtr channel)
{
    if (!channel)
        channel = Channel::getEmpty();
    const auto scope = scopeFor(channel);
    if (this->channel_ == channel && this->scope_ == scope)
        return;
    if (this->rulesTimer_.isActive())
        this->saveRulesEditor();
    // Lockdown and queued actions belong to the chat they started in
    if (this->lockdown_.active)
        this->toggleLockdown();
    this->actionQueue_.clear();
    this->actionTimer_.stop();
    this->log_.clear();
    this->ageKeys_.clear();
    this->ageTimer_.stop();
    this->unsupportedNoted_.clear();
    this->connections_.clear();
    {
        std::lock_guard lock(this->inbox_->mutex);
        this->inbox_->active = false;
        this->inbox_->pending.clear();
    }
    this->inbox_ = std::make_shared<Inbox>();
    this->channel_ = std::move(channel);
    this->scope_ = scope;
    this->merged_ = dynamic_cast<MergedChannel *>(this->channel_.get());
    this->selectedKey_.clear();
    this->confirmationKey_.clear();
    this->confirmingBulk_ = false;
    this->review_ = ModerationReview();
    this->loadPreferences();
    this->feedback({});
    this->renderedRevision_ = 0;
    this->connectChannel();
    this->refreshLists(true);
    this->refreshLog();
    this->updateLockdownBar();
}

void ModView::setMonitoring(bool enabled)
{
    if (this->monitoring_ == enabled)
        return;
    if (this->rulesTimer_.isActive())
        this->saveRulesEditor();
    this->monitoring_ = enabled;
    this->connections_.clear();
    {
        std::lock_guard lock(this->inbox_->mutex);
        this->inbox_->active = false;
        this->inbox_->pending.clear();
    }
    if (enabled)
    {
        this->connectChannel();
        this->refreshTimer_.start();
        this->refreshLists(true);
    }
    else
    {
        this->refreshTimer_.stop();
        this->ageTimer_.stop();
        // Rules only act while Mod View is open
        for (const auto &request : this->actionQueue_)
            this->updateLog(request.logID, "Cancelled: Mod View was closed",
                            true, true);
        this->actionQueue_.clear();
        this->actionTimer_.stop();
        this->clearConfirmation();
    }
    this->rulesEditor_->setLockdown(
        this->lockdownAvailable() || this->profileRules_.lockdown.raidRules,
        this->lockdown_.active);
}

void ModView::connectChannel()
{
    this->connections_.clear();
    if (!this->monitoring_ || !this->channel_ || this->channel_->isEmpty())
        return;
    const auto inbox = this->inbox_;
    {
        std::lock_guard lock(inbox->mutex);
        inbox->active = true;
    }
    this->connections_.managedConnect(
        this->channel_->messageAppended,
        [inbox](MessagePtr &message, std::optional<MessageFlags>) {
            inbox->push(message);
        });
    this->connections_.managedConnect(
        this->channel_->messageReplaced,
        [inbox](size_t, const MessagePtr &, const MessagePtr &message) {
            inbox->push(message, true);
        });
    auto *source = this->channel_.get();
    this->connections_.managedConnect(
        Channel::messageRemoved,
        [inbox, source](Channel *channel, size_t, const MessagePtr &message) {
            if (channel == source)
                inbox->push(message, true, true);
        });
    this->connections_.managedConnect(
        Channel::messageFlagsChanged,
        [inbox, source](Channel *channel, const MessagePtr &message) {
            if (channel == source)
                inbox->push(message, true);
        });
    // Retained history provides context but must not be interpreted as a fresh
    // burst when opening the pane, changing channels, or restoring a layout.
    for (const auto &message : this->channel_->getMessageSnapshot(500))
    {
        auto input = ReviewInput::fromMessage(*message);
        if (!this->mutedPlatforms_.contains(input.platform))
            this->review_.ingest(std::move(input), this->now(), false);
    }
}

void ModView::flushMessages()
{
    std::deque<Inbox::Event> events;
    quint64 skipped = 0;
    {
        std::lock_guard lock(this->inbox_->mutex);
        events.swap(this->inbox_->pending);
        skipped = this->inbox_->skipped;
    }
    for (const auto &event : events)
    {
        auto input = ReviewInput::fromMessage(*event.message);
        if (event.update)
        {
            this->review_.updateMessage(input, event.removed);
            continue;
        }
        // Platforms switched off for this chat are ignored entirely
        if (this->mutedPlatforms_.contains(input.platform))
            continue;
        const auto actor = this->actorID(input.platform);
        input.own = !actor.isEmpty() && actor == input.userID;
        const bool fresh =
            !input.timestamp.isValid() ||
            input.timestamp.msecsTo(QDateTime::currentDateTimeUtc()) < 5000;
        this->review_.ingest(std::move(input), this->now(), fresh);
    }
    this->processTriggers();
    this->ageKeys_.append(this->review_.takeAgeLookups());
    while (this->ageKeys_.size() > MAX_AGE_LOOKUPS)
        this->ageKeys_.removeFirst();
    if (!this->ageKeys_.isEmpty() && !this->ageLookupBusy_ &&
        !this->ageTimer_.isActive())
        this->ageTimer_.start();
    if (this->isVisible())
    {
        this->refreshLists();
        if (this->now() - this->lastCapabilityRefresh_ >= 1000)
        {
            this->lastCapabilityRefresh_ = this->now();
            this->updateCapabilities();
            this->queue_->viewport()->update();
            this->tracked_->viewport()->update();
        }
        if (skipped)
            this->feedback("Very high chat traffic: some messages were skipped "
                           "by the review reader.");
    }
}

void ModView::inspectMessage(const MessagePtr &message)
{
    if (!message)
        return;
    const auto key = this->review_.ingest(ReviewInput::fromMessage(*message),
                                          this->now(), false);
    if (key.isEmpty())
        return;
    this->tabs_->setCurrentIndex(TAB_TRACKING);
    this->showRecent_->setChecked(true);
    this->userSearch_->clear();
    this->refreshLists(true);
    this->selectUser(key);
}

void ModView::syncList(QListWidget *list, const QStringList &keys)
{
    QSignalBlocker blocker(list);
    // Rows that move lose their selection, so a multi-selection is restored
    QSet<QString> chosen;
    for (const auto *item : list->selectedItems())
        chosen.insert(item->data(Qt::UserRole).toString());
    const QSet<QString> desired(keys.begin(), keys.end());
    for (int i = list->count() - 1; i >= 0; --i)
        if (!desired.contains(list->item(i)->data(Qt::UserRole).toString()))
            delete list->takeItem(i);
    QHash<QString, QListWidgetItem *> existing;
    for (int i = 0; i < list->count(); ++i)
        existing.insert(list->item(i)->data(Qt::UserRole).toString(),
                        list->item(i));
    for (int i = 0; i < keys.size(); ++i)
    {
        const auto &key = keys[i];
        auto *item = existing.value(key);
        if (!item)
        {
            item = new QListWidgetItem;
            item->setData(Qt::UserRole, key);
            list->insertItem(i, item);
        }
        else if (list->row(item) != i)
            list->insertItem(i, list->takeItem(list->row(item)));
        const auto *user = this->review_.find(key);
        if (!user)
            continue;
        if (item->data(Qt::UserRole + 1).toULongLong() != user->revision)
        {
            this->describeRow(item, *user, list == this->queue_);
            item->setData(Qt::UserRole + 1, qulonglong(user->revision));
        }
    }
    if (chosen.size() > 1)
        for (int i = 0; i < list->count(); ++i)
        {
            auto *item = list->item(i);
            const bool on =
                chosen.contains(item->data(Qt::UserRole).toString());
            if (item->isSelected() != on)
                item->setSelected(on);
        }
    this->syncSelection(list);
}

void ModView::syncSelection(QListWidget *list)
{
    QSignalBlocker blocker(list);
    // Leave a multi-selection for bulk actions alone
    if (list->selectedItems().size() > 1)
        return;
    QListWidgetItem *selected = nullptr;
    for (int i = 0; i < list->count() && !selected; ++i)
        if (list->item(i)->data(Qt::UserRole).toString() == this->selectedKey_)
            selected = list->item(i);
    list->setCurrentItem(selected);
    if (!selected)
        list->clearSelection();
}

void ModView::describeRow(QListWidgetItem *item, const ReviewUser &user,
                          bool queue) const
{
    QStringList detail;
    if (user.pending && !user.reasons.isEmpty())
        detail.append(user.reasons.size() > 1
                          ? QString("%1 +%2")
                                .arg(user.reasons.first())
                                .arg(user.reasons.size() - 1)
                          : user.reasons.first());
    if (user.tracked)
        detail.append("Tracked");
    if (!queue && user.history.empty())
        detail.append("Not seen this session");
    else if (!queue && detail.isEmpty())
        detail.append("Recent");

    item->setText(user.displayName);
    item->setData(DetailRole, detail.join(" · "));
    item->setData(ColorRole, user.color);
    item->setData(IconRole,
                  this->merged_ ? platformIconPath(user.platform) : QString());
    item->setData(StampRole, queue                  ? user.markedAt
                             : user.history.empty() ? qint64(-1)
                                                    : user.lastSeen);
    item->setData(Qt::AccessibleTextRole,
                  user.displayName + ", " + detail.join(", "));
}

void ModView::refreshLists(bool force)
{
    if (!force && this->review_.revision() == this->renderedRevision_)
        return;
    this->renderedRevision_ = this->review_.revision();
    QStringList pending;
    QStringList tracked;
    const auto queueQuery = this->reviewSearch_->text().trimmed();
    const auto userQuery = this->userSearch_->text().trimmed();
    const bool messageSearch =
        this->searchMode_->currentIndex() == 1 && !userQuery.isEmpty();
    QHash<QString, QString> said;
    const auto matches = [](const ReviewUser &user, const QString &query) {
        return query.isEmpty() ||
               user.login.contains(query, Qt::CaseInsensitive) ||
               user.displayName.contains(query, Qt::CaseInsensitive) ||
               user.userID.contains(query, Qt::CaseInsensitive) ||
               ModerationReview::platformName(user.platform)
                   .contains(query, Qt::CaseInsensitive);
    };
    const auto saidMatch = [&](const ReviewUser &user) {
        for (auto it = user.history.rbegin(); it != user.history.rend(); ++it)
            if (it->text.contains(userQuery, Qt::CaseInsensitive))
            {
                said.insert(user.key, it->text);
                return true;
            }
        return false;
    };
    for (const auto &key : this->review_.keys())
    {
        const auto *user = this->review_.find(key);
        if (user->pending && matches(*user, queueQuery))
            pending.push_back(key);
        if (messageSearch ? saidMatch(*user)
                          : (user->tracked || this->showRecent_->isChecked() ||
                             !userQuery.isEmpty()) &&
                                matches(*user, userQuery))
            tracked.push_back(key);
    }
    // The most serious flags come first, then the newest
    std::sort(pending.begin(), pending.end(),
              [this](const QString &a, const QString &b) {
                  const auto *left = this->review_.find(a);
                  const auto *right = this->review_.find(b);
                  if (left->severity != right->severity)
                      return left->severity > right->severity;
                  return left->markedAt != right->markedAt
                             ? left->markedAt > right->markedAt
                             : a < b;
              });
    std::sort(tracked.begin(), tracked.end(),
              [this](const QString &a, const QString &b) {
                  const auto *left = this->review_.find(a);
                  const auto *right = this->review_.find(b);
                  if (left->tracked != right->tracked)
                      return left->tracked;
                  const auto order = left->displayName.compare(
                      right->displayName, Qt::CaseInsensitive);
                  return order != 0 ? order < 0 : a < b;
              });
    this->syncList(this->queue_, pending);
    this->syncList(this->tracked_, tracked);
    if (messageSearch)
        for (int i = 0; i < this->tracked_->count(); ++i)
        {
            auto *item = this->tracked_->item(i);
            const auto text =
                said.value(item->data(Qt::UserRole).toString()).simplified();
            const auto at = text.indexOf(userQuery, 0, Qt::CaseInsensitive);
            item->setData(DetailRole,
                          QString("said: %1")
                              .arg(at > 24 ? QString("…%1").arg(text.mid(at - 16))
                                           : text));
            item->setData(Qt::UserRole + 1, 0);
        }
    this->reviewTools_->setVisible(!pending.isEmpty() || !queueQuery.isEmpty());
    this->queue_->setVisible(!pending.isEmpty());
    this->emptyQueue_->setVisible(pending.isEmpty());
    this->tracked_->setVisible(!tracked.isEmpty());
    this->emptyTracked_->setVisible(tracked.isEmpty());
    this->refreshEmptyStates(!queueQuery.isEmpty(), !userQuery.isEmpty());
    const auto tabText = [](const QString &name, int count) {
        return count > 0 ? QString("%1  %2").arg(name).arg(count) : name;
    };
    const auto setTabText = [this](int index, const QString &text) {
        if (this->tabs_->tabText(index) != text)
            this->tabs_->setTabText(index, text);
    };
    setTabText(TAB_REVIEW, tabText("Review", this->review_.pendingCount()));
    setTabText(TAB_TRACKING, tabText("Tracking", this->review_.trackedCount()));
    if (!this->selectedKey_.isEmpty() &&
        !this->review_.find(this->selectedKey_))
        this->selectUser({});
    else if (force || (this->review_.find(this->selectedKey_) &&
                       this->review_.find(this->selectedKey_)->revision !=
                           this->selectionRevision_))
        this->refreshDetails();
    this->updateBulkBar();
}

void ModView::showRules()
{
    this->tabs_->setCurrentIndex(TAB_RULES);
}

void ModView::refreshEmptyStates(bool queueFiltered, bool trackingFiltered)
{
    this->emptyQueueTitle_->setText(queueFiltered ? "No matches"
                                                  : "Nothing to review");
    this->emptyQueueHint_->setText(
        queueFiltered ? "No flagged users match this filter."
                      : watchingSummary(this->review_.rules()));
    this->emptyTrackedTitle_->setText(trackingFiltered ? "No matches"
                                                       : "No tracked users");
    this->emptyTrackedHint_->setText(
        trackingFiltered && this->searchMode_->currentIndex() == 1
            ? "Nobody seen this session said that."
        : trackingFiltered
            ? "Only people seen in this chat can be found."
            : "Track someone from their review, or search people seen in "
              "this chat.");
}

void ModView::selectUser(const QString &key)
{
    if (this->selectedKey_ != key)
    {
        this->selectedKey_ = key;
        this->confirmationKey_.clear();
        this->history_->clear();
    }
    this->syncSelection(this->queue_);
    this->syncSelection(this->tracked_);
    this->refreshDetails();
}

void ModView::advanceQueue(int row)
{
    const auto count = this->queue_->count();
    this->selectUser(count > 0
                         ? this->queue_->item(std::clamp(row, 0, count - 1))
                               ->data(Qt::UserRole)
                               .toString()
                         : QString());
}

void ModView::refreshDetails()
{
    const auto *user = this->review_.find(this->selectedKey_);
    if (!user)
    {
        this->userLabel_->setText("Select a user to review");
        this->identityLabel_->clear();
        this->reasons_->clear();
        this->history_->clear();
        this->selectionRevision_ = 0;
    }
    else
    {
        const bool changedUser = this->history_->count() == 0;
        this->selectionRevision_ = user->revision;
        this->userLabel_->setText(user->displayName);
        auto color = user->color;
        if (color.isValid())
            this->theme->normalizeColor(color);
        auto palette = this->userLabel_->palette();
        palette.setColor(QPalette::WindowText,
                         color.isValid() ? color : this->style_.text);
        this->userLabel_->setPalette(palette);
        this->platformIcon_->setVisible(this->merged_);
        if (this->merged_)
        {
            const auto size = int(14 * this->scale());
            this->platformIcon_->setContentsMargins(0, 1,
                                                    int(6 * this->scale()), 0);
            this->platformIcon_->setPixmap(
                platformPixmap(platformIconPath(user->platform), size,
                               this->devicePixelRatioF()));
        }
        QStringList identity{
            ModerationReview::platformName(user->platform),
            user->userID.isEmpty() ? "ID unavailable" : "ID " + user->userID};
        if (user->trusted)
            identity.append("Mod or VIP");
        this->identityLabel_->setText(identity.join(" · "));
        this->identityLabel_->setToolTip(identity.join(" · "));
        this->reasons_->setText(user->reasons.values().join('\n'));
        this->track_->setText(user->tracked ? "Untrack" : "Track");

        const auto selected =
            this->history_->currentItem()
                ? this->history_->currentItem()->data(Qt::UserRole).toString()
                : QString();
        auto *scrollbar = this->history_->verticalScrollBar();
        const bool follow =
            changedUser || scrollbar->value() >= scrollbar->maximum() - 2;
        const auto scroll = scrollbar->value();
        this->history_->clear();
        for (const auto &message : user->history)
        {
            auto *item =
                new QListWidgetItem(message.text.left(320), this->history_);
            item->setData(TimeRole, chatTimestamp(message.timestamp));
            item->setData(DeletedRole, message.deleted);
            item->setToolTip(message.text.toHtmlEscaped());
            item->setData(Qt::UserRole,
                          message.deleted ? QString() : message.messageID);
            if (!selected.isEmpty() && message.messageID == selected)
                this->history_->setCurrentItem(item);
        }
        this->history_->doItemsLayout();
        scrollbar->setValue(scroll);
        if (follow)
            // The list is only laid out once the details panel has a size.
            QTimer::singleShot(0, this->history_, [history = this->history_] {
                history->scrollToBottom();
            });
    }
    this->reasons_->setVisible(user && !user->reasons.isEmpty());
    this->history_->setVisible(!user || !user->history.empty());
    this->emptyHistory_->setVisible(user && user->history.empty());
    this->dismiss_->setVisible(user && user->pending);
    this->mark_->setVisible(user && !user->pending);
    this->userCard_->setVisible(this->split_ != nullptr);
    this->userCard_->setEnabled(user && this->split_ && !user->login.isEmpty());
    this->updateDetailsVisibility();
    this->updateCapabilities();
}

void ModView::updateDetailsVisibility()
{
    const bool shown = this->review_.find(this->selectedKey_) != nullptr &&
                       this->tabs_->currentIndex() < TAB_RULES;
    if (shown == !this->details_->isHidden())
        return;
    if (!shown)
    {
        this->detailSizes_ = this->content_->sizes();
        this->details_->hide();
        return;
    }
    this->details_->show();
    const bool restorable = this->detailSizes_.size() == 2 &&
                            this->detailSizes_[0] > 0 &&
                            this->detailSizes_[1] > 0;
    this->content_->setSizes(restorable ? this->detailSizes_
                                        : QList<int>{2000, 3000});
}

void ModView::clearConfirmation()
{
    this->confirmationKey_.clear();
    this->updateCapabilities();
}

void ModView::loadPreferences()
{
    ModerationProfiles store(preferences());
    const auto before = store.toJson();
    this->profileID_ = store.profileFor(
        hasChat(this->channel_) ? this->scope_ : QString(),
        this->channel_ ? this->channel_->getName() : QString());
    const auto *profile = store.find(this->profileID_);
    this->profileName_ = profile ? profile->name : QString("Default");
    this->profileRules_ = profile ? profile->rules : ReviewRules();
    const auto chat = store.chat(this->scope_);
    this->mutedPlatforms_.clear();
    for (const auto &value : chat.value("mutedPlatforms").toArray())
        if (const auto platform = moderation::platformFromName(value.toString()))
            this->mutedPlatforms_.insert(*platform);
    this->review_.restoreTracked(chat.value("tracked").toArray());
    this->preferredWidth_ =
        std::clamp(chat.value("width").toInt(370), 260, 800);
    this->applyRules();
    this->loadRulesEditor();
    // Settings from older versions are moved into profiles once
    if (hasChat(this->channel_) && store.toJson() != before)
        this->writeProfiles(store);
}

void ModView::savePreferences()
{
    if (this->scope_.isEmpty() || !hasChat(this->channel_))
        return;
    ModerationProfiles store(preferences());
    auto chat = store.chat(this->scope_);
    QJsonArray muted;
    for (const auto platform : this->mutedPlatforms_)
        muted.append(moderation::platformKey(platform));
    chat.insert("profile", this->profileID_);
    chat.insert("tracked", this->review_.trackedJson());
    chat.insert("width", this->preferredWidth_);
    chat.insert("mutedPlatforms", muted);
    store.setChat(this->scope_, chat);
    this->writeProfiles(store);
}

void ModView::saveProfile()
{
    ModerationProfiles store(preferences());
    if (!store.find(this->profileID_))
    {
        // Removed in another chat while this one was editing it
        this->profileID_ = ModerationProfiles::DEFAULT_ID;
    }
    store.setRules(this->profileID_, this->profileRules_);
    this->writeProfiles(store);
}

void ModView::writeProfiles(const ModerationProfiles &profiles)
{
    this->writingPreferences_ = true;
    getSettings()->modViewPreferences = QString::fromUtf8(
        QJsonDocument(profiles.toJson()).toJson(QJsonDocument::Compact));
    this->writingPreferences_ = false;
    getSettings()->requestSave();
    this->updateProfileList(profiles);
}

void ModView::profilesChanged()
{
    // Edits waiting to be saved here win over the other chat's
    if (this->writingPreferences_ || this->rulesTimer_.isActive())
        return;
    ModerationProfiles store(preferences());
    auto id = hasChat(this->channel_)
                  ? store.chat(this->scope_).value("profile").toString()
                  : this->profileID_;
    if (!store.find(id))
        id = store.find(this->profileID_) ? this->profileID_
                                          : QString(ModerationProfiles::DEFAULT_ID);
    const auto *profile = store.find(id);
    if (!profile)
        return;
    const bool rulesChanged = id != this->profileID_ ||
                              profile->rules.toJson() != this->profileRules_.toJson();
    this->profileID_ = id;
    this->profileName_ = profile->name;
    if (rulesChanged)
    {
        this->profileRules_ = profile->rules;
        this->applyRules();
        this->loadRulesEditor();
        this->refreshLists(true);
    }
    else
        this->updateProfileList(store);
}

void ModView::updateProfileList(const ModerationProfiles &profiles)
{
    std::vector<ModViewRulesEditor::ProfileItem> items;
    for (const auto &profile : profiles.profiles())
        items.push_back({profile.id, profile.name});
    this->rulesEditor_->setProfiles(items, this->profileID_,
                                    profiles.usage(this->profileID_));
}

void ModView::selectProfile(const QString &id)
{
    if (id == this->profileID_)
        return;
    if (this->rulesTimer_.isActive())
        this->saveRulesEditor();
    ModerationProfiles store(preferences());
    const auto *profile = store.find(id);
    if (!profile)
        return;
    this->profileID_ = id;
    this->profileName_ = profile->name;
    this->profileRules_ = profile->rules;
    if (hasChat(this->channel_))
        store.setProfileFor(this->scope_, id);
    this->writeProfiles(store);
    this->applyRules();
    this->loadRulesEditor();
    this->refreshLists(true);
    this->feedback(
        QString("This chat now uses the \"%1\" profile.").arg(profile->name));
}

void ModView::runProfileCommand(const QString &command)
{
    if (this->rulesTimer_.isActive())
        this->saveRulesEditor();
    ModerationProfiles store(preferences());
    const auto ask = [this](const QString &title, const QString &value) {
        bool ok = false;
        const auto name =
            QInputDialog::getText(this, title, "Profile name:",
                                  QLineEdit::Normal, value, &ok)
                .simplified();
        return ok ? name.left(60) : QString();
    };
    const auto use = [this](ModerationProfiles &profiles, const QString &id) {
        const auto *profile = profiles.find(id);
        if (!profile)
            return;
        this->profileID_ = id;
        this->profileName_ = profile->name;
        this->profileRules_ = profile->rules;
        if (hasChat(this->channel_))
            profiles.setProfileFor(this->scope_, id);
        this->writeProfiles(profiles);
        this->applyRules();
        this->loadRulesEditor();
        this->refreshLists(true);
    };
    const auto full = [this] {
        this->feedback(QString("You can have up to %1 profiles. Delete one "
                               "first.")
                           .arg(ModerationProfiles::MAX_PROFILES));
    };

    if (command == "new" || command == "duplicate")
    {
        const bool copy = command == "duplicate";
        const auto name =
            ask(copy ? "Duplicate profile" : "New profile",
                store.uniqueName(copy ? this->profileName_ + " copy"
                                      : QString("New profile")));
        if (name.isEmpty())
            return;
        const auto id =
            store.create(name, copy ? this->profileRules_ : ReviewRules());
        if (id.isEmpty())
            return full();
        use(store, id);
        this->feedback(
            QString("Created the \"%1\" profile.").arg(this->profileName_));
    }
    else if (command == "rename")
    {
        const auto name = ask("Rename profile", this->profileName_);
        if (name.isEmpty() || name == this->profileName_)
            return;
        store.rename(this->profileID_, name);
        if (const auto *profile = store.find(this->profileID_))
            this->profileName_ = profile->name;
        this->writeProfiles(store);
    }
    else if (command == "delete")
    {
        if (this->profileID_ == ModerationProfiles::DEFAULT_ID)
            return;
        const auto name = this->profileName_;
        const auto users = store.usage(this->profileID_);
        const auto answer = QMessageBox::question(
            this, "Delete profile",
            users > 1
                ? QString("Delete the \"%1\" profile? The %2 chats using it "
                          "go back to the Default profile.")
                      .arg(name)
                      .arg(users)
                : QString("Delete the \"%1\" profile? This chat goes back to "
                          "the Default profile.")
                      .arg(name),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes)
            return;
        store = ModerationProfiles(preferences());
        store.remove(this->profileID_);
        use(store, ModerationProfiles::DEFAULT_ID);
        this->feedback(QString("Deleted the \"%1\" profile.").arg(name));
    }
    else if (command == "import")
    {
        const auto path = QFileDialog::getOpenFileName(
            this, "Import Mod View profiles", {},
            "Mod View profiles (*.json);;All files (*)");
        if (path.isEmpty())
            return;
        QString error;
        const auto document = readJsonFile(path, error);
        if (!error.isEmpty())
            return this->feedback(error);
        const auto imported = ModerationProfiles::importDocument(
            document, QFileInfo(path).completeBaseName());
        if (imported.empty())
            return this->feedback("No Mod View profiles found in that file.");
        store = ModerationProfiles(preferences());
        QString first;
        int added = 0;
        for (const auto &profile : imported)
        {
            const auto id = store.create(profile.name, profile.rules);
            if (id.isEmpty())
                break;
            if (first.isEmpty())
                first = id;
            ++added;
        }
        if (added == 0)
            return full();
        if (added == 1)
        {
            use(store, first);
            this->feedback(
                QString("Imported the \"%1\" profile. This chat uses it now.")
                    .arg(this->profileName_));
        }
        else
        {
            this->writeProfiles(store);
            this->feedback(
                QString("Imported %1 profiles. Pick one from the profile list.")
                    .arg(added));
        }
    }
    else if (command == "export" || command == "exportAll")
    {
        const bool one = command == "export";
        std::vector<moderation::ModerationProfile> chosen;
        if (one)
            chosen.push_back(
                {this->profileID_, this->profileName_, this->profileRules_});
        else
            chosen = store.profiles();
        const auto path = QFileDialog::getSaveFileName(
            this, one ? "Export profile" : "Export all profiles",
            (one ? safeFileName(this->profileName_)
                 : QString("Mod View profiles")) +
                ".json",
            "Mod View profiles (*.json)");
        if (path.isEmpty())
            return;
        const auto saved = writeFile(
            path, QJsonDocument(ModerationProfiles::exportDocument(chosen))
                      .toJson(QJsonDocument::Indented));
        this->feedback(saved ? QString("Saved %1.").arg(QFileInfo(path).fileName())
                             : QString("Couldn't save %1.")
                                   .arg(QFileInfo(path).fileName()));
    }
}

ReviewRules ModView::effectiveRules() const
{
    auto rules = this->profileRules_;
    if (this->lockdown_.active && rules.lockdown.raidRules)
        rules.applyPreset(moderation::RulePreset::Raid);
    return rules;
}

void ModView::applyRules()
{
    this->review_.setRules(this->effectiveRules());
}

void ModView::loadRulesEditor()
{
    this->loadingRules_ = true;
    this->rulesEditor_->setRules(this->profileRules_);
    this->rulesEditor_->setInlineActions(this->inlineActionsEnabled());
    const auto available = this->availablePlatforms();
    QSet<MessagePlatform> active;
    for (const auto platform : available)
        if (!this->mutedPlatforms_.contains(platform))
            active.insert(platform);
    this->rulesEditor_->setPlatforms(available, active);
    this->rulesEditor_->setLockdown(
        this->lockdownAvailable() || this->profileRules_.lockdown.raidRules,
        this->lockdown_.active);
    this->updateProfileList(ModerationProfiles(preferences()));
    this->loadingRules_ = false;
}

void ModView::saveRulesEditor()
{
    this->rulesTimer_.stop();
    this->profileRules_ = this->rulesEditor_->rules();
    this->profileRules_.normalize();
    this->applyRules();
    if (hasChat(this->channel_))
    {
        this->saveProfile();
        this->feedback(QString("Saved to the \"%1\" profile.")
                           .arg(this->profileName_));
    }
    this->rulesEditor_->setLockdown(
        this->lockdownAvailable() || this->profileRules_.lockdown.raidRules,
        this->lockdown_.active);
    this->refreshLists(true);
}

void ModView::testRules()
{
    if (!hasChat(this->channel_))
    {
        this->rulesEditor_->setTestResult("Open a chat to test rules.", {});
        return;
    }
    std::vector<ReviewInput> inputs;
    for (const auto &message : this->channel_->getMessageSnapshot(2000))
    {
        auto input = ReviewInput::fromMessage(*message);
        if (this->mutedPlatforms_.contains(input.platform))
            continue;
        const auto actor = this->actorID(input.platform);
        input.own = !actor.isEmpty() && actor == input.userID;
        inputs.push_back(std::move(input));
    }
    const auto result =
        ModerationReview::simulate(this->rulesEditor_->rules(), inputs);
    const auto plural = [](int count, const char *one, const char *many) {
        return QString("%1 %2").arg(count).arg(count == 1 ? one : many);
    };
    if (result.messages == 0)
    {
        this->rulesEditor_->setTestResult(
            "No chat messages are loaded yet. Try again once chat is active.",
            {});
        return;
    }
    auto summary =
        QString("In the last %1, these rules would flag %2")
            .arg(plural(result.messages, "message", "messages"),
                 plural(result.flagged, "person", "people"));
    summary += result.actions > 0
                   ? QString(" and take %1.")
                         .arg(plural(result.actions, "automatic action",
                                     "automatic actions"))
                   : QString(".");
    this->rulesEditor_->setTestResult(summary, result.examples.join('\n'));
}

std::vector<MessagePlatform> ModView::availablePlatforms() const
{
    std::vector<MessagePlatform> platforms;
    if (!hasChat(this->channel_))
        return platforms;
    if (auto *merged = dynamic_cast<MergedChannel *>(this->channel_.get()))
    {
        const auto &config = merged->config();
        if (config.twitchEnabled)
            platforms.push_back(MessagePlatform::AnyOrTwitch);
        if (config.kickEnabled)
            platforms.push_back(MessagePlatform::Kick);
        if (config.youtubeEnabled)
            platforms.push_back(MessagePlatform::YouTube);
        if (config.tiktokEnabled)
            platforms.push_back(MessagePlatform::TikTok);
    }
    else if (this->channel_->isTwitchChannel())
        platforms.push_back(MessagePlatform::AnyOrTwitch);
    else if (this->channel_->getType() == Channel::Type::Kick)
        platforms.push_back(MessagePlatform::Kick);
    return platforms;
}

void ModView::setPlatformActive(MessagePlatform platform, bool active)
{
    if (active)
        this->mutedPlatforms_.remove(platform);
    else
    {
        this->mutedPlatforms_.insert(platform);
        for (auto it = this->actionQueue_.begin();
             it != this->actionQueue_.end();)
        {
            if (it->platform != platform)
            {
                ++it;
                continue;
            }
            this->updateLog(it->logID,
                            "Cancelled: Mod View is off for this platform",
                            true, true);
            it = this->actionQueue_.erase(it);
        }
    }
    this->savePreferences();
    const auto name = ModerationReview::platformName(platform);
    this->feedback(active ? QString("Mod View reviews %1 chat again.").arg(name)
                          : QString("Mod View now ignores %1 chat here.")
                                .arg(name));
}

void ModView::editNotes()
{
    const auto *user = this->review_.find(this->selectedKey_);
    if (!user)
        return;
    const auto key = ModerationReview::notesKey(*user);
    auto *data = getApp()->getUserData();
    if (!data)
        return;
    auto *dialog = new EditUserNotesDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->updateWindowTitle(user->displayName);
    const auto existing = data->getUser(key);
    dialog->setNotes(existing ? existing->notes : QString());
    std::ignore = dialog->onOk.connect([key](const QString &notes) {
        getApp()->getUserData()->setUserNotes(key, notes);
    });
    dialog->show();
}

void ModView::addToAllowList()
{
    const auto *user = this->review_.find(this->selectedKey_);
    if (!user)
        return;
    if (this->rulesTimer_.isActive())
        this->saveRulesEditor();
    moderation::AllowEntry entry;
    entry.platform = user->platform;
    entry.name = user->login.isEmpty() ? user->displayName : user->login;
    entry.id = user->userID;
    const auto line = entry.toString();
    if (!this->profileRules_.allowList.contains(line))
        this->profileRules_.allowList.append(line);
    const auto name = user->displayName;
    this->review_.dismiss(user->key, this->now());
    this->applyRules();
    if (hasChat(this->channel_))
        this->saveProfile();
    this->loadRulesEditor();
    this->refreshLists(true);
    this->feedback(QString("%1 is on the allow-list of the \"%2\" profile.")
                       .arg(name, this->profileName_));
}

ChannelPtr ModView::platformChannel(MessagePlatform platform) const
{
    if (!this->channel_)
        return {};
    if (auto *merged = dynamic_cast<MergedChannel *>(this->channel_.get()))
    {
        if (platform == MessagePlatform::AnyOrTwitch)
            return merged->twitchChannel();
        if (platform == MessagePlatform::Kick)
            return merged->kickChannel();
        return {};
    }
    if ((platform == MessagePlatform::AnyOrTwitch &&
         this->channel_->isTwitchChannel()) ||
        (platform == MessagePlatform::Kick &&
         this->channel_->getType() == Channel::Type::Kick))
        return this->channel_;
    return {};
}

QString ModView::actorID(MessagePlatform platform) const
{
    auto *accounts = getApp()->getAccounts();
    if (!accounts)
        return {};
    switch (platform)
    {
        case MessagePlatform::AnyOrTwitch: {
            const auto account = accounts->twitch.getCurrent();
            return account && !account->isAnon() ? account->getUserId()
                                                 : QString();
        }
        case MessagePlatform::Kick: {
            const auto account = accounts->kick.current();
            return account && !account->isAnonymous()
                       ? QString::number(account->userID())
                       : QString();
        }
        case MessagePlatform::YouTube: {
            const auto account = accounts->youtube.current();
            return account && !account->isAnonymous() ? account->channelID()
                                                      : QString();
        }
        default:
            return {};
    }
}

QString ModView::actionBlocker(MessagePlatform platform, const QString &userID,
                               bool trusted, bool ignoreCooldown,
                               bool automatic) const
{
    if (!this->monitoring_)
        return "Open Mod View to moderate this chat.";
    if (!ignoreCooldown && this->now() - this->lastAction_ < 1000)
        return "Please wait a moment before sending another action.";
    if (platform == MessagePlatform::TikTok)
        return "TikTok supports local review, tracking and notes. Moderation "
               "actions are unavailable.";
    if (userID.isEmpty())
        return "A stable platform user ID is needed for moderation.";
    const auto actor = this->actorID(platform);
    if (actor.isEmpty())
        return "Sign in to this platform to use moderation actions.";
    if (actor == userID)
        return "You cannot moderate your own account here.";
    if (automatic && trusted)
        return "Mods, VIPs and owners are never actioned automatically.";
    if (platform == MessagePlatform::YouTube)
    {
        auto *merged = dynamic_cast<MergedChannel *>(this->channel_.get());
        auto *chat = merged ? merged->youtubeLiveChat() : nullptr;
        if (!chat || !chat->hasModeratorPrivileges())
            return "Moderator access to this YouTube chat is required.";
        if (trusted)
            return "Moderation of YouTube owners and moderators is "
                   "unavailable here.";
        return {};
    }
    auto channel = this->platformChannel(platform);
    if (!channel || !channel->hasModRights())
        return "Moderator access to this source chat is required.";
    if (auto *twitch = dynamic_cast<TwitchChannel *>(channel.get()))
    {
        if (twitch->roomId().isEmpty())
            return "Waiting for the Twitch channel ID.";
        if (twitch->roomId() == userID)
            return "The broadcaster cannot be moderated.";
        return {};
    }
    if (auto *kick = dynamic_cast<KickChannel *>(channel.get()))
    {
        bool valid = false;
        const auto id = userID.toULongLong(&valid);
        if (!valid || !id || !kick->userID())
            return "Waiting for valid Kick user and channel IDs.";
        if (kick->userID() == id)
            return "The broadcaster cannot be moderated.";
        return {};
    }
    return "Moderation is unavailable for this source.";
}

bool ModView::canAct(const ReviewUser &user, QString *reason,
                     bool ignoreCooldown) const
{
    const auto blocked = this->actionBlocker(user.platform, user.userID,
                                             user.trusted, ignoreCooldown,
                                             false);
    if (reason)
        *reason = blocked;
    return blocked.isEmpty();
}

void ModView::updateCapabilities()
{
    const auto *user = this->review_.find(this->selectedKey_);
    QString reason;
    // The short cooldown between actions must not make the controls blink;
    // requestAction() still enforces it.
    const bool available = user && this->canAct(*user, &reason, true);
    if (!this->confirmationKey_.isEmpty() &&
        (!available ||
         this->confirmationActor_ != this->actorID(user->platform)))
        this->confirmationKey_.clear();

    const bool confirming = !this->confirmationKey_.isEmpty();
    this->confirmation_->setVisible(confirming);
    this->primaryActions_->setVisible(!confirming);
    this->timeouts_->setVisible(available && !confirming);
    this->accessLabel_->setText(reason);
    this->accessLabel_->setVisible(user && !available);
    this->ban_->setEnabled(available);
    this->ban_->setVisible(available);
    // Still offers the allow-list when actions aren't available
    this->more_->setEnabled(user != nullptr);
    this->more_->setVisible(user != nullptr);
    this->duration_->setEnabled(available);
    if (!user)
        return;

    const auto platform = user->platform;
    const auto name = ModerationReview::platformName(platform);
    const int shortest = platform == MessagePlatform::Kick ? 60 : 1;
    const int longest = platform == MessagePlatform::YouTube ? 86400 : 1209600;
    for (const auto &preset : this->timeoutPresets_)
    {
        preset.button->setVisible(preset.seconds >= shortest &&
                                  preset.seconds <= longest);
        preset.button->setToolTip(QString("Timeout on %1").arg(name));
    }
    this->ban_->setToolTip(QString("Ban on %1").arg(name));
    this->duration_->setMaximum(longest);
}

void ModView::showTools()
{
    QMenu menu(this);
    // Mass moderation runs on one platform's chat at a time
    for (const auto platform :
         {MessagePlatform::AnyOrTwitch, MessagePlatform::Kick})
    {
        auto channel = this->platformChannel(platform);
        if (!hasChat(channel))
            continue;
        const auto suffix =
            this->merged_
                ? QString(" on %1").arg(ModerationReview::platformName(platform))
                : QString();
        const bool allowed = channel->hasModRights();
        menu.addAction(QString("Mass ban%1...").arg(suffix), this, [channel] {
                openMassModerationDialog(channel, true);
            })->setEnabled(allowed);
        menu.addAction(QString("Mass unban%1...").arg(suffix), this, [channel] {
                openMassModerationDialog(channel, false);
            })->setEnabled(allowed);
    }
    menu.addAction(this->lockdown_.active ? "End lockdown" : "Lock down chat",
                   this, &ModView::toggleLockdown)
        ->setEnabled(this->lockdown_.active || this->lockdownAvailable() ||
                     this->profileRules_.lockdown.raidRules);
    menu.addSeparator();
    menu.addAction("Configure inline moderation buttons...", this, [this] {
        getApp()->getWindows()->showSettingsDialog(
            this, SettingsDialogPreference::ModerationActions);
    });
    if (this->split_)
    {
        menu.addAction("Open platform moderator website", this, [this] {
            this->split_->openModViewInBrowser();
        });
        menu.addAction("Pop out / dock Mod View", this, [this] {
            this->popOutRequested.invoke();
        });
    }
    menu.exec(this->tools_->mapToGlobal(QPoint(0, this->tools_->height())));
}

void ModView::showMoreActions()
{
    const auto *user = this->review_.find(this->selectedKey_);
    if (!user)
        return;
    const bool available = this->canAct(*user, nullptr, true);
    QMenu menu(this);
    menu.addAction("Custom timeout...", this, [this] {
            this->requestAction(Action::Timeout);
        })->setEnabled(available);
    if (user->platform == MessagePlatform::AnyOrTwitch)
        menu.addAction("Warn...", this, [this] {
                this->requestAction(Action::Warn);
            })->setEnabled(available);
    menu.addAction("Unban / remove timeout", this, [this] {
            this->requestAction(Action::Unban);
        })->setEnabled(available);
    auto *remove = menu.addAction("Delete selected message", this, [this] {
        this->requestAction(Action::Delete);
    });
    remove->setEnabled(available && this->history_->currentItem() &&
                       !this->history_->currentItem()
                            ->data(Qt::UserRole)
                            .toString()
                            .isEmpty());
    menu.addSeparator();
    menu.addAction("Add to allow-list", this, &ModView::addToAllowList)
        ->setToolTip("Rules in this profile never flag or act on them");
    menu.exec(this->more_->mapToGlobal(QPoint(0, this->more_->height())));
}

void ModView::requestAction(Action action, int seconds)
{
    const auto *user = this->review_.find(this->selectedKey_);
    QString reason;
    if (!user || !this->canAct(*user, &reason))
    {
        this->feedback(reason);
        return;
    }
    QString messageID;
    if (action == Action::Delete)
    {
        if (auto *item = this->history_->currentItem())
            messageID = item->data(Qt::UserRole).toString();
        if (messageID.isEmpty())
        {
            this->feedback("Select an undeleted message first.");
            return;
        }
    }
    const auto actor = this->actorID(user->platform);
    if (action == Action::Warn)
    {
        const auto key = user->key;
        bool ok = false;
        const auto text =
            QInputDialog::getText(
                this, QString("Warn %1").arg(user->displayName),
                "Reason (they must acknowledge it before chatting again):",
                QLineEdit::Normal, {}, &ok)
                .simplified();
        if (!ok)
            return;
        if (text.isEmpty())
        {
            this->feedback("A warning needs a reason.");
            return;
        }
        this->executeAction(action, key, {}, actor, 0, text.left(500));
        return;
    }
    const bool customTimeout = action == Action::Timeout && seconds <= 0;
    if (action == Action::Ban || action == Action::Delete || customTimeout)
    {
        const auto platform = ModerationReview::platformName(user->platform);
        this->confirmationKey_ = user->key;
        this->confirmationMessageID_ = messageID;
        this->confirmationActor_ = actor;
        this->confirmationAction_ = action;
        this->duration_->setVisible(customTimeout);
        this->confirmationLabel_->setText(
            customTimeout ? QString("Timeout %1 for").arg(user->displayName)
            : action == Action::Ban
                ? QString("Ban %1 on %2?").arg(user->displayName, platform)
                : QString("Delete this message from %1?")
                      .arg(user->displayName));
        this->confirm_->setText(customTimeout           ? "Timeout"
                                : action == Action::Ban ? "Ban"
                                                        : "Delete");
        this->updateCapabilities();
        return;
    }
    this->executeAction(action, user->key, messageID, actor, seconds);
}

void ModView::executeAction(Action action, const QString &key,
                            const QString &messageID, const QString &actor,
                            int seconds, const QString &reason)
{
    const auto *user = this->review_.find(key);
    QString blocked;
    if (!user || key != this->selectedKey_ || !this->canAct(*user, &blocked))
    {
        this->feedback(blocked.isEmpty() ? "Select the user again before acting."
                                         : blocked);
        return;
    }
    if (actor != this->actorID(user->platform))
    {
        this->feedback(
            "Your signed-in account changed. Select the action again.");
        return;
    }
    if (action == Action::Delete &&
        (messageID.isEmpty() ||
         std::none_of(user->history.begin(), user->history.end(),
                      [&](const auto &m) {
                          return m.messageID == messageID && !m.deleted;
                      })))
    {
        this->feedback("That message is no longer available for deletion.");
        return;
    }
    if (!actionAvailable(action, user->platform))
    {
        this->feedback(QString("%1 isn't available on %2.")
                           .arg(actionText(action, seconds),
                                ModerationReview::platformName(user->platform)));
        return;
    }
    ActionRequest request;
    request.key = key;
    request.platform = user->platform;
    request.userID = user->userID;
    request.name = user->displayName;
    request.action = action;
    request.seconds = seconds;
    request.messageID = messageID;
    request.reason = reason;
    request.source = "You";
    request.actor = actor;
    request.logID = this->addLog(request, "Sending");
    this->feedback(QString("Sending %1 for %2...")
                       .arg(actionText(action, seconds).toLower(),
                            user->displayName));
    this->lastAction_ = this->now();
    this->sendAction(request);
    this->updateCapabilities();
}

QString ModView::actionText(Action action, int seconds)
{
    switch (action)
    {
        case Action::Timeout:
            return "Timeout " + moderation::durationLabel(seconds);
        case Action::Ban:
            return "Ban";
        case Action::Unban:
            return "Unban";
        case Action::Delete:
            return "Delete message";
        case Action::Warn:
            return "Warn";
    }
    return {};
}

bool ModView::actionAvailable(Action action, MessagePlatform platform)
{
    if (platform == MessagePlatform::TikTok)
        return false;
    return action != Action::Warn || platform == MessagePlatform::AnyOrTwitch;
}

void ModView::processTriggers()
{
    for (const auto &trigger : this->review_.takeTriggers())
    {
        const auto *user = this->review_.find(trigger.key);
        if (!user || !this->review_.rules().automaticActions ||
            this->mutedPlatforms_.contains(user->platform))
            continue;
        ActionRequest request;
        request.key = user->key;
        request.platform = user->platform;
        request.userID = user->userID;
        request.name = user->displayName;
        request.messageID = trigger.messageID;
        request.source = "Auto: " + trigger.rule;
        request.reason = "Mod View rule: " + trigger.rule;
        switch (trigger.action)
        {
            case RuleAction::Delete:
                request.action = Action::Delete;
                break;
            case RuleAction::Warn:
                request.action = Action::Warn;
                break;
            case RuleAction::Timeout:
                request.action = Action::Timeout;
                request.seconds = trigger.timeout;
                break;
            case RuleAction::Ban:
                request.action = Action::Ban;
                break;
            default:
                continue;
        }
        if (!actionAvailable(request.action, user->platform))
        {
            // Said once, rather than for every message that matches
            const auto platform = ModerationReview::platformName(user->platform);
            const auto text = actionText(request.action, request.seconds);
            if (!this->unsupportedNoted_.contains(platform + text))
            {
                this->unsupportedNoted_.insert(platform + text);
                this->addLog(request,
                             QString("Not sent: %1 can't be done on %2, so "
                                     "these users are only flagged")
                                 .arg(text, platform),
                             true);
            }
            continue;
        }
        this->enqueueAction(std::move(request));
    }
}

void ModView::lookUpAccountAges()
{
    if (this->ageLookupBusy_ || this->ageKeys_.isEmpty())
        return;
    QStringList ids;
    QHash<QString, QString> keys;
    while (!this->ageKeys_.isEmpty() && ids.size() < 100)
    {
        const auto key = this->ageKeys_.takeFirst();
        const auto *user = this->review_.find(key);
        if (!user || user->platform != MessagePlatform::AnyOrTwitch ||
            user->userID.isEmpty() || keys.contains(user->userID))
            continue;
        ids.append(user->userID);
        keys.insert(user->userID, key);
    }
    if (ids.isEmpty())
        return;
    this->ageLookupBusy_ = true;
    const QPointer<ModView> weak(this);
    const auto scope = this->scope_;
    getHelix()->fetchUsers(
        ids, {},
        [weak, scope, keys](const std::vector<HelixUser> &users) {
            if (!weak)
                return;
            weak->ageLookupBusy_ = false;
            if (weak->scope_ != scope)
                return;
            for (const auto &user : users)
            {
                const auto created =
                    QDateTime::fromString(user.createdAt, Qt::ISODate);
                if (created.isValid() && keys.contains(user.id))
                    weak->review_.applyAccountCreated(keys.value(user.id),
                                                      created, weak->now());
            }
            weak->processTriggers();
            weak->refreshLists();
            if (!weak->ageKeys_.isEmpty())
                weak->ageTimer_.start();
        },
        [weak] {
            if (weak)
                weak->ageLookupBusy_ = false;
        });
}

void ModView::enqueueAction(ActionRequest request)
{
    if (this->actionQueue_.size() >= MAX_QUEUED_ACTIONS)
    {
        this->addLog(request, "Not sent: too many actions are waiting", true);
        return;
    }
    request.logID = this->addLog(request, "Queued");
    this->actionQueue_.push_back(std::move(request));
    if (!this->actionTimer_.isActive())
    {
        this->actionTimer_.start();
        this->dispatchAction();
    }
}

void ModView::dispatchAction()
{
    if (this->actionQueue_.empty())
    {
        this->actionTimer_.stop();
        return;
    }
    const auto request = std::move(this->actionQueue_.front());
    this->actionQueue_.pop_front();
    this->sendAction(request);
}

void ModView::sendAction(const ActionRequest &request)
{
    const bool automatic = unattended(request.source);
    const auto *user = this->review_.find(request.key);
    QString blocked;
    if (!user && automatic)
        blocked = "they are no longer in Mod View";
    if (blocked.isEmpty())
        blocked = this->actionBlocker(request.platform, request.userID,
                                      user && user->trusted, true, automatic);
    const auto actor = this->actorID(request.platform);
    if (blocked.isEmpty() && !request.actor.isEmpty() && request.actor != actor)
        blocked = "your signed-in account changed";
    if (blocked.isEmpty() && !actionAvailable(request.action, request.platform))
        blocked = QString("%1 isn't available on %2")
                      .arg(actionText(request.action, request.seconds),
                           ModerationReview::platformName(request.platform));
    if (blocked.isEmpty() && request.action == Action::Delete &&
        (request.messageID.isEmpty() ||
         (user && std::none_of(user->history.begin(), user->history.end(),
                               [&](const auto &m) {
                                   return m.messageID == request.messageID &&
                                          !m.deleted;
                               }))))
        blocked = "the message is already gone";
    if (!blocked.isEmpty())
    {
        blocked[0] = blocked[0].toLower();
        if (blocked.endsWith('.'))
            blocked.chop(1);
        this->updateLog(request.logID, "Not sent: " + blocked, true, true);
        if (!automatic)
            this->feedback(QString("Not sent: %1.").arg(blocked));
        return;
    }
    this->updateLog(request.logID, "Sending", false, false);

    const auto scope = this->scope_;
    const auto logID = request.logID;
    const auto key = request.key;
    const auto name = request.name;
    const auto action = request.action;
    const QPointer<ModView> weak(this);
    const auto completed = [weak, scope, logID, key, name, action,
                            automatic](QString error) {
        postToThread([weak, scope, logID, key, name, action, automatic,
                      error = std::move(error)] {
            if (!weak || weak->scope_ != scope)
                return;
            weak->updateLog(logID, error.isEmpty() ? "Done" : "Failed: " + error,
                            !error.isEmpty(), true);
            if (error.isEmpty() &&
                (action == Action::Timeout || action == Action::Ban))
                weak->review_.noteActioned(key);
            if (!automatic)
                weak->feedback(
                    error.isEmpty()
                        ? QString("Action completed for %1.").arg(name)
                        : QString("Action failed for %1: %2").arg(name, error));
        });
    };
    // Every action goes to the chat of the platform the user is on, by their
    // platform user ID
    const auto seconds = std::max(1, request.seconds);
    if (request.platform == MessagePlatform::AnyOrTwitch)
    {
        auto channel = this->platformChannel(request.platform);
        auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
        if (!twitch)
            return completed("The Twitch chat is unavailable.");
        const auto room = twitch->roomId();
        const auto success = [completed] {
            completed({});
        };
        const auto failure = [completed](auto, const QString &error) {
            completed(error.isEmpty() ? "Twitch rejected the action." : error);
        };
        switch (request.action)
        {
            case Action::Delete:
                getHelix()->deleteChatMessages(room, actor, request.messageID,
                                               success, failure);
                break;
            case Action::Unban:
                getHelix()->unbanUser(room, actor, request.userID, success,
                                      failure);
                break;
            case Action::Warn:
                getHelix()->warnUser(room, actor, request.userID,
                                     request.reason, success, failure);
                break;
            case Action::Timeout:
            case Action::Ban:
                getHelix()->banUser(
                    room, actor, request.userID,
                    request.action == Action::Timeout
                        ? std::optional<int>(std::min(seconds, 1209600))
                        : std::nullopt,
                    request.reason, success, failure);
                break;
        }
    }
    else if (request.platform == MessagePlatform::Kick)
    {
        auto channel = this->platformChannel(request.platform);
        auto *kick = dynamic_cast<KickChannel *>(channel.get());
        if (!kick)
            return completed("The Kick chat is unavailable.");
        const auto callback = [completed](auto result) {
            completed(result ? QString() : result.error());
        };
        const auto userID = request.userID.toULongLong();
        switch (request.action)
        {
            case Action::Delete:
                getKickApi()->deleteChatMessage(request.messageID, callback);
                break;
            case Action::Unban:
                getKickApi()->unbanUser(kick->userID(), userID, callback);
                break;
            case Action::Timeout:
            case Action::Ban:
                getKickApi()->banUser(
                    kick->userID(), userID,
                    request.action == Action::Timeout
                        ? std::optional<std::chrono::minutes>(
                              std::chrono::minutes((seconds + 59) / 60))
                        : std::nullopt,
                    request.reason, callback);
                break;
            case Action::Warn:
                completed("Warnings are only available on Twitch.");
                break;
        }
    }
    else if (request.platform == MessagePlatform::YouTube)
    {
        auto *merged = dynamic_cast<MergedChannel *>(this->channel_.get());
        auto *chat = merged ? merged->youtubeLiveChat() : nullptr;
        if (!chat)
            return completed("The YouTube chat is unavailable.");
        switch (request.action)
        {
            case Action::Delete:
                chat->deleteMessage(request.messageID);
                break;
            case Action::Ban:
                chat->banUser(request.userID, name);
                break;
            case Action::Unban:
                chat->unbanUser(request.userID, name);
                break;
            case Action::Timeout:
                chat->timeoutUser(request.userID, std::min(seconds, 86400),
                                  name);
                break;
            case Action::Warn:
                return completed("Warnings are only available on Twitch.");
        }
        // YouTube reports the result in chat rather than to a callback
        this->updateLog(logID, "Sent to YouTube", false, true);
        if (action == Action::Timeout || action == Action::Ban)
            this->review_.noteActioned(key);
        if (!automatic)
            this->feedback(
                "Action sent to YouTube. Check chat for the platform result.");
    }
}

quint64 ModView::addLog(const ActionRequest &request, const QString &status,
                        bool failed)
{
    LogEntry entry;
    entry.id = ++this->logSequence_;
    entry.at = this->now();
    entry.time = QDateTime::currentDateTime();
    entry.request = request;
    entry.request.logID = entry.id;
    entry.status = status;
    entry.failed = failed;
    entry.done = failed;
    this->log_.push_back(std::move(entry));
    while (this->log_.size() > MAX_LOG)
        this->log_.pop_front();
    this->refreshLog();
    return this->logSequence_;
}

void ModView::updateLog(quint64 id, const QString &status, bool failed,
                        bool done)
{
    for (auto &entry : this->log_)
        if (entry.id == id)
        {
            entry.status = status;
            entry.failed = failed;
            entry.done = done;
            this->refreshLog();
            return;
        }
}

void ModView::refreshLog()
{
    this->logList_->setVisible(!this->log_.empty());
    this->emptyLog_->setVisible(this->log_.empty());
    this->exportLog_->setEnabled(!this->log_.empty());
    // Rebuilt only while it can be seen
    if (this->tabs_->currentIndex() != TAB_LOG)
        return;
    const auto selected = this->logList_->currentItem()
                              ? this->logList_->currentItem()
                                    ->data(Qt::UserRole)
                                    .toULongLong()
                              : 0;
    auto *scrollbar = this->logList_->verticalScrollBar();
    const auto scroll = scrollbar->value();
    {
        QSignalBlocker blocker(this->logList_);
        this->logList_->clear();
        for (auto it = this->log_.rbegin(); it != this->log_.rend(); ++it)
        {
            const auto &request = it->request;
            const auto platform = ModerationReview::platformName(request.platform);
            auto *item = new QListWidgetItem(
                QString("%1 · %2").arg(
                    actionText(request.action, request.seconds), request.name),
                this->logList_);
            item->setData(Qt::UserRole, qulonglong(it->id));
            item->setData(DetailRole,
                          QString("%1 · %2").arg(request.source, it->status));
            item->setData(StampRole, it->at);
            item->setData(IconRole, this->merged_
                                        ? platformIconPath(request.platform)
                                        : QString());
            if (it->failed)
                item->setData(ColorRole, this->style_.attention);
            item->setToolTip(
                QString("%1 · %2 · %3 (ID %4)\n%5 · %6")
                    .arg(it->time.toString("HH:mm:ss"), platform, request.name,
                         request.userID.isEmpty() ? QString("unknown")
                                                  : request.userID,
                         request.source, it->status)
                    .toHtmlEscaped());
            if (it->id == selected)
                this->logList_->setCurrentItem(item);
        }
    }
    scrollbar->setValue(scroll);
    bool undoable = false;
    for (const auto &entry : this->log_)
        if (entry.id == selected)
            undoable = entry.done && !entry.failed &&
                       (entry.request.action == Action::Timeout ||
                        entry.request.action == Action::Ban);
    this->undo_->setEnabled(undoable);
}

void ModView::undoLogEntry()
{
    auto *item = this->logList_->currentItem();
    if (!item)
        return;
    const auto id = item->data(Qt::UserRole).toULongLong();
    const auto it = std::find_if(this->log_.begin(), this->log_.end(),
                                 [id](const LogEntry &entry) {
                                     return entry.id == id;
                                 });
    if (it == this->log_.end() || !it->done || it->failed ||
        (it->request.action != Action::Timeout &&
         it->request.action != Action::Ban))
    {
        this->feedback("Only timeouts and bans that went through can be "
                       "undone.");
        return;
    }
    auto request = it->request;
    request.action = Action::Unban;
    request.seconds = 0;
    request.messageID.clear();
    request.reason.clear();
    request.source = "Undo";
    request.actor.clear();
    request.logID = this->addLog(request, "Sending");
    this->sendAction(request);
}

void ModView::exportLog()
{
    if (this->log_.empty())
        return;
    const auto path = QFileDialog::getSaveFileName(
        this, "Export action log",
        QString("Mod View log %1.csv")
            .arg(QDate::currentDate().toString(Qt::ISODate)),
        "CSV files (*.csv)");
    if (path.isEmpty())
        return;
    QStringList lines{"time,platform,user,user_id,action,duration_seconds,"
                      "source,reason,status"};
    for (const auto &entry : this->log_)
    {
        const auto &request = entry.request;
        lines.append(
            QStringList{
                csvField(entry.time.toString(Qt::ISODate)),
                csvField(ModerationReview::platformName(request.platform)),
                csvField(request.name), csvField(request.userID),
                csvField(actionText(request.action, 0).trimmed()),
                request.action == Action::Timeout
                    ? QString::number(request.seconds)
                    : QString(),
                csvField(request.source), csvField(request.reason),
                csvField(entry.status)}
                .join(','));
    }
    const auto saved = writeFile(path, (lines.join("\r\n") + "\r\n").toUtf8());
    this->feedback(saved ? QString("Saved %1.").arg(QFileInfo(path).fileName())
                         : QString("Couldn't save %1.")
                               .arg(QFileInfo(path).fileName()));
}

QStringList ModView::bulkKeys() const
{
    const auto tab = this->tabs_->currentIndex();
    const auto *list = tab == TAB_REVIEW     ? this->queue_
                       : tab == TAB_TRACKING ? this->tracked_
                                             : nullptr;
    QStringList keys;
    if (!list)
        return keys;
    for (const auto *item : list->selectedItems())
        keys.append(item->data(Qt::UserRole).toString());
    return keys;
}

void ModView::updateBulkBar()
{
    const auto count = this->bulkKeys().size();
    if (count < 2)
        this->confirmingBulk_ = false;
    this->bulkBar_->setVisible(count > 1);
    if (count < 2)
        return;
    const bool confirming = this->confirmingBulk_;
    this->bulkLabel_->setText(
        confirming
            ? QString("Ban %1 users, each on their own platform?").arg(count)
            : QString("%1 selected").arg(count));
    this->bulkDismiss_->setText(
        this->tabs_->currentIndex() == TAB_REVIEW ? "Dismiss" : "Untrack");
    this->bulkDismiss_->setVisible(!confirming);
    this->bulkTimeout_->setVisible(!confirming);
    this->bulkBan_->setVisible(!confirming);
    this->bulkConfirm_->setVisible(confirming);
    this->bulkCancel_->setText(confirming ? "Cancel" : "Clear");
}

void ModView::bulkAction(Action action, int seconds)
{
    const auto keys = this->bulkKeys();
    this->confirmingBulk_ = false;
    int queued = 0;
    int skipped = 0;
    for (const auto &key : keys)
    {
        const auto *user = this->review_.find(key);
        if (!user)
            continue;
        ActionRequest request;
        request.key = key;
        request.platform = user->platform;
        request.userID = user->userID;
        request.name = user->displayName;
        request.action = action;
        request.seconds = seconds;
        request.source = "Bulk";
        auto blocked = this->actionBlocker(user->platform, user->userID,
                                           user->trusted, true, true);
        if (blocked.isEmpty() && !actionAvailable(action, user->platform))
            blocked = QString("%1 isn't available on %2.")
                          .arg(actionText(action, seconds),
                               ModerationReview::platformName(user->platform));
        if (!blocked.isEmpty())
        {
            blocked[0] = blocked[0].toLower();
            this->addLog(request, "Not sent: " + blocked, true);
            ++skipped;
            continue;
        }
        this->enqueueAction(std::move(request));
        ++queued;
    }
    (this->tabs_->currentIndex() == TAB_REVIEW ? this->queue_ : this->tracked_)
        ->clearSelection();
    this->updateBulkBar();
    auto text = QString("Sending %1 %2.")
                    .arg(queued)
                    .arg(action == Action::Ban
                             ? (queued == 1 ? "ban" : "bans")
                             : (queued == 1 ? "timeout" : "timeouts"));
    if (skipped > 0)
        text += QString(" %1 skipped, see the Log tab.").arg(skipped);
    this->feedback(text);
}

bool ModView::lockdownAvailable() const
{
    auto channel = this->platformChannel(MessagePlatform::AnyOrTwitch);
    auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
    return this->monitoring_ && twitch && twitch->hasModRights() &&
           !twitch->roomId().isEmpty() &&
           !this->actorID(MessagePlatform::AnyOrTwitch).isEmpty();
}

void ModView::toggleLockdown()
{
    auto channel = this->platformChannel(MessagePlatform::AnyOrTwitch);
    auto *twitch = dynamic_cast<TwitchChannel *>(channel.get());
    const bool available = this->lockdownAvailable();
    const auto room = twitch ? twitch->roomId() : QString();
    const auto actor = this->actorID(MessagePlatform::AnyOrTwitch);
    const QPointer<ModView> weak(this);
    const auto scope = this->scope_;
    const auto failed = [weak, scope](const QString &mode) {
        return [weak, scope, mode](auto, const QString &error) {
            postToThread([weak, scope, mode, error] {
                if (weak && weak->scope_ == scope)
                    weak->feedback(QString("Couldn't change %1: %2")
                                       .arg(mode, error.isEmpty()
                                                      ? QString("Twitch said no")
                                                      : error));
            });
        };
    };
    const auto ok = [](const auto &) {};

    if (this->lockdown_.active)
    {
        const auto state = this->lockdown_;
        this->lockdown_ = {};
        if (available)
        {
            if (state.followers)
                getHelix()->updateFollowerMode(room, actor, std::nullopt, ok,
                                               failed("followers-only"));
            if (state.slow)
                getHelix()->updateSlowMode(room, actor, std::nullopt, ok,
                                           failed("slow mode"));
            if (state.emotes)
                getHelix()->updateEmoteMode(room, actor, false, ok,
                                            failed("emote-only"));
            if (state.subscribers)
                getHelix()->updateSubscriberMode(room, actor, false, ok,
                                                 failed("subscribers-only"));
            if (state.shield)
                getHelix()->updateShieldMode(room, actor, false, ok,
                                             failed("shield mode"));
            this->feedback("Lockdown ended.");
        }
        else if (state.followers || state.slow || state.emotes ||
                 state.subscribers || state.shield)
            this->feedback("Lockdown ended here, but Twitch chat modes "
                           "couldn't be changed back. Check them in chat.");
        else
            this->feedback("Lockdown ended.");
    }
    else
    {
        const auto &settings = this->profileRules_.lockdown;
        if (!available && !settings.raidRules)
        {
            this->feedback("Lockdown needs moderator access on Twitch.");
            return;
        }
        Lockdown state;
        state.active = true;
        if (available)
        {
            // Only modes that are off now are turned on, and later off again
            const auto modes = twitch->accessRoomModes();
            if (settings.followersOnly && modes->followerOnly < 0)
            {
                state.followers = true;
                getHelix()->updateFollowerMode(room, actor,
                                               settings.followersMinutes, ok,
                                               failed("followers-only"));
            }
            if (settings.slowMode && modes->slowMode <= 0)
            {
                state.slow = true;
                getHelix()->updateSlowMode(room, actor, settings.slowSeconds,
                                           ok, failed("slow mode"));
            }
            if (settings.emoteOnly && !modes->emoteOnly)
            {
                state.emotes = true;
                getHelix()->updateEmoteMode(room, actor, true, ok,
                                            failed("emote-only"));
            }
            if (settings.subscribersOnly && !modes->submode)
            {
                state.subscribers = true;
                getHelix()->updateSubscriberMode(room, actor, true, ok,
                                                 failed("subscribers-only"));
            }
            if (settings.shieldMode)
            {
                state.shield = true;
                getHelix()->updateShieldMode(room, actor, true, ok,
                                             failed("shield mode"));
            }
        }
        this->lockdown_ = state;
        this->feedback(available ? "Chat locked down."
                                 : "Raid rules on. Chat modes can only be "
                                   "changed on Twitch.");
    }
    this->applyRules();
    this->updateLockdownBar();
    this->refreshLists(true);
}

void ModView::updateLockdownBar()
{
    QStringList parts;
    if (this->lockdown_.followers)
        parts.append("followers-only");
    if (this->lockdown_.slow)
        parts.append("slow mode");
    if (this->lockdown_.emotes)
        parts.append("emote-only");
    if (this->lockdown_.subscribers)
        parts.append("subscribers-only");
    if (this->lockdown_.shield)
        parts.append("shield mode");
    if (this->profileRules_.lockdown.raidRules)
        parts.append("raid rules");
    this->lockdownLabel_->setText(
        parts.isEmpty() ? QString("Chat is locked down")
                        : QString("Locked down: %1").arg(parts.join(", ")));
    this->lockdownBar_->setVisible(this->lockdown_.active);
    this->rulesEditor_->setLockdown(
        this->lockdownAvailable() || this->profileRules_.lockdown.raidRules,
        this->lockdown_.active);
}

}  // namespace chatterino
