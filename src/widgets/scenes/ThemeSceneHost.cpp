// SPDX-License-Identifier: MIT

#include "widgets/scenes/ThemeSceneHost.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Message.hpp"
#include "messages/MessageFlag.hpp"
#include "providers/merged/MergedChannel.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Theme.hpp"
#include "widgets/helper/ActivityMessageUtils.hpp"
#include "widgets/helper/ChannelView.hpp"
#include "widgets/helper/NotebookTab.hpp"
#include "widgets/Notebook.hpp"
#include "widgets/splits/Split.hpp"
#include "widgets/splits/SplitContainer.hpp"
#include "widgets/splits/SplitHeader.hpp"
#include "widgets/splits/SplitInput.hpp"
#include "widgets/Window.hpp"

#include <QApplication>
#include <QCursor>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <utility>

namespace chatterino::scenes {

namespace {

/// Seconds a hype contribution counts towards the hype train
constexpr double HYPE_WINDOW = 300;
/// Hype points per level once the train is running
constexpr double HYPE_POINTS_PER_LEVEL = 10;
constexpr int MAX_HYPE_LEVEL = 10;
/// Logical height of the row a new message lands in
constexpr double ROW_HEIGHT = 24;
constexpr qreal MAX_SCENE_DPR = 1.5;

Split *owningSplit(const QWidget *widget)
{
    for (auto *parent = widget ? widget->parentWidget() : nullptr;
         parent != nullptr; parent = parent->parentWidget())
    {
        if (auto *split = dynamic_cast<Split *>(parent))
        {
            return split;
        }
    }
    return nullptr;
}

Split *focusedSplit(QWidget *window)
{
    auto *appWindow = dynamic_cast<Window *>(window);
    if (appWindow == nullptr)
    {
        return nullptr;
    }
    auto *page = appWindow->getNotebook().getSelectedPage();
    if (page == nullptr)
    {
        return nullptr;
    }
    if (auto *split = page->getSelectedSplit();
        split != nullptr && !split->isActivityPane())
    {
        return split;
    }
    for (auto *split : page->getSplits())
    {
        if (!split->isActivityPane())
        {
            return split;
        }
    }
    return nullptr;
}

int viewerCount(const ChannelPtr &channel)
{
    auto twitch = channel;
    if (auto *merged = dynamic_cast<MergedChannel *>(channel.get()))
    {
        twitch = merged->twitchChannel();
    }
    if (auto *tc = dynamic_cast<TwitchChannel *>(twitch.get()))
    {
        const auto status = tc->accessStreamStatus();
        if (status->live)
        {
            return static_cast<int>(status->viewerCount);
        }
    }
    return -1;
}

int firstNumber(const QString &text, const QRegularExpression &pattern)
{
    const auto match = pattern.match(text);
    if (!match.hasMatch())
    {
        return 0;
    }
    auto digits = match.captured(1);
    digits.remove(',');
    return digits.toInt();
}

QString currentUserName()
{
    auto *app = getApp();
    if (app == nullptr || app->getAccounts() == nullptr)
    {
        return {};
    }
    const auto account = app->getAccounts()->twitch.getCurrent();
    return account ? account->getUserName() : QString();
}

QImage renderPreview(const QString &key)
{
    auto scene = makeThemeScene(key);
    if (!scene)
    {
        return {};
    }

    // A small window with a chat split, an Activity pane and an input box
    constexpr double W = 960;
    constexpr double H = 540;
    SceneLayout layout;
    layout.window = QSizeF(W, H);
    layout.tabs = QRectF(0, 0, W, 24);
    const QRectF chat(0, 52, 630, H - 52 - 40);
    const QRectF input(6, H - 36, 618, 32);
    const QRectF activity(634, 52, W - 634, H - 52);
    layout.headers = {QRectF(0, 24, 630, 28), QRectF(634, 24, W - 634, 28)};
    layout.chats = {chat};
    layout.activity = {activity};
    layout.inputs = {input};
    layout.focusChat = chat;
    layout.focusInput = input;

    SceneState state;
    state.viewers = 1200;
    state.channel = QStringLiteral("preview");
    const double dt = 1.0 / 30.0;
    const QString draft = QStringLiteral("hi chat");
    for (int step = 0; step <= 90; ++step)
    {
        state.t = step * dt;
        state.dt = step == 0 ? 0 : dt;
        state.caret = QPointF(input.left() + 10 + state.draftLength * 7,
                              input.center().y());
        if (step % 12 == 0)
        {
            state.messageCount++;
            scene->event({.type = SceneEventType::Message,
                          .user = QStringLiteral("viewer%1").arg(step),
                          .text = QStringLiteral("hello"),
                          .color = QColor("#5fb0ff"),
                          .row = QRectF(chat.left(), chat.bottom() - 24,
                                        chat.width(), 24)},
                         state, layout);
        }
        if (step >= 18 && step < 18 + draft.size() * 2 && step % 2 == 0)
        {
            state.draftLength++;
            state.typing = true;
            scene->event({.type = SceneEventType::Key,
                          .text = draft.mid(state.draftLength - 1, 1),
                          .amount = state.draftLength,
                          .point = state.caret},
                         state, layout);
        }
        if (step == 48)
        {
            scene->event({.type = SceneEventType::Send,
                          .user = QStringLiteral("you"),
                          .text = draft,
                          .amount = int(draft.size()),
                          .row = QRectF(chat.left(), chat.bottom() - 24,
                                        chat.width(), 24),
                          .point = state.caret},
                         state, layout);
            state.draftLength = 0;
            state.typing = false;
        }
        if (step == 66)
        {
            scene->event({.type = SceneEventType::Sub,
                          .user = QStringLiteral("subscriber"),
                          .color = QColor("#ffb54a"),
                          .amount = 6},
                         state, layout);
        }
        state.speed = 2.5;
        state.energy = 1 - std::exp(-state.speed / 3);
        scene->step(state, layout);
    }

    SceneFrame frame;
    frame.render(*scene, state, layout);

    QImage image(static_cast<int>(W), static_cast<int>(H),
                 QImage::Format_RGB32);
    QPainter painter(&image);
    const auto info = scene->info();
    const auto glass = scene->glass();
    const auto surface = [&](const QRectF &rect, SceneSurface kind) {
        frame.paintSurface(painter, rect.toRect(), rect.topLeft(), kind,
                           info.surface, glass);
    };
    surface(QRectF(0, 0, W, H), SceneSurface::Window);
    surface(layout.tabs, SceneSurface::Tabs);
    for (const auto &header : layout.headers)
    {
        surface(header, SceneSurface::Header);
    }
    surface(chat, SceneSurface::Chat);
    surface(activity, SceneSurface::Activity);
    surface(input, SceneSurface::Input);
    frame.paintFront(painter, image.rect(), QPointF(0, 0));
    return image;
}

}  // namespace

struct ThemeSceneHost::WindowScene {
    std::unique_ptr<ThemeScene> scene;
    SceneFrame frame;
    SceneState state;
    SceneLayout layout;
    QPointer<QWidget> window;
    bool stale = true;
    int ticksSinceLayout = 1000;
    double lastTick = -1;

