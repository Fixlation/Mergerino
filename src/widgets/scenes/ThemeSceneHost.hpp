// SPDX-License-Identifier: MIT

#pragma once

#include "widgets/scenes/ThemeScene.hpp"

#include <pajlada/signals/signalholder.hpp>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QString>

#include <memory>
#include <unordered_map>
#include <vector>

class QPainter;
class QWidget;

namespace chatterino {

class ChannelView;
class SplitInput;
struct Message;

namespace scenes {

/// The interactive theme presets, in display order.
const std::vector<SceneInfo> &sceneCatalog();

/// Creates the scene with the given key, or nullptr.
std::unique_ptr<ThemeScene> makeThemeScene(const QString &key);

/// Paints a still preview of a scene into `target`, for theme pickers.
void paintScenePreview(QPainter &painter, const QRect &target,
                       const QString &key);

/// Runs the interactive scene of the current theme for every app window:
/// steps it each frame, feeds it what happens in the window, and paints it
/// behind the window's panels.
class ThemeSceneHost : public QObject
{
public:
    static ThemeSceneHost &instance();

    /// True when the current theme is an interactive scene.
    bool isActive() const;

    /// True when `widget` sits in a window that shows a scene, so its own
    /// background should stay transparent.
    bool showsScene(const QWidget *widget);

    /// Paints the scene behind `rect` of `widget` (widget coordinates) as
    /// `surface`, using `tint` as the panel colour. Returns false, painting
    /// nothing, when the widget's window doesn't show a scene.
    bool paintSurface(QWidget *widget, QPainter &painter, const QRect &rect,
                      SceneSurface surface, const QColor &tint);

    /// Paints the scene's front layer over `rect` of `widget`.
    void paintFront(QWidget *widget, QPainter &painter, const QRect &rect);

    /// A message was added to a chat view.
    void messageAppended(ChannelView *view, const Message &message);
    /// The mouse moved onto `message` (row in view coordinates), or off any
    /// message when `message` is nullptr.
    void messageHovered(ChannelView *view, const Message *message,
                        const QRectF &row);
    /// You sent `text` from `input`. Called before the input is cleared.
    void messageSent(SplitInput *input, const QString &text);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    ThemeSceneHost();

    struct WindowScene;

    void themeUpdated();
    void tick();
    WindowScene *sceneFor(QWidget *widget, bool create);
    void ensureFrame(QWidget *window, WindowScene &ws);
    void updateLayout(QWidget *window, WindowScene &ws);
    void pollState(QWidget *window, WindowScene &ws);
    void fire(WindowScene &ws, const SceneEvent &event);
    void addHypeContribution(WindowScene &ws, double points);
    QRectF windowRect(const QWidget *widget, const QWidget *window) const;

    QString sceneKey_;
    std::unordered_map<QWidget *, std::unique_ptr<WindowScene>> windows_;
    pajlada::Signals::SignalHolder connections_;
    QElapsedTimer clock_;
    unsigned long lastPressTimestamp_ = 0;
};

}  // namespace scenes

}  // namespace chatterino
