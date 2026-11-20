// SPDX-License-Identifier: MIT

#include "widgets/scenes/ThemeScene.hpp"

#include <QHash>
#include <QPainter>
#include <QRadialGradient>

#include <algorithm>

namespace chatterino::scenes {

namespace {

/// The blurred copy of the scene is this many times smaller than the scene
constexpr int BLUR_DIVISOR = 8;
constexpr int GLOW_SPRITE_SIZE = 96;

}  // namespace

bool SceneLayout::overText(QPointF point) const
{
    const auto contains = [&](const std::vector<QRectF> &rects) {
        return std::any_of(rects.begin(), rects.end(), [&](const QRectF &r) {
            return r.contains(point);
        });
    };
    return contains(this->chats) || contains(this->activity);
}

QRectF SceneLayout::inputBand() const
{
    if (this->focusChat.isEmpty())
    {
        return {};
    }
    const auto top = this->focusChat.bottom();
    return {this->focusChat.left(), top, this->focusChat.width(),
            std::max(0.0, this->window.height() - top)};
}

void SceneFrame::render(ThemeScene &scene, const SceneState &state,
                        const SceneLayout &layout)
{
    const auto dpr = std::max<qreal>(layout.dpr, 1.0);
    const QSize pixels(std::max(1, qRound(layout.window.width() * dpr)),
                       std::max(1, qRound(layout.window.height() * dpr)));
    if (this->back_.size() != pixels || this->dpr_ != dpr)
    {
        this->back_ = QImage(pixels, QImage::Format_RGB32);
        this->back_.setDevicePixelRatio(dpr);
        this->front_ = {};
        this->dpr_ = dpr;
    }

    {
        QPainter painter(&this->back_);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        scene.paintBack(painter, state, layout);
    }

    // A cheap blur: halve with nearest-neighbour, then average down
    const auto half =
        this->back_.scaled(std::max(1, pixels.width() / 2),
                           std::max(1, pixels.height() / 2),
                           Qt::IgnoreAspectRatio, Qt::FastTransformation);
    this->blurred_ = half.scaled(
        std::max(1, pixels.width() / BLUR_DIVISOR),
        std::max(1, pixels.height() / BLUR_DIVISOR), Qt::IgnoreAspectRatio,
        Qt::SmoothTransformation);

    this->hasFront_ = scene.hasFront();
    if (this->hasFront_)
    {
        if (this->front_.size() != pixels)
        {
            this->front_ = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
            this->front_.setDevicePixelRatio(dpr);
        }
        this->front_.fill(Qt::transparent);
        QPainter painter(&this->front_);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        scene.paintFront(painter, state, layout);
    }
}

void SceneFrame::paintSurface(QPainter &painter, const QRect &dest,
                              QPointF windowTopLeft, SceneSurface surface,
                              const QColor &tint,
                              const SceneGlass &glass) const
{
    if (this->back_.isNull() || dest.isEmpty())
    {
        painter.fillRect(dest, tint);
        return;
    }

    const auto dpr = this->dpr_;
    const QRectF source(windowTopLeft.x() * dpr, windowTopLeft.y() * dpr,
                        dest.width() * dpr, dest.height() * dpr);

    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF(dest), this->back_, source);

    double cover = 0;
    switch (surface)
    {
        case SceneSurface::Chat:
        case SceneSurface::Activity: {
            if (glass.blur > 0 && !this->blurred_.isNull())
            {
                const auto scale = double(this->blurred_.width()) /
                                   double(this->back_.width());
                const QRectF blurSource(source.x() * scale,
                                        source.y() * scale,
                                        source.width() * scale,
                                        source.height() * scale);
                painter.setOpacity(clamp01(glass.blur));
                painter.drawImage(QRectF(dest), this->blurred_, blurSource);
                painter.setOpacity(1.0);
            }
            cover = glass.text;
        }
        break;
        case SceneSurface::Tabs:
        case SceneSurface::Header:
        case SceneSurface::Input:
            cover = glass.chrome;
            break;
        case SceneSurface::Window:
            break;
    }

    if (cover > 0)
    {
        auto color = tint;
        color.setAlphaF(static_cast<float>(clamp01(cover)));
        painter.fillRect(dest, color);
    }
    painter.restore();
}