    /// When chat messages arrived, for the chat speed
    std::deque<double> messageTimes;
    /// Messages already passed on, so a channel open in two splits only
    /// counts once
    std::deque<const Message *> recentMessages;

    int lastDraft = 0;
    double lastKey = -10;

    QPointer<QWidget> lastPage;
    QString lastChannel;
    bool lastLive = true;
    bool polled = false;

    const Message *hovered = nullptr;

    QString comboWord;
    std::vector<QString> comboUsers;
    double comboLast = 0;
    int comboFired = 0;

    /// (time, points) of recent subs, gifts and bits
    std::deque<std::pair<double, double>> hype;
};

const std::vector<SceneInfo> &sceneCatalog()
{
    // Kept as plain data: theme presets are built during static
    // initialisation, before scenes may create anything
    static const std::vector<SceneInfo> catalog{
        {QStringLiteral("departure"), QStringLiteral("Departure Board"),
         QColor("#0b0e15"), QColor("#ece5d6"), QColor("#f2a93b"), false},
        {QStringLiteral("koi"), QStringLiteral("Koi Pond"), QColor("#25292f"),
         QColor("#efe4d2"), QColor("#dc895f"), false},
        {QStringLiteral("lanterns"), QStringLiteral("Lantern Festival"),
         QColor("#101126"), QColor("#efe2c6"), QColor("#f0933c"), false},
        {QStringLiteral("arcade"), QStringLiteral("Raid Arcade"),
         QColor("#070a16"), QColor("#d4f2e8"), QColor("#ff4fa3"), false},
    };
    return catalog;
}

std::unique_ptr<ThemeScene> makeThemeScene(const QString &key)
{
    if (key == u"departure")
    {
        return makeDepartureScene();
    }
    if (key == u"koi")
    {
        return makeKoiScene();
    }
    if (key == u"lanterns")
    {
        return makeLanternScene();
    }
    if (key == u"arcade")
    {
        return makeArcadeScene();
    }
    return nullptr;
}

void paintScenePreview(QPainter &painter, const QRect &target,
                       const QString &key)
{
    static std::map<QString, QImage> previews;
    auto it = previews.find(key);
    if (it == previews.end())
    {
        it = previews.emplace(key, renderPreview(key)).first;
    }
    const auto &image = it->second;
    if (image.isNull() || target.isEmpty())
    {
        painter.fillRect(target, Qt::black);
        return;
    }

    // Fill the target, cropping the preview to its shape
    const double targetAspect = double(target.width()) / target.height();
    const double imageAspect = double(image.width()) / image.height();
    QRectF source(image.rect());
    if (targetAspect > imageAspect)
    {
        const double height = image.width() / targetAspect;
        source = QRectF(0, (image.height() - height) / 2, image.width(),
                        height);
    }
    else
    {
        const double width = image.height() * targetAspect;
        source = QRectF((image.width() - width) / 2, 0, width,
                        image.height());
    }
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(target), image, source);
    painter.restore();
}

