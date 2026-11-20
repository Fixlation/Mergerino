// SPDX-License-Identifier: MIT

// Lantern Festival: a riverside town on festival night, built as a layered
// cut-paper diorama. Sky, hills, town, bridge, river and the near bank are
// separate paper layers with deckled edges, soft drop shadows and fibre
// grain. They are cached as images and shifted for parallax. Paper lanterns
// carry the events: your draft inflates a lantern beside the input, sending
// lets it fly up into a constellation, and the town answers the chat.

#include "widgets/scenes/ThemeScene.hpp"

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace chatterino::scenes {

namespace {

// Palette --------------------------------------------------------------------

const QColor SURFACE(0x10, 0x11, 0x26);
const QColor INK(0xef, 0xe2, 0xc6);
const QColor ACCENT(0xf0, 0x93, 0x3c);

const QColor SKY_TOP(0x06, 0x07, 0x18);
const QColor SKY_MID(0x0c, 0x0f, 0x2e);
const QColor SKY_LOW(0x1b, 0x19, 0x42);
const QColor HORIZON_WARM(0x8a, 0x40, 0x46);
const QColor STAR_PAPER(0xf1, 0xe4, 0xbf);
const QColor CLOUD_PAPER(0x25, 0x29, 0x5a);
const QColor HILL_FAR(0x2a, 0x30, 0x66);
const QColor HILL_NEAR(0x17, 0x19, 0x41);
const QColor TOWN_BACK(0x1d, 0x1d, 0x45);
const QColor TOWN(0x13, 0x13, 0x32);
const QColor TOWN_RIM(0x8a, 0x55, 0x52);
const QColor WINDOW_DARK(0x0a, 0x0a, 0x1e);
const QColor BRIDGE(0x55, 0x20, 0x30);
const QColor BRIDGE_RIM(0xc4, 0x68, 0x48);
const QColor RAIL(0x3a, 0x16, 0x24);
const QColor CROWD(0x0d, 0x0b, 0x1c);
const QColor CROWD_RIM(0x7a, 0x4a, 0x3c);
const QColor WATER_HIGH(0x1d, 0x22, 0x56);
const QColor WATER_MID(0x15, 0x19, 0x46);
const QColor WATER_DEEP(0x0d, 0x0f, 0x30);
const QColor BANK(0x07, 0x09, 0x16);
const QColor REED(0x0b, 0x11, 0x1d);
const QColor REED_RIM(0x34, 0x44, 0x40);
const QColor CURTAIN(0x52, 0x17, 0x31);
const QColor GOLD(0xd8, 0xa4, 0x4a);
const QColor LANTERN_RED(0xd8, 0x42, 0x2e);
const QColor LANTERN_ORANGE(0xf0, 0x8e, 0x36);
const QColor WARM_LIGHT(0xff, 0xc2, 0x72);
const QColor HOT_CORE(0xff, 0xf0, 0xc4);
const QColor MOON_PAPER(0xee, 0xe2, 0xc2);
const QColor SHADOW_INK(0x02, 0x02, 0x0a);
const QColor HULL_PAPER(0xc9, 0xb4, 0x8a);

/// Room around cached layers so parallax never shows an edge
constexpr double LAYER_MARGIN = 18;
/// Body width of cached lantern sprites, in sprite units
constexpr double SPRITE_BODY = 48;

int iround(double v)
{
    return static_cast<int>(std::lround(v));
}

double rnd(std::uint32_t seed, int a, int b = 0)
{
    return hash01(static_cast<int>(seed), a, b);
}

double deg(double radians)
{
    return radians * 360.0 / TAU;
}

double dist(QPointF a, QPointF b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

QPointF bezier(QPointF a, QPointF b, QPointF c, QPointF d, double u)
{
    const double v = 1 - u;
    return a * (v * v * v) + b * (3 * v * v * u) + c * (3 * v * u * u) +
           d * (u * u * u);
}

// Paper ----------------------------------------------------------------------

/// Value noise that repeats every `period` cells, for a seamless grain tile.
double tileNoise(double x, double y, int period, int salt)
{
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const double fx = x - x0;
    const double fy = y - y0;
    const double sx = fx * fx * (3 - 2 * fx);
    const double sy = fy * fy * (3 - 2 * fy);
    const auto h = [period, salt](int i, int j) {
        return hash01(((i % period) + period) % period,
                      ((j % period) + period) % period, salt);
    };
    return lerp(lerp(h(x0, y0), h(x0 + 1, y0), sx),
                lerp(h(x0, y0 + 1), h(x0 + 1, y0 + 1), sx), sy);
}

/// A seamless tile of paper: soft mottling plus short light and dark fibres.
const QImage &grainTile()
{
    static const QImage tile = [] {
        constexpr int size = 192;
        QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < size; ++y)
        {
            auto *line = reinterpret_cast<QRgb *>(img.scanLine(y));
            for (int x = 0; x < size; ++x)
            {
                const double n =
                    0.55 * tileNoise(x / 12.0, y / 12.0, size / 12, 5) +
                    0.45 * tileNoise(x / 3.0, y / 3.0, size / 3, 9);
                const double v = (n - 0.5) * 2;
                const int alpha = std::clamp(iround(std::abs(v) * 20), 0, 255);
                line[x] = v > 0 ? qPremultiply(qRgba(255, 244, 222, alpha))
                                : qPremultiply(qRgba(0, 0, 8, alpha));
            }
        }
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        SceneRng rng(4242);
        for (int i = 0; i < 520; ++i)
        {
            const QPointF from(rng.range(0, size), rng.range(0, size));
            const double angle = rng.range(0, TAU);
            const double len = rng.range(2, 9);
            const QPointF dir(std::cos(angle) * len, std::sin(angle) * len);
            const double bend = rng.range(-0.3, 0.3);
            QPainterPath fibre(from);
            fibre.quadTo(from + dir * 0.5 + QPointF(-dir.y(), dir.x()) * bend,
                         from + dir);
            const bool light = rng.next() < 0.6;
            const int alpha = iround(rng.range(8, 20));
            p.setPen(QPen(light ? QColor(255, 246, 225, alpha)
                                : QColor(0, 0, 10, alpha),
                          rng.range(0.4, 0.8)));
            for (int dx = -1; dx <= 1; ++dx)
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    p.drawPath(fibre.translated(dx * size, dy * size));
                }
            }
        }
        p.end();
        return img;
    }();
    return tile;
}

/// Lays paper fibre over whatever is already painted in `rect`.
void applyGrain(QPainter &p, const QRectF &rect, double strength)
{
    p.save();
    p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
    p.setOpacity(strength);
    p.fillRect(rect, QBrush(grainTile()));
    p.restore();
}

/// Splits each edge of a polygon into short pieces and nudges the joints
/// sideways, so straight cuts look hand cut and long edges look deckled.
QPainterPath cutPolygon(const QPolygonF &poly, double step, double jitter,
                        int seed)
{
    QPainterPath path;
    const auto n = static_cast<int>(poly.size());
    int k = 0;
    for (int i = 0; i < n; ++i)
    {
        const QPointF a = poly[i];
        const QPointF b = poly[(i + 1) % n];
        const QPointF d = b - a;
        const double len = std::hypot(d.x(), d.y());
        const int parts = std::max(1, static_cast<int>(len / step));
        const QPointF normal =
            len > 0 ? QPointF(-d.y() / len, d.x() / len) : QPointF();
        for (int j = 0; j < parts; ++j)
        {
            QPointF q = a + d * (double(j) / parts);
            if (j > 0)
            {
                q += normal * ((hash01(seed, k, 3) - 0.5) * 2 * jitter);
            }
            if (i == 0 && j == 0)
            {
                path.moveTo(q);
            }
            else
            {
                path.lineTo(q);
            }
            ++k;
        }
    }
    path.closeSubpath();
    return path;
}

QPainterPath cutShape(const QPainterPath &shape, double step, double jitter,
                      int seed)
{
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    int k = 0;
    for (const auto &poly : shape.toSubpathPolygons())
    {
        out.addPath(cutPolygon(poly, step, jitter, seed + 131 * k));
        ++k;
    }
    return out;
}

/// Fills a paper cut-out: a thin lit rim along its upper edges, the paper
/// body below it.
void fillPaper(QPainter &p, const QPainterPath &shape, const QBrush &body,
               const QColor &rim, double rimWidth)
{
    p.fillPath(shape, rim);
    p.fillPath(shape.translated(0, rimWidth), body);
}

/// Returns `src` with a soft drop shadow underneath, as if the paper layer
/// stood a little in front of the one behind it.
QImage withShadow(const QImage &src, QPointF offset, double alpha, int blur)
{
    const QSize small(std::max(1, src.width() / blur),
                      std::max(1, src.height() / blur));
    QImage shadow =
        src.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_ARGB32_Premultiplied);
    shadow.setDevicePixelRatio(1);
    {
        QPainter sp(&shadow);
        sp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        sp.fillRect(shadow.rect(), SHADOW_INK);
    }
    QImage out(src.size(), QImage::Format_ARGB32_Premultiplied);
    out.setDevicePixelRatio(src.devicePixelRatio());
    out.fill(Qt::transparent);
    const qreal dpr = src.devicePixelRatio();
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setOpacity(alpha);
    p.drawImage(QRectF(offset, QSizeF(src.width() / dpr, src.height() / dpr)),
                shadow);
    p.setOpacity(1);
    p.drawImage(QPointF(0, 0), src);
    p.end();
    return out;
}

/// An upturned eave roof, the shape of every roof in town.
QPainterPath roofPath(double cx, double eaveY, double halfW, double rise,
                      double curl)
{
    const QPointF tipL(cx - halfW - curl * 0.6, eaveY - curl);
    const QPointF tipR(cx + halfW + curl * 0.6, eaveY - curl);
    const QPointF ridgeL(cx - halfW * 0.42, eaveY - rise);
    const QPointF ridgeR(cx + halfW * 0.42, eaveY - rise);
    QPainterPath path(tipL);
    path.quadTo(QPointF(cx - halfW * 0.6, eaveY - rise * 0.12), ridgeL);
    path.lineTo(ridgeR);
    path.quadTo(QPointF(cx + halfW * 0.6, eaveY - rise * 0.12), tipR);
    path.quadTo(QPointF(cx + halfW * 0.7, eaveY + 1.5),
                QPointF(cx, eaveY + 1.2));
    path.quadTo(QPointF(cx - halfW * 0.7, eaveY + 1.5), tipL);
    path.closeSubpath();
    return path;
}

// Lanterns -------------------------------------------------------------------

struct LanternLook {
    QColor paper = LANTERN_RED;
    /// Candle strength: 0 unlit, 1 normal, above 1 flaring
    double light = 1;
    bool ornate = false;
    /// Tassel swing in radians
    double tassel = 0;
};

/// A paper lantern centred on the origin with a body of w x h: paper lit
/// from inside, bamboo ribs, lacquered caps and a tassel.
void paintLanternShape(QPainter &p, double w, double h, const LanternLook &look)
{
    const double lit = clamp01(look.light);
    const double flare = std::clamp(look.light - 1, 0.0, 1.0);
    const QColor paper = look.paper;
    const QColor dark = paper.darker(340);
    const double capW = w * (look.ornate ? 0.5 : 0.4);
    const double capH = std::max(1.0, w * 0.1);
    const double line = std::max(0.45, w * 0.028);
    const QRectF body(-w / 2, -h / 2, w, h);

    // Hot around the flame, deeper towards the curved edges where the paper
    // is seen at a grazing angle
    QRadialGradient fill(0, h * 0.14, std::max(w, h) * 0.6);
    fill.setColorAt(0.0, mixColor(mixColor(dark, paper, 0.16),
                                  mixColor(paper, HOT_CORE, 0.5 + 0.35 * flare),
                                  lit));
    fill.setColorAt(0.55, mixColor(mixColor(dark, paper, 0.08),
                                   mixColor(paper, WARM_LIGHT, 0.15), lit));
    fill.setColorAt(1.0, mixColor(dark.darker(130), paper.darker(175), lit));
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawEllipse(body);

    if (lit > 0.02)
    {
        drawGlow(p, QPointF(0, h * 0.16), w * (0.3 + 0.18 * flare), HOT_CORE,
                 0.5 * lit + 0.35 * flare);
    }

    // Bamboo ribs showing through the paper
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(withAlpha(paper.darker(260), 0.55), line));
    p.drawEllipse(QRectF(-w * 0.3, -h / 2, w * 0.6, h));
    p.drawLine(QPointF(0, -h / 2), QPointF(0, h / 2));
    p.setPen(QPen(withAlpha(paper.darker(240), 0.32), line * 0.8));
    constexpr int rings = 5;
    for (int i = 1; i < rings; ++i)
    {
        const double y = -h / 2 + h * i / rings;
        const double f = 2 * y / h;
        const double hw = w / 2 * std::sqrt(std::max(0.0, 1 - f * f));
        p.drawLine(QPointF(-hw, y), QPointF(hw, y));
    }
    p.setPen(QPen(withAlpha(dark, 0.9), line));
    p.drawEllipse(body);

    if (look.ornate)
    {
        // Painted gold bands around the body
        p.setPen(QPen(withAlpha(GOLD, 0.9), line * 1.7));
        for (const double fy : {-0.24, 0.24})
        {
            const double y = h * fy;
            const double f = 2 * y / h;
            const double hw = w / 2 * std::sqrt(std::max(0.0, 1 - f * f));
            p.drawArc(QRectF(-hw, y - h * 0.05, hw * 2, h * 0.1), 180 * 16,
                      180 * 16);
        }
    }

    // Lacquered caps with a gold edge
    p.setPen(QPen(withAlpha(GOLD, 0.75), std::max(0.4, line * 0.8)));
    p.setBrush(QColor(0x2a, 0x17, 0x12));
    p.drawRect(QRectF(-capW / 2, -h / 2 - capH * 0.7, capW, capH));
    p.drawRect(QRectF(-capW / 2, h / 2 - capH * 0.3, capW, capH));
    if (look.ornate)
    {
        p.drawRect(QRectF(-capW * 0.3, -h / 2 - capH * 1.55, capW * 0.6,
                          capH * 0.9));
        p.setBrush(GOLD);
        p.drawEllipse(QPointF(0, -h / 2 - capH * 2.05), capH * 0.45,
                      capH * 0.45);
    }

    // Tassel
    const bool reddish = paper.hueF() >= 0 &&
                         (paper.hueF() < 0.035 || paper.hueF() > 0.93);
    const QColor cord =
        (look.ornate || reddish) ? GOLD : LANTERN_RED.darker(110);
    const double len = h * (look.ornate ? 0.62 : 0.45) + 2;
    const double tw = std::max(0.6, w * 0.05);
    const auto tassel = [&](double x, double scale) {
        p.save();
        p.translate(x, h / 2 + capH * 0.7);
        p.rotate(deg(look.tassel));
        p.setPen(QPen(cord, std::max(0.5, w * 0.03)));
        p.drawLine(QPointF(0, 0), QPointF(0, len * 0.3 * scale));
        p.setPen(Qt::NoPen);
        p.setBrush(cord);
        const double knot = std::max(0.6, w * 0.045);
        p.drawEllipse(QPointF(0, len * 0.3 * scale), knot, knot);
        const QPointF tuft[] = {{-tw, len * 0.32 * scale},
                                {tw, len * 0.32 * scale},
                                {tw * 1.8, len * scale},
                                {-tw * 1.8, len * scale}};
        p.drawPolygon(tuft, 4);
        p.restore();
    };
    tassel(0, 1);
    if (look.ornate)
    {
        tassel(-capW * 0.45, 0.7);
        tassel(capW * 0.45, 0.7);
    }
}

