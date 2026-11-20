// SPDX-License-Identifier: MIT
#include "widgets/splits/PriorityChat.hpp"

#include "Application.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/ignores/IgnoreController.hpp"
#include "providers/IvrApi.hpp"
#include "providers/kick/KickApi.hpp"
#include "providers/kick/KickChannel.hpp"
#include "providers/kick/KickUserBlock.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/api/Helix.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Settings.hpp"
#include "singletons/Theme.hpp"
#include "util/PostToThread.hpp"
#include "widgets/BasePopup.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/splits/Split.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QCursor>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringListModel>
#include <QStyle>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <deque>
#include <mutex>

namespace chatterino {
using namespace priority;
namespace {
constexpr qint64 RETRY_MS = 10 * 60 * 1000;
// All panes share a request pace; opening several channels must not multiply it.
qint64 nextGlobalLookup = 0;
QLabel *label(const QString &text, QWidget *parent)
{
    auto *result = new QLabel(text, parent);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    result->setMinimumWidth(0);
    return result;
}
QSpinBox *minimumMonths(QWidget *parent)
{
    // An enabled rule needs at least one month; zero would match nobody.
    auto *spin = new QSpinBox(parent);
    spin->setRange(1, 1200);
    spin->setSuffix(" month");
    QObject::connect(spin, &QSpinBox::valueChanged, spin, [spin](int value) {
        spin->setSuffix(value == 1 ? " month" : " months");
    });
    return spin;
}
QIcon platformIcon(MessagePlatform platform)
{
    switch (platform)
    {
        case MessagePlatform::Kick:
            return QIcon(QStringLiteral(":/platforms/kick.svg"));
        case MessagePlatform::YouTube:
            return QIcon(QStringLiteral(":/platforms/youtube.svg"));
        case MessagePlatform::TikTok:
            return QIcon(QStringLiteral(":/platforms/tiktok.svg"));
        default:
            return QIcon(QStringLiteral(":/platforms/twitch.svg"));
    }
}
// Both tabs share one size, so switching tabs never resizes the window. The
// Rules tab sets the natural size; the Users list scrolls to fit into it.
// QTabWidget sizes itself to the largest page, so this page only asks for its
// minimum.
class FittingPage final : public QWidget
{
public:
    using QWidget::QWidget;
    QSize sizeHint() const override
    {
        return minimumSizeHint();
    }
    int heightForWidth(int /*width*/) const override
    {
        return -1;
    }
};

// Dark surfaces derived from the theme palette, so the window matches the app
// instead of Fusion's light gray tab page.
struct SurfaceColors {
    QColor window, panel, well, border, hover, text, muted;
};
SurfaceColors surfaceColors(const QPalette &palette)
{
    const auto window = palette.color(QPalette::Window);
    const auto text = palette.color(QPalette::WindowText);
    const auto mix = [&](qreal amount) {
        const auto channel = [amount](float from, float to) {
            return std::clamp(float(from + (to - from) * amount), 0.F, 1.F);
        };
        return QColor::fromRgbF(channel(window.redF(), text.redF()),
                                channel(window.greenF(), text.greenF()),
                                channel(window.blueF(), text.blueF()));
    };
    return {window,    mix(0.035), mix(-0.02), mix(0.16),
            mix(0.07), text,       mix(0.55)};
}

// One quiet surface joins the platform prefix and username editor.
class UserEntryWell final : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto colors = surfaceColors(palette());
        painter.setPen(colors.border);
        painter.setBrush(colors.well);
        const qreal radius = fontMetrics().height() * 0.22;
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5),
                                radius, radius);
    }
};

class PlatformPicker final : public QComboBox
{
public:
    explicit PlatformPicker(QWidget *parent)
        : QComboBox(parent)
    {
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setAccessibleName("User platform");
        QObject::connect(this, &QComboBox::currentIndexChanged, this, [this] {
            setToolTip("Choose platform: " + currentText());
            update();
        });
    }
    QSize sizeHint() const override
    {
        auto size = QComboBox::sizeHint();
        size.setWidth(iconSize().width() + fontMetrics().height() * 2);
        return size;
    }
    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }
    void showPopup() override
    {
        int textWidth = 0;
        for (int i = 0; i < count(); ++i)
            textWidth = std::max(textWidth,
                                 fontMetrics().horizontalAdvance(itemText(i)));
        view()->setMinimumWidth(textWidth + iconSize().width() +
                                fontMetrics().height() * 3);
        QComboBox::showPopup();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto colors = surfaceColors(palette());
        const qreal unit = fontMetrics().height() / 14.0;
        if (underMouse() && isEnabled())
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.hover);
            painter.drawRoundedRect(QRectF(rect()).adjusted(2, 2, -2, -2),
                                    2 * unit, 2 * unit);
        }
        const auto icon = iconSize();
        const QRect iconRect(qRound(6 * unit), (height() - icon.height()) / 2,
                             icon.width(), icon.height());
        itemIcon(currentIndex())
            .paint(&painter, iconRect, Qt::AlignCenter,
                   isEnabled() ? QIcon::Normal : QIcon::Disabled);
        const QPointF center(width() - 10 * unit, height() / 2.0);
        QPainterPath arrow;
        arrow.moveTo(center + QPointF(-3 * unit, -1.5 * unit));
        arrow.lineTo(center + QPointF(0, 1.5 * unit));
        arrow.lineTo(center + QPointF(3 * unit, -1.5 * unit));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(colors.muted, 1.2 * unit, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(arrow);
    }
};

void platforms(QComboBox *combo)
{
    for (int i = 0; i < 4; ++i)
    {
        const auto platform = static_cast<MessagePlatform>(i);
        combo->addItem(platformIcon(platform), platformName(platform), i);
    }
}

// Flat tabs: the selected one joins the page below, the others are just text.
class SettingsTabBar final : public QTabBar
{
public:
    explicit SettingsTabBar(QWidget *parent)
        : QTabBar(parent)
    {
        setExpanding(false);
        setDrawBase(false);
        setMouseTracking(true);
    }

protected:
    QSize tabSizeHint(int index) const override
    {
        const QFontMetrics metrics(font());
        return {metrics.horizontalAdvance(tabText(index)) +
                    metrics.height() * 2,
                metrics.height() + metrics.height() * 3 / 4};
    }
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        const auto colors = surfaceColors(palette());
        const int hovered =
            underMouse() ? tabAt(mapFromGlobal(QCursor::pos())) : -1;
        for (int i = 0; i < count(); ++i)
        {
            const auto rect = tabRect(i);
            const bool selected = i == currentIndex();
            if (selected)
            {
                painter.fillRect(rect, colors.panel);
                painter.setPen(colors.border);
                painter.drawLine(rect.topLeft(), rect.bottomLeft());
                painter.drawLine(rect.topLeft(), rect.topRight());
                painter.drawLine(rect.topRight(), rect.bottomRight());
            }
            else if (i == hovered)
                painter.fillRect(rect.adjusted(1, 1, -1, -1), colors.hover);
            painter.setPen(selected ? colors.text : colors.muted);
            painter.drawText(rect, Qt::AlignCenter, tabText(i));
        }
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        QTabBar::mouseMoveEvent(event);
        update();
    }
    void leaveEvent(QEvent *event) override
    {
        QTabBar::leaveEvent(event);
        update();
    }
};