void SceneFrame::paintFront(QPainter &painter, const QRect &dest,
                            QPointF windowTopLeft) const
{
    if (!this->hasFront_ || this->front_.isNull() || dest.isEmpty())
    {
        return;
    }
    const auto dpr = this->dpr_;
    const QRectF source(windowTopLeft.x() * dpr, windowTopLeft.y() * dpr,
                        dest.width() * dpr, dest.height() * dpr);
    painter.drawImage(QRectF(dest), this->front_, source);
}

double hash01(int a, int b, int c)
{
    auto h = static_cast<std::uint32_t>(a) * 374761393U +
             static_cast<std::uint32_t>(b) * 668265263U +
             static_cast<std::uint32_t>(c) * 2147483647U;
    h = (h ^ (h >> 13)) * 1274126177U;
    h ^= h >> 16;
    return static_cast<double>(h) / 4294967295.0;
}

double valueNoise(double x, double y)
{
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const auto fx = x - x0;
    const auto fy = y - y0;
    const auto sx = fx * fx * (3 - 2 * fx);
    const auto sy = fy * fy * (3 - 2 * fy);
    const auto a = hash01(x0, y0);
    const auto b = hash01(x0 + 1, y0);
    const auto c = hash01(x0, y0 + 1);
    const auto d = hash01(x0 + 1, y0 + 1);
    return lerp(lerp(a, b, sx), lerp(c, d, sx), sy);
}

double SceneRng::next()
{
    std::uint32_t t = (this->state_ += 0x6D2B79F5U);
    t = (t ^ (t >> 15)) * (t | 1U);
    t ^= t + ((t ^ (t >> 7)) * (t | 61U));
    return static_cast<double>(t ^ (t >> 14)) / 4294967296.0;
}

QColor mixColor(const QColor &a, const QColor &b, double t)
{
    t = clamp01(t);
    return QColor::fromRgbF(
        static_cast<float>(lerp(a.redF(), b.redF(), t)),
        static_cast<float>(lerp(a.greenF(), b.greenF(), t)),
        static_cast<float>(lerp(a.blueF(), b.blueF(), t)),
        static_cast<float>(lerp(a.alphaF(), b.alphaF(), t)));
}

QColor withAlpha(QColor color, double alpha)
{
    color.setAlphaF(static_cast<float>(clamp01(alpha)));
    return color;
}

void drawGlow(QPainter &painter, QPointF center, double radius,
              const QColor &color, double alpha)
{
    if (radius <= 0.5 || alpha <= 0.003)
    {
        return;
    }

    static QHash<QRgb, QPixmap> sprites;
    const auto key = color.rgb();
    auto it = sprites.find(key);
    if (it == sprites.end())
    {
        if (sprites.size() > 256)
        {
            sprites.clear();
        }
        QPixmap sprite(GLOW_SPRITE_SIZE, GLOW_SPRITE_SIZE);
        sprite.fill(Qt::transparent);
        QPainter p(&sprite);
        QRadialGradient gradient(GLOW_SPRITE_SIZE / 2.0, GLOW_SPRITE_SIZE / 2.0,
                                 GLOW_SPRITE_SIZE / 2.0);
        auto c = QColor(key);
        gradient.setColorAt(0.0, withAlpha(c, 1.0));
        gradient.setColorAt(0.25, withAlpha(c, 0.55));
        gradient.setColorAt(0.6, withAlpha(c, 0.14));
        gradient.setColorAt(1.0, withAlpha(c, 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawEllipse(0, 0, GLOW_SPRITE_SIZE, GLOW_SPRITE_SIZE);
        p.end();
        it = sprites.insert(key, sprite);
    }

    const auto previous = painter.opacity();
    painter.setOpacity(previous * clamp01(alpha));
    painter.drawPixmap(QRectF(center.x() - radius, center.y() - radius,
                              radius * 2, radius * 2),
                       *it, QRectF(0, 0, GLOW_SPRITE_SIZE, GLOW_SPRITE_SIZE));
    painter.setOpacity(previous);
}

QImage makeLayer(QSizeF size, qreal dpr)
{
    QImage image(std::max(1, qRound(size.width() * dpr)),
                 std::max(1, qRound(size.height() * dpr)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    return image;
}

}  // namespace chatterino::scenes