/// Cached lantern sprites in three resolutions. The body (SPRITE_BODY wide)
/// is centred at (32, 48) of a 64 x 128 box in sprite units.
const QImage &lanternSprite(const QColor &paper, bool ornate, bool lit, int mip)
{
    static QHash<quint64, QImage> cache;
    const quint64 key = (quint64(paper.rgb() & 0xffffff) << 4) |
                        (ornate ? 1U : 0U) | (lit ? 2U : 0U) |
                        (quint64(mip) << 2);
    auto it = cache.find(key);
    if (it != cache.end())
    {
        return *it;
    }
    if (cache.size() > 160)
    {
        cache.clear();
    }
    constexpr std::array<double, 3> scales{0.3, 0.75, 2.0};
    const double k = scales[std::clamp(mip, 0, 2)];
    QImage img(std::max(1, iround(64 * k)), std::max(1, iround(128 * k)),
               QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(k, k);
    p.translate(32, 48);
    LanternLook look;
    look.paper = paper;
    look.light = lit ? 1.0 : 0.0;
    look.ornate = ornate;
    paintLanternShape(p, SPRITE_BODY, SPRITE_BODY * 1.04, look);
    p.end();
    return *cache.insert(key, img);
}

/// Draws a cached lantern with body width `w`, tilted around its centre.
/// `light` blends between unlit and lit paper.
void drawLantern(QPainter &p, QPointF center, double w, const QColor &paper,
                 double light, double tilt, double alpha, bool ornate = false)
{
    if (alpha <= 0.01 || w < 0.8)
    {
        return;
    }
    const double device = w * p.device()->devicePixelRatioF();
    const int mip = device > 30 ? 2 : (device > 12 ? 1 : 0);
    const double s = w / SPRITE_BODY;
    const QRectF box(-32 * s, -48 * s, 64 * s, 128 * s);
    p.save();
    p.translate(center);
    if (std::abs(tilt) > 0.002)
    {
        p.rotate(deg(tilt));
    }
    const double base = p.opacity();
    const double lit = clamp01(light);
    if (lit < 0.99)
    {
        p.setOpacity(base * alpha);
        p.drawImage(box, lanternSprite(paper, ornate, false, mip));
    }
    if (lit > 0.01)
    {
        p.setOpacity(base * alpha * lit);
        p.drawImage(box, lanternSprite(paper, ornate, true, mip));
    }
    p.restore();
}

/// The warm light a lantern throws around itself, drawn before its body.
void lanternHalo(QPainter &p, QPointF center, double w, double light,
                 double alpha)
{
    if (light <= 0.01)
    {
        return;
    }
    drawGlow(p, center, w * (1.5 + 0.5 * std::min(1.0, light)), WARM_LIGHT,
             alpha * 0.3 * std::min(1.6, light));
}

// World ----------------------------------------------------------------------

struct WindowLight {
    QRectF rect;
    QColor color = WARM_LIGHT;
    /// Order in which windows light up as the town fills
    int rank = 0;
    double glow = 0;
    double chatterUntil = -1;
    /// A small lantern hung under an eave rather than a window
    bool eave = false;
    bool nearRiver = false;
    /// Fully lit and painted into the cached composite
    bool baked = false;
};

struct Person {
    QPainterPath shape;
    QPointF hand;
    bool lantern = false;
    double phase = 0;
};

struct ReedClump {
    QPointF base;
    int sprite = 0;
    double scale = 1;
    double phase = 0;
    double flex = 1;
};

struct Layer {
    QImage image;
    /// Window position of the image's top-left corner
    QPointF origin;
    /// Parallax depth, 0 far away, 1 nearest
    double depth = 0;
};

/// One town layout: every cached paper layer plus the places events use.
/// Rebuilt when the window changes or a tab switch picks another town.
struct World {
    std::uint32_t seed = 0;
    QSizeF size;
    qreal dpr = 1;
    /// Size of the art relative to a 1280 x 760 window
    double s = 1;
    /// Where the town stands tall: behind the Activity pane when there is one
    QRectF stage;
    bool pane = false;
    double riverY = 0;

    Layer sky;
    Layer hills;
    Layer town;
    Layer bridgeBack;
    Layer bridgeFront;
    Layer river;
    Layer bank;
    QImage bridgeReflection;
    QPointF reflectionOrigin;

    QImage moonFull;
    QImage moonCrescent;
    QPointF moonSmallPos;
    double moonSmallR = 12;
    QPointF moonFrom;
    QPointF moonTo;
    double moonFullR = 40;

    std::vector<QImage> reedSprites;
    std::vector<ReedClump> reeds;
    std::vector<WindowLight> windows;
    /// Window indices sorted by rank
    std::vector<int> byRank;
    std::vector<Person> people;
    std::vector<QPointF> rooftops;
    std::vector<QPointF> twinkles;
    /// Hooks of the paper garland along the top edge
    std::vector<QPointF> garland;

    double bx0 = 0;
    double bx1 = 0;
    double deckBase = 0;
    double archH = 0;
    QPointF poleA;
    QPointF poleB;
    double sag = 0;
    int stringCount = 8;

    QImage composite;
    std::array<int, 9> compositeKey{};
    bool compositeValid = false;
    /// Bumped whenever the set of fully lit windows changes
    int lightsVersion = 0;

    double deckY(double x) const
    {
        const double f =
            clamp01((x - this->bx0) / std::max(1.0, this->bx1 - this->bx0));
        return this->deckBase -
               this->archH * std::pow(std::sin(f * TAU / 2), 0.85);
    }

    double bridgeMid() const
    {
        return (this->bx0 + this->bx1) / 2;
    }

    double hspan() const
    {
        return this->riverY - this->stage.top();
    }

    /// 1 under the stage, falling off to 0 away from it
    double bump(double x) const
    {
        const double sigma = this->stage.width() * 0.62 + 40;
        const double d = (x - this->stage.center().x()) / sigma;
        return std::exp(-d * d);
    }
};

/// An empty layer image covering `rect` in window coordinates.
Layer makePaperLayer(const World &w, const QRectF &rect, double depth)
{
    Layer layer;
    layer.origin = rect.topLeft();
    layer.depth = depth;
    layer.image = makeLayer(rect.size(), w.dpr);
    return layer;
}

double ridgeY(const World &w, double x, double low, double high, double phase,
              double rough)
{
    const double b = w.bump(x);
    const double n = valueNoise(x / 190.0 + phase, phase * 1.7) - 0.5;
    const double fine = valueNoise(x / 47.0 + phase * 2.3, phase + 9) - 0.5;
    return w.riverY - (low + (high - low) * b) - n * rough * (0.6 + b) -
           fine * rough * 0.35;
}

void buildSky(World &w, const SceneLayout &layout)
{
    const double W = w.size.width();
    const double H = w.size.height();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    w.sky.origin = QPointF(-M, -M);
    w.sky.depth = 0.06;
    w.sky.image = QImage(std::max(1, iround((W + 2 * M) * w.dpr)),
                         std::max(1, iround((H + 2 * M) * w.dpr)),
                         QImage::Format_RGB32);
    w.sky.image.setDevicePixelRatio(w.dpr);
    QPainter p(&w.sky.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(M, M);
    const QRectF all(-M, -M, W + 2 * M, H + 2 * M);

    QLinearGradient g(0, -M, 0, H + M);
    const auto at = [&](double y) {
        return clamp01((y + M) / (H + 2 * M));
    };
    g.setColorAt(0, SKY_TOP);
    g.setColorAt(at(H * 0.45), SKY_MID);
    g.setColorAt(at(w.riverY), SKY_LOW);
    g.setColorAt(1, SKY_LOW);
    p.fillRect(all, g);

    // The town's festival light warms the bottom of the sky everywhere
    QLinearGradient warm(0, w.riverY - H * 0.3, 0, w.riverY);
    warm.setColorAt(0, withAlpha(HORIZON_WARM, 0));
    warm.setColorAt(1, withAlpha(HORIZON_WARM, 0.22));
    p.fillRect(QRectF(-M, w.riverY - H * 0.3, W + 2 * M, H * 0.3 + M), warm);

    const auto inChat = [&](QPointF q) {
        return std::any_of(layout.chats.begin(), layout.chats.end(),
                           [&](const QRectF &r) {
                               return r.contains(q);
                           });
    };

    // Long cut-paper clouds, mostly over the stage and along the top
    SceneRng rng(w.seed ^ 0x51u);
    const int clouds = 3 + static_cast<int>(rng.next() * 2.5);
    for (int i = 0; i < clouds; ++i)
    {
        const double cx =
            w.stage.left() + w.stage.width() * rng.range(-0.35, 1.05);
        const double cy = w.stage.top() + w.hspan() * rng.range(0.05, 0.36);
        const double len = rng.range(120, 260) * s;
        const double alpha = inChat(QPointF(cx, cy)) ? 0.35 : 0.9;
        for (int layer = 0; layer < 2; ++layer)
        {
            // The back tone sits a little higher and to the side
            const double lx = cx + (layer == 0 ? len * 0.12 : 0);
            const double ly = cy - (layer == 0 ? 5 * s : 0);
            const double ll = len * (layer == 0 ? 0.7 : 1.0);
            // Scalloped top traced from overlapping puffs, flat cut base
            struct Puff {
                double x;
                double y;
                double rx;
                double ry;
            };
            std::vector<Puff> puffs;
            const int bumps = 4 + static_cast<int>(rng.next() * 4);
            for (int b = 0; b < bumps; ++b)
            {
                const double f = (b + 0.5) / bumps;
                const double r = rng.range(7, 15) * s *
                                 (0.45 + 0.55 * std::sin(f * TAU / 2));
                puffs.push_back({lx - ll / 2 + ll * f, ly - r * 0.35,
                                 std::max(r * 1.25, ll / bumps * 0.8), r});
            }
            const double baseY = ly + 2.5 * s;
            const double x0 = puffs.front().x - puffs.front().rx;
            const double x1 = puffs.back().x + puffs.back().rx;
            QPolygonF outline;
            outline << QPointF(x0, baseY);
            for (double x = x0; x <= x1; x += 2)
            {
                double top = baseY;
                for (const auto &puff : puffs)
                {
                    const double d = (x - puff.x) / puff.rx;
                    if (std::abs(d) < 1)
                    {
                        top = std::min(top, puff.y - puff.ry *
                                                         std::sqrt(1 - d * d));
                    }
                }
                outline << QPointF(x, top);
            }
            outline << QPointF(x1, baseY);
            const QPainterPath cloud = cutPolygon(
                outline, 3, 0.45, static_cast<int>(w.seed) + i * 17 + layer);
            const QColor tone =
                layer == 0 ? CLOUD_PAPER.darker(118) : CLOUD_PAPER;
            fillPaper(p, cloud, withAlpha(tone, alpha),
                      withAlpha(mixColor(tone, STAR_PAPER, 0.22), alpha), 1.0);
        }
    }

    // Paper stars: big cut stars outside chat, faint pinpricks inside it
    const int stars = iround(W * H / 6200);
    for (int i = 0; i < stars; ++i)
    {
        const QPointF q(rng.range(-M, W + M), rng.range(-M, w.riverY - 30));
        const bool quiet =
            inChat(q) || q.y() < layout.tabs.bottom() + 4 ||
            std::any_of(layout.headers.begin(), layout.headers.end(),
                        [&](const QRectF &r) {
                            return r.contains(q);
                        });
        double size = rng.range(0.7, 1.8) * s;
        if (!quiet && rng.next() < 0.3)
        {
            size = rng.range(2.4, 4.4) * s;
        }
        const double alpha =
            quiet ? rng.range(0.1, 0.28) : rng.range(0.35, 0.85);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(STAR_PAPER, alpha));
        if (size < 1.9)
        {
            p.drawEllipse(q, size * 0.5, size * 0.5);
            continue;
        }
        const int points = rng.next() < 0.5 ? 4 : 5;
        const double rot = rng.range(0, TAU);
        QPolygonF star;
        for (int k = 0; k < points * 2; ++k)
        {
            const double r = (k % 2 == 0) ? size : size * 0.42;
            const double a = rot + k * TAU / (points * 2);
            star << q + QPointF(std::cos(a) * r, std::sin(a) * r);
        }
        p.drawPolygon(star);
    }

    // Twinkling stars are drawn live, only where chat text isn't
    for (int i = 0; i < 60 && w.twinkles.size() < 14; ++i)
    {
        const QPointF q(rng.range(0, W),
                        rng.range(4, w.riverY - w.hspan() * 0.45));
        const bool overText =
            inChat(q) || q.y() < layout.tabs.bottom() + 4 ||
            std::any_of(layout.headers.begin(), layout.headers.end(),
                        [&](const QRectF &r) {
                            return r.contains(q);
                        });
        if (!overText)
        {
            w.twinkles.push_back(q);
        }
    }

    // Soft vignette, so the corners recede
    QRadialGradient vignette(W / 2, H * 0.55, std::hypot(W, H) * 0.62);
    vignette.setColorAt(0.55, withAlpha(SKY_TOP, 0));
    vignette.setColorAt(1.0, withAlpha(SKY_TOP, 0.55));
    p.fillRect(all, vignette);
    applyGrain(p, all, 0.45);
    p.end();

    // Moons: a small crescent normally, a large full moon when the festival
    // closes
    const auto makeMoon = [&](double r, bool crescent) {
        QImage img = makeLayer(QSizeF(2 * r + 8, 2 * r + 8), w.dpr);
        QPainter mp(&img);
        mp.setRenderHint(QPainter::Antialiasing);
        mp.translate(r + 4, r + 4);
        QPainterPath disc;
        disc.addEllipse(QPointF(0, 0), r, r);
        if (crescent)
        {
            QPainterPath bite;
            bite.addEllipse(QPointF(r * 0.45, -r * 0.2), r * 0.86, r * 0.86);
            disc = disc.subtracted(bite);
        }
        disc = cutShape(disc, 3, 0.35, static_cast<int>(w.seed) + 5);
        fillPaper(mp, disc, MOON_PAPER, STAR_PAPER.lighter(108), 0.8);
        if (!crescent)
        {
            SceneRng crater(w.seed ^ 0x77u);
            mp.setPen(Qt::NoPen);
            for (int i = 0; i < 7; ++i)
            {
                const double a = crater.range(0, TAU);
                const double d = crater.range(0, r * 0.62);
                const double cr = crater.range(0.08, 0.2) * r;
                mp.setBrush(withAlpha(QColor(0xb8, 0xa8, 0x8c), 0.35));
                mp.drawEllipse(QPointF(std::cos(a) * d, std::sin(a) * d), cr,
                               cr * 0.85);
            }
        }
        applyGrain(mp, QRectF(-r - 4, -r - 4, 2 * r + 8, 2 * r + 8), 1.0);
        mp.end();
        return img;
    };
    w.moonSmallR = 11 * s;
    w.moonSmallPos = QPointF(
        w.stage.left() + w.stage.width() * (0.62 + 0.25 * rnd(w.seed, 3)),
        w.stage.top() + w.hspan() * (0.07 + 0.08 * rnd(w.seed, 4)));
    w.moonCrescent = makeMoon(w.moonSmallR, true);
    w.moonFullR =
        std::clamp(std::min(w.stage.width() * 0.24, w.hspan() * 0.17), 30 * s,
                   120 * s);
    w.moonTo = QPointF(w.stage.center().x() + w.stage.width() * 0.08,
                       w.stage.top() + w.hspan() * 0.24);
    w.moonFrom =
        QPointF(w.moonTo.x(), w.riverY - w.hspan() * 0.2 + w.moonFullR);
    w.moonFull = makeMoon(w.moonFullR, false);
}

void buildHills(World &w)
{
    const double W = w.size.width();
    const double H = w.size.height();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    const double farHigh = w.pane ? w.hspan() * 0.44 : 120 * s;
    const double nearHigh = w.pane ? w.hspan() * 0.26 : 76 * s;
    const double phase = rnd(w.seed, 1) * 50;
    const double top = w.riverY - farHigh - 60 * s;
    const QRectF rect(-M, top, W + 2 * M, H + M - top);

    const auto ridge = [&](double low, double high, double ph, double rough,
                           int salt) {
        QPainterPath path(QPointF(-M, H + M));
        int i = 0;
        for (double x = -M; x <= W + M; x += 3, ++i)
        {
            double y = ridgeY(w, x, low, high, ph, rough);
            y += (rnd(w.seed, i, salt) - 0.5) * 1.1;
            if (rnd(w.seed, i, salt + 1) < 0.03)
            {
                y += 1.6;
            }
            path.lineTo(x, y);
        }
        path.lineTo(W + M, H + M);
        path.closeSubpath();
        return path;
    };

    Layer far = makePaperLayer(w, rect, 0.22);
    {
        QPainter p(&far.image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-rect.topLeft());
        QLinearGradient g(0, w.riverY - farHigh, 0, w.riverY);
        g.setColorAt(0, HILL_FAR.lighter(112));
        g.setColorAt(1, HILL_FAR.darker(125));
        // Sparse pines and a far-off shrine on the highest hill
        const auto farY = [&](double x) {
            return ridgeY(w, x, 70 * s, farHigh, phase, 58 * s);
        };
        QPainterPath farShape = ridge(70 * s, farHigh, phase, 58 * s, 11);
        QPainterPath extras;
        SceneRng frng(w.seed ^ 0x2fu);
        for (double x = -M; x < W + M; x += frng.range(9, 24) * s)
        {
            const double b = w.bump(x);
            if (frng.next() > 0.25 + 0.35 * b)
            {
                continue;
            }
            const double base = farY(x) + 2;
            const double th = frng.range(5, 10) * s * (0.6 + 0.5 * b);
            for (int k = 0; k < 2; ++k)
            {
                const double y0 = base - th * k * 0.35;
                const double hw = th * (0.3 - 0.08 * k);
                extras.addPolygon(QPolygonF({QPointF(x - hw, y0),
                                             QPointF(x + hw, y0),
                                             QPointF(x, y0 - th * 0.55)}));
                extras.closeSubpath();
            }
        }
        double peakX = w.stage.center().x();
        for (double x = w.stage.left(); x < w.stage.right(); x += 4)
        {
            if (farY(x) < farY(peakX))
            {
                peakX = x;
            }
        }
        const double peakY = farY(peakX) + 2 * s;
        extras.addRect(QRectF(peakX - 3 * s, peakY - 7 * s, 6 * s, 8 * s));
        extras.addPath(roofPath(peakX, peakY - 7 * s, 5.5 * s, 4 * s, 1.4 * s));
        extras.addRect(QRectF(peakX - 0.6 * s, peakY - 15 * s, 1.2 * s, 5 * s));
        farShape.addPath(cutShape(extras, 2.5, 0.3, static_cast<int>(w.seed) + 9));
        farShape.setFillRule(Qt::WindingFill);
        fillPaper(p, farShape, g,
                  mixColor(HILL_FAR.lighter(150), TOWN_RIM, 0.35), 1.8);
        applyGrain(p, rect, 0.8);
        // Far-off houses on the slopes, a few warm pinpricks
        p.setPen(Qt::NoPen);
        for (int k = 0; k < 14; ++k)
        {
            const double x = w.stage.left() +
                             w.stage.width() * frng.range(-0.4, 1.2);
            const double y = farY(x) + frng.range(8, 40) * s;
            drawGlow(p, QPointF(x, y), 4 * s, WARM_LIGHT, 0.35);
            p.setBrush(withAlpha(WARM_LIGHT, 0.7));
            p.drawRect(QRectF(x - 0.8 * s, y - 0.8 * s, 1.6 * s, 1.6 * s));
        }
        p.drawRect(QRectF(peakX - 0.9 * s, peakY - 5 * s, 1.8 * s, 2.4 * s));
        // Mist pooled at the foot of the far hills parts them from the near
        QLinearGradient mist(0, w.riverY - nearHigh * 1.5, 0, w.riverY);
        mist.setColorAt(0, withAlpha(HILL_FAR.lighter(150), 0));
        mist.setColorAt(1, withAlpha(HILL_FAR.lighter(150), 0.45));
        p.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        p.fillRect(rect, mist);
    }

    QImage nearImage = makeLayer(rect.size(), w.dpr);
    {
        QPainter p(&nearImage);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-rect.topLeft());
        QPainterPath near = ridge(36 * s, nearHigh, phase + 13, 34 * s, 21);
        // Pines and round trees cut along the near ridge
        SceneRng rng(w.seed ^ 0x3bu);
        QPainterPath trees;
        for (double x = -M; x < W + M; x += rng.range(7, 20) * s)
        {
            const double b = w.bump(x);
            if (rng.next() > 0.45 + 0.4 * b)
            {
                continue;
            }
            const double base =
                ridgeY(w, x, 36 * s, nearHigh, phase + 13, 34 * s) + 2;
            const double th = rng.range(8, 17) * s * (0.7 + 0.6 * b);
            if (rng.next() < 0.6)
            {
                for (int k = 0; k < 3; ++k)
                {
                    const double y0 = base - th * k * 0.3;
                    const double hw = th * (0.34 - 0.08 * k);
                    trees.addPolygon(QPolygonF({QPointF(x - hw, y0),
                                                QPointF(x + hw, y0),
                                                QPointF(x, y0 - th * 0.5)}));
                    trees.closeSubpath();
                }
            }
            else
            {
                trees.addEllipse(QPointF(x, base - th * 0.45), th * 0.36,
                                 th * 0.42);
                trees.addEllipse(QPointF(x + th * 0.25, base - th * 0.3),
                                 th * 0.28, th * 0.3);
            }
        }
        near.addPath(cutShape(trees, 2.5, 0.35, static_cast<int>(w.seed) + 3));
        near.setFillRule(Qt::WindingFill);
        QLinearGradient g(0, w.riverY - nearHigh, 0, w.riverY);
        g.setColorAt(0, HILL_NEAR.lighter(110));
        g.setColorAt(1, HILL_NEAR.darker(130));
        fillPaper(p, near, g, mixColor(HILL_NEAR.lighter(140), TOWN_RIM, 0.5),
                  1.6);
        applyGrain(p, rect, 0.8);
    }
    nearImage = withShadow(nearImage, QPointF(2.5 * s, 3.5 * s), 0.6, 6);
    {
        QPainter p(&far.image);
        p.drawImage(QPointF(0, 0), nearImage);
    }
    far.image = withShadow(far.image, QPointF(2 * s, 3 * s), 0.45, 8);
    w.hills = far;
}

/// A house: walls, upturned roof and dark windows that light up live.
void addHouse(World &w, QPainter &p, double cx, double base, double hw,
              double hh, const QColor &color, int row, int index)
{
    const double s = w.s;
    const std::uint32_t seed = w.seed + static_cast<std::uint32_t>(index * 7);
    const double b = w.bump(cx);
    const QRectF walls(cx - hw * 0.42, base - hh, hw * 0.84, hh + 2);
    const double rise = hh * 0.42 + 2 * s;
    QPainterPath body;
    body.addRect(walls);
    QPainterPath roof = roofPath(cx, base - hh, hw * 0.5, rise, 2.2 * s);
    // A second storey on some houses near the stage
    QPainterPath upper;
    QRectF upperWalls;
    if (b > 0.45 && rnd(seed, 1) < 0.35)
    {
        upperWalls = QRectF(cx - hw * 0.28, base - hh - rise - hh * 0.5,
                            hw * 0.56, hh * 0.5 + rise * 0.5);
        upper.addRect(upperWalls);
        upper.addPath(roofPath(cx, upperWalls.top(), hw * 0.36, rise * 0.8,
                               1.8 * s));
    }
    const int cut = static_cast<int>(seed);
    body = cutShape(body, 5, 0.35, cut);
    roof = cutShape(roof, 4, 0.35, cut + 1);
    if (!upper.isEmpty())
    {
        upper = cutShape(upper, 4, 0.35, cut + 2);
    }

    // A crisp shadow onto the row behind
    p.setPen(Qt::NoPen);
    const QPointF drop(2 * s, 2.5 * s);
    p.fillPath(body.translated(drop), withAlpha(SHADOW_INK, 0.35));
    p.fillPath(roof.translated(drop), withAlpha(SHADOW_INK, 0.35));
    const QColor rim = mixColor(color, TOWN_RIM, 0.55);
    if (!upper.isEmpty())
    {
        p.fillPath(upper.translated(drop), withAlpha(SHADOW_INK, 0.3));
        fillPaper(p, upper, mixColor(color, BRIDGE, 0.1), rim, 1.0);
    }
    fillPaper(p, body, color, rim, 1.0);
    fillPaper(p, roof, mixColor(color, BRIDGE, 0.14), rim, 1.2);

    const auto addWindow = [&](QRectF r, int salt) {
        p.fillRect(r, WINDOW_DARK);
        WindowLight light;
        light.rect = r;
        light.nearRiver = row == 0;
        light.color =
            mixColor(WARM_LIGHT, LANTERN_ORANGE, rnd(seed, salt, 9) * 0.4);
        w.windows.push_back(light);
    };
    const int count =
        1 + (hw > 34 * s ? 1 : 0) + (rnd(seed, 2) < 0.35 ? 1 : 0);
    const double ww = std::max(2.8, 4.6 * s);
    const double wh = std::max(3.4, 6.0 * s);
    for (int k = 0; k < count; ++k)
    {
        const double x = walls.left() + walls.width() * (k + 1) / (count + 1);
        if (hh < wh * 1.8)
        {
            break;
        }
        addWindow(QRectF(x - ww / 2, base - hh * 0.66, ww, wh), k);
    }
    if (!upperWalls.isEmpty())
    {
        addWindow(QRectF(cx - ww / 2,
                         upperWalls.top() + upperWalls.height() * 0.3, ww, wh),
                  5);
    }
    if (b > 0.3 && rnd(seed, 3) < 0.3)
    {
        WindowLight eave;
        eave.eave = true;
        eave.rect =
            QRectF(cx - hw * 0.46, base - hh + 1.5 * s, 3.2 * s, 4.2 * s);
        eave.color = LANTERN_RED;
        w.windows.push_back(eave);
    }
    w.rooftops.push_back(QPointF(cx, base - hh - rise));
}

/// A sagging cord between two eaves hung with small lanterns that light
/// with the rest of the town.
void addFestivalString(World &w, QPainter &p, QPointF a, QPointF b, int index)
{
    const double s = w.s;
    const double gap = std::abs(b.x() - a.x());
    if (gap < 6 * s || gap > 110 * s)
    {
        return;
    }
    const double sag = 3 * s + gap * 0.12;
    const auto along = [&](double f) {
        return QPointF(lerp(a.x(), b.x(), f),
                       lerp(a.y(), b.y(), f) + sag * 4 * f * (1 - f));
    };
    QPolygonF cord;
    for (int k = 0; k <= 10; ++k)
    {
        cord << along(k / 10.0);
    }
    p.setPen(QPen(withAlpha(TOWN.darker(130), 0.9), 0.7 * s));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(cord);
    const int count = std::clamp(static_cast<int>(gap / (9 * s)), 2, 7);
    const std::array<QColor, 3> papers{LANTERN_RED, LANTERN_ORANGE,
                                       QColor(0xf2, 0xc0, 0x5a)};
    for (int k = 0; k < count; ++k)
    {
        const QPointF at = along((k + 1.0) / (count + 1));
        WindowLight light;
        light.eave = true;
        light.rect = QRectF(at.x() - 1.3 * s, at.y() + 0.6 * s, 2.6 * s, 3.3 * s);
        light.color = papers[static_cast<size_t>((index + k) % 3)];
        p.setPen(Qt::NoPen);
        p.setBrush(light.color.darker(260));
        p.drawEllipse(light.rect);
        w.windows.push_back(light);
    }
}