class SettingsTabs final : public QTabWidget
{
public:
    explicit SettingsTabs(QWidget *parent)
        : QTabWidget(parent)
    {
        setTabBar(new SettingsTabBar(this));
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        // The page, with its top edge on the tab bar's last row so the
        // selected tab covers it.
        QPainter painter(this);
        const auto colors = surfaceColors(palette());
        auto page = rect();
        page.setTop(tabBar()->geometry().bottom());
        painter.fillRect(page, colors.panel);
        painter.setPen(colors.border);
        painter.drawRect(page.adjusted(0, 0, -1, -1));
    }
};

// A darker, bordered well for the Users list.
class UsersWell final : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        QPainter painter(this);
        const auto colors = surfaceColors(palette());
        painter.fillRect(rect(), colors.well);
        painter.setPen(colors.border);
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }
};

// A Users row, with a divider above every row but the first.
class UserRow final : public QWidget
{
public:
    UserRow(bool divider, QWidget *parent = nullptr)
        : QWidget(parent)
        , divider_(divider)
    {
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override
    {
        if (!divider_)
            return;
        QPainter painter(this);
        painter.setPen(surfaceColors(palette()).border);
        painter.drawLine(0, 0, width(), 0);
    }

private:
    bool divider_;
};

// The small x on each Users row, drawn like the giveaway entrant list's.
class RemoveUserButton final : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const auto color = underMouse() ? QColor(0xff, 0x6b, 0x6b)
                                        : palette().color(QPalette::ButtonText);
        painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
        const auto center = QRectF(rect()).center();
        const qreal half = width() * 0.15;
        painter.drawLine(center + QPointF(-half, -half),
                         center + QPointF(half, half));
        painter.drawLine(center + QPointF(half, -half),
                         center + QPointF(-half, half));
    }
};

class PrioritySettingsPopup final : public BasePopup
{
public:
    PrioritySettingsPopup(QDialogButtonBox *buttons, QLineEdit *addName,
                          QWidget *parent)
        : BasePopup(
              {BaseWindow::EnableCustomFrame, BaseWindow::DisableLayoutSave,
               BaseWindow::BoundsCheckOnShow},
              parent)
        , buttons_(buttons)
        , addName_(addName)
    {
        themeChangedEvent();
    }

protected:
    void themeChangedEvent() override
    {
        BasePopup::themeChangedEvent();
        setPalette(getTheme()->palette);
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (!addName_->hasFocus() && BasePopup::handleEnter(event, buttons_))
            return;
        BasePopup::keyPressEvent(event);
    }

private:
    QDialogButtonBox *buttons_;
    QLineEdit *addName_;
};
}  // namespace

struct PriorityChat::Inbox {
    std::mutex mutex;
    std::deque<MessagePtr> pending;
    bool active = false;
    bool dirty = false;
    void push(const MessagePtr &message, bool changed = false)
    {
        std::lock_guard lock(mutex);
        if (!active)
            return;
        if (pending.size() >= 512)
            pending.pop_front();
        pending.push_back(message);
        dirty |= changed;
    }
};

