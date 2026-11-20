// SPDX-License-Identifier: MIT

#pragma once

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSizeF>
#include <QString>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class QPainter;

/// Interactive themes ("scenes") paint one picture behind the whole window
/// and react to what happens in it: typing, sending, chat, subs, raids and so
/// on. This header only depends on QtGui so scenes can be rendered and
/// checked outside the app.
namespace chatterino::scenes {

/// Where the parts of the window are, in window coordinates (logical pixels).
struct SceneLayout {
    QSizeF window;
    /// Device pixel ratio the scene is rendered at. Create cached layers with
    /// it so they stay sharp.
    qreal dpr = 1.0;
    /// The tab strip along the top, across the window. May be empty.
    QRectF tabs;
    /// Where the tab names are inside the strip (tabs can wrap onto several
    /// rows). Keep bright or busy art off it. May be empty.
    QRectF tabLabels;
    /// Message areas of chat splits. Keep busy or bright art out of these.
    std::vector<QRectF> chats;
    /// Message areas of Activity panes (subs, gifts, raids are listed here).
    std::vector<QRectF> activity;
    /// Split headers (channel name bar above each split).
    std::vector<QRectF> headers;
    /// Message input boxes: the box you type in, with its buttons on the
    /// right (about 100-150 px). An emote bar or reply bar may sit just above
    /// a box, inside the band between the chat and the box.
    std::vector<QRectF> inputs;
    /// Message area of the selected split, or the first chat.
    QRectF focusChat;
    /// Input box of the selected split. May be empty.
    QRectF focusInput;

    /// True if `point` lies over chat or Activity messages.
    bool overText(QPointF point) const;
    /// The band around the focused input box: from the bottom of the focused
    /// chat to the bottom of the window, across the chat's width. It holds
    /// the input box and may hold an emote bar above it.
    QRectF inputBand() const;
};

/// Everything a scene may read each frame.
struct SceneState {
    /// Seconds since the scene started. Animate from this and `dt` only.
    double t = 0;
    /// Seconds since the previous step, at most 0.1.
    double dt = 0;
    /// Chat messages per second across the window, averaged over 8 s.
    double speed = 0;
    /// 0..1, 1 - exp(-speed / 3).
    double energy = 0;
    /// Viewers of the selected channel, -1 when unknown.
    int viewers = -1;
    bool live = true;
    /// A key was pressed in the focused input in the last 1.2 s.
    bool typing = false;
    /// Characters in the focused input.
    int draftLength = 0;
    /// Text caret of the focused input.
    QPointF caret;
    /// Mouse position while it is over the window.
    std::optional<QPointF> pointer;
    /// 0 while there is no hype train.
    int hypeLevel = 0;
    /// 0..1 towards the next hype level.
    double hypeProgress = 0;
    /// The user asked for less motion: rest almost still, no flashes or
    /// shake, but still show state changes.
    bool reduced = false;
    /// Name of the selected channel.
    QString channel;
    /// Index of the selected tab.
    int tabIndex = 0;
    /// Chat messages seen since the scene started.
    int messageCount = 0;
};

enum class SceneEventType : std::uint8_t {
    /// A key was typed or deleted in the focused input.
    /// amount = draft length, point = caret, deleted = a character was removed
    Key,
    /// You sent a message. text = message, amount = length,
    /// point = caret before sending, row = where your message lands
    Send,
    /// A chat message. user, color, text, row, firstTime
    Message,
    /// A subscription. user, color, amount = months
    Sub,
    /// Gifted subs. user, color, amount = how many
    Gift,
    /// Bits (or Kicks or TikTok diamonds). user, color, amount
    Bits,
    /// A raid. user = raiding channel, color, amount = viewers.
    /// A flood of messages usually follows.
    Raid,
    /// A new follower. user
    Follow,
    /// Someone mentioned you. user, color, text, row
    Mention,
    /// The same emote posted by several users in a row. text = emote,
    /// amount = how many times
    Combo,
    /// Hype train progress. amount = level, levelUp
    Hype,
    /// A shoutout. user = channel shouted out, color
    Shoutout,
    /// A moderator timed out or banned someone. user = who, row
    Timeout,
    /// The selected channel went live.
    Live,
    /// The selected channel went offline.
    Offline,
    /// You switched tab or channel. text = channel, amount = tab index
    Tab,
    /// The mouse moved onto a message. user, color, row
    Hover,
    /// The mouse left the hovered message.
    Unhover,
    /// Mouse click anywhere in the window. point
    Click,
};

struct SceneEvent {
    SceneEventType type = SceneEventType::Message;
    QString user;
    QString text;
    /// User colour, may be invalid.
    QColor color;
    int amount = 0;
    /// Where the message appeared, in window coordinates. Approximate.
    QRectF row;
    QPointF point;
    bool firstTime = false;
    bool deleted = false;
    bool levelUp = false;
};

/// How the window's panels sit on top of the scene.
struct SceneGlass {
    /// How much of the theme's background covers the scene under chat and
    /// Activity messages, 0..1. Higher means more readable text.
    double text = 0.72;
    /// The same over headers, tabs and input boxes.
    double chrome = 0.4;
    /// How much the scene under messages is blurred, 0..1.
    double blur = 0.7;
};

/// A theme preset made from a scene.
struct SceneInfo {
    QString key;
    QString name;
    /// Theme palette: background, text, accent.
    QColor surface;
    QColor ink;
    QColor accent;
    bool light = false;
};

class ThemeScene
{
public:
    virtual ~ThemeScene() = default;