ThemeSceneHost &ThemeSceneHost::instance()
{
    // Never destroyed: it is connected to the theme and the application,
    // which both outlive any static destructor order we could rely on
    static auto *host = new ThemeSceneHost();
    return *host;
}

ThemeSceneHost::ThemeSceneHost()
{
    this->clock_.start();
    if (auto *theme = getTheme())
    {
        this->connections_.managedConnect(theme->effectFrame, [this] {
            this->tick();
        });
        this->connections_.managedConnect(theme->updated, [this] {
            this->themeUpdated();
        });
    }
    if (qApp != nullptr)
    {
        qApp->installEventFilter(this);
    }
    this->themeUpdated();
}

bool ThemeSceneHost::isActive() const
{
    const auto *theme = getTheme();
    return theme != nullptr && theme->effect().isScene();
}

void ThemeSceneHost::themeUpdated()
{
    const auto key = this->isActive() ? getTheme()->effect().scene : QString();
    if (key != this->sceneKey_)
    {
        this->sceneKey_ = key;
        this->windows_.clear();
    }
}

ThemeSceneHost::WindowScene *ThemeSceneHost::sceneFor(QWidget *widget,
                                                      bool create)
{
    if (widget == nullptr || !this->isActive())
    {
        return nullptr;
    }
    if (this->sceneKey_.isEmpty())
    {
        this->themeUpdated();
    }

    auto *window = widget->window();
    if (dynamic_cast<Window *>(window) == nullptr)
    {
        return nullptr;
    }

    if (auto it = this->windows_.find(window); it != this->windows_.end())
    {
        if (it->second->window)
        {
            return it->second.get();
        }
        this->windows_.erase(it);
    }
    if (!create)
    {
        return nullptr;
    }

    auto scene = makeThemeScene(this->sceneKey_);
    if (!scene)
    {
        return nullptr;
    }
    auto ws = std::make_unique<WindowScene>();
    ws->scene = std::move(scene);
    ws->window = window;
    QObject::connect(window, &QObject::destroyed, this, [this, window] {
        this->windows_.erase(window);
    });
    auto *raw = ws.get();
    this->windows_.emplace(window, std::move(ws));
    this->updateLayout(window, *raw);
    this->pollState(window, *raw);
    return raw;
}

void ThemeSceneHost::tick()
{
    if (!this->isActive())
    {
        return;
    }

    for (auto *top : QApplication::topLevelWidgets())
    {
        auto *window = dynamic_cast<Window *>(top);
        if (window == nullptr || !window->isVisible() || window->isMinimized())
        {
            continue;
        }
        auto *ws = this->sceneFor(window, true);
        if (ws == nullptr)
        {
            continue;
        }

        if (++ws->ticksSinceLayout >= 3 ||
            ws->layout.window != QSizeF(window->size()))
        {
            this->updateLayout(window, *ws);
        }
        this->pollState(window, *ws);
        ws->scene->step(ws->state, ws->layout);
        ws->stale = true;
        window->update();
    }
}