PriorityChat::PriorityChat(Split *split, QWidget *parent, ChannelView *feedView)
    : BaseWidget(parent)
    , split_(split)
    , inbox_(std::make_shared<Inbox>())
{
    setObjectName("PriorityChat");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);
    // Tabs sit on a dark, bordered page.
    tabs_ = new SettingsTabs(this);
    view_ = feedView
                ? feedView
                : new ChannelView(this, split_,
                                  ChannelView::Context::PriorityChat, 1000);
    view_->setPausable(true);
    view_->setMessagePredicate(
        [this](const MessagePtr &message) {
            return accepts(message);
        },
        true);
    if (!feedView)
        view_->hide();

    auto *rulesPage = new QWidget(this);
    auto *rulesLayout = new QVBoxLayout(rulesPage);
    rulesLayout->setContentsMargins(10, 10, 10, 10);
    rulesLayout->setSpacing(10);
    rulesLayout->addWidget(
        label("Priority Chat shows messages from users who match a rule "
              "you turn on below.",
              rulesPage));

    auto *platformGrid = new QGridLayout;
    platformGrid->setSpacing(10);
    platformGrid->setColumnStretch(0, 1);
    platformGrid->setColumnStretch(1, 1);
    const QStringList platformIds = {"Twitch", "Kick", "YouTube"};
    const QStringList descriptions = {
        "Twitch: reported total subscription months. Follow age requires your "
        "broadcaster/moderator permission; anonymous mode stays anonymous.",
        "Kick: reported subscription months and follow date. Website lookups "
        "may be unavailable; missing data stays unknown.",
        "YouTube: membership badges establish a minimum age when their label "
        "includes a duration. Generic badges cannot prove tenure."};
    // Lines a sub-option up with its parent checkbox's text.
    const int subIndent =
        style()->pixelMetric(QStyle::PM_IndicatorWidth) +
        style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing);
    const auto botControls = [this](int i, const QString &id, QWidget *parent) {
        auto &ui = ruleControls_[i];
        ui.bots = new QCheckBox("Bots", parent);
        ui.bots->setObjectName("PriorityChat" + id + "Bots");
        ui.bots->setToolTip(
            "Off: recognized bots stay out unless manually included. "
            "On: include recognized bots. "
            "Explicitly excluded or blocked users stay excluded.");
        ui.botAccounts = new QLineEdit(parent);
        ui.botAccounts->setObjectName("PriorityChat" + id + "BotAccounts");
        ui.botAccounts->setMaxLength(4096);
        ui.botAccounts->setPlaceholderText(
            i == 2 ? "Custom bot channel IDs (UC...), comma-separated"
                   : "Custom bot usernames, comma-separated");
        ui.botAccounts->setToolTip(
            i == 2 ? "Additional bot channel IDs, separated by commas. YouTube "
                     "display names are not unique. Built-in accounts: "
                     "StreamElements, Streamlabs and Nightbot."
            : i == 3 ? "Enter the exact TikTok handles used by your bots, "
                       "separated by commas. TikTok bots often use your own "
                       "account. These accounts stay out while Bots is off, "
                       "unless manually included."
                     : "Additional bot usernames, separated by commas. Bot "
                       "badges and known bot accounts are detected "
                       "automatically. These accounts stay out while Bots "
                       "is off, unless manually included.");
    };
    for (int i = 0; i < 3; ++i)
    {
        auto &ui = ruleControls_[i];
        const auto &id = platformIds[i];
        const bool youtube = i == 2;
        auto *group = new QGroupBox(id, rulesPage);
        group->setObjectName("PriorityChat" + id + "Rules");
        group->setToolTip(descriptions[i]);
        auto *grid = new QGridLayout(group);
        grid->setHorizontalSpacing(10);
        grid->setVerticalSpacing(3);
        grid->setColumnStretch(0, 1);
        ui.moderators = new QCheckBox("Moderators", group);
        ui.moderators->setObjectName("PriorityChat" + id + "Moderators");
        ui.moderators->setToolTip("Include messages from moderators.");
        auto *rolesRow = new QHBoxLayout;
        rolesRow->setSpacing(16);
        rolesRow->addWidget(ui.moderators);
        if (!youtube)
        {
            ui.vips = new QCheckBox("VIPs", group);
            ui.vips->setObjectName("PriorityChat" + id + "Vips");
            ui.vips->setToolTip("Include messages from VIPs.");
            rolesRow->addWidget(ui.vips);
        }
        botControls(i, id, group);
        rolesRow->addWidget(ui.bots);
        rolesRow->addStretch(1);
        grid->addLayout(rolesRow, 0, 0, 1, 2);
        ui.subscriptions = new QCheckBox(
            youtube ? "Member for at least" : "Subscribed for at least", group);
        ui.subscriptions->setObjectName("PriorityChat" + id + "Subscriptions");
        ui.subscriptions->setToolTip(
            "Include users who have been subscribed or a member for at least "
            "this long. Users without known membership age don't qualify.");
        ui.subMonths = minimumMonths(group);
        ui.subMonths->setObjectName("PriorityChat" + id + "SubMonths");
        grid->addWidget(ui.subscriptions, 1, 0);
        grid->addWidget(ui.subMonths, 1, 1);
        ui.activeOnly = new QCheckBox(
            youtube ? "Must still be a member" : "Must still be subscribed",
            group);
        ui.activeOnly->setObjectName("PriorityChat" + id + "ActiveOnly");
        ui.activeOnly->setToolTip(
            "Skip users whose subscription or membership has ended.");
        auto *activeRow = new QHBoxLayout;
        activeRow->setContentsMargins(subIndent, 0, 0, 0);
        activeRow->addWidget(ui.activeOnly);
        grid->addLayout(activeRow, 2, 0, 1, 2);
        if (!youtube)
        {
            ui.follows = new QCheckBox("Followed for at least", group);
            ui.follows->setObjectName("PriorityChat" + id + "Follows");
            ui.follows->setToolTip(
                descriptions[i] +
                " Include users who have followed for at least this long. "
                "Unavailable follow dates don't qualify.");
            ui.followMonths = minimumMonths(group);
            ui.followMonths->setObjectName("PriorityChat" + id +
                                           "FollowMonths");
            grid->addWidget(ui.follows, 3, 0);
            grid->addWidget(ui.followMonths, 3, 1);
            ui.matching = new QCheckBox("Must match both", group);
            ui.matching->setObjectName("PriorityChat" + id + "Matching");
            ui.matching->setToolTip(
                "Off: matching either rule is enough. On: users must match "
                "both. Only used when both rules are turned on.");
            grid->addWidget(ui.matching, 4, 0, 1, 2);
        }
        grid->addWidget(ui.botAccounts, 5, 0, 1, 2);
        grid->setRowStretch(grid->rowCount(), 1);
        platformGrid->addWidget(group, i / 2, i % 2);
    }
    auto *tiktokGroup = new QGroupBox("TikTok", rulesPage);
    tiktokGroup->setObjectName("PriorityChatTikTokRules");
    tiktokGroup->setToolTip(
        "Automatic membership and follow age are not verified for TikTok. "
        "Manually included users appear in the same Priority Chat feed.");
    auto *tiktokLayout = new QVBoxLayout(tiktokGroup);
    botControls(3, "TikTok", tiktokGroup);
    tiktokLayout->addWidget(ruleControls_[3].bots);
    tiktokLayout->addWidget(ruleControls_[3].botAccounts);
    tiktokLayout->addWidget(
        label("TikTok doesn't share subscription or follow age. Add TikTok "
              "users on the Users tab.",
              tiktokGroup));
    tiktokLayout->addStretch(1);
    platformGrid->addWidget(tiktokGroup, 1, 1);
    rulesLayout->addLayout(platformGrid);
    rulesLayout->addStretch(1);
    tabs_->addTab(rulesPage, "Rules");

    auto *usersPage = new FittingPage(this);
    auto *usersLayout = new QVBoxLayout(usersPage);
    usersLayout->setContentsMargins(10, 10, 10, 10);
    usersLayout->setSpacing(10);
    usersLayout->addWidget(
        label("Manually add users to manage them in Priority Chat.", usersPage));
    auto *well = new UsersWell(usersPage);
    auto *wellLayout = new QVBoxLayout(well);
    wellLayout->setContentsMargins(1, 1, 1, 1);
    users_ = new QListWidget(well);
    users_->setObjectName("PriorityChatUsers");
    users_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    users_->setSelectionMode(QAbstractItemView::NoSelection);
    users_->setFocusPolicy(Qt::NoFocus);
    users_->setFrameShape(QFrame::NoFrame);
    users_->setStyleSheet(QStringLiteral(
        "QToolButton { background: transparent; border: none; padding: 0; }"));
    users_->viewport()->setAutoFillBackground(false);
    users_->setAutoFillBackground(false);
    wellLayout->addWidget(users_);
    usersLayout->addWidget(well, 1);
    auto *addGroup = new QGroupBox("Add a user", usersPage);
    auto *addLayout = new QVBoxLayout(addGroup);
    auto *addRow = new QHBoxLayout;
    auto *entry = new UserEntryWell(addGroup);
    entry->setObjectName("PriorityChatUserEntry");
    auto *entryLayout = new QHBoxLayout(entry);
    entryLayout->setContentsMargins(0, 1, 6, 1);
    entryLayout->setSpacing(0);
    addPlatform_ = new PlatformPicker(entry);
    addPlatform_->setObjectName("PriorityChatAddPlatform");
    platforms(addPlatform_);
    addName_ = new QLineEdit(entry);
    addName_->setFrame(false);
    QPalette editorPalette;
    editorPalette.setColor(QPalette::Base, Qt::transparent);
    addName_->setPalette(editorPalette);
    addName_->setObjectName("PriorityChatAddName");
    addName_->setClearButtonEnabled(true);
    nameSuggestions_ = new QStringListModel(this);
    auto *completer = new QCompleter(nameSuggestions_, addName_);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    addName_->setCompleter(completer);
    add_ = new QPushButton("Add", addGroup);
    add_->setObjectName("PriorityChatAddUser");
    add_->setEnabled(false);
    entryLayout->addWidget(addPlatform_);
    entryLayout->addWidget(addName_, 1);
    addRow->addWidget(entry, 1);
    addRow->addWidget(add_);
    addLayout->addLayout(addRow);
    addHint_ = label({}, addGroup);
    addHint_->setObjectName("PriorityChatAddHint");
    addLayout->addWidget(addHint_);
    usersLayout->addWidget(addGroup);
    tabs_->addTab(usersPage, "Users");
    resetAddHint();
    layout->addWidget(tabs_, 1);
    buttons_ = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons_->setContentsMargins({});
    layout->addWidget(buttons_);
    QObject::connect(buttons_, &QDialogButtonBox::accepted, this, [this] {
        acceptSettings();
    });
    QObject::connect(buttons_, &QDialogButtonBox::rejected, this, [this] {
        if (settingsPopup_)
            settingsPopup_->close();
    });

    QObject::connect(addPlatform_, &QComboBox::currentIndexChanged, this,
                     [this] {
                         resetAddHint();
                         updateNameSuggestions();
                     });
    QObject::connect(tabs_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 1)
        {
            refreshPeople();
            updateNameSuggestions();
        }
    });
    QObject::connect(addName_, &QLineEdit::textChanged, this, [this] {
        setAddBusy(addBusy_);
    });
    QObject::connect(add_, &QPushButton::clicked, this, [this] {
        addPerson(Choice::Include);
    });
    QObject::connect(addName_, &QLineEdit::returnPressed, this, [this] {
        addPerson(Choice::Include);
    });
    QObject::connect(users_, &QListWidget::itemDoubleClicked, this,
                     [this](QListWidgetItem *item) {
                         showUserCard(item->data(Qt::UserRole).toString());
                     });
    for (const auto &ui : ruleControls_)
    {
        for (auto *box : {ui.bots, ui.moderators, ui.vips, ui.subscriptions,
                          ui.activeOnly, ui.follows, ui.matching})
            if (box)
                QObject::connect(box, &QCheckBox::toggled, this, [this] {
                    saveRules();
                });
        for (auto *spin : {ui.subMonths, ui.followMonths})
            if (spin)
                QObject::connect(spin, &QSpinBox::valueChanged, this, [this] {
                    saveRules();
                });
    }
    for (const auto &ui : ruleControls_)
        QObject::connect(ui.botAccounts, &QLineEdit::editingFinished, this,
                         [this] {
                             saveRules();
                         });
    timer_.setInterval(200);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] {
        flush();
    });
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(300);
    QObject::connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        refreshFeed();
    });
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(2000);
    QObject::connect(&saveTimer_, &QTimer::timeout, this, [this] {
        savePreferences();
    });
    getSettings()->priorityChatPreferences.connect(
        [this] {
            if (!loading_)
                loadPreferences();
        },
        connections_);
    accountConnection_ =
        getApp()->getAccounts()->twitch.currentUserChanged.connect([this] {
            accountsChanged();
        });
    scaleChangedEvent(scale());
}