void buildTown(World &w)
{
    const double W = w.size.width();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    const double hspan = w.hspan();
    const int rows = w.pane ? 4 : 2;
    const double rowStep = w.pane ? hspan * 0.055 : 9 * s;

    const double px =
        w.stage.left() + w.stage.width() * (0.26 + 0.46 * rnd(w.seed, 21));
    const int tiers = 3 + static_cast<int>(rnd(w.seed, 22) * 3.99);
    const double pagodaH = w.pane ? hspan * 0.5 : 110 * s;
    const double pagodaBase =
        w.riverY - (rows - 1 + 0.4) * rowStep * (0.35 + 0.65 * w.bump(px));
    const double baseW = std::clamp(w.stage.width() * 0.25, 44 * s, 118 * s);
    const double top = pagodaBase - pagodaH - 24 * s;
    const QRectF rect(-M, top, W + 2 * M, w.riverY + 8 - top);
    w.town = makePaperLayer(w, rect, 0.4);
    QPainter p(&w.town.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-rect.topLeft());

    SceneRng rng(w.seed ^ 0x7f1u);
    int index = 0;
    const auto paintRow = [&](int r) {
        const QColor color = mixColor(TOWN, TOWN_BACK, double(r) / rows);
        // Round trees behind the row
        for (double x = -M; x < W + M; x += rng.range(30, 90) * s)
        {
            const double b = w.bump(x);
            if (r > 0 && b < 0.3 + 0.16 * r)
            {
                continue;
            }
            const double base = w.riverY - r * rowStep * (0.35 + 0.65 * b);
            const double tr = rng.range(5, 10) * s * (0.8 + 0.5 * b);
            QPainterPath tree;
            tree.addEllipse(QPointF(x, base - tr * 1.6), tr, tr * 1.1);
            tree.addEllipse(QPointF(x + tr * 0.7, base - tr * 1.1), tr * 0.75,
                            tr * 0.8);
            tree.addRect(QRectF(x - 0.8 * s, base - tr, 1.6 * s, tr));
            fillPaper(p, cutShape(tree.simplified(), 3, 0.4, index++),
                      color.lighter(106), mixColor(color, TOWN_RIM, 0.4), 1.0);
        }
        double x = -M - rng.range(0, 20);
        std::optional<QPointF> lastTip;
        while (x < W + M)
        {
            const double hw = rng.range(22, 52) * s;
            const double cx = x + hw / 2;
            const double b = w.bump(cx);
            if (r > 0 && b < 0.3 + 0.16 * r)
            {
                x += hw * 0.8;
                lastTip.reset();
                continue;
            }
            const double base = w.riverY - r * rowStep * (0.35 + 0.65 * b);
            const double hh = rng.range(10, 24) * s * (0.7 + 0.5 * b);
            addHouse(w, p, cx, base, hw, hh, color, r, index++);
            const double curl = 2.2 * s;
            const QPointF tipL(cx - hw * 0.5 - curl * 0.6, base - hh - curl);
            const QPointF tipR(cx + hw * 0.5 + curl * 0.6, base - hh - curl);
            if (lastTip && r <= 1 && b > 0.3 && rng.next() < 0.8)
            {
                addFestivalString(w, p, *lastTip, tipL, index++);
            }
            lastTip = tipR;
            x += hw + rng.range(-3, 6) * s;
        }
    };

    for (int r = rows - 1; r >= 0; --r)
    {
        paintRow(r);
        if (r != rows - 1)
        {
            continue;
        }
        // The pagoda stands in the back row, the tallest thing in town
        const double spire = pagodaH * 0.14;
        const double th = (pagodaH - spire) / tiers;
        const QColor color = mixColor(TOWN_BACK, TOWN, 0.4);
        const QColor rim = mixColor(color, TOWN_RIM, 0.65);
        double roofTop = pagodaBase;
        for (int t = 0; t < tiers; ++t)
        {
            const double tw = baseW * (1 - 0.4 * double(t) / tiers);
            const double yb = pagodaBase - t * th;
            const QRectF body(px - tw * 0.34, yb - th * 0.6, tw * 0.68,
                              th * 0.6 + 2);
            QPainterPath tier;
            tier.addRect(body);
            tier = cutShape(tier, 5, 0.3, static_cast<int>(w.seed) + 40 + t);
            QPainterPath roof = cutShape(
                roofPath(px, body.top(), tw * 0.6, th * 0.42, th * 0.2), 4,
                0.3, static_cast<int>(w.seed) + 60 + t);
            const QPointF drop(2 * s, 3 * s);
            p.fillPath(roof.translated(drop), withAlpha(SHADOW_INK, 0.4));
            fillPaper(p, tier, color, rim, 1.0);
            fillPaper(p, roof, mixColor(color, BRIDGE, 0.2), rim, 1.3);
            const int windows = t == 0 ? 3 : (tw > 60 * s ? 2 : 1);
            for (int k = 0; k < windows; ++k)
            {
                const double wx =
                    body.left() + body.width() * (k + 1) / (windows + 1);
                const double ww = std::max(2.6, tw * 0.05);
                const QRectF hole(wx - ww / 2,
                                  body.top() + body.height() * 0.3, ww,
                                  body.height() * 0.34);
                p.fillRect(hole, WINDOW_DARK);
                WindowLight light;
                light.rect = hole;
                light.rank = -100 + t * 3 + k;
                w.windows.push_back(light);
            }
            // Eave lanterns hang from the roof corners
            for (const double side : {-1.0, 1.0})
            {
                WindowLight eave;
                eave.eave = true;
                eave.rank = -60 + t;
                eave.color = LANTERN_RED;
                eave.rect = QRectF(px + side * tw * 0.55 - 1.6 * s,
                                   body.top() + 1.5 * s, 3.2 * s, 4.2 * s);
                w.windows.push_back(eave);
            }
            roofTop = body.top() - th * 0.42;
        }
        QPainterPath tip;
        tip.addRect(QRectF(px - 1.2 * s, roofTop - spire, 2.4 * s, spire + 2));
        for (int k = 0; k < 4; ++k)
        {
            const double ry = roofTop - spire * (0.25 + 0.17 * k);
            tip.addEllipse(QPointF(px, ry), 3.4 * s - k * 0.5 * s, 1.1 * s);
        }
        tip.addEllipse(QPointF(px, roofTop - spire - 1.5 * s), 2 * s, 2 * s);
        fillPaper(p, tip.simplified(), color, rim, 0.8);
        w.rooftops.push_back(QPointF(px, roofTop - spire));
    }

    // Lights come on in a stable shuffled order, pagoda and eaves first
    SceneRng order(w.seed ^ 0x99u);
    for (auto &light : w.windows)
    {
        light.rank = light.rank * 1000 + iround(order.next() * 999);
    }
    std::vector<int> idx(w.windows.size());
    for (size_t i = 0; i < idx.size(); ++i)
    {
        idx[i] = static_cast<int>(i);
    }
    std::sort(idx.begin(), idx.end(), [&](int a, int b) {
        return w.windows[a].rank < w.windows[b].rank;
    });
    for (size_t i = 0; i < idx.size(); ++i)
    {
        w.windows[idx[i]].rank = static_cast<int>(i);
    }
    w.byRank = idx;

    applyGrain(p, rect, 0.75);
    p.end();
    w.town.image = withShadow(w.town.image, QPointF(2 * s, 3 * s), 0.5, 6);
}

void buildBridge(World &w)
{
    const double H = w.size.height();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    const double span = w.bx1 - w.bx0;
    const double mid = w.bridgeMid();
    const double top = w.poleA.y() - 16 * s;
    const double foot = w.riverY + 4;

    QPainterPath body;
    {
        QPainterPath deck(QPointF(w.bx0 - 16 * s, foot));
        for (double x = w.bx0 - 16 * s; x <= w.bx1 + 16 * s; x += 3)
        {
            deck.lineTo(x, w.deckY(x));
        }
        deck.lineTo(w.bx1 + 16 * s, w.deckY(w.bx1));
        deck.lineTo(w.bx1 + 16 * s, foot);
        deck.closeSubpath();
        const double ry = foot - (w.deckY(mid) + 9 * s);
        QPainterPath opening;
        opening.addEllipse(QPointF(mid, foot), span * 0.36, ry);
        body = deck.subtracted(opening);
    }
    body = cutShape(body, 4, 0.4, static_cast<int>(w.seed) + 300);
    const double rx = span * 0.36;
    const double ry = foot - (w.deckY(mid) + 9 * s);

    // Back: the arched body and the far railing
    const QRectF backRect(w.bx0 - 34 * s, top, span + 68 * s, foot + 6 - top);
    w.bridgeBack = makePaperLayer(w, backRect, 0.58);
    {
        QPainter p(&w.bridgeBack.image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-backRect.topLeft());
        QPainterPath far;
        for (double x = w.bx0; x <= w.bx1 + 0.1; x += 3)
        {
            const QPointF q(x, w.deckY(x) - 12 * s);
            if (x == w.bx0)
            {
                far.moveTo(q);
            }
            else
            {
                far.lineTo(q);
            }
        }
        p.setPen(QPen(RAIL.darker(130), 1.6 * s));
        p.setBrush(Qt::NoBrush);
        p.drawPath(far);
        for (double x = w.bx0 + 11 * s; x < w.bx1; x += 15 * s)
        {
            p.drawLine(QPointF(x, w.deckY(x) - 12 * s),
                       QPointF(x, w.deckY(x)));
        }
        QLinearGradient g(0, w.deckY(mid), 0, foot);
        g.setColorAt(0, BRIDGE.lighter(108));
        g.setColorAt(1, BRIDGE.darker(150));
        fillPaper(p, body, g, BRIDGE_RIM, 1.4);
        // Arch stones and a gold trim along the deck face
        p.setPen(QPen(withAlpha(BRIDGE.lighter(160), 0.5), 0.9 * s));
        for (int k = 1; k < 18; ++k)
        {
            const double a = TAU / 2 * k / 18.0;
            const QPointF dir(std::cos(a), -std::sin(a));
            p.drawLine(QPointF(mid + dir.x() * rx, foot + dir.y() * ry),
                       QPointF(mid + dir.x() * (rx + 5 * s),
                               foot + dir.y() * (ry + 5 * s)));
        }
        QPainterPath trim;
        for (double x = w.bx0 - 10 * s; x <= w.bx1 + 10 * s; x += 3)
        {
            const QPointF q(x, w.deckY(x) + 3.5 * s);
            if (trim.isEmpty())
            {
                trim.moveTo(q);
            }
            else
            {
                trim.lineTo(q);
            }
        }
        p.setPen(QPen(withAlpha(GOLD, 0.5), 1.0 * s));
        p.drawPath(trim);
        applyGrain(p, backRect, 0.8);
    }
    w.bridgeBack.image =
        withShadow(w.bridgeBack.image, QPointF(2 * s, 3 * s), 0.5, 6);

    // Front: railing posts, rails and the two poles of the lantern string
    const QRectF frontRect(w.bx0 - 24 * s, top, span + 48 * s,
                           w.deckBase + 8 - top);
    w.bridgeFront = makePaperLayer(w, frontRect, 0.58);
    {
        QPainter p(&w.bridgeFront.image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-frontRect.topLeft());
        QPainterPath rail;
        rail.setFillRule(Qt::WindingFill);
        for (double x = w.bx0 + 4 * s; x <= w.bx1 - 4 * s + 0.1; x += 15 * s)
        {
            const double y = w.deckY(x);
            rail.addRect(QRectF(x - 1.3 * s, y - 13 * s, 2.6 * s, 13 * s + 1));
            rail.addEllipse(QPointF(x, y - 13.5 * s), 1.9 * s, 1.9 * s);
        }
        const auto band = [&](double off, double thick) {
            QPolygonF poly;
            for (double x = w.bx0 + 4 * s; x <= w.bx1 - 4 * s + 0.1; x += 3)
            {
                poly << QPointF(x, w.deckY(x) - off);
            }
            for (double x = w.bx1 - 4 * s; x >= w.bx0 + 4 * s - 0.1; x -= 3)
            {
                poly << QPointF(x, w.deckY(x) - off + thick);
            }
            rail.addPolygon(poly);
            rail.closeSubpath();
        };
        band(11.5 * s, 2.2 * s);
        band(6 * s, 1.4 * s);
        for (const QPointF pole : {w.poleA, w.poleB})
        {
            rail.addRect(QRectF(pole.x() - 1.7 * s, pole.y(), 3.4 * s,
                                w.deckBase - pole.y()));
            rail.addPath(roofPath(pole.x(), pole.y() + 1, 4.5 * s, 4 * s,
                                  1.6 * s));
        }
        rail = cutShape(rail, 4, 0.3, static_cast<int>(w.seed) + 400);
        fillPaper(p, rail, RAIL, mixColor(RAIL, BRIDGE_RIM, 0.7), 1.0);
        applyGrain(p, frontRect, 0.8);
    }
    w.bridgeFront.image =
        withShadow(w.bridgeFront.image, QPointF(1.5 * s, 2.5 * s), 0.5, 5);

    // Its reflection completes the moon-bridge circle in the water
    const double ry0 = w.riverY + 3;
    const QRectF refRect(w.bx0 - 34 * s, ry0, span + 68 * s,
                         std::max(4.0, H + M - ry0));
    w.bridgeReflection = makeLayer(refRect.size(), w.dpr);
    w.reflectionOrigin = refRect.topLeft();
    {
        QPainter p(&w.bridgeReflection);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-refRect.topLeft());
        const QTransform mirror(1, 0, 0, -1, 0, 2 * ry0);
        p.fillPath(mirror.map(body),
                   withAlpha(mixColor(BRIDGE, WATER_HIGH, 0.35), 0.8));
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        QLinearGradient fade(0, ry0, 0, ry0 + (ry0 - top) * 0.7);
        fade.setColorAt(0, QColor(0, 0, 0, 230));
        fade.setColorAt(1, QColor(0, 0, 0, 20));
        p.fillRect(refRect, fade);
    }
}

void buildRiver(World &w)
{
    const double W = w.size.width();
    const double H = w.size.height();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    const double top = w.riverY - 4;
    const QRectF rect(-M, top, W + 2 * M, H + M - top);
    w.river = makePaperLayer(w, rect, 0.7);
    QPainter p(&w.river.image);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-rect.topLeft());
    const double depth = H - w.riverY;
    const std::array<double, 3> tops{0, 0.3, 0.62};
    const std::array<QColor, 3> colors{WATER_HIGH, WATER_MID, WATER_DEEP};
    for (int band = 0; band < 3; ++band)
    {
        const double y0 = w.riverY + depth * tops[band];
        const double amp = (band == 0 ? 0.8 : 2.2) * s;
        const double wave = (70 + 40 * band) * s;
        const double ph = rnd(w.seed, 50 + band) * TAU;
        QPainterPath path(QPointF(-M, H + M));
        int i = 0;
        for (double x = -M; x <= W + M; x += 3, ++i)
        {
            const double y = y0 + std::sin(x / wave * TAU + ph) * amp +
                             (rnd(w.seed, i, 60 + band) - 0.5) * 0.9;
            path.lineTo(x, y);
        }
        path.lineTo(W + M, H + M);
        path.closeSubpath();
        fillPaper(p, path, colors[band],
                  mixColor(colors[band], STAR_PAPER, 0.16), 1.0);
    }
    // The far bank where the town meets the water
    p.fillRect(QRectF(-M, w.riverY - 1, W + 2 * M, 2.2 * s),
               withAlpha(TOWN.darker(150), 0.9));
    // Cut slivers of reflected sky
    SceneRng rng(w.seed ^ 0x4cu);
    const int slivers = iround(W / 14);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < slivers; ++i)
    {
        const double f = rng.next();
        const double y = w.riverY + 4 + f * (depth - 6);
        const double x = rng.range(-M, W + M);
        const double len = rng.range(6, 26) * s * (1 - 0.4 * f);
        const double warm = w.bump(x);
        p.setBrush(withAlpha(mixColor(STAR_PAPER, WARM_LIGHT, warm),
                             rng.range(0.05, 0.13) + 0.12 * warm));
        p.drawEllipse(QPointF(x, y), len / 2, 0.6 * s);
    }
    applyGrain(p, rect, 0.6);
}

void buildBank(World &w, const SceneLayout &layout)
{
    const double W = w.size.width();
    const double H = w.size.height();
    const double M = LAYER_MARGIN;
    const double s = w.s;
    const double top = H - 30 * s;
    const QRectF rect(-M, top, W + 2 * M, H + M - top);
    w.bank = makePaperLayer(w, rect, 1.0);
    {
        QPainter p(&w.bank.image);
        p.setRenderHint(QPainter::Antialiasing);
        p.translate(-rect.topLeft());
        const double ph = rnd(w.seed, 70) * 40;
        QPainterPath path(QPointF(-M, H + M));
        int i = 0;
        for (double x = -M; x <= W + M; x += 3, ++i)
        {
            const double y =
                H - (3 + 8 * valueNoise(x / 90.0 + ph, 3.3)) * s;
            path.lineTo(x, y);
            if (rnd(w.seed, i, 71) < 0.3)
            {
                path.lineTo(x + 1.5, y - rnd(w.seed, i, 72) * 6 * s);
            }
        }
        path.lineTo(W + M, H + M);
        path.closeSubpath();
        fillPaper(p, path, BANK, withAlpha(REED_RIM, 0.7), 1.0);
        applyGrain(p, rect, 0.7);
    }
    w.bank.image = withShadow(w.bank.image, QPointF(1.5 * s, -1.5 * s), 0.6, 6);

    // Reed sprites: blades and cattails cut from dark paper
    const QSizeF size(40 * s, 76 * s);
    for (int k = 0; k < 6; ++k)
    {
        QImage img = makeLayer(size, w.dpr);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        SceneRng rng(w.seed ^ (0x100u + static_cast<std::uint32_t>(k)));
        const double bx = size.width() / 2;
        const double by = size.height();
        QPainterPath blades;
        blades.setFillRule(Qt::WindingFill);
        QPainterPath heads;
        const int count = 3 + static_cast<int>(rng.next() * 4);
        for (int i = 0; i < count; ++i)
        {
            const double h = rng.range(0.5, 0.97) * (by - 4);
            const double lean = rng.range(-0.3, 0.3) * h;
            const double x0 = bx + rng.range(-5, 5) * s;
            const double wb = rng.range(1.4, 2.4) * s;
            QPainterPath blade(QPointF(x0 - wb, by));
            blade.quadTo(QPointF(x0 - wb * 0.5 + lean * 0.2, by - h * 0.6),
                         QPointF(x0 + lean, by - h));
            blade.quadTo(QPointF(x0 + wb * 0.5 + lean * 0.25, by - h * 0.55),
                         QPointF(x0 + wb, by));
            blade.closeSubpath();
            blades.addPath(blade);
            if (rng.next() < 0.4)
            {
                const double ch = std::min(by - 6 * s, h * 1.05);
                const double cx = x0 + lean * 0.25;
                blades.addPolygon(QPolygonF({QPointF(x0 - 0.6 * s, by),
                                             QPointF(x0 + 0.6 * s, by),
                                             QPointF(cx + 0.4 * s, by - ch),
                                             QPointF(cx - 0.4 * s, by - ch)}));
                blades.closeSubpath();
                heads.addEllipse(QPointF(cx, by - ch + 5.5 * s), 1.7 * s,
                                 5.5 * s);
            }
        }
        fillPaper(p, cutShape(blades, 3, 0.25, k * 31), REED, REED_RIM, 0.9);
        fillPaper(p, heads, QColor(0x24, 0x17, 0x14), QColor(0x5a, 0x40, 0x30),
                  0.8);
        applyGrain(p, QRectF(QPointF(0, 0), size), 0.7);
        p.end();
        w.reedSprites.push_back(img);
    }

    SceneRng rng(w.seed ^ 0x5eu);
    const QRectF in = layout.focusInput;
    for (double x = -10; x < W + 10; x += rng.range(16, 36) * s)
    {
        ReedClump c;
        c.base = QPointF(x, H + 2);
        c.sprite = static_cast<int>(rng.next() * 5.99);
        double scale = rng.range(0.55, 0.85) + 0.4 * w.bump(x);
        if (!in.isEmpty() && x > in.left() + 12 && x < in.right() - 12)
        {
            scale *= 0.38;
        }
        if (x < 40 * s || x > W - 40 * s)
        {
            scale *= 1.2;
        }
        c.scale = scale;
        c.phase = rng.range(0, TAU);
        c.flex = rng.range(0.7, 1.2);
        w.reeds.push_back(c);
    }
}