void ThemeSceneHost::ensureFrame(QWidget *window, WindowScene &ws)
{
    const auto dpr = std::min(window->devicePixelRatioF(), MAX_SCENE_DPR);
    if (ws.layout.window != QSizeF(window->size()) || ws.layout.dpr != dpr)
    {
        this->updateLayout(window, ws);
        ws.stale = true;
    }
    if (ws.stale || ws.frame.isEmpty())
    {
        ws.frame.render(*ws.scene, ws.state, ws.layout);
        ws.stale = false;
    }
}

bool ThemeSceneHost::showsScene(const QWidget *widget)
{
    return this->sceneFor(const_cast<QWidget *>(widget), true) != nullptr;
}

bool ThemeSceneHost::paintSurface(QWidget *widget, QPainter &painter,
                                  const QRect &rect, SceneSurface surface,
                                  const QColor &tint)
{
    auto *ws = this->sceneFor(widget, true);
    if (ws == nullptr)
    {
        return false;
    }
    auto *window = widget->window();
    this->ensureFrame(window, *ws);
    const auto topLeft = widget->mapTo(window, rect.topLeft());
    ws->frame.paintSurface(painter, rect, QPointF(topLeft), surface, tint,
                           ws->scene->glass());
    return true;
}

void ThemeSceneHost::paintFront(QWidget *widget, QPainter &painter,
                                const QRect &rect)
{
    auto *ws = this->sceneFor(widget, false);
    if (ws == nullptr || ws->frame.isEmpty())
    {
        return;
    }
    const auto topLeft = widget->mapTo(widget->window(), rect.topLeft());
    ws->frame.paintFront(painter, rect, QPointF(topLeft));
}

QRectF ThemeSceneHost::windowRect(const QWidget *widget,
                                  const QWidget *window) const
{
    return {QPointF(widget->mapTo(window, QPoint(0, 0))),
            QSizeF(widget->size())};
}

void ThemeSceneHost::updateLayout(QWidget *window, WindowScene &ws)
{
    SceneLayout layout;
    layout.window = QSizeF(window->size());
    layout.dpr = std::min(window->devicePixelRatioF(), MAX_SCENE_DPR);

    const auto shown = [window](const QWidget *widget) {
        return widget->isVisible() && widget->window() == window &&
               widget->width() > 0 && widget->height() > 0;
    };

    for (auto *view : window->findChildren<ChannelView *>())
    {
        auto *split = owningSplit(view);
        if (!shown(view) || split == nullptr ||
            &split->getChannelView() != view)
        {
            continue;
        }
        (split->isActivityPane() ? layout.activity : layout.chats)
            .push_back(this->windowRect(view, window));
    }
    for (auto *header : window->findChildren<SplitHeader *>())
    {
        if (shown(header))
        {
            layout.headers.push_back(this->windowRect(header, window));
        }
    }
    const auto composer = [this, window](const SplitInput *input) {
        return QRectF(QPointF(input->mapTo(window,
                                           input->composerRect().topLeft())),
                      QSizeF(input->composerRect().size()));
    };
    for (auto *input : window->findChildren<SplitInput *>())
    {
        if (shown(input))
        {
            layout.inputs.push_back(composer(input));
        }
    }

    QRectF tabs;
    for (auto *tab : window->findChildren<NotebookTab *>())
    {
        if (shown(tab))
        {
            tabs = tabs.isNull() ? this->windowRect(tab, window)
                                 : tabs.united(this->windowRect(tab, window));
        }
    }
    layout.tabLabels = tabs;
    if (!tabs.isEmpty() && tabs.width() >= tabs.height())
    {
        // A horizontal tab strip spans the window
        tabs.setLeft(0);
        tabs.setRight(layout.window.width());
    }
    layout.tabs = tabs;

    if (auto *split = focusedSplit(window))
    {
        if (shown(&split->getChannelView()))
        {
            layout.focusChat = this->windowRect(&split->getChannelView(), window);
        }
        if (shown(&split->getInput()))
        {
            layout.focusInput = composer(&split->getInput());
        }
    }
    if (layout.focusChat.isEmpty() && !layout.chats.empty())
    {
        layout.focusChat = layout.chats.front();
    }
    if (layout.focusInput.isEmpty() && !layout.inputs.empty())
    {
        layout.focusInput = layout.inputs.front();
    }

    ws.layout = std::move(layout);
    ws.ticksSinceLayout = 0;
}