PriorityChat::~PriorityChat()
{
    if (saveTimer_.isActive())
        savePreferences();
    if (view_)
        view_->setMessagePredicate({});
    channelConnections_.clear();
    connections_.clear();
    accountConnection_.disconnect();
    std::lock_guard lock(inbox_->mutex);
    inbox_->active = false;
}
QString PriorityChat::title() const
{
    return model_.title;
}
int PriorityChat::preferredWidth() const
{
    return model_.width;
}
void PriorityChat::setPreferredWidth(int width)
{
    model_.width = std::clamp(width, 240, 800);
    savePreferences();
}
priority::Model &PriorityChat::settingsModel()
{
    return settingsDraft_ ? *settingsDraft_ : model_;
}

void PriorityChat::acceptSettings()
{
    if (!settingsPopup_ || !settingsDraft_)
        return;
    saveRules();
    for (size_t i = 0; i < model_.rules.size(); ++i)
        if (settingsDraft_->rules[i].toJson() !=
            settingsOriginal_.rules[i].toJson())
            model_.rules[i] = settingsDraft_->rules[i];
    // Apply edited choices without overwriting changes made in another chat
    // while this settings window was open. Live observations remain intact.
    for (auto it = settingsDraft_->manual.cbegin();
         it != settingsDraft_->manual.cend(); ++it)
        if (it->choice != settingsOriginal_.choice(it.key()))
            model_.manual.insert(it.key(), it.value());
    for (auto it = settingsOriginal_.manual.cbegin();
         it != settingsOriginal_.manual.cend(); ++it)
        if (!settingsDraft_->manual.contains(it.key()))
            model_.manual.remove(it.key());
    settingsDraft_.reset();
    titleChanged.invoke();
    savePreferences();
    queueRefresh();
    settingsPopup_->close();
}

void PriorityChat::showRules()
{
    tabs_->setCurrentIndex(0);
    showSettings();
}

void PriorityChat::showSettings()
{
    if (!settingsPopup_)
    {
        ++settingsGeneration_;
        settingsOriginal_ = model_;
        settingsOriginal_.people.clear();
        settingsDraft_ = settingsOriginal_;
        addName_->clear();
        setAddBusy(false);
        resetAddHint();
        loadRules();
        refreshPeople();
        updateNameSuggestions();
        auto *popup = new PrioritySettingsPopup(buttons_, addName_, split_);
        popup->setAttribute(Qt::WA_DeleteOnClose);
        popup->setWindowTitle(title() + " settings");
        settingsPopup_ = popup;
        popup->installEventFilter(this);
        setParent(popup);
        auto *layout = new QVBoxLayout(popup->getLayoutContainer());
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(this);
        show();
        popup->adjustSize();
        popup->show();
        // Dropping the native frame on first show enlarges the client area;
        // fit the window to its contents again.
        popup->adjustSize();
    }
    show();
    settingsPopup_->show();
    settingsPopup_->raise();
}

void PriorityChat::setChannel(ChannelPtr channel)
{
    if (channel_ == channel)
        return;
    if (settingsPopup_)
        settingsPopup_->close();
    if (saveTimer_.isActive())
    {
        saveTimer_.stop();
        savePreferences();
    }
    ++generation_;
    inFlight_ = 0;
    nextLookup_ = 0;
    followPermissionDenied_ = false;
    followStatus_.clear();
    channelConnections_.clear();
    {
        std::lock_guard lock(inbox_->mutex);
        inbox_->active = false;
        inbox_->pending.clear();
        inbox_->dirty = false;
    }
    channel_ = std::move(channel);
    selectedKey_.clear();
    scope_.clear();
    savedScope_.clear();
    model_ = Model();
    retainedPeople_.clear();
    if (channel_ && !channel_->isEmpty())
    {
        const auto signature = Split::activityChannelSignature(channel_);
        scope_ = QString::fromLatin1(
            QCryptographicHash::hash(
                QJsonDocument(signature).toJson(QJsonDocument::Compact),
                QCryptographicHash::Sha256)
                .toHex());
        loadPreferences();
    }
    loadRules();
    if (monitoring_)
        connectChannel();
    else
        view_->setChannel(Channel::getEmpty());
    refreshPeople();
}
void PriorityChat::setMonitoring(bool enabled)
{
    if (!enabled && settingsPopup_)
        settingsPopup_->close();
    if (monitoring_ == enabled)
        return;
    monitoring_ = enabled;
    if (enabled)
    {
        followPermissionDenied_ = false;
        followStatus_.clear();
        connectChannel();
        timer_.start();
    }
    else
    {
        ++generation_;
        inFlight_ = 0;
        channelConnections_.clear();
        retainedPeople_.clear();
        timer_.stop();
        refreshTimer_.stop();
        {
            std::lock_guard lock(inbox_->mutex);
            inbox_->active = false;
            inbox_->pending.clear();
        }
        if (view_)
            view_->setChannel(Channel::getEmpty());
        if (saveTimer_.isActive())
        {
            saveTimer_.stop();
            savePreferences();
        }
    }
}
void PriorityChat::connectChannel()
{
    channelConnections_.clear();
    if (!monitoring_ || !channel_ || channel_->isEmpty())
        return;
    {
        std::lock_guard lock(inbox_->mutex);
        inbox_->active = true;
    }
    const auto inbox = inbox_;
    channelConnections_.managedConnect(
        channel_->messageAppended,
        [inbox](MessagePtr &message, std::optional<MessageFlags>) {
            inbox->push(message);
        });
    channelConnections_.managedConnect(
        channel_->messageReplaced,
        [inbox](size_t, const MessagePtr &, const MessagePtr &message) {
            inbox->push(message, true);
        });
    auto *source = channel_.get();
    channelConnections_.managedConnect(
        Channel::messageFlagsChanged,
        [inbox, source](Channel *channel, const MessagePtr &message) {
            if (channel == source)
                inbox->push(message, true);
        });
    channelConnections_.managedConnect(
        Channel::messageRemoved,
        [inbox, source](Channel *channel, size_t, const MessagePtr &message) {
            if (channel == source)
                inbox->push(message, true);
        });
    channelConnections_.managedConnect(
        channel_->messagesAddedAtStart,
        [inbox](std::vector<MessagePtr> &messages) {
            for (const auto &m : messages)
                inbox->push(m);
        });
    channelConnections_.managedConnect(
        channel_->filledInMessages,
        [inbox](const std::vector<MessagePtr> &messages) {
            for (const auto &m : messages)
                inbox->push(m);
        });
    for (const auto &message : channel_->getMessageSnapshot(1000))
        model_.observe(*message);
    refreshFilters();
    view_->setChannel(channel_);
    refreshFeed();
}
void PriorityChat::refreshFilters()
{
    if (split_)
        view_->setFilters(split_->getFilters());
    queueRefresh();
}
void PriorityChat::queueRefresh()
{
    if (monitoring_ && !refreshTimer_.isActive())
        refreshTimer_.start();
}
void PriorityChat::flush()
{
    std::deque<MessagePtr> messages;
    bool dirty = false;
    {
        std::lock_guard lock(inbox_->mutex);
        messages.swap(inbox_->pending);
        dirty = inbox_->dirty;
        inbox_->dirty = false;
    }
    if (!messages.empty())
        rememberRetainedPeople();
    bool historyChanged = false;
    for (const auto &message : messages)
    {
        const auto key =
            personKey(message->platform, message->userID, message->loginName);
        const auto before = model_.people.value(key).months;
        const bool matched =
            model_.matches(*message, QDateTime::currentDateTimeUtc());
        model_.observe(*message);
        dirty |= matched !=
                 model_.matches(*message, QDateTime::currentDateTimeUtc());
        historyChanged |= model_.people.value(key).months != before;
    }
    if (dirty)
        queueRefresh();
    if (historyChanged && !saveTimer_.isActive())
        saveTimer_.start();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (now >= nextAgeRefresh_)
    {
        nextAgeRefresh_ = now + 60000;
        if (std::ranges::any_of(model_.rules, [](const auto &r) {
                return r.configured();
            }))
            queueRefresh();
    }
    lookUpNext();
}
bool PriorityChat::accepts(const MessagePtr &message) const
{
    if (!message || !isChatMessage(*message))
        return false;
    const auto now = QDateTime::currentDateTimeUtc();
    const auto key =
        personKey(message->platform, message->userID, message->loginName);
    const auto retained = retainedPeople_.constFind(key);
    if (model_.choice(key) != Choice::Include &&
        retained != retainedPeople_.cend() && model_.isBot(*retained) &&
        !model_.rules[static_cast<int>(message->platform)].bots)
        return false;
    bool eligible = model_.matches(*message, now);
    if (!eligible && !model_.isBot(*message))
    {
        if (!model_.people.contains(key))
        {
            eligible = retained != retainedPeople_.cend() &&
                       model_.qualifies(*retained, now);
        }
    }
    if (!eligible)
        return false;
    if (message->platform == MessagePlatform::Kick &&
        isKickUserLocallyBlocked(message->loginName))
        return false;
    const bool twitch = message->platform == MessagePlatform::AnyOrTwitch;
    return !isIgnoredMessage(
        {message->messageText, twitch ? message->userID : QString(),
         twitch ? message->loginName : QString(), false, false});
}
void PriorityChat::refreshFeed()
{
    if (!monitoring_)
        return;
    rememberRetainedPeople();
    view_->refreshMessagePredicate();
    rememberRetainedPeople();
}