/// Paper silhouettes standing on the bridge, in the order they arrive.
void buildCrowd(World &w)
{
    const double s = w.s;
    const double span = w.bx1 - w.bx0;
    const int slotCount = std::clamp(iround((span - 16 * s) / (9 * s)), 4, 44);
    SceneRng rng(w.seed ^ 0xc0u);
    for (int i = 0; i < slotCount; ++i)
    {
        const double x = w.bx0 + 8 * s + (span - 16 * s) * (i + 0.5) / slotCount +
                         rng.range(-2, 2) * s;
        const double feet = w.deckY(x) - 1;
        const bool kid = rng.next() < 0.18;
        const double h = (kid ? 12 : rng.range(17, 22)) * s;
        const double head = h * 0.13;
        const double sw = h * 0.34;
        const double bw = h * 0.44;
        QPainterPath shape;
        shape.addPolygon(QPolygonF({QPointF(x - sw / 2, feet - h * 0.76),
                                    QPointF(x + sw / 2, feet - h * 0.76),
                                    QPointF(x + bw / 2, feet),
                                    QPointF(x - bw / 2, feet)}));
        shape.closeSubpath();
        shape.addEllipse(QPointF(x, feet - h * 0.88), head, head * 1.08);
        if (!kid && rng.next() < 0.2)
        {
            shape.addPolygon(QPolygonF({QPointF(x - head * 2.3, feet - h * 0.9),
                                        QPointF(x + head * 2.3, feet - h * 0.9),
                                        QPointF(x, feet - h * 1.08)}));
            shape.closeSubpath();
        }
        Person person;
        person.shape = shape;
        person.shape.setFillRule(Qt::WindingFill);
        person.hand = QPointF(x + sw * 0.55, feet - h * 0.55);
        person.lantern = rng.next() < 0.5;
        person.phase = rng.range(0, TAU);
        w.people.push_back(person);
    }
    for (int i = static_cast<int>(w.people.size()) - 1; i > 0; --i)
    {
        const int j = static_cast<int>(rng.next() * (i + 1)) % (i + 1);
        std::swap(w.people[i], w.people[j]);
    }
}

std::unique_ptr<World> buildWorld(const SceneLayout &layout,
                                  std::uint32_t seed)
{
    auto w = std::make_unique<World>();
    w->seed = seed;
    w->size = layout.window;
    w->dpr = std::max<qreal>(1.0, layout.dpr);
    const double W = layout.window.width();
    const double H = layout.window.height();
    w->s = std::clamp(std::min(H / 760.0, W / 1280.0), 0.72, 1.5);
    const double s = w->s;

    QRectF stage;
    for (const auto &r : layout.activity)
    {
        if (r.width() * r.height() > stage.width() * stage.height())
        {
            stage = r;
        }
    }
    w->pane = stage.width() > 120 && stage.height() > 160;
    const double top = std::max(0.0, layout.tabs.bottom());
    if (!w->pane)
    {
        stage = QRectF(W * 0.7, top, W * 0.3, H - top);
    }
    w->stage = stage;
    w->riverY = H - std::clamp(H * 0.105, 46.0, 120.0);

    // Bridge geometry first, the town and river build around it
    const double inset = stage.width() * 0.07;
    w->bx0 = stage.left() + inset;
    w->bx1 = stage.right() - inset;
    const double span = w->bx1 - w->bx0;
    w->deckBase = w->riverY - 3 * s;
    w->archH = std::min(span * 0.15, w->hspan() * 0.1);
    const double poleH = w->archH + 66 * s;
    w->poleA = QPointF(w->bx0 + 5 * s, w->deckBase - poleH);
    w->poleB = QPointF(w->bx1 - 5 * s, w->deckBase - poleH);
    w->sag = std::min(span * 0.1, 30 * s);
    w->stringCount = std::clamp(iround(span / (34 * s)), 5, 12);

    buildSky(*w, layout);
    buildHills(*w);
    buildTown(*w);
    buildBridge(*w);
    buildRiver(*w);
    buildBank(*w, layout);
    buildCrowd(*w);

    // A paper garland of small lanterns along the top, never over tab names.
    // It hangs from the window's top edge when the tab strip starts there,
    // otherwise (a title bar with text above the tabs) from the top of the
    // tab strip, right of the tab names.
    const QRectF labels = layout.tabLabels;
    const bool titleBar = !layout.tabs.isEmpty() && layout.tabs.top() > 8;
    const double hookY = titleBar ? layout.tabs.top() - 1 : -1;
    const double drop = 30 * s;
    double gx = W * 0.48;
    if (!labels.isEmpty() && labels.top() < hookY + drop &&
        labels.bottom() > hookY)
    {
        gx = std::max(gx, labels.right() + 16 * s);
    }
    if (titleBar && !labels.isEmpty())
    {
        gx = std::max(gx, labels.right() + 16 * s);
    }
    if (W - gx > 110 * s)
    {
        const int swags = std::max(1, iround((W - gx) / (150 * s)));
        for (int k = 0; k <= swags; ++k)
        {
            w->garland.push_back(
                QPointF(gx + (W + 8 - gx) * k / swags, hookY));
        }
    }
    return w;
}

/// The cut-paper curtain that sweeps across on a tab switch.
QImage buildCurtain(QSizeF size, qreal dpr, double s, double &width,
                    double &margin)
{
    const double H = size.height();
    const double cw = std::clamp(size.width() * 0.3, 200.0, 480.0);
    const double M = 24 * s;
    width = cw;
    margin = M;
    QImage img = makeLayer(QSizeF(cw + 2 * M, H), dpr);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(M, 0);
    const double r = 9 * s;
    QPainterPath shape(QPointF(0, 0));
    shape.lineTo(cw, 0);
    for (double y = 0; y < H; y += 2 * r)
    {
        shape.arcTo(QRectF(cw - r, y, 2 * r, 2 * r), 90, -180);
    }
    shape.lineTo(cw, H);
    shape.lineTo(0, H);
    for (double y = H; y > 0; y -= 2 * r)
    {
        shape.arcTo(QRectF(-r, y - 2 * r, 2 * r, 2 * r), 270, -180);
    }
    shape.closeSubpath();
    shape = cutShape(shape, 4, 0.4, 909);
    p.setClipPath(shape);
    p.fillRect(QRectF(-M, 0, cw + 2 * M, H), CURTAIN);
    // Folded pleats
    const double pleat = 26 * s;
    for (double x = -r; x < cw + r; x += pleat)
    {
        QLinearGradient g(x, 0, x + pleat, 0);
        g.setColorAt(0, CURTAIN.darker(150));
        g.setColorAt(0.45, CURTAIN.lighter(118));
        g.setColorAt(1, CURTAIN.darker(150));
        p.fillRect(QRectF(x, 0, pleat, H), g);
    }
    // Gold paper bands with cut diamonds
    for (const double y : {H * 0.08, H - 64 * s})
    {
        p.fillRect(QRectF(-r, y, cw + 2 * r, 9 * s), withAlpha(GOLD, 0.75));
        p.setPen(Qt::NoPen);
        p.setBrush(CURTAIN.darker(130));
        for (double x = 6 * s; x < cw; x += 14 * s)
        {
            const QPointF c(x, y + 4.5 * s);
            const QPointF d[] = {c + QPointF(0, -3 * s), c + QPointF(3 * s, 0),
                                 c + QPointF(0, 3 * s), c + QPointF(-3 * s, 0)};
            p.drawPolygon(d, 4);
        }
    }
    applyGrain(p, QRectF(-M, 0, cw + 2 * M, H), 0.9);
    p.end();
    return withShadow(img, QPointF(7 * s, 0), 0.7, 6);
}

// Scene ----------------------------------------------------------------------

enum class Kind : std::uint8_t {
    /// Drifts slowly over the town
    Ambient,
    /// Tiny and far away, rises from the town with the chat speed
    Distant,
    /// Released by a click or a follow
    Free,
    Gift,
    Sub,
    /// Its flame went out (timeout)
    Falling,
    Mention,
    Shout,
};

struct Lantern {
    int id = 0;
    Kind kind = Kind::Free;
    QPointF pos;
    QPointF vel;
    QPointF from;
    QPointF target;
    double size = 10;
    QColor color = LANTERN_ORANGE;
    double age = 0;
    double life = 12;
    double delay = 0;
    double light = 1;
    double phase = 0;
    double tilt = 0;
    double tiltVel = 0;
    double hover = 0;
    bool ornate = false;
    /// Also drawn above the panels for its first moments
    bool front = false;
    QString tag;
};

/// Your sent lantern on its way up
struct Flyer {
    QPointF start;
    QPointF c1;
    QPointF c2;
    QPointF end;
    QPointF pos;
    QColor color = LANTERN_ORANGE;
    double age = 0;
    double duration = 12;
    double size0 = 26;
    double size1 = 10;
    double aspect0 = 1.04;
    double aspect = 1.04;
    double size = 26;
    double tilt = 0;
    double phase = 0;
    double light = 1.5;
    double u = 0;
    bool done = false;
};

/// A sent lantern resting in the constellation
struct Star {
    int id = 0;
    QPointF pos;
    QColor color = LANTERN_ORANGE;
    double size = 10;
    double age = 0;
    double life = 100;
    double phase = 0;
    double hover = 0;
};

struct Spark {
    QPointF pos;
    QPointF vel;
    QColor color = GOLD;
    double age = 0;
    double life = 1;
    double size = 2;
    double angle = 0;
    double spin = 0;
    double gravity = 90;
    bool front = true;
};

struct Boat {
    double startX = 0;
    double targetX = 0;
    double travel = 8;
    double age = 0;
    double life = 34;
    double phase = 0;
    double size = 1;
    QColor color = LANTERN_ORANGE;
};

struct Firework {
    QPointF center;
    QColor a = LANTERN_RED;
    QColor b = GOLD;
    double age = 0;
    double delay = 0;
    double life = 2.1;
    double radius = 36;
    double rot = 0;
    int petals = 14;
};

struct Ripple {
    QPointF pos;
    double age = 0;
    double size = 10;
};

/// Paper lotus candles floating along the river
struct Lotus {
    double x = 0;
    double speed = 4;
    double phase = 0;
    double size = 6;
};

constexpr double CURTAIN_TIME = 1.6;
constexpr double DRAGON_TIME = 3.0;
constexpr double SPARKLER_TIME = 2.6;
constexpr int MAX_STARS = 24;

double approach(double value, double target, double maxStep)
{
    if (value < target)
    {
        return std::min(target, value + maxStep);
    }
    return std::max(target, value - maxStep);
}

std::uint32_t channelSeed(const QString &channel)
{
    return static_cast<std::uint32_t>(qHash(channel)) ^ 0x2545f491U;
}

QRectF biggestActivity(const SceneLayout &layout)
{
    QRectF stage;
    for (const auto &r : layout.activity)
    {
        if (r.width() * r.height() > stage.width() * stage.height())
        {
            stage = r;
        }
    }
    return stage;
}

/// Keeps user colours usable as glowing paper: never too dark or grey.
QColor lanternPaper(const QColor &color, const QColor &fallback)
{
    if (!color.isValid())
    {
        return fallback;
    }
    QColor c = color.toHsl();
    const float light = std::clamp(c.lightnessF(), 0.45F, 0.62F);
    const float sat = std::max(c.hslSaturationF(), 0.55F);
    c.setHslF(std::max(0.0F, c.hslHueF()), sat, light);
    return mixColor(c.toRgb(), fallback, 0.25);
}

/// Bits tiers each get their own paper colour.
QColor bitsPaper(int bits)
{
    if (bits >= 10000)
    {
        return QColor(0xff, 0x5a, 0x4a);
    }
    if (bits >= 5000)
    {
        return QColor(0x5a, 0xa8, 0xff);
    }
    if (bits >= 1000)
    {
        return QColor(0x4f, 0xd1, 0xa0);
    }
    if (bits >= 100)
    {
        return QColor(0xb8, 0x82, 0xf0);
    }
    return QColor(0xe0, 0xd6, 0xc2);
}

/// How long a lantern is also drawn above the panels.
double frontLimit(const Lantern &l)
{
    switch (l.kind)
    {
        case Kind::Mention:
        case Kind::Falling:
            return 3.0;
        case Kind::Sub:
        case Kind::Gift:
            return 2.7;
        default:
            return 1.3;
    }
}

double lanternAlpha(const Lantern &l)
{
    const double a = l.age - l.delay;
    if (a < 0)
    {
        return 0;
    }
    double fadeIn = 0.35;
    double fadeOut = 3.0;
    if (l.kind == Kind::Ambient)
    {
        fadeIn = 3.0;
        fadeOut = 6.0;
    }
    else if (l.kind == Kind::Distant)
    {
        fadeIn = 1.5;
    }
    else if (l.kind == Kind::Falling)
    {
        fadeIn = 0;
        fadeOut = 0.5;
    }
    const double in = fadeIn > 0 ? smoothstep(0, fadeIn, a) : 1.0;
    return in * (1 - smoothstep(l.life - fadeOut, l.life, a));
}

class LanternScene : public ThemeScene
{
public:
    SceneInfo info() const override
    {
        return {"lanterns", "Lantern Festival", SURFACE, INK, ACCENT, false};
    }

    SceneGlass glass() const override
    {
        return {0.56, 0.36, 0.5};
    }

    void step(const SceneState &state, const SceneLayout &layout) override;
    void event(const SceneEvent &event, const SceneState &state,
               const SceneLayout &layout) override;
    void paintBack(QPainter &painter, const SceneState &state,
                   const SceneLayout &layout) override;
    bool hasFront() const override;
    void paintFront(QPainter &painter, const SceneState &state,
                    const SceneLayout &layout) override;

private:
    struct TypingPose {
        QPointF center;
        QPointF peg;
        double w = 0;
        double h = 0;
        double tilt = 0;
        double alpha = 0;
    };

    void ensureWorld(const SceneState &state, const SceneLayout &layout);
    QPointF offset(const World &w, double depth) const;
    TypingPose typingPose(const SceneState &state,
                          const SceneLayout &layout) const;
    Lantern &addLantern(Kind kind, QPointF pos, double size,
                        const QColor &color, double life);
    void addSparks(QPointF at, int count, const QColor &color, double speed,
                   double spread, bool front = true);
    QPointF pickStarSlot(const SceneLayout &layout);
    void lightWindow(const SceneEvent &event, const SceneState &state);
    double viewerLevel(const SceneState &state) const;
    QPointF sparklerPoint(const World &w) const;

    void stepTyping(const SceneState &state);
    void stepLanterns(const SceneState &state);
    void stepFlyers(const SceneState &state);
    void stepEffects(const SceneState &state);
    void stepTown(const SceneState &state);

    void composeBack(World &w, const SceneState &state);
    void paintTownLights(QPainter &p, const World &w, const SceneState &state);
    void paintBridge(QPainter &p, const World &w, const SceneState &state);
    void paintRiver(QPainter &p, const World &w, const SceneState &state);
    void paintNear(QPainter &p, const World &w, const SceneState &state);
    void paintFireworks(QPainter &p);
    void paintLanterns(QPainter &p, const SceneState &state, bool distant);
    void paintLantern(QPainter &p, const Lantern &l, double alpha);
    void paintBoats(QPainter &p, const World &w, const SceneState &state);
    void paintConstellation(QPainter &p, const SceneState &state);
    void paintFlyer(QPainter &p, const Flyer &f, double alpha);
    void paintTyping(QPainter &p, const SceneState &state,
                     const SceneLayout &layout);
    void paintSparks(QPainter &p, bool front);
    void paintCurtain(QPainter &p);
    void reflect(QPainter &p, const World &w, QPointF light, double width,
                 const QColor &color, double alpha);
    void paintDragon(QPainter &p, const World &w, const SceneState &state,
                     double opacity);
    void paintSparkler(QPainter &p, const World &w, const SceneState &state);

    /// Paints world-bound art for the current town, and during a tab
    /// switch also for the old one, each on its side of the curtain.
    template <typename Fn>
    void forWorlds(QPainter &p, Fn &&fn)
    {
        if (!this->oldWorld_ || this->curtainAge_ < 0)
        {
            fn(*this->world_);
            return;
        }
        const double W = this->world_->size.width();
        const double H = this->world_->size.height();
        const double f = clamp01(this->curtainAge_ / CURTAIN_TIME);
        if (this->reduced_)
        {
            // Almost still: a soft crossfade instead of a sweep
            fn(*this->oldWorld_);
            p.save();
            p.setOpacity(p.opacity() * smoothstep(0.1, 0.9, f));
            fn(*this->world_);
            p.restore();
            return;
        }
        const double cut = this->curtainLead() - this->curtainW_ * 0.5;
        p.save();
        p.setClipRect(QRectF(cut, -10, W - cut + 20, H + 20));
        fn(*this->oldWorld_);
        p.restore();
        p.save();
        p.setClipRect(QRectF(-20, -10, cut + 20, H + 20));
        fn(*this->world_);
        p.restore();
    }

    double curtainLead() const
    {
        const double W = this->world_->size.width();
        const double f = clamp01(this->curtainAge_ / CURTAIN_TIME);
        return lerp(-this->curtainM_, W + this->curtainW_ + this->curtainM_,
                    easeInOutSine(f));
    }

    std::unique_ptr<World> world_;
    std::unique_ptr<World> oldWorld_;
    std::array<double, 12> layoutKey_{};
    std::uint32_t seed_ = 0;
    bool seeded_ = false;

    QImage curtain_;
    double curtainW_ = 0;
    double curtainM_ = 0;
    double curtainAge_ = -1;

    // Pointer and wind
    QPointF par_;
    std::optional<QPointF> lastPointer_;
    QPointF pointerVel_;
    double wind_ = 0;
    double gustAge_ = -1;
    double gust_ = 0;

    // The lantern by the input
    double inflate_ = 0;
    double inflateVel_ = 0;
    double flame_ = 0.3;
    double sag_ = 0;
    double typingAppear_ = 1;
    double appearDelay_ = 0;
    double sendFlare_ = -1;
    QPointF sendFrom_;

    std::vector<Lantern> lanterns_;
    std::vector<Flyer> flyers_;
    std::vector<Star> stars_;
    std::vector<Spark> sparks_;
    std::vector<Boat> boats_;
    std::vector<Firework> fireworks_;
    std::vector<Ripple> ripples_;
    std::vector<Lotus> lotus_;
    int nextId_ = 1;
    SceneRng rng_{20261118U};
    double distantAcc_ = 0;

    // Town
    double litShown_ = 0;
    double crowdShown_ = 0;
    double festival_ = 1;
    double moonRise_ = 0;
    bool live_ = true;
    double level_ = 0.45;

    // On the bridge
    double hypeShown_ = 0;
    double hypePulse_ = 0;
    std::array<double, 12> ignitedAt_{-10, -10, -10, -10, -10, -10,
                                      -10, -10, -10, -10, -10, -10};
    double dragonAge_ = -1;
    int dragonLength_ = 6;
    double sparklerAge_ = -1;
    int sparklerPerson_ = -1;
    double sparkBudget_ = 0;
    double sparkEmitted_ = 0;
    QColor sparkColor_ = GOLD;

    // Hover
    int hoverId_ = -1;
    bool hoverActive_ = false;

    double t_ = 0;
    bool reduced_ = false;
    bool primed_ = false;
    QRectF inputRect_;
    std::optional<QPointF> pointer_;
};

void LanternScene::ensureWorld(const SceneState &state,
                               const SceneLayout &layout)
{
    if (!this->seeded_)
    {
        this->seed_ = channelSeed(state.channel);
        this->seeded_ = true;
    }
    const QRectF stage = biggestActivity(layout);
    QRectF chats;
    for (const auto &r : layout.chats)
    {
        chats = chats.united(r);
    }
    const std::array<double, 12> key{
        layout.window.width(), layout.window.height(), layout.dpr,
        stage.x(),             stage.y(),              stage.width(),
        stage.height(),        layout.tabs.bottom(),   layout.focusInput.left(),
        layout.focusInput.right(), chats.width(),      chats.height()};
    if (this->world_ && key == this->layoutKey_)
    {
        return;
    }
    this->layoutKey_ = key;
    this->world_ = buildWorld(layout, this->seed_);
    this->oldWorld_.reset();
    this->curtainAge_ = -1;
    this->curtain_ = buildCurtain(layout.window, this->world_->dpr,
                                  this->world_->s, this->curtainW_,
                                  this->curtainM_);
    // Warm the sprite cache for the town's own lantern colours, so the first
    // raid or hype train doesn't stall a frame
    for (const QColor &paper :
         {LANTERN_RED, LANTERN_ORANGE, QColor(0xf2, 0xb8, 0x4a),
          QColor(0xf0, 0xb0, 0x40)})
    {
        for (int mip = 0; mip < 3; ++mip)
        {
            lanternSprite(paper, false, true, mip);
            lanternSprite(paper, false, false, mip);
        }
    }
    if (this->lotus_.empty())
    {
        for (int i = 0; i < 4; ++i)
        {
            Lotus lotus;
            lotus.x = layout.window.width() * (0.12 + 0.24 * i) +
                      this->rng_.range(-30, 30);
            lotus.speed = this->rng_.range(2.5, 5.5);
            lotus.phase = this->rng_.range(0, TAU);
            lotus.size = this->rng_.range(5, 7);
            this->lotus_.push_back(lotus);
        }
    }
}

QPointF LanternScene::offset(const World &w, double depth) const
{
    const double k = 12 * w.s * depth;
    const double dpr = w.dpr;
    return QPointF(std::round(-this->par_.x() * k * dpr) / dpr,
                   std::round(-this->par_.y() * k * 0.45 * dpr) / dpr);
}

double LanternScene::viewerLevel(const SceneState &state) const
{
    if (state.viewers < 0)
    {
        return 0.45;
    }
    return clamp01((std::log10(std::max(1, state.viewers)) - 1.0) / 3.3);
}

