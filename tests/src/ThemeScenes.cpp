#include "mocks/BaseApplication.hpp"
#include "singletons/Theme.hpp"
#include "widgets/scenes/ThemeScene.hpp"
#include "widgets/scenes/ThemeSceneHost.hpp"

#include <gtest/gtest.h>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <functional>
#include <vector>

using namespace chatterino;
using namespace chatterino::scenes;

namespace {

double contrast(const QColor &foreground, const QColor &background)
{
    const auto luminance = [](const QColor &color) {
        const auto linear = [](double value) {
            return value <= 0.04045 ? value / 12.92
                                    : std::pow((value + 0.055) / 1.055, 2.4);
        };
        return linear(color.redF()) * 0.2126 + linear(color.greenF()) * 0.7152 +
               linear(color.blueF()) * 0.0722;
    };
    const auto a = luminance(foreground);
    const auto b = luminance(background);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

/// A window like Mergerino's: tabs, one chat split and an Activity pane
SceneLayout windowLayout(double width, double height, bool withPanels = true)
{
    SceneLayout layout;
    layout.window = QSizeF(width, height);
    if (!withPanels)
    {
        return layout;
    }
    const double split = std::round(width * 0.66);
    layout.tabs = QRectF(0, 0, width, 28);
    const QRectF chat(0, 60, split, height - 108);
    const QRectF input(6, height - 44, split - 12, 38);
    layout.headers = {QRectF(0, 28, split, 32),
                      QRectF(split + 4, 28, width - split - 4, 32)};
    layout.chats = {chat};
    layout.activity = {QRectF(split + 4, 60, width - split - 4, height - 60)};
    layout.inputs = {input};
    layout.focusChat = chat;
    layout.focusInput = input;
    return layout;
}

/// Plays every kind of event through a scene and calls `onFrame` after
/// each painted frame.
void playEverything(ThemeScene &scene, const SceneLayout &layout,
                    const std::function<void(const QImage &)> &onFrame)
{
    SceneState state;
    state.viewers = 800;
    state.channel = QStringLiteral("riverlight");
    const auto row = layout.focusChat.isEmpty()
                         ? QRectF(0, 0, layout.window.width(), 24)
                         : QRectF(layout.focusChat.left(),
                                  layout.focusChat.bottom() - 24,
                                  layout.focusChat.width(), 24);
    const auto caret = layout.focusInput.isEmpty()
                           ? QPointF(20, layout.window.height() - 20)
                           : layout.focusInput.center();

    const std::vector<std::pair<int, SceneEvent>> script{
        {5, {.type = SceneEventType::Key, .text = "h", .amount = 1,
             .point = caret}},
        {7, {.type = SceneEventType::Key, .text = "i", .amount = 2,
             .point = caret}},
        {9, {.type = SceneEventType::Key, .amount = 1, .point = caret,
             .deleted = true}},
        {12, {.type = SceneEventType::Send, .user = "you", .text = "hi chat",
              .color = QColor("#ff8800"), .amount = 7, .row = row,
              .point = caret}},
        {16, {.type = SceneEventType::Sub, .user = "sub", .amount = 12,
              .row = row}},
        {20, {.type = SceneEventType::Gift, .user = "gifter", .amount = 50,
              .row = row}},
        {24, {.type = SceneEventType::Bits, .user = "cheerer", .amount = 10000,
              .row = row}},
        {28, {.type = SceneEventType::Raid, .user = "raider", .amount = 25000,
              .row = row}},
        {32, {.type = SceneEventType::Follow, .user = "follower"}},
        {36, {.type = SceneEventType::Mention, .user = "friend", .text = "@you",
              .row = row}},
        {40, {.type = SceneEventType::Combo, .text = "Clap", .amount = 12,
              .row = row}},
        {44, {.type = SceneEventType::Hype, .amount = 5, .levelUp = true}},
        {48, {.type = SceneEventType::Shoutout, .user = "friend", .row = row}},
        {52, {.type = SceneEventType::Timeout, .user = "spammer", .row = row}},
        {56, {.type = SceneEventType::Offline}},
        {64, {.type = SceneEventType::Live}},
        {70, {.type = SceneEventType::Tab, .text = "saltwave", .amount = 1}},
        {74, {.type = SceneEventType::Hover, .user = "friend", .row = row}},
        {78, {.type = SceneEventType::Unhover}},
        {80, {.type = SceneEventType::Click, .point = QPointF(50, 50)}},
        // Events far outside the window must not break anything
        {84, {.type = SceneEventType::Click, .point = QPointF(-500, 9000)}},
        {86, {.type = SceneEventType::Send, .text = QString(400, 'x'),
              .amount = 400, .row = row, .point = QPointF(-10, -10)}},
    };

    const double dt = 1.0 / 30.0;
    size_t next = 0;
    for (int step = 0; step < 100; ++step)
    {
        state.t = step * dt;
        state.dt = step == 0 ? 0 : dt;
        state.pointer = step % 20 < 10 ? std::optional<QPointF>(QPointF(
                                             step * 9.0, step * 5.0))
                                       : std::nullopt;
        state.speed = step < 50 ? 0.2 : 30;
        state.energy = 1 - std::exp(-state.speed / 3);
        state.viewers = step < 50 ? 3 : 2000000;
        // Chat on every other frame, and a flood after the raid
        if (step % 2 == 0 || (step > 28 && step < 40))
        {
            state.messageCount++;
            scene.event({.type = SceneEventType::Message,
                         .user = QStringLiteral("viewer%1").arg(step % 7),
                         .text = "hello",
                         .color = QColor::fromHsv((step * 37) % 360, 200, 230),
                         .row = row,
                         .firstTime = step % 9 == 0},
                        state, layout);
        }
        while (next < script.size() && script[next].first == step)
        {
            if (script[next].second.type == SceneEventType::Offline)
            {
                state.live = false;
            }
            if (script[next].second.type == SceneEventType::Live)
            {
                state.live = true;
            }
            if (script[next].second.type == SceneEventType::Hype)
            {
                state.hypeLevel = 5;
                state.hypeProgress = 0.5;
            }
            scene.event(script[next].second, state, layout);
            ++next;
        }
        state.typing = step > 5 && step < 12;
        state.reduced = step > 90;
        scene.step(state, layout);

        if (step % 9 == 0 || step == 99)
        {
            QImage image(
                std::max(1, static_cast<int>(layout.window.width() * layout.dpr)),
                std::max(1,
                         static_cast<int>(layout.window.height() * layout.dpr)),
                QImage::Format_RGB32);
            image.setDevicePixelRatio(layout.dpr);
            image.fill(QColor(255, 0, 255));
            {
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing);
                scene.paintBack(painter, state, layout);
                if (scene.hasFront())
                {
                    scene.paintFront(painter, state, layout);
                }
            }
            onFrame(image);
        }
    }
}

bool hasUnpaintedPixels(const QImage &image)
{
    const auto unpainted = qRgb(255, 0, 255);
    for (int y = 0; y < image.height(); y += 3)
    {
        const auto *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x += 3)
        {
            if ((line[x] & 0xFFFFFF) == (unpainted & 0xFFFFFF))
            {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST(ThemeScenes, EverySceneIsAnInteractivePreset)
{
    mock::BaseApplication app(QStringLiteral("{}"));
    ASSERT_EQ(sceneCatalog().size(), 4);
    for (const auto &info : sceneCatalog())
    {
        SCOPED_TRACE(info.key.toStdString());
        const auto key = QStringLiteral("scene.") + info.key;
        ASSERT_TRUE(app.theme.isAnimatedTheme(key));

        app.theme.themeName = key;
        app.theme.appearanceMode = "Dark";
        ASSERT_TRUE(app.theme.effect().isScene());
        EXPECT_EQ(app.theme.effect().scene, info.key);
        EXPECT_EQ(app.theme.isLightTheme(), info.light);
        const auto darkJson = app.theme.currentThemeJson();
        EXPECT_GE(contrast(app.theme.messages.textColors.regular,
                           app.theme.messages.backgrounds.regular),
                  4.5);
        EXPECT_GE(contrast(app.theme.splits.input.text,
                           app.theme.splits.input.background),
                  4.5);

        // A scene keeps its one look in light mode too
        app.theme.appearanceMode = "Light";
        EXPECT_TRUE(app.theme.effect().isScene());
        EXPECT_EQ(app.theme.currentThemeJson(), darkJson);
        EXPECT_EQ(app.theme.isLightTheme(), info.light);

        auto scene = makeThemeScene(info.key);
        ASSERT_NE(scene, nullptr);
        EXPECT_EQ(scene->info().key, info.key);
        EXPECT_EQ(scene->info().name, info.name);
        EXPECT_EQ(scene->info().surface, info.surface);
        EXPECT_EQ(scene->info().ink, info.ink);
        EXPECT_EQ(scene->info().accent, info.accent);
        EXPECT_EQ(scene->info().light, info.light);
    }
    app.theme.appearanceMode = "Dark";
    app.theme.themeName = "Dark";
    EXPECT_FALSE(app.theme.effect().isAnimated());
}

TEST(ThemeScenes, EverySceneCoversTheWindowThroughEveryEvent)
{
    struct Size {
        double width;
        double height;
        qreal dpr;
        bool panels;
    };
    const std::vector<Size> sizes{
        {1280, 760, 1.0, true},
        {700, 450, 1.0, true},
        {1920, 1080, 1.5, true},
        {900, 600, 1.0, false},
    };

    for (const auto &info : sceneCatalog())
    {
        for (const auto &size : sizes)
        {
            SCOPED_TRACE(QStringLiteral("%1 at %2x%3")
                             .arg(info.key)
                             .arg(size.width)
                             .arg(size.height)
                             .toStdString());
            auto scene = makeThemeScene(info.key);
            ASSERT_NE(scene, nullptr);
            auto layout = windowLayout(size.width, size.height, size.panels);
            layout.dpr = size.dpr;
            int frames = 0;
            playEverything(*scene, layout, [&](const QImage &image) {
                ++frames;
                EXPECT_FALSE(hasUnpaintedPixels(image));
            });
            EXPECT_GT(frames, 5);
        }
    }
}

TEST(ThemeScenes, ScenesFollowWindowResizes)
{
    for (const auto &info : sceneCatalog())
    {
        SCOPED_TRACE(info.key.toStdString());
        auto scene = makeThemeScene(info.key);
        ASSERT_NE(scene, nullptr);
        SceneState state;
        SceneFrame frame;
        for (const auto &[w, h] :
             std::vector<std::pair<double, double>>{{1280, 760},
                                                    {640, 400},
                                                    {1600, 900},
                                                    {320, 240}})
        {
            const auto layout = windowLayout(w, h);
            state.t += 0.5;
            state.dt = 0.1;
            scene->step(state, layout);
            frame.render(*scene, state, layout);
            EXPECT_EQ(frame.back().size(),
                      QSize(static_cast<int>(w), static_cast<int>(h)));
        }
    }
}