void ThemeSceneHost::pollState(QWidget *window, WindowScene &ws)
{
    auto &state = ws.state;
    const double now = double(this->clock_.elapsed()) / 1000.0;
    state.dt = ws.lastTick < 0 ? 0 : std::clamp(now - ws.lastTick, 0.0, 0.1);
    ws.lastTick = now;
    state.t += state.dt;

    const auto local = window->mapFromGlobal(QCursor::pos());
    if (window->rect().contains(local))
    {
        state.pointer = QPointF(local);
    }
    else
    {
        state.pointer.reset();
    }

    while (!ws.messageTimes.empty() && ws.messageTimes.front() < state.t - 8)
    {
        ws.messageTimes.pop_front();
    }
    state.speed = double(ws.messageTimes.size()) / 8.0;
    state.energy = 1 - std::exp(-state.speed / 3);

    // Hype runs out once nobody has contributed for a while
    if (!ws.hype.empty() && ws.hype.front().first < state.t - HYPE_WINDOW)
    {
        while (!ws.hype.empty() &&
               ws.hype.front().first < state.t - HYPE_WINDOW)
        {
            ws.hype.pop_front();
        }
        this->addHypeContribution(ws, 0);
    }

    auto *split = focusedSplit(window);
    auto *appWindow = dynamic_cast<Window *>(window);
    QWidget *page =
        appWindow != nullptr ? appWindow->getNotebook().getSelectedPage()
                             : nullptr;
    const int tabIndex = (appWindow != nullptr && page != nullptr)
                             ? appWindow->getNotebook().indexOf(page)
                             : 0;

    QString channelName;
    bool live = false;
    int viewers = -1;
    if (split != nullptr)
    {
        if (const auto channel = split->getChannel())
        {
            channelName = channel->getName();
            live = channel->isLive();
            viewers = viewerCount(channel);
        }
    }

    const bool switched = ws.polled && (page != ws.lastPage.data() ||
                                        channelName != ws.lastChannel);
    state.channel = channelName;
    state.tabIndex = std::max(0, tabIndex);
    state.viewers = viewers;
    if (switched)
    {
        this->fire(ws, {.type = SceneEventType::Tab,
                        .text = channelName,
                        .amount = state.tabIndex});
    }
    else if (ws.polled && live != ws.lastLive)
    {
        state.live = live;
        this->fire(ws, {.type = live ? SceneEventType::Live
                                     : SceneEventType::Offline});
    }
    state.live = live;
    ws.lastPage = page;
    ws.lastChannel = channelName;
    ws.lastLive = live;

    if (split != nullptr)
    {
        auto &input = split->getInput();
        const auto text = input.getInputText();
        const int length = static_cast<int>(text.size());
        state.caret =
            QPointF(input.mapTo(window, input.caretRect().center()));
        if (ws.polled && !switched && length != ws.lastDraft &&
            input.isVisible())
        {
            ws.lastKey = state.t;
            this->fire(ws, {.type = SceneEventType::Key,
                            .text = text.right(1),
                            .amount = length,
                            .point = state.caret,
                            .deleted = length < ws.lastDraft});
        }
        ws.lastDraft = length;
        state.draftLength = length;
    }
    else
    {
        state.draftLength = 0;
    }
    state.typing = state.t - ws.lastKey < 1.2;
    ws.polled = true;
}

void ThemeSceneHost::fire(WindowScene &ws, const SceneEvent &event)
{
    ws.scene->event(event, ws.state, ws.layout);
}

void ThemeSceneHost::addHypeContribution(WindowScene &ws, double points)
{
    if (points > 0)
    {
        ws.hype.emplace_back(ws.state.t, points);
    }
    double total = 0;
    for (const auto &[time, value] : ws.hype)
    {
        (void)time;
        total += value;
    }

    // Like Twitch, a train starts after three contributions
    int level = 0;
    double progress = 0;
    if (ws.hype.size() >= 3)
    {
        level = std::min(MAX_HYPE_LEVEL,
                         1 + static_cast<int>(total / HYPE_POINTS_PER_LEVEL));
        progress = std::fmod(total, HYPE_POINTS_PER_LEVEL) /
                   HYPE_POINTS_PER_LEVEL;
    }

    const int previous = ws.state.hypeLevel;
    ws.state.hypeLevel = level;
    ws.state.hypeProgress = progress;
    if (level != previous)
    {
        this->fire(ws, {.type = SceneEventType::Hype,
                        .amount = level,
                        .levelUp = level > previous});
    }
}