LanternScene::TypingPose LanternScene::typingPose(
    const SceneState &state, const SceneLayout &layout) const
{
    TypingPose pose;
    const QRectF in = layout.focusInput;
    if (in.isEmpty())
    {
        return pose;
    }
    // It stands on the top edge of the message box at its right end: above
    // the box's buttons and clear of the typed text, in the free right part
    // of the band (an emote or reply bar keeps its emotes on the left). With
    // a bar there it is sized to fit the room above the box.
    const QRectF band = layout.inputBand();
    const double room = band.isEmpty() ? 0 : in.top() - band.top();
    const double fit = room > 30 ? std::clamp(room / 60.0, 0.7, 1.1) : 1.1;
    const double k = std::clamp(in.height() / 36.0, 0.8, 1.3) * fit;
    const double inflate = std::max(0.0, this->inflate_);
    const double full = clamp01(inflate);
    const double grow = 1 + std::min(0.3, state.draftLength / 260.0);
    pose.w = (19 + 7 * std::min(1.2, inflate)) * k * grow;
    pose.h = pose.w * (0.22 + 0.84 * inflate) * (1 - 0.12 * this->sag_);
    const double right =
        band.isEmpty() ? in.right() : std::min(in.right(), band.right());
    pose.peg = QPointF(right - 30 * k, in.top() + 1);
    const double lift = (3 + 6 * full) * k;
    const double bob =
        state.reduced ? 0 : std::sin(state.t * 1.9) * 1.3 * full;
    const double capH = std::max(1.0, pose.w * 0.1);
    const double tassel = pose.h * 0.45 + 2;
    pose.center = QPointF(pose.peg.x() + this->wind_ * 3 * full,
                          pose.peg.y() - lift - capH - tassel * 0.8 -
                              pose.h / 2 + bob + this->sag_ * 2);
    pose.tilt = (state.reduced ? 0 : std::sin(state.t * 1.3) * 0.05 * full) +
                this->wind_ * 0.12 * full - this->sag_ * 0.1;
    pose.alpha = this->typingAppear_;
    return pose;
}

Lantern &LanternScene::addLantern(Kind kind, QPointF pos, double size,
                                  const QColor &color, double life)
{
    Lantern l;
    l.id = this->nextId_++;
    l.kind = kind;
    l.pos = pos;
    l.from = pos;
    l.target = pos;
    l.size = size;
    l.color = color;
    l.life = life;
    l.phase = this->rng_.range(0, TAU);
    this->lanterns_.push_back(l);
    return this->lanterns_.back();
}

void LanternScene::addSparks(QPointF at, int count, const QColor &color,
                             double speed, double spread, bool front)
{
    for (int i = 0; i < count && this->sparks_.size() < 260; ++i)
    {
        Spark sp;
        const double a = -TAU / 4 + this->rng_.range(-spread, spread);
        const double v = speed * this->rng_.range(0.45, 1.0);
        sp.pos = at;
        sp.vel = QPointF(std::cos(a) * v, std::sin(a) * v);
        sp.life = this->rng_.range(0.7, 1.5);
        sp.size = this->rng_.range(1.3, 2.7);
        sp.angle = this->rng_.range(0, TAU);
        sp.spin = this->rng_.range(-7, 7);
        sp.color = mixColor(color, HOT_CORE, this->rng_.range(0, 0.4));
        sp.gravity = this->reduced_ ? 0 : 110;
        sp.front = front;
        if (front)
        {
            sp.size *= 1.25;
        }
        this->sparks_.push_back(sp);
    }
}

QPointF LanternScene::pickStarSlot(const SceneLayout &layout)
{
    const World &w = *this->world_;
    const double s = w.s;
    struct Zone {
        QRectF rect;
        double weight = 1;
    };
    std::vector<Zone> zones;
    if (w.pane)
    {
        zones.push_back({QRectF(w.stage.left() + 18 * s, w.stage.top() + 16 * s,
                                w.stage.width() - 36 * s, w.hspan() * 0.2),
                         3.0});
    }
    for (const auto &h : layout.headers)
    {
        if (!w.pane || h.center().x() < w.stage.left() ||
            h.center().x() > w.stage.right())
        {
            continue;
        }
        const QRectF r(h.left() + 110 * s, h.top() + 8 * s,
                       h.width() - 130 * s, std::max(1.0, h.height() - 16 * s));
        if (r.width() > 30)
        {
            zones.push_back({r, 1.0});
        }
    }
    if (zones.empty())
    {
        zones.push_back({QRectF(w.stage.left() + 16 * s,
                                w.stage.top() + 16 * s,
                                w.stage.width() - 32 * s, w.hspan() * 0.2),
                         1.0});
    }
    double total = 0;
    for (const auto &z : zones)
    {
        total += z.weight * z.rect.width() * z.rect.height();
    }
    QPointF best = zones.front().rect.center();
    double bestScore = -1;
    for (int k = 0; k < 12; ++k)
    {
        double pick = this->rng_.next() * total;
        const Zone *zone = &zones.front();
        for (const auto &z : zones)
        {
            pick -= z.weight * z.rect.width() * z.rect.height();
            zone = &z;
            if (pick <= 0)
            {
                break;
            }
        }
        const QPointF q(zone->rect.left() + this->rng_.next() * zone->rect.width(),
                        zone->rect.top() + this->rng_.next() * zone->rect.height());
        double score = 1e9;
        for (const auto &st : this->stars_)
        {
            score = std::min(score, dist(q, st.pos));
        }
        for (const auto &f : this->flyers_)
        {
            score = std::min(score, dist(q, f.end));
        }
        if (score > bestScore)
        {
            bestScore = score;
            best = q;
        }
    }
    return best;
}

void LanternScene::lightWindow(const SceneEvent &event, const SceneState &state)
{
    auto &windows = this->world_->windows;
    const int n = static_cast<int>(windows.size());
    if (n == 0)
    {
        return;
    }
    const int start = static_cast<int>(this->rng_.next() * n) % n;
    for (int k = 0; k < n; ++k)
    {
        auto &win = windows[(start + k) % n];
        if (win.eave || win.chatterUntil > state.t || win.glow > 0.3)
        {
            continue;
        }
        win.chatterUntil = state.t + 70 + this->rng_.range(0, 30);
        if (event.color.isValid())
        {
            win.color = mixColor(WARM_LIGHT, event.color, 0.3);
        }
        return;
    }
}

QPointF LanternScene::sparklerPoint(const World &w) const
{
    const QPointF off = this->offset(w, 0.58);
    if (this->sparklerPerson_ >= 0 &&
        this->sparklerPerson_ < static_cast<int>(w.people.size()))
    {
        return w.people[this->sparklerPerson_].hand +
               QPointF(1.5 * w.s, -12 * w.s) + off;
    }
    const double mid = w.bridgeMid();
    return QPointF(mid, w.deckY(mid) - 16 * w.s) + off;
}

void LanternScene::event(const SceneEvent &event, const SceneState &state,
                         const SceneLayout &layout)
{
    this->ensureWorld(state, layout);
    this->reduced_ = state.reduced;
    World &w = *this->world_;
    const double s = w.s;
    const bool reduced = state.reduced;
    const double mid = w.bridgeMid();

    switch (event.type)
    {
        case SceneEventType::Key: {
            // Each keystroke puffs the lantern and feeds its candle;
            // deleting lets it sag
            if (event.deleted)
            {
                this->inflateVel_ -= 1.2;
                this->sag_ = 1;
                this->flame_ = std::max(0.2, this->flame_ - 0.12);
            }
            else
            {
                this->inflateVel_ += 1.5;
                this->flame_ = std::min(1.7, this->flame_ + 0.24);
            }
        }
        break;

        case SceneEventType::Send: {
            const TypingPose pose = this->typingPose(state, layout);
            Flyer f;
            f.start = pose.w > 0 ? pose.center : event.point;
            f.end = this->pickStarSlot(layout);
            const double rise = std::max(40.0, f.start.y() - f.end.y());
            f.c1 = f.start + QPointF(4 * s, -std::min(300.0, rise * 0.6));
            f.c2 = QPointF(lerp(f.start.x(), f.end.x(), 0.55),
                           f.end.y() + rise * 0.15);
            f.size0 = std::max(pose.w, 22 * s);
            f.aspect0 = pose.w > 0 ? std::max(0.3, pose.h / pose.w) : 1.04;
            f.size1 = 9.5 * s;
            f.duration = std::clamp(rise / 50.0, 10.0, 16.0);
            f.phase = this->rng_.range(0, TAU);
            f.pos = f.start;
            f.size = f.size0;
            this->flyers_.push_back(f);
            if (this->flyers_.size() > 8)
            {
                this->flyers_.front().done = true;
            }
            // A flare and a ring of paper sparks as it lets go
            this->sendFlare_ = 0;
            this->sendFrom_ = f.start;
            this->addSparks(f.start + QPointF(0, f.size0 * 0.45),
                            reduced ? 4 : 26, WARM_LIGHT,
                            reduced ? 10 : 120 * s, TAU * 0.45);
            // The next lantern unfolds by the input a moment later
            this->inflate_ = 0;
            this->inflateVel_ = 0;
            this->typingAppear_ = 0;
            this->appearDelay_ = 0.9;
            this->flame_ = 0.3;
            this->sag_ = 0;
        }
        break;

        case SceneEventType::Message: {
            if (event.firstTime)
            {
                this->lightWindow(event, state);
            }
        }
        break;

        case SceneEventType::Sub: {
            // A big ornate lantern in the subscriber's colour leaves the bridge
            const double months = std::max(1, event.amount);
            auto &l = this->addLantern(
                Kind::Sub, QPointF(mid, w.deckY(mid) - 34 * s),
                (30 + std::min(20.0, months * 0.8)) * s,
                lanternPaper(event.color, LANTERN_RED), 17);
            l.ornate = true;
            l.front = true;
            l.vel = QPointF(0, -14 * s);
            this->addSparks(QPointF(mid, w.deckY(mid) - 22 * s) +
                                this->offset(w, 0.58),
                            reduced ? 4 : 16, GOLD, reduced ? 10 : 75 * s, 1.2);
        }
        break;

        case SceneEventType::Gift: {
            // A cluster of lanterns rises together, more for bigger gifts
            const int count = std::clamp(
                iround(3 * std::sqrt(std::max(1, event.amount)) + 1), 3, 30);
            const double spread = (w.bx1 - w.bx0) * 0.42 *
                                  (0.45 + 0.55 * std::min(1.0, count / 14.0));
            const QColor gifter = lanternPaper(event.color, LANTERN_ORANGE);
            for (int i = 0; i < count; ++i)
            {
                const double x = mid + this->rng_.range(-1, 1) * spread;
                const QPointF at(x, w.deckY(x) - this->rng_.range(18, 44) * s);
                QColor paper = i % 2 == 0 ? LANTERN_ORANGE : LANTERN_RED;
                if (i % 3 == 0)
                {
                    paper = gifter;
                }
                auto &l = this->addLantern(
                    Kind::Gift, at,
                    this->rng_.range(11, 17) * s * (count > 15 ? 1.1 : 1.0),
                    paper, this->rng_.range(13, 18));
                l.delay = i * (1.4 / count) + this->rng_.range(0, 0.25);
                l.front = true;
                l.vel = QPointF(this->rng_.range(-4, 4) * s,
                                -this->rng_.range(14, 22) * s);
            }
        }
        break;

        case SceneEventType::Bits: {
            // Someone on the bridge lights a sparkler
            const int bits = std::max(1, event.amount);
            this->sparklerAge_ = 0;
            this->sparkBudget_ =
                std::clamp(14 + 16 * std::log10(double(bits)), 14.0, 96.0) *
                (reduced ? 0.3 : 1.0);
            this->sparkEmitted_ = 0;
            this->sparkColor_ = bitsPaper(bits);
            const int present = static_cast<int>(this->crowdShown_);
            this->sparklerPerson_ =
                present > 0 ? static_cast<int>(this->rng_.next() * present) %
                                  present
                            : -1;
        }
        break;

        case SceneEventType::Raid: {
            // Lantern boats float in on the river, fireworks over the hills
            const int viewers = std::max(1, event.amount);
            const int boats = std::clamp(
                iround(1.5 * std::log2(viewers / 5.0 + 1)), 2, 14);
            const double W = w.size.width();
            const QColor raider = lanternPaper(event.color, LANTERN_ORANGE);
            // Free stretches of river beside the message box
            const QRectF in = layout.focusInput;
            double freeLeft = 0;
            double freeRight = 0;
            double gapLeft = W * 0.4;
            if (!in.isEmpty())
            {
                freeLeft = std::max(0.0, in.left() - 8);
                freeRight = std::max(0.0, W - in.right() - 8);
                gapLeft = in.right() + 8;
            }
            else
            {
                freeRight = W * 0.57;
            }
            for (int i = 0; i < boats; ++i)
            {
                Boat b;
                b.startX = -30 * s - i * 34 * s;
                const double f = (boats - i - 0.5) / boats;
                const double total = freeLeft + freeRight;
                const double along = f * std::max(1.0, total);
                b.targetX = along < freeRight || total < 1
                                ? gapLeft + along
                                : (along - freeRight) * 0.9 + 4;
                b.travel = 7 + i * 0.45 + this->rng_.range(0, 1);
                b.life = 34 + this->rng_.range(0, 8);
                b.phase = this->rng_.range(0, TAU);
                b.size = this->rng_.range(1.0, 1.3) * s;
                b.color = i == 0 ? raider
                                 : (i % 2 == 0 ? LANTERN_ORANGE : LANTERN_RED);
                this->boats_.push_back(b);
            }
            while (this->boats_.size() > 20)
            {
                this->boats_.erase(this->boats_.begin());
            }
            const int bursts = std::clamp(
                1 + iround(std::log2(viewers / 40.0 + 1)), 1, 7);
            const std::array<QColor, 6> colors{
                LANTERN_RED, GOLD, LANTERN_ORANGE, QColor(0xe8, 0x7a, 0xa8),
                QColor(0x7a, 0xc8, 0xb0), STAR_PAPER};
            for (int i = 0; i < bursts; ++i)
            {
                Firework f;
                const double top = std::max(w.stage.top(), 40.0);
                f.center = QPointF(
                    w.stage.left() + w.stage.width() * this->rng_.range(0.12, 0.88),
                    top + (w.riverY - top) * this->rng_.range(0.08, 0.34));
                f.delay = i * (1.2 / bursts) + this->rng_.range(0, 0.15);
                f.radius = this->rng_.range(26, 42) * s *
                           (1 + 0.3 * std::min(1.0, viewers / 2000.0));
                f.petals = 12 + static_cast<int>(this->rng_.next() * 8);
                f.rot = this->rng_.range(0, TAU);
                f.a = colors[static_cast<size_t>(this->rng_.next() * 5.99)];
                f.b = colors[static_cast<size_t>(this->rng_.next() * 5.99)];
                this->fireworks_.push_back(f);
            }
        }
        break;

        case SceneEventType::Follow: {
            if (!w.rooftops.empty())
            {
                const auto n = w.rooftops.size();
                const QPointF roof =
                    w.rooftops[static_cast<size_t>(this->rng_.next() * n) % n];
                auto &l = this->addLantern(Kind::Free, roof - QPointF(0, 8 * s),
                                           7 * s, LANTERN_ORANGE, 12);
                l.vel = QPointF(0, -8 * s);
            }
        }
        break;

        case SceneEventType::Mention: {
            // One lantern drifts down to the edge of the message row
            std::erase_if(this->lanterns_, [](const Lantern &l) {
                return l.kind == Kind::Mention;
            });
            QRectF row = event.row;
            if (row.isEmpty())
            {
                row = QRectF(layout.focusChat.left(),
                             layout.focusChat.bottom() - 22,
                             layout.focusChat.width(), 22);
            }
            const QPointF target(row.right() - 22 * s, row.center().y());
            auto &l = this->addLantern(
                Kind::Mention,
                target - QPointF(0, std::min(170.0, target.y() - 20)), 19 * s,
                lanternPaper(event.color, LANTERN_ORANGE), 9);
            l.target = target;
            l.front = true;
        }
        break;

        case SceneEventType::Combo: {
            this->dragonAge_ = 0;
            this->dragonLength_ = std::clamp(4 + event.amount, 5, 14);
        }
        break;

        case SceneEventType::Hype: {
            if (event.levelUp)
            {
                this->hypePulse_ = 1;
            }
        }
        break;

        case SceneEventType::Shoutout: {
            // A lantern with the name on a paper tag, hung where it reads
            std::erase_if(this->lanterns_, [](const Lantern &l) {
                return l.kind == Kind::Shout;
            });
            QPointF target(w.stage.center().x(), w.stage.top() + 40 * s);
            for (const auto &h : layout.headers)
            {
                if (h.center().x() > w.stage.left() &&
                    h.center().x() < w.stage.right())
                {
                    target = QPointF(h.right() - 110 * s, h.top() + 11 * s);
                }
            }
            auto &l = this->addLantern(
                Kind::Shout, QPointF(mid, w.deckY(mid) - 30 * s), 14 * s,
                lanternPaper(event.color, LANTERN_RED), 10);
            l.target = target;
            l.tag = event.user;
        }
        break;

        case SceneEventType::Timeout: {
            // A gust flattens the reeds and blows one flame out
            this->gustAge_ = 0;
            int best = -1;
            double bestScore = -1;
            for (int i = 0; i < static_cast<int>(this->lanterns_.size()); ++i)
            {
                const auto &l = this->lanterns_[i];
                const bool candidate =
                    l.kind == Kind::Ambient || l.kind == Kind::Free ||
                    l.kind == Kind::Gift || l.kind == Kind::Sub;
                if (!candidate || l.age < l.delay + 0.5 || l.pos.y() < 10)
                {
                    continue;
                }
                const double score =
                    l.size + (w.stage.contains(l.pos) ? 20.0 : 0.0);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = i;
                }
            }
            if (best < 0)
            {
                auto &l = this->addLantern(
                    Kind::Ambient,
                    QPointF(w.stage.center().x() + this->rng_.range(-40, 40) * s,
                            w.stage.top() + w.hspan() * 0.35),
                    12 * s, LANTERN_ORANGE, 60);
                l.age = 3;
                best = static_cast<int>(this->lanterns_.size()) - 1;
            }
            auto &l = this->lanterns_[best];
            l.kind = Kind::Falling;
            l.front = true;
            l.age = 0;
            l.delay = 0;
            l.life = 3.0;
            l.from = l.pos;
            l.vel = QPointF(this->wind_ * 10 * s, 0);
        }
        break;

        case SceneEventType::Live:
            this->live_ = true;
            break;

        case SceneEventType::Offline:
            this->live_ = false;
            break;

        case SceneEventType::Tab: {
            // A paper curtain sweeps across and reveals another town
            const auto seed = channelSeed(event.text.isEmpty() ? state.channel
                                                                : event.text);
            if (seed == this->seed_)
            {
                break;
            }
            this->seed_ = seed;
            this->oldWorld_ = std::move(this->world_);
            this->world_ = buildWorld(layout, seed);
            this->curtainAge_ = 0;
            this->sparklerAge_ = -1;
            this->dragonAge_ = -1;
            this->litShown_ = 0;
        }
        break;

        case SceneEventType::Hover: {
            // The lantern nearest the hovered row glows brighter
            this->hoverActive_ = true;
            const QPointF anchor(event.row.right() - 40, event.row.center().y());
            double bestDistance = 1e9;
            for (const auto &l : this->lanterns_)
            {
                if (l.kind == Kind::Distant || l.kind == Kind::Falling)
                {
                    continue;
                }
                const double d = dist(anchor, l.pos);
                if (d < bestDistance)
                {
                    bestDistance = d;
                    this->hoverId_ = l.id;
                }
            }
            for (const auto &st : this->stars_)
            {
                const double d = dist(anchor, st.pos);
                if (d < bestDistance)
                {
                    bestDistance = d;
                    this->hoverId_ = st.id;
                }
            }
        }
        break;

        case SceneEventType::Unhover:
            this->hoverActive_ = false;
            break;

        case SceneEventType::Click: {
            int clicks = 0;
            for (const auto &l : this->lanterns_)
            {
                clicks += l.kind == Kind::Free && l.front ? 1 : 0;
            }
            if (clicks >= 10)
            {
                const auto it = std::find_if(
                    this->lanterns_.begin(), this->lanterns_.end(),
                    [](const Lantern &l) {
                        return l.kind == Kind::Free && l.front;
                    });
                if (it != this->lanterns_.end())
                {
                    this->lanterns_.erase(it);
                }
            }
            const std::array<QColor, 3> papers{LANTERN_ORANGE, LANTERN_RED,
                                               QColor(0xf2, 0xb8, 0x4a)};
            auto &l = this->addLantern(
                Kind::Free, event.point, 10 * s,
                papers[static_cast<size_t>(this->rng_.next() * 2.99)], 13);
            l.vel = QPointF(0, -4 * s);
            l.front = true;
            this->addSparks(event.point + QPointF(0, 6 * s), reduced ? 2 : 8,
                            WARM_LIGHT, reduced ? 8 : 50 * s, 1.4);
        }
        break;
    }
}