void PriorityChat::rememberRetainedPeople()
{
    QHash<QString, Person> retained;
    const auto snapshot = view_->channel()->getMessageSnapshot();
    retained.reserve(static_cast<qsizetype>(snapshot.size()));
    for (const auto &message : snapshot)
    {
        const auto key =
            personKey(message->platform, message->userID, message->loginName);
        if (const auto current = model_.people.constFind(key);
            current != model_.people.cend())
            retained.insert(key, *current);
        else if (const auto previous = retainedPeople_.constFind(key);
                 previous != retainedPeople_.cend())
            retained.insert(key, *previous);
    }
    // At most one record per user represented in the bounded Priority buffer.
    retainedPeople_ = std::move(retained);
}
void PriorityChat::loadPreferences()
{
    if (scope_.isEmpty())
        return;
    const auto store =
        QJsonDocument::fromJson(
            getSettings()->priorityChatPreferences.getValue().toUtf8())
            .object();
    const auto data = store[scope_].toObject();
    const auto encoded = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (encoded == savedScope_)
        return;
    savedScope_ = encoded;
    // Keep live observations when another copy of this channel changes rules.
    auto observed = std::move(model_.people);
    const auto previousManual = model_.manual;
    model_.load(data);
    for (auto it = observed.cbegin(); it != observed.cend(); ++it)
        if (model_.people.contains(it.key()) ||
            model_.people.size() < Model::MAX_PEOPLE)
            model_.people.insert(it.key(), it.value());
    if (settingsDraft_)
    {
        // A user right-clicked in chat shows up in the open settings window.
        // The original changes too, so OK neither re-applies nor misses it.
        for (auto it = model_.manual.cbegin(); it != model_.manual.cend(); ++it)
            if (previousManual.value(it.key()).choice != it->choice)
            {
                settingsOriginal_.manual.insert(it.key(), it.value());
                settingsDraft_->manual.insert(it.key(), it.value());
            }
        for (auto it = previousManual.cbegin(); it != previousManual.cend();
             ++it)
            if (!model_.manual.contains(it.key()))
            {
                settingsOriginal_.manual.remove(it.key());
                settingsDraft_->manual.remove(it.key());
            }
    }
    titleChanged.invoke();
    if (!settingsDraft_)
        loadRules();
    queueRefresh();
    refreshPeople();
}
void PriorityChat::savePreferences()
{
    if (scope_.isEmpty())
        return;
    auto store = QJsonDocument::fromJson(
                     getSettings()->priorityChatPreferences.getValue().toUtf8())
                     .object();
    const auto data = model_.toJson();
    savedScope_ = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (store[scope_].toObject() == data)
        return;
    store.insert(scope_, data);
    loading_ = true;
    getSettings()->priorityChatPreferences =
        QString::fromUtf8(QJsonDocument(store).toJson(QJsonDocument::Compact));
    loading_ = false;
    getSettings()->requestSave();
}
void PriorityChat::loadRules()
{
    loading_ = true;
    for (size_t i = 0; i < ruleControls_.size(); ++i)
    {
        const auto &rule = settingsModel().rules[i];
        const auto &ui = ruleControls_[i];
        ui.bots->setChecked(rule.bots);
        ui.botAccounts->setText(rule.botAccounts.join(", "));
        if (!ui.moderators)
            continue;
        ui.moderators->setChecked(rule.moderators);
        if (ui.vips)
            ui.vips->setChecked(rule.vips);
        ui.subscriptions->setChecked(rule.subscriptions);
        ui.subMonths->setValue(rule.subscriptionMonths);
        ui.subMonths->setEnabled(rule.subscriptions);
        ui.activeOnly->setChecked(rule.activeOnly);
        ui.activeOnly->setEnabled(rule.subscriptions);
        if (ui.follows)
        {
            ui.follows->setChecked(rule.follows);
            ui.followMonths->setValue(rule.followMonths);
            ui.followMonths->setEnabled(rule.follows);
        }
        if (ui.matching)
        {
            ui.matching->setChecked(rule.matchAll);
            // Either/both only means something once both rules are on.
            ui.matching->setEnabled(rule.subscriptions && rule.follows);
        }
    }
    loading_ = false;
}
void PriorityChat::saveRules()
{
    if (loading_ || !settingsDraft_)
        return;
    for (size_t i = 0; i < ruleControls_.size(); ++i)
    {
        auto &rule = settingsDraft_->rules[i];
        const auto &ui = ruleControls_[i];
        rule.bots = ui.bots->isChecked();
        rule.botAccounts = parseBotAccounts(ui.botAccounts->text());
        if (!ui.moderators)
            continue;
        rule.moderators = ui.moderators->isChecked();
        if (ui.vips)
            rule.vips = ui.vips->isChecked();
        rule.subscriptions = ui.subscriptions->isChecked();
        rule.subscriptionMonths = ui.subMonths->value();
        rule.activeOnly = ui.activeOnly->isChecked();
        if (ui.follows)
        {
            rule.follows = ui.follows->isChecked();
            rule.followMonths = ui.followMonths->value();
        }
        if (ui.matching)
            rule.matchAll = ui.matching->isChecked();
    }
    loadRules();
}
void PriorityChat::setUserChoice(const MessagePtr &message, Choice value)
{
    if (!message || !isChatMessage(*message))
        return;
    model_.observe(*message);
    const auto key =
        personKey(message->platform, message->userID, message->loginName);
    auto person = model_.people.value(key);
    person.platform = message->platform;
    person.id = message->userID;
    person.login = message->loginName;
    person.name = message->displayName;
    model_.setChoice(person, value);
    // Show it in an open settings window too. Updating the original as well
    // means OK neither re-applies it nor misses a later removal.
    if (settingsDraft_)
    {
        settingsOriginal_.setChoice(person, value);
        settingsDraft_->setChoice(person, value);
    }
    savePreferences();
    queueRefresh();
    refreshPeople();
}
Choice PriorityChat::userChoice(const Message &message) const
{
    return model_.choice(
        personKey(message.platform, message.userID, message.loginName));
}