void ThemeSceneHost::messageSent(SplitInput *input, const QString &text)
{
    auto *ws = this->sceneFor(input, true);
    if (ws == nullptr)
    {
        return;
    }
    auto *window = input->window();

    QRectF row;
    if (auto *split = owningSplit(input))
    {
        const auto view = this->windowRect(&split->getChannelView(), window);
        row = QRectF(view.left(), view.bottom() - ROW_HEIGHT, view.width(),
                     ROW_HEIGHT);
    }
    const auto caret = QPointF(input->mapTo(window, input->caretRect().center()));
    ws->state.caret = caret;
    this->fire(*ws, {.type = SceneEventType::Send,
                     .user = currentUserName(),
                     .text = text,
                     .color = getTheme()->tabs.selected.line.regular,
                     .amount = static_cast<int>(text.size()),
                     .row = row,
                     .point = caret});

    // The input is cleared next; that isn't typing
    ws->lastDraft = 0;
    ws->lastKey = ws->state.t - 2;
}

void ThemeSceneHost::messageAppended(ChannelView *view, const Message &message)
{
    if (!this->isActive() || view == nullptr || !view->isVisible())
    {
        return;
    }
    auto *split = owningSplit(view);
    if (split == nullptr || split->isActivityPane() ||
        &split->getChannelView() != view)
    {
        return;
    }
    auto *ws = this->sceneFor(view, true);
    if (ws == nullptr || message.flags.has(MessageFlag::RecentMessage))
    {
        return;
    }

    if (std::find(ws->recentMessages.begin(), ws->recentMessages.end(),
                  &message) != ws->recentMessages.end())
    {
        return;
    }
    ws->recentMessages.push_back(&message);
    if (ws->recentMessages.size() > 128)
    {
        ws->recentMessages.pop_front();
    }

    auto *window = view->window();
    const auto viewRect = this->windowRect(view, window);
    const QRectF row(viewRect.left(), viewRect.bottom() - ROW_HEIGHT,
                     viewRect.width(), ROW_HEIGHT);
    const auto user = message.displayName.isEmpty() ? message.loginName
                                                    : message.displayName;
    const auto &text = message.messageText;
    const auto &flags = message.flags;

    SceneEvent event{.user = user,
                     .text = text,
                     .color = message.usernameColor,
                     .row = row};

    static const QRegularExpression raidViewers(
        QStringLiteral(R"((\d[\d,]*)\s+(?:raiders|viewers))"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression anyNumber(QStringLiteral(R"((\d[\d,]*))"));
    static const QRegularExpression months(
        QStringLiteral(R"((\d+)\s+months?)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression shoutout(
        QStringLiteral(R"(gave a shoutout to\s+(\S+?)[.!]?$)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression follow(
        QStringLiteral(R"(^(\S+)\s+(?:just\s+)?followed\b)"),
        QRegularExpression::CaseInsensitiveOption);

    if (flags.has(MessageFlag::Raid))
    {
        event.type = SceneEventType::Raid;
        event.amount = firstNumber(text, raidViewers);
        if (event.amount == 0)
        {
            event.amount = firstNumber(text, anyNumber);
        }
        this->fire(*ws, event);
        return;
    }

    if (const auto gifts = getActivityGiftBombRecipientCount(message))
    {
        event.type = SceneEventType::Gift;
        event.amount = std::max(1, *gifts);
        this->addHypeContribution(*ws, event.amount);
        this->fire(*ws, event);
        return;
    }
    if (isActivityGiftRecipientMessage(message) ||
        isActivityTwitchBitsBadgeMessage(message))
    {
        return;
    }

    const auto bits = std::max({static_cast<int>(message.bits),
                                static_cast<int>(message.kickGiftKicks),
                                static_cast<int>(message.tiktokGiftDiamondCount)});
    if (bits > 0)
    {
        event.type = SceneEventType::Bits;
        event.amount = bits;
        this->addHypeContribution(*ws, std::max(1.0, bits / 100.0));
        this->fire(*ws, event);
        return;
    }

    if (flags.has(MessageFlag::Subscription))
    {
        event.type = SceneEventType::Sub;
        event.amount = std::max(1, firstNumber(text, months));
        this->addHypeContribution(*ws, 1);
        this->fire(*ws, event);
        return;
    }

    if (flags.has(MessageFlag::Timeout) && !flags.has(MessageFlag::Untimeout))
    {
        event.type = SceneEventType::Timeout;
        if (!message.timeoutUser.isEmpty())
        {
            event.user = message.timeoutUser;
        }
        this->fire(*ws, event);
        return;
    }

    if (flags.has(MessageFlag::System))
    {
        if (const auto match = shoutout.match(text); match.hasMatch())
        {
            event.type = SceneEventType::Shoutout;
            event.user = match.captured(1);
            this->fire(*ws, event);
        }
        else if (const auto followed = follow.match(text);
                 followed.hasMatch() && !text.contains(u"mode"))
        {
            event.type = SceneEventType::Follow;
            event.user = followed.captured(1);
            this->fire(*ws, event);
        }
        return;
    }

    // An ordinary chat message
    ws->messageTimes.push_back(ws->state.t);
    ws->state.messageCount++;

    const auto self = currentUserName();
    if (!self.isEmpty() &&
        message.loginName.compare(self, Qt::CaseInsensitive) == 0)
    {
        // Your own message; sending already reacted to it
        return;
    }

    if (flags.has(MessageFlag::ShowInMentions) ||
        flags.has(MessageFlag::Highlighted))
    {
        event.type = SceneEventType::Mention;
        this->fire(*ws, event);
    }

    event.type = SceneEventType::Message;
    event.firstTime = flags.has(MessageFlag::FirstMessage);
    this->fire(*ws, event);

    // Emote combos: the same single word from several users in a row
    const auto word = text.trimmed();
    if (word.isEmpty() || word.contains(u' ') || word.size() > 40)
    {
        ws->comboWord.clear();
        return;
    }
    if (word != ws->comboWord || ws->state.t - ws->comboLast > 6)
    {
        ws->comboWord = word;
        ws->comboUsers = {user};
        ws->comboFired = 0;
    }
    else if (std::find(ws->comboUsers.begin(), ws->comboUsers.end(), user) ==
             ws->comboUsers.end())
    {
        ws->comboUsers.push_back(user);
    }
    ws->comboLast = ws->state.t;
    const int count = static_cast<int>(ws->comboUsers.size());
    if (count >= 4 && (ws->comboFired == 0 || count >= ws->comboFired + 4))
    {
        ws->comboFired = count;
        this->fire(*ws, {.type = SceneEventType::Combo,
                         .text = word,
                         .amount = count,
                         .row = row});
    }
}

void ThemeSceneHost::messageHovered(ChannelView *view, const Message *message,
                                    const QRectF &row)
{
    auto *ws = this->sceneFor(view, false);
    if (ws == nullptr || message == ws->hovered)
    {
        return;
    }
    ws->hovered = message;
    if (message == nullptr)
    {
        this->fire(*ws, {.type = SceneEventType::Unhover});
        return;
    }
    const auto offset = QPointF(view->mapTo(view->window(), QPoint(0, 0)));
    this->fire(*ws, {.type = SceneEventType::Hover,
                     .user = message->displayName.isEmpty()
                                 ? message->loginName
                                 : message->displayName,
                     .color = message->usernameColor,
                     .row = row.translated(offset)});
}

bool ThemeSceneHost::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() != QEvent::MouseButtonPress || !watched->isWidgetType() ||
        !this->isActive())
    {
        return false;
    }
    auto *mouse = static_cast<QMouseEvent *>(event);
    // The same press is delivered again to parents that ignore it
    if (mouse->timestamp() == this->lastPressTimestamp_)
    {
        return false;
    }
    this->lastPressTimestamp_ = mouse->timestamp();

    auto *widget = static_cast<QWidget *>(watched);
    auto *ws = this->sceneFor(widget, false);
    if (ws == nullptr)
    {
        return false;
    }
    const auto point =
        widget->window()->mapFromGlobal(mouse->globalPosition().toPoint());
    this->fire(*ws, {.type = SceneEventType::Click, .point = QPointF(point)});
    return false;
}

}  // namespace chatterino::scenes