void LanternScene::step(const SceneState &state, const SceneLayout &layout)
{
    this->ensureWorld(state, layout);
    const double dt = state.dt;
    this->t_ = state.t;
    this->reduced_ = state.reduced;
    this->pointer_ = state.pointer;
    this->inputRect_ = layout.focusInput;
    const World &w = *this->world_;

    // Pointer: smoothed parallax, and wind from its movement
    QPointF target(0, 0);
    if (state.pointer && !state.reduced)
    {
        target = QPointF(
            std::clamp(state.pointer->x() / w.size.width() * 2 - 1, -1.0, 1.0),
            std::clamp(state.pointer->y() / w.size.height() * 2 - 1, -1.0, 1.0));
    }
    this->par_ += (target - this->par_) * (1 - std::exp(-dt / 0.35));
    QPointF velocity(0, 0);
    if (state.pointer && this->lastPointer_ && dt > 0)
    {
        velocity = (*state.pointer - *this->lastPointer_) / dt;
    }
    this->lastPointer_ = state.pointer;
    this->pointerVel_ += (velocity - this->pointerVel_) *
                         (1 - std::exp(-dt / 0.15));
    const double breeze =
        std::sin(state.t * 0.21) * 0.25 + std::sin(state.t * 0.53 + 1) * 0.1;
    double windTarget =
        breeze + std::clamp(this->pointerVel_.x() / 700.0, -1.3, 1.3);
    if (state.reduced)
    {
        windTarget *= 0.15;
    }
    this->wind_ += (windTarget - this->wind_) * (1 - std::exp(-dt / 0.8));
    if (this->gustAge_ >= 0)
    {
        this->gustAge_ += dt;
        const double a = this->gustAge_;
        this->gust_ = a < 0.25 ? a / 0.25 : std::exp(-(a - 0.25) / 0.8);
        if (state.reduced)
        {
            this->gust_ *= 0.4;
        }
        if (a > 4)
        {
            this->gustAge_ = -1;
            this->gust_ = 0;
        }
    }

    this->stepTyping(state);
    this->stepLanterns(state);
    this->stepFlyers(state);
    this->stepEffects(state);
    this->stepTown(state);

    if (this->sendFlare_ >= 0)
    {
        this->sendFlare_ += dt;
        if (this->sendFlare_ > 3)
        {
            this->sendFlare_ = -1;
        }
    }
    if (this->curtainAge_ >= 0)
    {
        this->curtainAge_ += dt;
        if (this->curtainAge_ > CURTAIN_TIME)
        {
            this->curtainAge_ = -1;
            this->oldWorld_.reset();
        }
    }
}

void LanternScene::stepTyping(const SceneState &state)
{
    const double dt = state.dt;
    const double len = state.draftLength;
    const double target = len > 0 ? 1 - std::exp(-len / 14.0) : 0;
    // A springy fill: each keystroke puffs it a little past its size
    const double damping = state.reduced ? 16 : 8;
    this->inflateVel_ +=
        ((target - this->inflate_) * 55 - this->inflateVel_ * damping) * dt;
    this->inflate_ =
        std::clamp(this->inflate_ + this->inflateVel_ * dt, 0.0, 1.35);
    this->sag_ *= std::exp(-dt / 0.6);
    const double rest = 0.22 + 0.68 * clamp01(this->inflate_);
    this->flame_ += (rest - this->flame_) * (1 - std::exp(-dt / 0.55));
    if (this->appearDelay_ > 0)
    {
        this->appearDelay_ -= dt;
    }
    else
    {
        this->typingAppear_ = std::min(1.0, this->typingAppear_ + dt / 0.5);
    }
}

void LanternScene::stepLanterns(const SceneState &state)
{
    const World &w = *this->world_;
    const double dt = state.dt;
    const double s = w.s;
    const double slow = state.reduced ? 0.25 : 1.0;

    // Distant lanterns rise from the town as fast as the chat moves
    int distant = 0;
    int ambient = 0;
    for (const auto &l : this->lanterns_)
    {
        distant += l.kind == Kind::Distant ? 1 : 0;
        ambient += l.kind == Kind::Ambient ? 1 : 0;
    }
    if (this->live_ && !w.rooftops.empty())
    {
        const double rate = std::min(2.4, 0.06 + 0.32 * state.speed) *
                            (state.reduced ? 0.35 : 1.0);
        this->distantAcc_ += rate * dt;
        const int cap = state.reduced ? 14 : 46;
        const auto n = w.rooftops.size();
        while (this->distantAcc_ >= 1)
        {
            this->distantAcc_ -= 1;
            if (distant >= cap)
            {
                continue;
            }
            QPointF from = w.rooftops[static_cast<size_t>(this->rng_.next() * n) % n];
            for (int k = 0; k < 4 && w.bump(from.x()) < 0.4 &&
                            this->rng_.next() < 0.75;
                 ++k)
            {
                from = w.rooftops[static_cast<size_t>(this->rng_.next() * n) % n];
            }
            auto &l = this->addLantern(
                Kind::Distant,
                from + QPointF(this->rng_.range(-4, 4) * s, -4 * s),
                this->rng_.range(2.8, 5.0) * s,
                this->rng_.next() < 0.7 ? LANTERN_ORANGE : LANTERN_RED,
                this->rng_.range(16, 26));
            l.vel = QPointF(this->rng_.range(-1.5, 1.5) * s,
                            -this->rng_.range(7, 13) * s);
            ++distant;
        }
    }
    // A few big lanterns drift over the town at any time
    const int ambientWanted =
        this->live_ ? (state.reduced ? 2 : 3 + iround(4 * this->level_)) : 0;
    if (ambient < ambientWanted &&
        (state.t < 0.5 || this->rng_.next() < dt * 0.2))
    {
        const QPointF at(
            w.stage.left() + w.stage.width() * this->rng_.range(0.05, 0.95),
            w.stage.top() + w.hspan() * this->rng_.range(0.12, 0.5));
        auto &l = this->addLantern(Kind::Ambient, at,
                                   this->rng_.range(9, 13) * s,
                                   this->rng_.next() < 0.5 ? LANTERN_ORANGE
                                                           : LANTERN_RED,
                                   this->rng_.range(50, 80));
        l.vel = QPointF(0, -this->rng_.range(1.5, 3.5) * s);
    }

    for (auto &l : this->lanterns_)
    {
        l.age += dt;
        if (l.age < l.delay)
        {
            continue;
        }
        const double a = l.age - l.delay;
        // Wind sways every lantern; the pointer pushes the ones near it
        double push = 0;
        if (state.pointer && !state.reduced)
        {
            const double d = dist(*state.pointer, l.pos);
            if (d < 170)
            {
                push = this->pointerVel_.x() * (1 - d / 170);
            }
        }
        const double targetTilt =
            this->wind_ * 0.22 + this->gust_ * 0.5 + push * 0.0006 +
            std::sin(state.t * 1.1 + l.phase) * 0.05 * slow;
        l.tiltVel += ((targetTilt - l.tilt) * 18 - l.tiltVel * 4) * dt;
        l.tilt += l.tiltVel * dt;
        const bool hovered =
            (this->hoverActive_ && l.id == this->hoverId_) ||
            (state.pointer && dist(*state.pointer, l.pos) < l.size * 1.6 + 14);
        l.hover += ((hovered ? 1.0 : 0.0) - l.hover) *
                   (1 - std::exp(-dt / 0.25));
        const double drift = (this->wind_ * 5 * s + push * 0.03) * dt * slow;
        const double flicker =
            state.reduced ? 0 : 0.08 * std::sin(state.t * 9 + l.phase * 3);

        switch (l.kind)
        {
            case Kind::Distant:
            case Kind::Ambient:
                l.pos += l.vel * dt * slow + QPointF(drift, 0);
                l.light = 0.9 + flicker;
                break;
            case Kind::Free:
            case Kind::Gift:
            case Kind::Sub: {
                // They rise faster as the warm air fills them
                const double top = (l.kind == Kind::Sub ? 26 : 30) * s;
                l.vel.setY(std::max(-top, l.vel.y() - 5 * s * dt));
                const double sway = std::sin(a * 0.9 + l.phase) *
                                    (l.kind == Kind::Sub ? 7 : 3) * s;
                l.pos += (l.vel + QPointF(sway, 0)) * dt * slow +
                         QPointF(drift, 0);
                l.light = 1 + flicker;
            }
            break;
            case Kind::Falling: {
                l.light = std::max(0.0, l.light - dt / 0.35);
                if (a > 0.45)
                {
                    l.vel.ry() += 150 * s * dt * slow;
                    l.vel.rx() += (this->wind_ * 25 * s - l.vel.x()) * dt;
                    l.pos += l.vel * dt * slow;
                    l.tiltVel += 2.5 * dt;
                }
                if (l.pos.y() > w.riverY + 6)
                {
                    this->ripples_.push_back(
                        {QPointF(l.pos.x(), w.riverY + 8 * s), 0, l.size});
                    l.age = l.delay + l.life + 1;
                }
            }
            break;
            case Kind::Mention: {
                const double u = easeOutCubic(a / 1.4);
                QPointF at = l.from + (l.target - l.from) * u;
                at += QPointF(std::sin(a * 1.6 + l.phase) * 3 * s * u,
                              std::sin(a * 1.1) * 1.5 * s * u) *
                      slow;
                if (a > l.life - 2.2)
                {
                    at.ry() -= std::pow(a - (l.life - 2.2), 2) * 10 * s;
                }
                l.pos = at;
                l.light = 1 + flicker;
            }
            break;
            case Kind::Shout: {
                const double u = easeInOutSine(a / 2.6);
                const QPointF over(l.from.x(), l.target.y() + 60 * s);
                const QPointF base =
                    bezier(l.from, over, l.target + QPointF(0, 30 * s),
                           l.target, u);
                l.pos = base + QPointF(std::sin(a * 1.3 + l.phase) * 2.5 * s * u,
                                       0) *
                                   slow;
                if (a > l.life - 2)
                {
                    l.pos.ry() -= std::pow(a - (l.life - 2), 2) * 12 * s;
                }
                l.light = 1 + flicker;
            }
            break;
        }
    }
    std::erase_if(this->lanterns_, [&](const Lantern &l) {
        return l.age - l.delay > l.life || l.pos.y() < -60 ||
               l.pos.x() < -80 || l.pos.x() > w.size.width() + 80;
    });
}

void LanternScene::stepFlyers(const SceneState &state)
{
    const double dt = state.dt;
    const double s = this->world_->s;
    for (auto &f : this->flyers_)
    {
        f.age += dt;
        constexpr double lift = 0.3;
        QPointF base = f.start;
        double u = 0;
        if (state.reduced)
        {
            // Almost still: it fades at the input and appears in its place
            u = f.age < 1.0 ? 0 : 1;
            base = f.age < 1.0 ? f.start : f.end;
        }
        else if (f.age < lift)
        {
            // Dips as the flame flares, then lets go
            base = f.start +
                   QPointF(0, 2.5 * s * std::sin(TAU / 2 * f.age / lift));
        }
        else
        {
            // Accelerates off the ground, then rises slower and slower
            const double x = f.age - lift;
            constexpr double c = 0.25;
            const double warped = x - c * (1 - std::exp(-x / c));
            const double tau = clamp01(warped / f.duration);
            u = 1 - std::pow(1 - tau, 2.6);
            base = bezier(f.start, f.c1, f.c2, f.end, u);
        }
        const double settle = 1 - u;
        const double swayIn = smoothstep(0.2, 1.2, f.age);
        const double swayAmp = state.reduced ? 0 : 9 * s * settle * swayIn;
        f.pos = base + QPointF(std::sin(f.age * 1.8 + f.phase) * swayAmp +
                                   this->wind_ * 6 * s * settle,
                               0);
        f.tilt = state.reduced ? 0
                               : std::cos(f.age * 1.8 + f.phase) * 0.14 *
                                         settle * swayIn +
                                     this->wind_ * 0.12 * settle;
        f.u = u;
        f.size = lerp(f.size0, f.size1, easeInOutSine(u));
        f.aspect = lerp(f.aspect0, 1.04, smoothstep(0, 0.4, f.age));
        f.light = f.age < 0.7 ? 1.55 - 0.55 * (f.age / 0.7) : 1.0;
        if (u >= 0.999 || (state.reduced && f.age > 2.2))
        {
            f.done = true;
        }
        // Embers drop from it while it climbs past the rooftops
        if (!state.reduced && f.age > lift && f.age < 3.5 &&
            this->rng_.next() < dt * 5)
        {
            this->addSparks(f.pos + QPointF(0, f.size * 0.55), 1, WARM_LIGHT,
                            14 * s, TAU / 2, false);
        }
    }
    // Arrived lanterns join your constellation
    for (const auto &f : this->flyers_)
    {
        if (!f.done)
        {
            continue;
        }
        Star star;
        star.id = this->nextId_++;
        star.pos = f.end;
        star.color = f.color;
        star.size = f.size1;
        star.phase = f.phase;
        star.life = 95 + this->rng_.range(0, 25);
        this->stars_.push_back(star);
    }
    std::erase_if(this->flyers_, [](const Flyer &f) {
        return f.done;
    });
    while (static_cast<int>(this->stars_.size()) > MAX_STARS)
    {
        this->stars_.erase(this->stars_.begin());
    }
    if (static_cast<int>(this->stars_.size()) == MAX_STARS)
    {
        // The oldest one makes room by fading early
        auto &oldest = this->stars_.front();
        oldest.age = std::max(oldest.age, oldest.life - 4);
    }
    for (auto &star : this->stars_)
    {
        star.age += dt;
        const bool hovered =
            (this->hoverActive_ && star.id == this->hoverId_) ||
            (state.pointer && dist(*state.pointer, star.pos) < star.size + 14);
        star.hover += ((hovered ? 1.0 : 0.0) - star.hover) *
                      (1 - std::exp(-dt / 0.25));
    }
    std::erase_if(this->stars_, [](const Star &star) {
        return star.age > star.life;
    });
}

void LanternScene::stepEffects(const SceneState &state)
{
    const World &w = *this->world_;
    const double dt = state.dt;
    const double s = w.s;
    const double slow = state.reduced ? 0.25 : 1.0;

    for (auto &sp : this->sparks_)
    {
        sp.age += dt;
        sp.vel.ry() += sp.gravity * s * dt;
        sp.vel *= std::exp(-dt * 1.4);
        sp.pos += sp.vel * dt;
        sp.angle += sp.spin * dt * slow;
    }
    std::erase_if(this->sparks_, [](const Spark &sp) {
        return sp.age > sp.life;
    });

    if (this->sparklerAge_ >= 0)
    {
        this->sparklerAge_ += dt;
        const QPointF at = this->sparklerPoint(w);
        const double want =
            this->sparkBudget_ * clamp01(this->sparklerAge_ / SPARKLER_TIME);
        while (this->sparkEmitted_ < want)
        {
            this->sparkEmitted_ += 1;
            this->addSparks(at, 1, this->sparkColor_,
                            state.reduced ? 10 : 115 * s, 1.6);
        }
        if (this->sparklerAge_ > SPARKLER_TIME + 0.4)
        {
            this->sparklerAge_ = -1;
        }
    }

    for (auto &b : this->boats_)
    {
        b.age += dt;
    }
    std::erase_if(this->boats_, [](const Boat &b) {
        return b.age > b.life;
    });
    for (auto &f : this->fireworks_)
    {
        f.age += dt;
    }
    std::erase_if(this->fireworks_, [](const Firework &f) {
        return f.age > f.delay + f.life;
    });
    for (auto &r : this->ripples_)
    {
        r.age += dt;
    }
    std::erase_if(this->ripples_, [](const Ripple &r) {
        return r.age > 1.6;
    });
    const double W = w.size.width();
    for (auto &lotus : this->lotus_)
    {
        lotus.x += lotus.speed * s * dt * slow;
        if (lotus.x > W + 30)
        {
            lotus.x = -30;
        }
    }

    if (this->dragonAge_ >= 0)
    {
        this->dragonAge_ += dt;
        if (this->dragonAge_ > DRAGON_TIME)
        {
            this->dragonAge_ = -1;
        }
    }

    // The string across the bridge lights one lantern at a time
    const int n = w.stringCount;
    const int perLevel = (n + 4) / 5;
    const double target = (this->live_ && state.hypeLevel > 0)
                              ? std::min(n, state.hypeLevel * perLevel)
                              : 0;
    const double rate = target > this->hypeShown_ ? 1 / 0.35 : 1 / 0.5;
    const double before = this->hypeShown_;
    this->hypeShown_ = approach(this->hypeShown_, target, rate * dt);
    for (int i = 0; i < std::min(n, static_cast<int>(this->ignitedAt_.size()));
         ++i)
    {
        if (before < i + 1 && this->hypeShown_ >= i + 1)
        {
            this->ignitedAt_[static_cast<size_t>(i)] = state.t;
        }
    }
    this->hypePulse_ *= std::exp(-dt / 0.8);
}

void LanternScene::stepTown(const SceneState &state)
{
    World &w = *this->world_;
    const double dt = state.dt;
    this->live_ = state.live;
    this->level_ = this->viewerLevel(state);
    this->festival_ =
        approach(this->festival_, this->live_ ? 1.0 : 0.0, dt / 7.0);
    this->moonRise_ = approach(this->moonRise_, this->live_ ? 0.0 : 1.0,
                               dt / (this->live_ ? 5.0 : 8.0));

    // How much of the town is lit follows the viewers; new chatters light
    // their own window. Closing the festival puts them out one by one.
    const double windows = double(w.windows.size());
    const double litTarget = windows * (0.22 + 0.74 * this->level_);
    const double crowdTarget =
        this->live_ ? std::round(this->level_ * double(w.people.size())) : 0;
    if (!this->primed_)
    {
        // The festival is already under way when the theme starts
        this->primed_ = true;
        this->litShown_ = litTarget;
        this->crowdShown_ = crowdTarget;
    }
    this->litShown_ = approach(this->litShown_, litTarget,
                               dt * std::max(6.0, windows / 3));
    int wanted = 0;
    for (const auto &win : w.windows)
    {
        if (win.rank < this->litShown_ || win.chatterUntil > state.t)
        {
            ++wanted;
        }
    }
    const int allowed = static_cast<int>(std::ceil(this->festival_ * wanted));
    int seen = 0;
    for (const int i : w.byRank)
    {
        auto &win = w.windows[i];
        const bool lit = win.rank < this->litShown_ || win.chatterUntil > state.t;
        const bool on = lit && seen < allowed;
        seen += lit ? 1 : 0;
        const double want = on ? 1.0 : 0.0;
        win.glow = state.t < 0.05 ? want
                                  : approach(win.glow, want,
                                             dt / (want > win.glow ? 0.6 : 1.0));
        const bool full = win.glow >= 0.999;
        if (full != win.baked)
        {
            win.baked = full;
            ++w.lightsVersion;
        }
    }

    this->crowdShown_ = approach(this->crowdShown_, crowdTarget, dt * 4);
}

// Painting -------------------------------------------------------------------

/// The paper head of the lantern dragon, facing right.
void paintDragonHead(QPainter &p, QPointF c, double size, double tilt,
                     double alpha)
{
    p.save();
    p.translate(c);
    p.rotate(deg(tilt));
    p.setOpacity(p.opacity() * alpha);
    drawGlow(p, QPointF(0, 0), size * 1.6, WARM_LIGHT, 0.4);
    QPainterPath horns;
    for (const double x : {-0.05, -0.32})
    {
        horns.moveTo(size * x, -size * 0.38);
        horns.quadTo(size * (x - 0.15), -size * 0.95, size * (x - 0.42),
                     -size * 0.9);
        horns.quadTo(size * (x - 0.2), -size * 0.7, size * (x - 0.16),
                     -size * 0.36);
        horns.closeSubpath();
    }
    p.fillPath(horns, GOLD);
    QPainterPath head;
    head.addRoundedRect(QRectF(-size * 0.55, -size * 0.45, size * 0.95,
                               size * 0.8),
                        size * 0.3, size * 0.3);
    head.addPolygon(QPolygonF({QPointF(size * 0.25, -size * 0.3),
                               QPointF(size * 0.9, -size * 0.12),
                               QPointF(size * 0.86, size * 0.16),
                               QPointF(size * 0.25, size * 0.26)}));
    head.closeSubpath();
    head = head.simplified();
    QRadialGradient fill(0, size * 0.05, size * 0.8);
    fill.setColorAt(0, mixColor(LANTERN_RED, HOT_CORE, 0.45));
    fill.setColorAt(1, LANTERN_RED.darker(150));
    p.setPen(QPen(LANTERN_RED.darker(300), std::max(0.6, size * 0.05)));
    p.setBrush(fill);
    p.drawPath(head);
    QPainterPath jaw;
    jaw.addPolygon(QPolygonF({QPointF(size * 0.15, size * 0.24),
                              QPointF(size * 0.8, size * 0.26),
                              QPointF(size * 0.22, size * 0.5)}));
    p.setBrush(GOLD.darker(115));
    p.drawPath(jaw);
    // Mane of gold paper flames along the back of the head
    p.setPen(Qt::NoPen);
    p.setBrush(GOLD);
    for (int k = 0; k < 3; ++k)
    {
        const double y = -size * 0.3 + k * size * 0.25;
        const QPointF flame[] = {{-size * 0.5, y - size * 0.1},
                                 {-size * 0.95, y + size * 0.02},
                                 {-size * 0.5, y + size * 0.12}};
        p.drawPolygon(flame, 3);
    }
    p.setBrush(HOT_CORE);
    p.drawEllipse(QPointF(size * 0.18, -size * 0.14), size * 0.12, size * 0.12);
    p.setBrush(QColor(0x14, 0x0c, 0x0a));
    p.drawEllipse(QPointF(size * 0.21, -size * 0.14), size * 0.06, size * 0.07);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(withAlpha(GOLD, 0.9), std::max(0.5, size * 0.04)));
    QPainterPath whiskers;
    whiskers.moveTo(size * 0.8, -size * 0.05);
    whiskers.quadTo(size * 0.6, -size * 0.6, size * 0.05, -size * 0.7);
    whiskers.moveTo(size * 0.8, size * 0.1);
    whiskers.quadTo(size * 0.7, size * 0.7, size * 0.1, size * 0.8);
    p.drawPath(whiskers);
    p.restore();
}