bool PriorityChat::includesUser(const Message &message) const
{
    if (!isChatMessage(message))
        return false;
    const auto key =
        personKey(message.platform, message.userID, message.loginName);
    const auto choice = model_.choice(key);
    if (choice != Choice::Automatic)
        return choice == Choice::Include;

    // Include evidence on the clicked message, even before the pane has
    // processed it. Keep the observation out of the live model.
    Model eligibility;
    eligibility.rules = model_.rules;
    eligibility.people.insert(
        key, model_.people.value(key, retainedPeople_.value(key)));
    eligibility.observe(message);
    if (eligibility.matches(message, QDateTime::currentDateTimeUtc()))
        return true;

    return std::ranges::any_of(
        view_->channel()->getMessageSnapshot(), [&key](const auto &visible) {
            return personKey(visible->platform, visible->userID,
                             visible->loginName) == key;
        });
}

void PriorityChat::refreshPeople()
{
    if (!users_)
        return;
    const int scroll = users_->verticalScrollBar()->value();
    bool scrolled = false;
    users_->clear();
    QList<QPair<QString, ManualPerson>> entries;
    const auto &manual = settingsModel().manual;
    for (auto it = manual.cbegin(); it != manual.cend(); ++it)
        entries.append({it.key(), it.value()});
    const auto display = [](const ManualPerson &p) {
        return p.name.isEmpty() ? p.login : p.name;
    };
    std::sort(entries.begin(), entries.end(), [&](const auto &a, const auto &b) {
        return display(a.second).compare(display(b.second),
                                         Qt::CaseInsensitive) < 0;
    });
    const bool light = getTheme()->isLightTheme();
    QIcon visibilityIcons;
    visibilityIcons.addFile(light ? QStringLiteral(":/buttons/eyeDark.svg")
                                  : QStringLiteral(":/buttons/eye.svg"),
                            {}, QIcon::Normal, QIcon::On);
    visibilityIcons.addFile(light ? QStringLiteral(":/buttons/eyeClosedDark.svg")
                                  : QStringLiteral(":/buttons/eyeClosed.svg"),
                            {}, QIcon::Normal, QIcon::Off);
    const auto s = scale();
    const auto px = [s](int value) {
        return qRound(value * s);
    };
    const int rowHeight =
        std::max(QFontMetrics(font()).height() + px(8), px(26));
    bool firstRow = true;
    for (const auto &[key, p] : entries)
    {
        const auto name = display(p);
        auto *item = new QListWidgetItem(users_);
        item->setData(Qt::UserRole, key);
        item->setSizeHint(QSize(0, rowHeight));
        auto *row = new UserRow(!firstRow);
        firstRow = false;
        row->setToolTip(platformName(p.platform) +
                        " · double-click to open their user card");
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(px(6), 0, px(4), 0);
        rowLayout->setSpacing(px(8));
        auto *icon = new QLabel(row);
        icon->setPixmap(
            platformIcon(p.platform).pixmap(QSize(px(16), px(16))));
        icon->setFixedSize(px(16), px(16));
        auto *nameLabel = new QLabel(name, row);
        nameLabel->setObjectName("PriorityChatUserName");
        nameLabel->setTextFormat(Qt::PlainText);
        nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        rowLayout->addWidget(icon);
        rowLayout->addWidget(nameLabel, 1);

        auto *visibility = new QToolButton(row);
        visibility->setObjectName("PriorityChatUserVisibility");
        visibility->setAutoRaise(true);
        visibility->setCursor(Qt::PointingHandCursor);
        visibility->setFixedSize(px(20), px(20));
        visibility->setIconSize(QSize(px(16), px(16)));
        visibility->setIcon(visibilityIcons);
        visibility->setAccessibleName("Show " + name + " in Priority Chat");
        visibility->setCheckable(true);
        visibility->setChecked(p.choice == Choice::Include);
        const auto updateVisibilityTooltip = [visibility](bool active) {
            visibility->setToolTip(
                active ? "Active in Priority Chat. Click to deactivate."
                       : "Excluded from Priority Chat. Click to activate.");
        };
        updateVisibilityTooltip(visibility->isChecked());
        QObject::connect(visibility, &QToolButton::toggled, this,
                         [this, key, updateVisibilityTooltip](bool active) {
                             if (!settingsDraft_)
                                 return;
                             auto person = settingsDraft_->manual.find(key);
                             if (person == settingsDraft_->manual.end())
                                 return;
                             person->choice = active ? Choice::Include
                                                     : Choice::Exclude;
                             updateVisibilityTooltip(active);
                         });
        rowLayout->addWidget(visibility);

        auto *remove = new RemoveUserButton(row);
        remove->setObjectName("PriorityChatRemoveUser");
        remove->setAutoRaise(true);
        remove->setCursor(Qt::PointingHandCursor);
        remove->setFixedSize(px(20), px(20));
        remove->setToolTip("Remove " + name);
        remove->setAccessibleName("Remove " + name);
        // The row is rebuilt on removal, so leave the click handler first.
        QObject::connect(remove, &QToolButton::clicked, this, [this, key] {
            QTimer::singleShot(0, this, [this, key] {
                removeUser(key);
            });
        });
        rowLayout->addWidget(remove);
        users_->setItemWidget(item, row);
        if (key == selectedKey_)
        {
            users_->scrollToItem(item);
            scrolled = true;
        }
    }
    if (entries.isEmpty())
    {
        auto *empty = new QListWidgetItem("No users added yet", users_);
        empty->setFlags(Qt::NoItemFlags);
        empty->setSizeHint(QSize(0, rowHeight));
    }
    if (!scrolled)
        users_->verticalScrollBar()->setValue(scroll);
    selectedKey_.clear();
}
void PriorityChat::removeUser(const QString &key)
{
    if (!settingsDraft_)
        return;
    const auto it = settingsDraft_->manual.constFind(key);
    if (it == settingsDraft_->manual.cend())
        return;
    const auto name = it->name.isEmpty() ? it->login : it->name;
    settingsDraft_->manual.remove(key);
    refreshPeople();
    addHint_->setText("Removed " + name + ". Click Save to apply.");
}
void PriorityChat::showUserCard(const QString &key)
{
    if (!split_ || key.isEmpty())
        return;
    if (auto it = model_.people.constFind(key); it != model_.people.cend())
        split_->getChannelView().showUserInfoPopup(it->login, it->platform, {},
                                                   it->id);
    else if (auto m = settingsModel().manual.constFind(key);
             m != settingsModel().manual.cend())
        split_->getChannelView().showUserInfoPopup(m->login, m->platform, {},
                                                   m->id);
}
void PriorityChat::updateNameSuggestions()
{
    // Offer names of users seen in this chat on the chosen platform.
    const auto platform =
        static_cast<MessagePlatform>(addPlatform_->currentIndex());
    QStringList names;
    for (const auto &p : model_.people)
        if (p.platform == platform)
            names << (p.name.isEmpty() ? p.login : p.name);
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    nameSuggestions_->setStringList(names);
}
void PriorityChat::setAddBusy(bool busy)
{
    addBusy_ = busy;
    const bool canAdd = !busy && !addName_->text().trimmed().isEmpty();
    add_->setEnabled(canAdd);
    buttons_->button(QDialogButtonBox::Save)->setEnabled(!busy);
}
void PriorityChat::resetAddHint()
{
    switch (static_cast<MessagePlatform>(addPlatform_->currentIndex()))
    {
        case MessagePlatform::Kick:
            addName_->setPlaceholderText("Kick username");
            break;
        case MessagePlatform::YouTube:
            addName_->setPlaceholderText("Name from chat, or channel ID");
            break;
        case MessagePlatform::TikTok:
            addName_->setPlaceholderText("TikTok username");
            break;
        default:
            addName_->setPlaceholderText("Twitch username");
            break;
    }
    addHint_->setText(
        "Or right-click a message in chat to add or exclude the user.");
}
QString PriorityChat::sourceName(MessagePlatform platform) const
{
    if (!channel_)
        return {};
    if (auto *merged = dynamic_cast<MergedChannel *>(channel_.get()))
    {
        if (platform == MessagePlatform::Kick && merged->kickChannel())
            return merged->kickChannel()->getName();
        if (platform == MessagePlatform::AnyOrTwitch && merged->twitchChannel())
            return merged->twitchChannel()->getName();
        return {};
    }
    if ((platform == MessagePlatform::Kick && channel_->isKickChannel()) ||
        (platform == MessagePlatform::AnyOrTwitch &&
         channel_->isTwitchChannel()))
        return channel_->getName();
    return {};
}
void PriorityChat::addPerson(Choice choice)
{
    auto input = addName_->text().trimmed();
    if (input.startsWith('@'))
        input.remove(0, 1);
    if (input.isEmpty() || addBusy_ || !settingsDraft_)
        return;
    const auto platform =
        static_cast<MessagePlatform>(addPlatform_->currentIndex());
    const auto same = [&](const QString &id, const QString &login,
                          const QString &name) {
        return (!id.isEmpty() && id == input) ||
               login.compare(input, Qt::CaseInsensitive) == 0 ||
               name.compare(input, Qt::CaseInsensitive) == 0;
    };
    // Someone already listed or seen in this chat needs no lookup.
    const auto &manual = settingsDraft_->manual;
    for (auto it = manual.cbegin(); it != manual.cend(); ++it)
        if (it->platform == platform && same(it->id, it->login, it->name))
        {
            Person person;
            person.platform = it->platform;
            person.id = it->id;
            person.login = it->login;
            person.name = it->name;
            addChosenPerson(person, choice);
            return;
        }
    for (const auto &p : model_.people)
        if (p.platform == platform && same(p.id, p.login, p.name))
        {
            addChosenPerson(p, choice);
            return;
        }
    const auto generation = generation_;
    const auto settingsGeneration = settingsGeneration_;
    QPointer<PriorityChat> guard(this);
    auto add = [guard, generation, settingsGeneration, platform, input,
                choice](QString id, QString login) {
        runInGuiThread([guard, generation, settingsGeneration, platform, input,
                        choice, id = std::move(id), login = std::move(login)] {
            if (!guard)
                return;
            Person person;
            person.platform = platform;
            person.id = id;
            person.login = login;
            person.name = input;
            guard->addResolvedPerson(generation, settingsGeneration, person,
                                     choice);
        });
    };
    if (platform == MessagePlatform::AnyOrTwitch)
    {
        if (getApp()->getAccounts()->twitch.getCurrent()->isAnon())
        {
            addHint_->setText(input + " hasn't chatted here. Log in to Twitch "
                                      "to add users by name.");
            return;
        }
        addHint_->setText("Looking up " + input + "…");
        setAddBusy(true);
        getHelix()->getUserByName(
            input,
            [add](const HelixUser &user) {
                add(user.id, user.login);
            },
            [add] {
                add({}, {});
            });
    }
    else if (platform == MessagePlatform::Kick)
    {
        const auto source = sourceName(platform);
        if (source.isEmpty())
        {
            addHint_->setText("This chat has no Kick channel.");
            return;
        }
        addHint_->setText("Looking up " + input + "…");
        setAddBusy(true);
        KickApi::privateUserInChannelInfo(source, input, [add](auto result) {
            if (result && result->userID > 0)
                add(QString::number(result->userID), result->username);
            else
                add({}, {});
        });
    }
    else if (platform == MessagePlatform::YouTube)
    {
        static const QRegularExpression idPattern(
            QStringLiteral("^UC[A-Za-z0-9_-]{22}$"));
        if (!idPattern.match(input).hasMatch())
        {
            addHint_->setText(input + " hasn't chatted here. Use their channel "
                                      "ID instead (starts with UC).");
            return;
        }
        add(input, input);
    }
    else
    {
        static const QRegularExpression handlePattern(
            QStringLiteral("^[A-Za-z0-9_.]{1,24}$"));
        if (!handlePattern.match(input).hasMatch())
        {
            addHint_->setText("That isn't a valid TikTok username.");
            return;
        }
        add(input, input);
    }
}
void PriorityChat::addResolvedPerson(quint64 generation,
                                     quint64 settingsGeneration,
                                     const Person &person, Choice choice)
{
    if (generation != generation_ ||
        settingsGeneration != settingsGeneration_ || !settingsDraft_ ||
        !settingsPopup_)
        return;
    setAddBusy(false);
    if (person.id.isEmpty())
    {
        addHint_->setText("Couldn't find " + person.name +
                          ". Check the spelling.");
        return;
    }
    addChosenPerson(person, choice);
}
void PriorityChat::addChosenPerson(const Person &person, Choice choice)
{
    settingsDraft_->setChoice(person, choice);
    selectedKey_ = personKey(person.platform, person.id, person.login);
    refreshPeople();
    addName_->clear();
    const auto name = person.name.isEmpty() ? person.login : person.name;
    addHint_->setText("Added " + name + ". Click Save to apply.");
}