    virtual SceneInfo info() const = 0;
    virtual SceneGlass glass() const
    {
        return {};
    }

    /// Advances the simulation by `state.dt`.
    virtual void step(const SceneState &state, const SceneLayout &layout) = 0;
    virtual void event(const SceneEvent &event, const SceneState &state,
                       const SceneLayout &layout) = 0;
    /// Paints the whole window behind the panels. Must cover every pixel.
    virtual void paintBack(QPainter &painter, const SceneState &state,
                           const SceneLayout &layout) = 0;

    /// Short accents drawn above the panels (and above message text, so
    /// keep them brief). The front layer starts transparent every frame.
    virtual bool hasFront() const
    {
        return false;
    }
    virtual void paintFront(QPainter &painter, const SceneState &state,
                            const SceneLayout &layout)
    {
        (void)painter;
        (void)state;
        (void)layout;
    }
};

std::unique_ptr<ThemeScene> makeDepartureScene();
std::unique_ptr<ThemeScene> makeKoiScene();
std::unique_ptr<ThemeScene> makeLanternScene();
std::unique_ptr<ThemeScene> makeArcadeScene();

/// The part of the window a panel covers, for compositing.
enum class SceneSurface : std::uint8_t {
    /// Gaps between panels; the scene shows through untouched.
    Window,
    Tabs,
    Header,
    Chat,
    Activity,
    Input,
};

/// One rendered frame of a scene for one window.
class SceneFrame
{
public:
    void render(ThemeScene &scene, const SceneState &state,
                const SceneLayout &layout);

    /// Paints the scene behind a panel. `dest` is in painter coordinates,
    /// `windowTopLeft` is where `dest` starts in window coordinates.
    void paintSurface(QPainter &painter, const QRect &dest,
                      QPointF windowTopLeft, SceneSurface surface,
                      const QColor &tint, const SceneGlass &glass) const;

    /// Paints the front layer over a panel's content.
    void paintFront(QPainter &painter, const QRect &dest,
                    QPointF windowTopLeft) const;

    bool isEmpty() const
    {
        return this->back_.isNull();
    }

    const QImage &back() const
    {
        return this->back_;
    }

private:
    QImage back_;
    QImage blurred_;
    QImage front_;
    bool hasFront_ = false;
    qreal dpr_ = 1.0;
};

// Helpers shared by scenes ---------------------------------------------------

constexpr double TAU = 6.283185307179586;

inline double clamp01(double v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

inline double lerp(double a, double b, double t)
{
    return a + (b - a) * t;
}

inline double smoothstep(double edge0, double edge1, double x)
{
    const auto t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3 - 2 * t);
}

inline double easeOutCubic(double t)
{
    t = clamp01(t);
    return 1 - std::pow(1 - t, 3);
}

inline double easeInOutSine(double t)
{
    return 0.5 - 0.5 * std::cos(clamp01(t) * TAU / 2);
}

/// Deterministic value in [0, 1] for integer coordinates.
double hash01(int a, int b, int c = 0);

/// Smooth value noise in [0, 1].
double valueNoise(double x, double y);

/// Small seeded random number generator (mulberry32).
class SceneRng
{
public:
    explicit SceneRng(std::uint32_t seed = 1)
        : state_(seed)
    {
    }

    /// [0, 1)
    double next();
    double range(double lo, double hi)
    {
        return lo + (hi - lo) * this->next();
    }

private:
    std::uint32_t state_;
};

QColor mixColor(const QColor &a, const QColor &b, double t);
QColor withAlpha(QColor color, double alpha);

/// Draws a soft round glow from a cached sprite. Cheap enough for hundreds
/// of particles per frame.
void drawGlow(QPainter &painter, QPointF center, double radius,
              const QColor &color, double alpha);

/// A transparent image of the given logical size at `dpr`.
QImage makeLayer(QSizeF size, qreal dpr);

}  // namespace chatterino::scenes