/// A lit window (or a small eave lantern) at strength `g`.
void paintWindow(QPainter &p, const WindowLight &win, double g, double s)
{
    const QPointF c = win.rect.center();
    if (win.eave)
    {
        drawGlow(p, c, 7 * s, WARM_LIGHT, 0.35 * g);
        p.setBrush(mixColor(win.color.darker(250), win.color.lighter(120), g));
        p.drawEllipse(c, win.rect.width() * 0.5, win.rect.height() * 0.5);
        return;
    }
    drawGlow(p, c, win.rect.width() * 3 + 3 * s, win.color, 0.5 * g);
    p.fillRect(win.rect, withAlpha(win.color, 0.92 * g));
    if (win.rect.width() >= 3.2)
    {
        // Window bars cut from the paper
        const QColor bar = withAlpha(TOWN, 0.85 * g);
        p.fillRect(QRectF(c.x() - 0.4, win.rect.top(), 0.8, win.rect.height()),
                   bar);
        p.fillRect(QRectF(win.rect.left(), c.y() - 0.4, win.rect.width(), 0.8),
                   bar);
    }
}

void LanternScene::composeBack(World &w, const SceneState &state)
{
    (void)state;
    const QPointF skyOff = this->offset(w, w.sky.depth);
    const QPointF hillOff = this->offset(w, w.hills.depth);
    const QPointF townOff = this->offset(w, w.town.depth);
    const double haze =
        (0.32 + 0.5 * this->level_) * (0.2 + 0.8 * this->festival_);
    const std::array<int, 9> key{iround(skyOff.x() * w.dpr),
                                 iround(skyOff.y() * w.dpr),
                                 iround(hillOff.x() * w.dpr),
                                 iround(hillOff.y() * w.dpr),
                                 iround(townOff.x() * w.dpr),
                                 iround(townOff.y() * w.dpr),
                                 iround(this->moonRise_ * 300),
                                 iround(haze * 40),
                                 w.lightsVersion};
    if (w.compositeValid && key == w.compositeKey)
    {
        return;
    }
    w.compositeKey = key;
    w.compositeValid = true;
    const QSize pixels(std::max(1, iround(w.size.width() * w.dpr)),
                       std::max(1, iround(w.size.height() * w.dpr)));
    if (w.composite.size() != pixels)
    {
        w.composite = QImage(pixels, QImage::Format_RGB32);
        w.composite.setDevicePixelRatio(w.dpr);
    }
    QPainter p(&w.composite);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(w.sky.origin + skyOff, w.sky.image);

    // A small crescent; when the festival closes a big full moon rises from
    // behind the hills instead
    const double crescent = 1 - smoothstep(0, 0.35, this->moonRise_);
    if (crescent > 0.01)
    {
        const QPointF c = w.moonSmallPos + skyOff;
        drawGlow(p, c, w.moonSmallR * 3.6, MOON_PAPER, 0.16 * crescent);
        const QSizeF size(w.moonCrescent.width() / w.dpr,
                          w.moonCrescent.height() / w.dpr);
        p.setOpacity(crescent);
        p.drawImage(QRectF(c - QPointF(size.width() / 2, size.height() / 2), size),
                    w.moonCrescent);
        p.setOpacity(1);
    }
    if (this->moonRise_ > 0.001)
    {
        const double rise = easeInOutSine(this->moonRise_);
        const QPointF c = w.moonFrom + (w.moonTo - w.moonFrom) * rise + skyOff;
        const double k = 0.8 + 0.2 * rise;
        const double r = w.moonFullR * k;
        drawGlow(p, c, r * 3.4, MOON_PAPER,
                 0.24 * smoothstep(0, 0.6, this->moonRise_));
        const double half = (w.moonFullR + 4) * k;
        p.drawImage(QRectF(c.x() - half, c.y() - half, half * 2, half * 2),
                    w.moonFull);
    }

    // The festival's warm haze over the town grows with the crowd
    drawGlow(p,
             QPointF(w.stage.center().x(), w.riverY - w.hspan() * 0.12) +
                 hillOff,
             std::max(w.stage.width() * 0.9, w.hspan() * 0.65), HORIZON_WARM,
             haze);
    p.drawImage(w.hills.origin + hillOff, w.hills.image);
    p.drawImage(w.town.origin + townOff, w.town.image);
    p.translate(townOff);
    p.setPen(Qt::NoPen);
    for (const auto &win : w.windows)
    {
        if (win.baked)
        {
            paintWindow(p, win, 1.0, w.s);
        }
    }
}

void LanternScene::paintTownLights(QPainter &p, const World &w,
                                   const SceneState &state)
{
    const double s = w.s;
    const QPointF skyOff = this->offset(w, w.sky.depth);
    for (size_t i = 0; i < w.twinkles.size(); ++i)
    {
        const double phase = double(i) * 1.7;
        const double a =
            state.reduced
                ? 0.5
                : 0.5 + 0.5 * std::sin(state.t * (0.45 + 0.11 * double(i % 5)) +
                                       phase);
        drawGlow(p, w.twinkles[i] + skyOff, 3.5 * s, STAR_PAPER, 0.35 * a * a);
    }

    // Windows still fading in or out; the fully lit ones are in the composite
    p.save();
    p.translate(this->offset(w, w.town.depth));
    p.setPen(Qt::NoPen);
    for (const auto &win : w.windows)
    {
        if (win.glow >= 0.01 && !win.baked)
        {
            paintWindow(p, win, win.glow, s);
        }
    }
    p.restore();
}

void LanternScene::paintBridge(QPainter &p, const World &w,
                               const SceneState &state)
{
    const double s = w.s;
    const bool current = &w == this->world_.get();
    const QPointF off = this->offset(w, w.bridgeBack.depth);
    p.drawImage(w.bridgeBack.origin + off, w.bridgeBack.image);

    p.save();
    p.translate(off);
    // The crowd along the railing, arriving one by one, backlit by the
    // warm light of the lanterns they carry
    const int present = std::min(static_cast<int>(w.people.size()),
                                 static_cast<int>(std::ceil(this->crowdShown_)));
    const double crowd =
        w.people.empty() ? 0 : this->crowdShown_ / double(w.people.size());
    if (crowd > 0.01)
    {
        for (int k = 0; k < 5; ++k)
        {
            const double x = lerp(w.bx0, w.bx1, (k + 0.5) / 5);
            drawGlow(p, QPointF(x, w.deckY(x) - 14 * s),
                     (w.bx1 - w.bx0) * 0.16, WARM_LIGHT, 0.2 * crowd);
        }
    }
    for (int i = 0; i < present; ++i)
    {
        p.setOpacity(clamp01(this->crowdShown_ - i));
        p.fillPath(w.people[i].shape.translated(0, -1), CROWD_RIM);
        p.fillPath(w.people[i].shape, CROWD);
    }
    p.setOpacity(1);
    for (int i = 0; i < present; ++i)
    {
        const auto &person = w.people[i];
        if (!person.lantern)
        {
            continue;
        }
        const double alpha = clamp01(this->crowdShown_ - i);
        const double bob =
            state.reduced ? 0 : std::sin(state.t * 1.4 + person.phase) * 1.2 * s;
        const QPointF tip = person.hand + QPointF(4 * s, -9 * s);
        p.setPen(QPen(withAlpha(CROWD, alpha), 0.9 * s));
        p.drawLine(person.hand, tip);
        const QPointF lantern = tip + QPointF(0, 3.5 * s + bob);
        lanternHalo(p, lantern, 4.5 * s, this->festival_, alpha);
        drawLantern(p, lantern, 4.5 * s, LANTERN_RED, this->festival_,
                    std::sin(state.t + person.phase) * 0.1, alpha);
    }

    // The lantern dragon weaves along the deck on its poles
    if (current && this->dragonAge_ >= 0)
    {
        this->paintDragon(p, w, state, 1.0);
    }
    p.restore();

    p.drawImage(w.bridgeFront.origin + off, w.bridgeFront.image);

    // The string of lanterns between the poles lights with the hype train
    p.save();
    p.translate(off);
    const int n = w.stringCount;
    const double swing = state.reduced
                             ? 0
                             : std::sin(state.t * 0.7) * 1.5 * s + this->wind_ * 2 * s;
    const auto along = [&](double f) {
        return QPointF(lerp(w.poleA.x(), w.poleB.x(), f),
                       lerp(w.poleA.y(), w.poleB.y(), f) +
                           (w.sag + swing) * 4 * f * (1 - f));
    };
    QPolygonF cord;
    for (int k = 0; k <= 16; ++k)
    {
        cord << along(k / 16.0);
    }
    p.setPen(QPen(withAlpha(RAIL.lighter(130), 0.95), 0.9 * s));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(cord);
    const double hype = current ? this->hypeShown_ : 0;
    for (int i = 0; i < n; ++i)
    {
        const double f = (i + 1.0) / (n + 1);
        const QPointF hook = along(f);
        const double lit = clamp01(hype - i);
        double light = lit;
        if (current && i == static_cast<int>(hype) && state.hypeLevel > 0)
        {
            light = std::max(light, 0.35 * state.hypeProgress);
        }
        if (lit > 0)
        {
            const double wave = (1 - this->hypePulse_) * n;
            light += this->hypePulse_ * 0.5 *
                     std::max(0.0, 1 - std::abs(i - wave) / 2.0);
        }
        const double size = 12 * s;
        const double tilt =
            (state.reduced ? 0 : std::sin(state.t * 1.2 + i * 0.6) * 0.06) +
            this->wind_ * 0.15;
        const QPointF center = hook + QPointF(0, 3 * s + size * 0.6);
        p.setPen(QPen(RAIL, 0.7 * s));
        p.drawLine(hook, center - QPointF(0, size * 0.55));
        lanternHalo(p, center, size * 1.3, light, 1.4);
        drawLantern(p, center, size, i % 2 == 0 ? LANTERN_RED : LANTERN_ORANGE,
                    light, tilt, 1);
    }
    p.restore();

    // Bits: someone on the bridge holds up a sparkler (its light and sparks
    // are drawn above the panels)
    if (current && this->sparklerAge_ >= 0 &&
        this->sparklerAge_ < SPARKLER_TIME &&
        this->sparklerPerson_ >= 0 &&
        this->sparklerPerson_ < static_cast<int>(w.people.size()))
    {
        const double fade = 1 - smoothstep(SPARKLER_TIME - 0.5, SPARKLER_TIME,
                                           this->sparklerAge_);
        p.setPen(QPen(withAlpha(CROWD, fade), 0.9 * s));
        p.drawLine(w.people[this->sparklerPerson_].hand + off,
                   this->sparklerPoint(w));
    }
}

void LanternScene::paintSparkler(QPainter &p, const World &w,
                                 const SceneState &state)
{
    if (this->sparklerAge_ < 0 || this->sparklerAge_ >= SPARKLER_TIME)
    {
        return;
    }
    const double s = w.s;
    const QPointF at = this->sparklerPoint(w);
    const double fade =
        1 - smoothstep(SPARKLER_TIME - 0.5, SPARKLER_TIME, this->sparklerAge_);
    const double flick =
        state.reduced ? 1.0 : 0.85 + 0.15 * std::sin(state.t * 19);
    drawGlow(p, at, 26 * s, this->sparkColor_, 0.55 * fade);
    drawGlow(p, at, 7 * s, HOT_CORE, 0.95 * fade * flick);
}

void LanternScene::paintDragon(QPainter &p, const World &w,
                               const SceneState &state, double opacity)
{
    const double s = w.s;
    const double a = this->dragonAge_;
    const double fade = opacity * smoothstep(0, 0.3, a) *
                        (1 - smoothstep(DRAGON_TIME - 0.4, DRAGON_TIME, a));
    const double spacing = 13 * s;
    const double headX =
        lerp(w.bx0 + 4 * s, w.bx1 + spacing * this->dragonLength_,
             easeInOutSine(a / DRAGON_TIME));
    const double weave = state.reduced ? 1.0 * s : 9 * s;
    for (int i = this->dragonLength_; i >= 0; --i)
    {
        const double x = headX - i * spacing;
        if (x < w.bx0 - 4 || x > w.bx1 + 4)
        {
            continue;
        }
        const double edge = smoothstep(w.bx0, w.bx0 + 12 * s, x) *
                            (1 - smoothstep(w.bx1 - 12 * s, w.bx1, x));
        const double alpha = fade * edge;
        const double deck = w.deckY(x);
        const double y = deck - 30 * s - std::sin(a * 6 - i * 0.75) * weave;
        const double tilt = std::cos(a * 6 - i * 0.75) * 0.3;
        p.setPen(QPen(withAlpha(RAIL.darker(120), alpha), 1.1 * s));
        p.drawLine(QPointF(x, y + 4 * s), QPointF(x, deck - 2));
        if (i == 0)
        {
            paintDragonHead(p, QPointF(x, y), 16 * s, tilt * 0.5, alpha);
            continue;
        }
        const QColor paper = i % 2 == 0 ? LANTERN_RED
                                        : QColor(0xf0, 0xb0, 0x40);
        lanternHalo(p, QPointF(x, y), 11 * s, 1, alpha);
        drawLantern(p, QPointF(x, y), 11 * s, paper, 1, tilt, alpha);
    }
}

void LanternScene::reflect(QPainter &p, const World &w, QPointF light,
                           double width, const QColor &color, double alpha)
{
    if (alpha < 0.02)
    {
        return;
    }
    const double above = w.riverY - light.y();
    if (above < -2)
    {
        return;
    }
    const double s = w.s;
    const double y = w.riverY + 3 * s + std::max(0.0, above) * 0.45;
    if (y > w.size.height() + 4)
    {
        return;
    }
    const double amp = this->reduced_ ? 0.3 : 2.0;
    p.setPen(Qt::NoPen);
    for (int k = -1; k <= 1; ++k)
    {
        const double dy = k * 2.8 * s;
        const double wk = width * (1.6 - std::abs(k) * 0.45);
        const double dx =
            std::sin(this->t_ * 1.7 + k * 1.3 + light.x() * 0.05) * amp * s;
        p.fillRect(QRectF(light.x() - wk / 2 + dx, y + dy, wk, 1.3 * s),
                   withAlpha(color, alpha * (0.55 - 0.1 * std::abs(k))));
    }
}

void LanternScene::paintRiver(QPainter &p, const World &w,
                              const SceneState &state)
{
    const double s = w.s;
    const QPointF off = this->offset(w, w.river.depth);
    p.drawImage(w.river.origin + off, w.river.image);

    // The bridge's reflection, cut into wobbling strips
    const QImage &img = w.bridgeReflection;
    const double dpr = w.dpr;
    const double h = img.height() / dpr;
    const double width = img.width() / dpr;
    const QPointF roff = this->offset(w, w.bridgeBack.depth);
    constexpr double slice = 3;
    for (double y = 0; y < h; y += slice)
    {
        const double depth = y / std::max(1.0, h);
        const double wobble =
            state.reduced
                ? 0
                : std::sin(state.t * 1.3 + y * 0.35) * (0.6 + 2.4 * depth) * s;
        p.drawImage(QRectF(w.reflectionOrigin.x() + roff.x() + wobble,
                           w.reflectionOrigin.y() + y, width, slice),
                    img, QRectF(0, y * dpr, img.width(), slice * dpr));
    }

    // Lit windows on the bank and the lit string shimmer in the water
    const QPointF townOff = this->offset(w, w.town.depth);
    for (const auto &win : w.windows)
    {
        if (win.nearRiver && win.glow > 0.05)
        {
            this->reflect(p, w, win.rect.center() + townOff,
                          win.rect.width() * 1.2, win.color, win.glow * 0.6);
        }
    }
    if (&w == this->world_.get() && this->hypeShown_ > 0)
    {
        for (int i = 0; i < w.stringCount; ++i)
        {
            const double lit = clamp01(this->hypeShown_ - i);
            const double f = (i + 1.0) / (w.stringCount + 1);
            const QPointF at(lerp(w.poleA.x(), w.poleB.x(), f), w.poleA.y());
            this->reflect(p, w, at + roff, 7 * s, WARM_LIGHT, lit * 0.5);
        }
    }
}

void LanternScene::paintBoats(QPainter &p, const World &w,
                              const SceneState &state)
{
    const double s = w.s;
    const double H = w.size.height();
    const QPointF off = this->offset(w, w.river.depth);

    // Paper lotus candles drift down the river
    const QRectF input = this->inputRect_;
    for (const auto &lotus : this->lotus_)
    {
        const double x = lotus.x + off.x();
        double alpha = 0.3 + 0.7 * this->festival_;
        if (!input.isEmpty())
        {
            const double inside =
                smoothstep(input.left() - 20, input.left() + 10, x) *
                (1 - smoothstep(input.right() - 10, input.right() + 20, x));
            alpha *= 1 - 0.85 * inside;
        }
        const double y = w.riverY + (H - w.riverY) * 0.62 + off.y() +
                         (state.reduced ? 0 : std::sin(state.t * 0.8 + lotus.phase) * 0.8);
        const double k = lotus.size * s / 6;
        drawGlow(p, QPointF(x, y - 3 * k), 11 * k, WARM_LIGHT,
                 0.35 * alpha * this->festival_);
        p.setPen(Qt::NoPen);
        for (int j = -2; j <= 2; ++j)
        {
            const double tipX = x + j * 2.6 * k;
            const double tipY = y - (5 - std::abs(j)) * 1.3 * k;
            const QPointF petal[] = {{x + j * 1.2 * k - 1.6 * k, y},
                                     {tipX, tipY},
                                     {x + j * 1.2 * k + 1.6 * k, y}};
            p.setBrush(withAlpha(mixColor(QColor(0xe8, 0x9a, 0xb0), STAR_PAPER,
                                          0.35 + 0.15 * std::abs(j)),
                                 alpha * 0.9));
            p.drawPolygon(petal, 3);
        }
        p.setBrush(withAlpha(HOT_CORE, alpha * this->festival_));
        p.drawEllipse(QPointF(x, y - 2.5 * k), 0.9 * k, 1.3 * k);
        p.fillRect(QRectF(x - 4 * k, y + 2 * k, 8 * k, 1.1 * s),
                   withAlpha(WARM_LIGHT, 0.25 * alpha * this->festival_));
    }

    // Raid boats: a line of paper boats with lanterns on their masts
    const double lane = w.riverY + (H - w.riverY) * 0.55;
    for (const auto &b : this->boats_)
    {
        const double a = b.age;
        const double u = state.reduced ? 1.0 : easeOutCubic(a / b.travel);
        const double x =
            lerp(b.startX, b.targetX, u) + a * 2 * s * (state.reduced ? 0 : 1) +
            off.x();
        const double bob =
            state.reduced ? 0 : std::sin(a * 1.6 + b.phase) * 1.2 * s;
        const double y = lane + bob + off.y();
        const double alpha =
            smoothstep(0, state.reduced ? 1.5 : 0.6, a) *
            (1 - smoothstep(b.life - 5, b.life, a));
        const double k = b.size;
        p.save();
        p.setOpacity(p.opacity() * alpha);
        // Reflection first, under the hull
        p.setPen(Qt::NoPen);
        for (int r = 0; r < 4; ++r)
        {
            const double wob =
                state.reduced ? 0 : std::sin(state.t * 1.8 + r + b.phase) * 1.5 * s;
            p.fillRect(QRectF(x - (6 - r) * k + wob, y + (5 + r * 2.6) * k,
                              (12 - 2 * r) * k, 1.2 * s),
                       withAlpha(b.color, 0.35 - r * 0.07));
        }
        QPainterPath hull(QPointF(x - 12 * k, y - 4 * k));
        hull.quadTo(QPointF(x, y + 7 * k), QPointF(x + 12 * k, y - 4 * k));
        hull.lineTo(QPointF(x + 7 * k, y - 0.5 * k));
        hull.quadTo(QPointF(x, y + 1.5 * k), QPointF(x - 7 * k, y - 0.5 * k));
        hull.closeSubpath();
        fillPaper(p, hull, mixColor(HULL_PAPER, TOWN, 0.4), HULL_PAPER, 0.8);
        p.setPen(QPen(QColor(0x2a, 0x1c, 0x18), 0.9 * s));
        p.drawLine(QPointF(x, y), QPointF(x, y - 12 * k));
        const QPointF lantern(x, y - 15 * k);
        lanternHalo(p, lantern, 6.5 * k, 1, 1);
        drawLantern(p, lantern, 6.5 * k, b.color, 1,
                    state.reduced ? 0 : std::sin(a * 1.6 + b.phase) * 0.12, 1);
        p.restore();
    }

    for (const auto &r : this->ripples_)
    {
        const double f = r.age / 1.6;
        const double radius = r.size * (0.5 + f * 1.6);
        p.setPen(QPen(withAlpha(STAR_PAPER, 0.35 * (1 - f)), 0.9 * s));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(r.pos, radius, radius * 0.28);
    }
}