void PriorityChat::accountsChanged()
{
    ++generation_;
    inFlight_ = 0;
    followPermissionDenied_ = false;
    followStatus_.clear();
    setAddBusy(false);
    for (auto *people : {&model_.people, &retainedPeople_})
        for (auto &p : *people)
            if (p.platform == MessagePlatform::AnyOrTwitch)
            {
                p.followingKnown = false;
                p.followedAt = {};
                p.followAttempt = 0;
            }
    queueRefresh();
}
void PriorityChat::lookupFinished(quint64 generation, bool refresh)
{
    if (generation != generation_)
        return;
    inFlight_ = std::max(0, inFlight_ - 1);
    if (refresh)
    {
        queueRefresh();
        if (!saveTimer_.isActive())
            saveTimer_.start();
    }
}
void PriorityChat::lookUpNext()
{
    const auto now = QDateTime::currentMSecsSinceEpoch();
    if (!monitoring_ || inFlight_ >= 2 || now < nextLookup_ ||
        now < nextGlobalLookup)
        return;
    auto *merged = dynamic_cast<MergedChannel *>(channel_.get());
    auto twitchSource = merged ? merged->twitchChannel() : channel_;
    auto *tc = dynamic_cast<TwitchChannel *>(twitchSource.get());
    const auto account = getApp()->getAccounts()->twitch.getCurrent();
    const bool canFollow = tc && !account->isAnon() && tc->hasModRights();
    const auto generation = generation_;
    QPointer<PriorityChat> guard(this);
    for (auto it = model_.people.begin(); it != model_.people.end(); ++it)
    {
        auto &p = it.value();
        const auto key = it.key();
        const auto &rule = model_.rules[static_cast<int>(p.platform)];
        if (!rule.configured() || model_.choice(key) != Choice::Automatic ||
            p.lastSeen < QDateTime::currentDateTimeUtc().addSecs(-600))
            continue;
        if (!rule.matchAll &&
            model_.qualifies(p, QDateTime::currentDateTimeUtc()))
            continue;
        if (p.platform == MessagePlatform::Kick &&
            (rule.follows || rule.subscriptions) &&
            now - p.kickAttempt >= RETRY_MS &&
            !sourceName(p.platform).isEmpty())
        {
            p.kickAttempt = now;
            ++inFlight_;
            nextLookup_ = now + 1000;
            nextGlobalLookup = now + 1000;
            const auto source = sourceName(p.platform), id = p.id;
            KickApi::privateUserInChannelInfo(
                source, p.login, [guard, generation, key, id](auto result) {
                    std::optional<KickPrivateUserInChannelInfo> value;
                    if (result)
                        value = *result;
                    runInGuiThread([guard, generation, key, id,
                                    value = std::move(value)] {
                        if (!guard || guard->generation_ != generation)
                            return;
                        bool changed = false;
                        auto it = guard->model_.people.find(key);
                        if (it != guard->model_.people.end() && value &&
                            (id.isEmpty() ||
                             QString::number(value->userID) == id))
                        {
                            it->followingKnown = true;
                            it->followedAt =
                                value->followingSince.value_or(QDateTime());
                            it->active = value->subscriptionMonths.has_value();
                            it->activeAt = QDateTime::currentDateTimeUtc();
                            if (value->subscriptionMonths)
                                it->months =
                                    std::max(it->months.value_or(0),
                                             int(*value->subscriptionMonths));
                            changed = true;
                        }
                        guard->lookupFinished(generation, changed);
                    });
                });
            return;
        }
        if (p.platform != MessagePlatform::AnyOrTwitch)
            continue;
        if (rule.follows && !canFollow)
            followStatus_ =
                "Twitch follow age needs broadcaster/mod permission";
        if (rule.follows && canFollow && !followPermissionDenied_ &&
            !p.id.isEmpty() && !tc->roomId().isEmpty() &&
            now - p.followAttempt >= RETRY_MS)
        {
            p.followAttempt = now;
            ++inFlight_;
            nextLookup_ = now + 1000;
            nextGlobalLookup = now + 1000;
            QUrl url("https://api.twitch.tv/helix/channels/followers");
            QUrlQuery query;
            query.addQueryItem("broadcaster_id", tc->roomId());
            query.addQueryItem("user_id", p.id);
            url.setQuery(query);
            NetworkRequest(url)
                .header("Client-ID", account->getOAuthClient().toUtf8())
                .header("Authorization",
                        ("Bearer " + account->getOAuthToken()).toUtf8())
                .timeout(8000)
                .onSuccess(
                    [guard, generation, key](const NetworkResult &result) {
                        const auto root = result.parseJson();
                        runInGuiThread([guard, generation, key, root] {
                            if (!guard || guard->generation_ != generation)
                                return;
                            auto it = guard->model_.people.find(key);
                            const bool valid = root["data"].isArray();
                            if (valid)
                                guard->followStatus_.clear();
                            if (it != guard->model_.people.end() && valid)
                            {
                                const auto data = root["data"].toArray();
                                it->followingKnown = true;
                                it->followedAt =
                                    data.isEmpty()
                                        ? QDateTime()
                                        : QDateTime::fromString(
                                              data.first()
                                                  .toObject()["followed_at"]
                                                  .toString(),
                                              Qt::ISODate);
                            }
                            guard->lookupFinished(generation, valid);
                        });
                    })
                .onError([guard, generation](const NetworkResult &result) {
                    const bool denied =
                        result.status() == 401 || result.status() == 403;
                    runInGuiThread([guard, generation, denied] {
                        if (!guard || guard->generation_ != generation)
                            return;
                        if (denied)
                        {
                            guard->followPermissionDenied_ = true;
                            guard->followStatus_ =
                                "Twitch follow permission unavailable";
                            guard->queueRefresh();
                        }
                        guard->lookupFinished(generation, false);
                    });
                })
                .execute();
            return;
        }
        if (rule.subscriptions &&
            (!p.months || *p.months < rule.subscriptionMonths) &&
            now - p.subAttempt >= RETRY_MS && !sourceName(p.platform).isEmpty())
        {
            p.subAttempt = now;
            ++inFlight_;
            nextLookup_ = now + 1000;
            nextGlobalLookup = now + 1000;
            getIvr()->getSubage(
                p.login, sourceName(p.platform),
                [guard, generation, key](const IvrSubage &data) {
                    runInGuiThread([guard, generation, key, data] {
                        if (!guard || guard->generation_ != generation)
                            return;
                        bool changed = false;
                        auto it = guard->model_.people.find(key);
                        if (it != guard->model_.people.end() &&
                            !data.isSubHidden)
                        {
                            if (data.totalSubMonths > 0 &&
                                data.totalSubMonths <= 1200)
                                it->months = std::max(it->months.value_or(0),
                                                      data.totalSubMonths);
                            // A chat message received during the request wins for current state.
                            if (it->activeAt.toMSecsSinceEpoch() <
                                it->subAttempt)
                            {
                                it->active = data.isSubbed;
                                it->activeAt = QDateTime::currentDateTimeUtc();
                            }
                            changed = true;
                        }
                        guard->lookupFinished(generation, changed);
                    });
                },
                [guard, generation] {
                    runInGuiThread([guard, generation] {
                        if (guard)
                            guard->lookupFinished(generation, false);
                    });
                });
            return;
        }
    }
}
void PriorityChat::themeChangedEvent()
{
    setPalette(getTheme()->palette);
    setAutoFillBackground(true);
    refreshPeople();
}
void PriorityChat::scaleChangedEvent(float scale)
{
    BaseWidget::scaleChangedEvent(scale);
    setFont(getApp()->getFonts()->getFont(FontStyle::UiMedium, scale));
    const QSize icon(qRound(16 * scale), qRound(16 * scale));
    addPlatform_->setIconSize(icon);
    const int entryHeight =
        std::max(add_->sizeHint().height(),
                 addName_->sizeHint().height() + qRound(4 * scale));
    addName_->parentWidget()->setFixedHeight(entryHeight);
    add_->setFixedHeight(entryHeight);
    // Reserve the hint row so new text never shifts the layout or grows the
    // window.
    const QFontMetrics metrics(font());
    addHint_->setMinimumHeight(metrics.lineSpacing());
    users_->setMinimumHeight(
        std::max(metrics.height() + qRound(8 * scale), qRound(26 * scale)) *
            4 +
        2 * users_->frameWidth());
    themeChangedEvent();
}
bool PriorityChat::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == settingsPopup_.data() && event->type() == QEvent::Close)
    {
        settingsPopup_->getLayoutContainer()->layout()->removeWidget(this);
        settingsPopup_.clear();
        ++settingsGeneration_;
        settingsDraft_.reset();
        setAddBusy(false);
        setParent(split_);
        hide();
    }
    return BaseWidget::eventFilter(watched, event);
}
}  // namespace chatterino