void LanternScene::paintFireworks(QPainter &p)
{
    const double s = this->world_->s;
    for (const auto &f : this->fireworks_)
    {
        const double a = f.age - f.delay;
        if (a < 0)
        {
            continue;
        }
        const double u = clamp01(a / f.life);
        const double open =
            this->reduced_ ? 1.0 : easeOutCubic(std::min(1.0, a / 0.55));
        const double r = f.radius * open;
        const double alpha =
            (this->reduced_ ? smoothstep(0, 0.5, a) : smoothstep(0, 0.08, a)) *
            (1 - smoothstep(0.55, 1.0, u));
        const double droop =
            this->reduced_ ? 0 : std::pow(std::max(0.0, u - 0.35), 2) * 34 * s;
        drawGlow(p, f.center, r * 1.4, f.a, 0.14 * alpha);
        p.setPen(Qt::NoPen);
        // Two rings of cut paper petals unfolding from the centre
        for (int ring = 0; ring < 2; ++ring)
        {
            const double reach = ring == 0 ? r : r * 0.55;
            const QColor &color = ring == 0 ? f.a : f.b;
            for (int j = 0; j < f.petals; ++j)
            {
                const double ang =
                    f.rot + (j + 0.5 * ring) * TAU / f.petals;
                const QPointF dir(std::cos(ang), std::sin(ang));
                const QPointF side(-dir.y(), dir.x());
                const QPointF inner = f.center + dir * (reach * 0.3);
                const QPointF tip = f.center + dir * reach +
                                    QPointF(0, droop * (ring == 0 ? 1.0 : 0.6));
                const double width = reach * 0.08 + 0.6;
                const QPointF midPoint = (inner + tip) / 2;
                const QPointF petal[] = {inner, midPoint + side * width, tip,
                                         midPoint - side * width};
                p.setBrush(withAlpha(j % 2 == 0 ? color : mixColor(color, STAR_PAPER, 0.3),
                                     alpha * 0.92));
                p.drawPolygon(petal, 4);
                if (ring == 0)
                {
                    drawGlow(p, tip, 3.2 * s, color, alpha * 0.55);
                }
            }
        }
        p.setBrush(withAlpha(GOLD, alpha));
        p.drawEllipse(f.center, 2.2 * s * open, 2.2 * s * open);
    }
}

void LanternScene::paintLantern(QPainter &p, const Lantern &l, double alpha)
{
    if (alpha <= 0.01)
    {
        return;
    }
    const double a = l.age - l.delay;
    double size = l.size;
    if (l.kind == Kind::Free || l.kind == Kind::Gift || l.kind == Kind::Sub)
    {
        // Rising lanterns shrink into the distance
        size *= 1 - 0.35 * clamp01(a / l.life);
    }
    const double light = l.light + 0.7 * l.hover;
    lanternHalo(p, l.pos, size * (1 + 0.9 * l.hover), light, alpha);
    drawLantern(p, l.pos, size, l.color, std::min(1.0, l.light + 0.3 * l.hover),
                l.tilt, alpha, l.ornate);
    if (l.hover > 0.01)
    {
        drawGlow(p, l.pos + QPointF(0, size * 0.15), size * 0.7, HOT_CORE,
                 0.35 * l.hover * alpha);
    }
    if (l.kind == Kind::Falling && a < 1.2)
    {
        // A curl of smoke where the flame went out
        drawGlow(p, l.pos - QPointF(0, size * (0.7 + a * 1.2)), size * 0.6,
                 QColor(0x8a, 0x88, 0x98), 0.22 * (1 - a / 1.2) * alpha);
    }
    if (l.kind == Kind::Shout && !l.tag.isEmpty())
    {
        // A paper tag with the name, hanging from the lantern
        static const QFont font = [] {
            QFont f("Segoe UI");
            f.setBold(true);
            return f;
        }();
        QFont tagFont = font;
        const double s = this->world_->s;
        tagFont.setPixelSize(std::max(10, iround(11 * s)));
        const QString text =
            l.tag.size() > 18 ? l.tag.left(17) + QChar(0x2026) : l.tag;
        const QFontMetricsF metrics(tagFont);
        const double tw = metrics.horizontalAdvance(text) + 10 * s;
        const double th = 13 * s;
        p.save();
        p.translate(l.pos + QPointF(size * 0.55, size * 0.35));
        p.rotate(deg(l.tilt * 0.6 + 0.08));
        p.setOpacity(p.opacity() * alpha);
        p.setPen(QPen(withAlpha(GOLD, 0.9), 0.8 * s));
        p.drawLine(QPointF(0, 0), QPointF(3 * s, 5 * s));
        const QPolygonF tag({QPointF(3 * s, 5 * s), QPointF(3 * s + tw, 5 * s),
                             QPointF(3 * s + tw, 5 * s + th),
                             QPointF(3 * s, 5 * s + th),
                             QPointF(0, 5 * s + th / 2)});
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(SHADOW_INK, 0.4));
        p.drawPolygon(tag.translated(1.2 * s, 1.6 * s));
        p.setBrush(STAR_PAPER);
        p.drawPolygon(tag);
        p.setBrush(withAlpha(QColor(0x5a, 0x3a, 0x2a), 0.8));
        p.drawEllipse(QPointF(3.5 * s, 5 * s + th / 2), 1.1 * s, 1.1 * s);
        p.setFont(tagFont);
        p.setPen(QColor(0x3a, 0x1c, 0x14));
        p.drawText(QRectF(5 * s, 5 * s, tw, th), Qt::AlignCenter, text);
        p.restore();
    }
}

void LanternScene::paintLanterns(QPainter &p, const SceneState &state,
                                 bool distant)
{
    (void)state;
    for (const auto &l : this->lanterns_)
    {
        if ((l.kind == Kind::Distant) != distant)
        {
            continue;
        }
        double alpha = lanternAlpha(l);
        if (!distant)
        {
            this->paintLantern(p, l, alpha);
            continue;
        }
        // Tiny and far: a glow and a sliver of lit paper, fading with height
        alpha *= clamp01((l.pos.y() - 10) / 140);
        if (alpha <= 0.01)
        {
            continue;
        }
        drawGlow(p, l.pos, l.size * 2.6, WARM_LIGHT, alpha * 0.45 * l.light);
        p.fillRect(QRectF(l.pos.x() - l.size * 0.4, l.pos.y() - l.size * 0.5,
                          l.size * 0.8, l.size),
                   withAlpha(mixColor(l.color, HOT_CORE, 0.35), alpha * 0.9));
    }
}

void LanternScene::paintConstellation(QPainter &p, const SceneState &state)
{
    const double s = this->world_->s;
    const auto starAlpha = [](const Star &star) {
        return smoothstep(0, 1.2, star.age) *
               (1 - smoothstep(star.life - 25, star.life, star.age));
    };
    // Faint paper threads join your lanterns in the order you sent them
    for (size_t i = 1; i < this->stars_.size(); ++i)
    {
        const auto &a = this->stars_[i - 1];
        const auto &b = this->stars_[i];
        if (dist(a.pos, b.pos) > 280 * s)
        {
            continue;
        }
        p.setPen(QPen(withAlpha(STAR_PAPER,
                                0.18 * std::min(starAlpha(a), starAlpha(b))),
                      0.8 * s, Qt::DashLine));
        p.drawLine(a.pos, b.pos);
    }
    for (const auto &star : this->stars_)
    {
        const double alpha = starAlpha(star);
        const double bob =
            state.reduced ? 0 : std::sin(state.t * 0.9 + star.phase) * 1.2 * s;
        const double light =
            0.85 +
            (state.reduced ? 0 : 0.1 * std::sin(state.t * 2.3 + star.phase * 2)) +
            0.6 * star.hover;
        const QPointF at = star.pos + QPointF(0, bob);
        lanternHalo(p, at, star.size * (1 + 0.5 * star.hover), light, alpha);
        drawLantern(p, at, star.size, star.color, std::min(1.0, light),
                    std::sin(state.t * 0.7 + star.phase) * 0.06, alpha);
        if (star.hover > 0.01)
        {
            drawGlow(p, at, star.size * 1.2, HOT_CORE, 0.3 * star.hover * alpha);
        }
    }
}

void LanternScene::paintFlyer(QPainter &p, const Flyer &f, double alpha)
{
    if (alpha <= 0.01)
    {
        return;
    }
    const double h = f.size * f.aspect;
    lanternHalo(p, f.pos, f.size * 1.3, f.light, std::min(1.0, alpha * 1.6));
    if (f.light > 1.02)
    {
        drawGlow(p, f.pos + QPointF(0, h * 0.2),
                 f.size * (0.9 + (f.light - 1)), HOT_CORE,
                 alpha * 0.6 * (f.light - 1));
    }
    if (std::abs(f.aspect - 1.04) > 0.02)
    {
        // Still finishing its inflation: draw the live shape
        p.save();
        p.translate(f.pos);
        p.rotate(deg(f.tilt));
        p.setOpacity(p.opacity() * alpha);
        LanternLook look;
        look.paper = f.color;
        look.light = f.light;
        paintLanternShape(p, f.size, h, look);
        p.restore();
        return;
    }
    drawLantern(p, f.pos, f.size, f.color, std::min(1.0, f.light), f.tilt,
                alpha);
}

void LanternScene::paintTyping(QPainter &p, const SceneState &state,
                               const SceneLayout &layout)
{
    const TypingPose pose = this->typingPose(state, layout);
    if (pose.w <= 0 || pose.alpha <= 0.01)
    {
        return;
    }
    const double alpha = pose.alpha;
    const double unfold = 0.4 + 0.6 * easeOutCubic(alpha);
    const double w = pose.w * unfold;
    const double h = pose.h * unfold;
    const double capH = std::max(1.0, w * 0.1);
    const double full = clamp01(this->inflate_);

    // Tethered to a peg on the bank; the cord goes taut as it fills
    const QPointF bottom = pose.center + QPointF(-w * 0.2, h / 2 + capH * 0.7);
    const QPointF peg = pose.peg - QPointF(w * 0.45, 0);
    QPainterPath cord(bottom);
    cord.quadTo(QPointF((bottom.x() + peg.x()) / 2 - 3,
                        (bottom.y() + peg.y()) / 2 + 3 * (1 - full)),
                peg);
    p.setPen(QPen(withAlpha(GOLD.darker(160), 0.85 * alpha), 0.9));
    p.setBrush(Qt::NoBrush);
    p.drawPath(cord);
    p.fillRect(QRectF(peg.x() - 1.5, peg.y() - 2.5, 3, 3),
               withAlpha(QColor(0x2a, 0x17, 0x12), alpha));

    // With no bar above the box it stands inside the chat's corner, under the
    // text glass, so it burns a little brighter to read through it
    const bool overChat =
        std::any_of(layout.chats.begin(), layout.chats.end(),
                    [&](const QRectF &r) {
                        return r.contains(pose.center);
                    });
    const double boost = overChat ? 1.6 : 1.0;
    lanternHalo(p, pose.center, w, this->flame_, alpha * 1.2 * boost);
    p.save();
    p.translate(pose.center);
    p.rotate(deg(pose.tilt));
    p.setOpacity(p.opacity() * alpha);
    LanternLook look;
    look.paper = LANTERN_ORANGE;
    look.light = this->flame_ * (overChat ? 1.2 : 1.0);
    look.tassel = -pose.tilt * 0.8 +
                  (state.reduced ? 0 : std::sin(state.t * 2.1) * 0.05);
    paintLanternShape(p, w, h, look);
    p.restore();
}

void LanternScene::paintNear(QPainter &p, const World &w,
                             const SceneState &state)
{
    const double s = w.s;
    const QPointF off = this->offset(w, w.bank.depth);
    p.drawImage(w.bank.origin + off, w.bank.image);

    // Reeds lean with the wind; a gust lays them flat
    for (const auto &c : w.reeds)
    {
        const QImage &img = w.reedSprites[static_cast<size_t>(c.sprite)];
        const double sw = img.width() / w.dpr * c.scale;
        const double sh = img.height() / w.dpr * c.scale;
        const double sway =
            state.reduced ? 0
                          : std::sin(state.t * 0.9 + c.phase) * 0.05 +
                                std::sin(state.t * 2.1 + c.phase * 2) * 0.015;
        const double angle =
            (this->wind_ * 0.18 + sway + this->gust_ * 1.15) * c.flex;
        p.save();
        p.translate(c.base + off);
        p.rotate(deg(angle));
        p.drawImage(QRectF(-sw / 2, -sh, sw, sh), img);
        p.restore();
    }

    // A garland of small paper lanterns along the top edge
    if (w.garland.size() < 2)
    {
        return;
    }
    const QPointF goff = this->offset(w, 1.15);
    const double sag = 13 * s;
    const double sway = state.reduced ? 0 : std::sin(state.t * 0.8) * 1.2 * s;
    const int count = static_cast<int>(w.garland.size() - 1) * 4;
    const std::array<QColor, 3> papers{LANTERN_RED, LANTERN_ORANGE,
                                       QColor(0xf2, 0xb8, 0x4a)};
    int index = 0;
    for (size_t k = 0; k + 1 < w.garland.size(); ++k)
    {
        const QPointF a = w.garland[k] + goff;
        const QPointF b = w.garland[k + 1] + goff;
        const auto along = [&](double f) {
            return QPointF(lerp(a.x(), b.x(), f),
                           lerp(a.y(), b.y(), f) + (sag + sway) * 4 * f * (1 - f));
        };
        QPolygonF line;
        for (int j = 0; j <= 12; ++j)
        {
            line << along(j / 12.0);
        }
        p.setPen(QPen(withAlpha(RAIL.lighter(150), 0.9), 0.9 * s));
        p.setBrush(Qt::NoBrush);
        p.drawPolyline(line);
        for (int j = 1; j <= 4; ++j, ++index)
        {
            const QPointF hook = along(j / 5.0);
            const double size = 6.5 * s;
            const QPointF center = hook + QPointF(0, size * 0.62 + 1.5 * s);
            // Closing time puts them out one by one from the far end
            const double lit =
                clamp01(this->festival_ * (count + 2) - (count - index));
            double hover = 0;
            double push = 0;
            if (this->pointer_)
            {
                const double d = dist(*this->pointer_, center);
                hover = clamp01(1 - d / 40);
                push = d < 120 ? this->pointerVel_.x() * (1 - d / 120) : 0;
            }
            const double tilt =
                this->wind_ * 0.25 + push * 0.0007 +
                (state.reduced ? 0 : std::sin(state.t * 1.3 + index) * 0.07);
            p.setPen(QPen(withAlpha(RAIL.lighter(150), 0.9), 0.7 * s));
            p.drawLine(hook, center - QPointF(0, size * 0.55));
            lanternHalo(p, center, size * (1 + 0.6 * hover),
                        lit * (1 + 0.6 * hover), 1);
            drawLantern(p, center, size, papers[static_cast<size_t>(index % 3)],
                        lit, tilt, 1);
            if (hover > 0.01)
            {
                drawGlow(p, center, size * 1.2, HOT_CORE, 0.3 * hover * lit);
            }
        }
    }
}

void LanternScene::paintSparks(QPainter &p, bool front)
{
    p.setPen(Qt::NoPen);
    for (const auto &sp : this->sparks_)
    {
        if (sp.front != front)
        {
            continue;
        }
        const double alpha = 1 - smoothstep(0.45, 1.0, sp.age / sp.life);
        drawGlow(p, sp.pos, sp.size * 2.8, sp.color, 0.5 * alpha);
        const QPointF d(std::cos(sp.angle) * sp.size,
                        std::sin(sp.angle) * sp.size);
        const QPointF n(-d.y() * 0.4, d.x() * 0.4);
        const QPointF diamond[] = {sp.pos + d, sp.pos + n, sp.pos - d,
                                   sp.pos - n};
        p.setBrush(withAlpha(sp.color, alpha));
        p.drawPolygon(diamond, 4);
    }
}

void LanternScene::paintCurtain(QPainter &p)
{
    if (this->curtainAge_ < 0 || this->reduced_ || !this->oldWorld_)
    {
        return;
    }
    const double x = this->curtainLead() - this->curtainW_ - this->curtainM_;
    p.drawImage(QPointF(x, 0), this->curtain_);
}

void LanternScene::paintBack(QPainter &painter, const SceneState &state,
                             const SceneLayout &layout)
{
    this->ensureWorld(state, layout);
    this->reduced_ = state.reduced;
    QPainter &p = painter;

    // Far to near: sky, moon, hills and town, then the bridge, the river,
    // everything in the air, your lantern, and the near bank
    this->forWorlds(p, [&](World &w) {
        this->composeBack(w, state);
        p.drawImage(QPointF(0, 0), w.composite);
        this->paintTownLights(p, w, state);
    });
    this->paintFireworks(p);
    this->paintLanterns(p, state, true);
    this->forWorlds(p, [&](World &w) {
        this->paintBridge(p, w, state);
    });
    this->forWorlds(p, [&](World &w) {
        this->paintRiver(p, w, state);
    });
    this->paintBoats(p, *this->world_, state);
    this->paintLanterns(p, state, false);
    this->paintConstellation(p, state);
    for (const auto &f : this->flyers_)
    {
        double alpha = 1;
        if (state.reduced)
        {
            alpha = f.age < 1.0 ? 1 - smoothstep(0, 1.0, f.age)
                                : smoothstep(1.0, 2.2, f.age);
        }
        this->paintFlyer(p, f, alpha);
    }
    this->paintTyping(p, state, layout);
    this->forWorlds(p, [&](World &w) {
        this->paintNear(p, w, state);
    });
    this->paintSparks(p, false);
    this->paintCurtain(p);
}

bool LanternScene::hasFront() const
{
    if (this->sendFlare_ >= 0 && this->sendFlare_ < 2.6)
    {
        return true;
    }
    for (const auto &f : this->flyers_)
    {
        if (f.age < 2.2)
        {
            return true;
        }
    }
    for (const auto &l : this->lanterns_)
    {
        if (l.front && l.age - l.delay < frontLimit(l))
        {
            return true;
        }
    }
    for (const auto &sp : this->sparks_)
    {
        if (sp.front)
        {
            return true;
        }
    }
    for (const double at : this->ignitedAt_)
    {
        if (this->t_ - at >= 0 && this->t_ - at < 1.6)
        {
            return true;
        }
    }
    return this->dragonAge_ >= 0 ||
           (this->sparklerAge_ >= 0 && this->sparklerAge_ < SPARKLER_TIME);
}

void LanternScene::paintFront(QPainter &painter, const SceneState &state,
                              const SceneLayout &layout)
{
    (void)layout;
    QPainter &p = painter;
    const double s = this->world_ ? this->world_->s : 1.0;
    // The flare as your lantern lets go, then the lantern itself, crisp above
    // the panels for its first moments
    if (this->sendFlare_ >= 0 && this->sendFlare_ < 0.7 && !state.reduced)
    {
        const double a = this->sendFlare_ / 0.7;
        drawGlow(p, this->sendFrom_, (30 + 50 * a) * s, WARM_LIGHT,
                 0.55 * (1 - a));
        // A ring of light pushed out by the flare
        p.setPen(QPen(withAlpha(HOT_CORE, 0.45 * (1 - a)), 1.4 * s));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(this->sendFrom_, (14 + 46 * a) * s, (10 + 30 * a) * s);
    }
    for (const auto &f : this->flyers_)
    {
        if (f.age >= 2.2)
        {
            continue;
        }
        const double alpha = state.reduced
                                 ? 0.9 * (1 - smoothstep(0, 1.0, f.age))
                                 : 1 - smoothstep(1.3, 2.2, f.age);
        this->paintFlyer(p, f, alpha);
    }
    for (const auto &l : this->lanterns_)
    {
        const double limit = frontLimit(l);
        const double a = l.age - l.delay;
        if (!l.front || a >= limit || a < 0)
        {
            continue;
        }
        this->paintLantern(p, l,
                           lanternAlpha(l) * (1 - smoothstep(limit - 0.8, limit, a)));
    }
    if (this->world_)
    {
        const World &w = *this->world_;
        if (this->dragonAge_ >= 0)
        {
            p.save();
            p.translate(this->offset(w, w.bridgeBack.depth));
            this->paintDragon(p, w, state, 0.9);
            p.restore();
        }
        this->paintSparkler(p, w, state);

        // Each string lantern flares as it catches
        const QPointF off = this->offset(w, w.bridgeBack.depth);
        const double swing =
            state.reduced ? 0
                          : std::sin(state.t * 0.7) * 1.5 * s + this->wind_ * 2 * s;
        const int n = std::min(w.stringCount,
                               static_cast<int>(this->ignitedAt_.size()));
        for (int i = 0; i < n; ++i)
        {
            const double since =
                state.t - this->ignitedAt_[static_cast<size_t>(i)];
            if (since < 0 || since > 1.6)
            {
                continue;
            }
            const double a = 1 - smoothstep(0.7, 1.6, since);
            const double f = (i + 1.0) / (n + 1);
            const QPointF hook(lerp(w.poleA.x(), w.poleB.x(), f),
                               lerp(w.poleA.y(), w.poleB.y(), f) +
                                   (w.sag + swing) * 4 * f * (1 - f));
            const double size = 12 * s;
            const QPointF center = hook + off + QPointF(0, 3 * s + size * 0.6);
            if (!state.reduced)
            {
                drawGlow(p, center, size * (2.6 - since), HOT_CORE,
                         0.45 * (1 - since / 1.6));
            }
            drawLantern(p, center, size,
                        i % 2 == 0 ? LANTERN_RED : LANTERN_ORANGE, 1, 0, a);
        }
    }
    this->paintSparks(p, true);
}

}  // namespace

std::unique_ptr<ThemeScene> makeLanternScene()
{
    return std::make_unique<LanternScene>();
}

}  // namespace chatterino::scenes
