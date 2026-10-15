// SPDX-License-Identifier: MIT

#include "widgets/helper/ThemeBackdrop.hpp"

#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using namespace chatterino;

/// Size, in logical pixels, of one cell of the coarse effect grid
constexpr int FIELD_STEP = 6;
constexpr double TAU = 6.283185307179586;

/// Deterministic pseudo-random value in [0, 1] for a particle slot
double hash01(int a, int b, int c)
{
    auto h = static_cast<std::uint32_t>(a) * 374761393U +
             static_cast<std::uint32_t>(b) * 668265263U +
             static_cast<std::uint32_t>(c) * 2147483647U;
    h = (h ^ (h >> 13)) * 1274126177U;
    h ^= h >> 16;
    return static_cast<double>(h) / 4294967295.0;
}

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double fract(double value)
{
    return value - std::floor(value);
}

struct Rgb {
    double r;
    double g;
    double b;

    static Rgb of(const QColor &color)
    {
        return {static_cast<double>(color.red()),
                static_cast<double>(color.green()),
                static_cast<double>(color.blue())};
    }

    Rgb mixed(const Rgb &other, double amount) const
    {
        return {r + (other.r - r) * amount, g + (other.g - g) * amount,
                b + (other.b - b) * amount};
    }

    void mixIn(const Rgb &other, double amount)
    {
        *this = this->mixed(other, amount);
    }

    QRgb toRgb() const
    {
        return qRgb(std::clamp(static_cast<int>(r + 0.5), 0, 255),
                    std::clamp(static_cast<int>(g + 0.5), 0, 255),
                    std::clamp(static_cast<int>(b + 0.5), 0, 255));
    }
};

QColor withAlpha(QColor color, double alpha)
{
    color.setAlphaF(static_cast<float>(clamp01(alpha)));
    return color;
}

}  // namespace

namespace chatterino {

void ThemeBackdrop::paint(QPainter &painter, const QRect &target,
                          QPoint windowOrigin, QSize windowSize,
                          const ThemeEffect &effect, const QColor &surface,
                          double seconds)
{
    if (target.isEmpty())
    {
        return;
    }

    auto opaqueSurface = surface;
    opaqueSurface.setAlpha(255);
    if (!effect.isAnimated())
    {
        painter.fillRect(target, opaqueSurface);
        return;
    }

    // Light backgrounds show tints far more readily than dark ones
    const auto lightSurface = opaqueSurface.lightnessF() > 0.5F;
    const Frame frame{
        .target = target,
        .origin = windowOrigin,
        .window = windowSize.expandedTo(target.size()),
        .effect = effect,
        .surface = opaqueSurface,
        .time = seconds * effect.speed,
        .intensity = effect.intensity * (lightSurface ? 0.75 : 1.0),
    };

    painter.save();
    painter.setClipRect(target, Qt::IntersectClip);
    this->paintField(painter, frame);

    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setPen(Qt::NoPen);
    switch (effect.kind)
    {
        case ThemeEffectKind::Starfield:
            this->paintStars(painter, frame);
            break;
        case ThemeEffectKind::Embers:
            this->paintEmbers(painter, frame);
            break;
        case ThemeEffectKind::Rain:
            this->paintRain(painter, frame);
            break;
        case ThemeEffectKind::Aurora:
        case ThemeEffectKind::Drift:
        case ThemeEffectKind::None:
            break;
    }
    painter.restore();
}

void ThemeBackdrop::paintField(QPainter &painter, const Frame &frame)
{
    const auto columns = frame.target.width() / FIELD_STEP + 2;
    const auto rows = frame.target.height() / FIELD_STEP + 2;
    if (this->field_.width() != columns || this->field_.height() != rows)
    {
        this->field_ = QImage(columns, rows, QImage::Format_RGB32);
    }

    const auto t = frame.time;
    const auto intensity = frame.intensity;
    const auto height = static_cast<double>(frame.target.height());
    const auto base = Rgb::of(frame.surface);
    const std::array<Rgb, 3> colors{
        Rgb::of(frame.effect.colors[0]),
        Rgb::of(frame.effect.colors[1]),
        Rgb::of(frame.effect.colors[2]),
    };
    const auto columnX = [&frame](int column) {
        return static_cast<double>(frame.origin.x() + column * FIELD_STEP);
    };

    switch (frame.effect.kind)
    {
        case ThemeEffectKind::Aurora: {
            // Two curtains: a bright band with a sharp lower edge that fades
            // upwards into the third colour, shimmering with vertical rays.
            struct Curtain {
                double center;
                double strength;
                Rgb color;
            };
            std::vector<std::array<Curtain, 2>> curtains(columns);
            for (int column = 0; column < columns; ++column)
            {
                const auto x = columnX(column);
                for (int k = 0; k < 2; ++k)
                {
                    const auto center =
                        height * ((k == 0 ? 0.30 : 0.48) +
                                  0.075 * std::sin(x * 0.0042 + t * 0.11 +
                                                   k * 2.1) +
                                  0.035 * std::sin(x * 0.0115 - t * 0.19 + k));
                    auto rays =
                        0.5 + 0.5 * std::sin(x * 0.031 +
                                             1.9 * std::sin(x * 0.0055 +
                                                            t * 0.15 + k * 3) +
                                             t * 0.35);
                    rays = 0.35 + 0.65 * rays * rays;
                    const auto hue =
                        0.5 + 0.5 * std::sin(x * 0.0028 + t * 0.06 + k * 1.3);
                    curtains[column][k] = {
                        .center = center,
                        .strength = rays * (k == 0 ? 1.0 : 0.55) * intensity *
                                    0.75,
                        .color = colors[0].mixed(colors[1], hue),
                    };
                }
            }

            const auto lowerEdge = std::max(1.0, height * 0.05);
            const auto upperFade = std::max(1.0, height * 0.2);
            const auto topBlend = std::max(1.0, height * 0.28);
            for (int row = 0; row < rows; ++row)
            {
                const auto y = static_cast<double>(row * FIELD_STEP);
                auto *line = reinterpret_cast<QRgb *>(this->field_.scanLine(row));
                for (int column = 0; column < columns; ++column)
                {
                    auto pixel = base;
                    for (const auto &curtain : curtains[column])
                    {
                        const auto d = y - curtain.center;
                        const auto profile =
                            d > 0 ? std::exp(-(d / lowerEdge) * (d / lowerEdge))
                                  : std::exp(d / upperFade);
                        const auto color = curtain.color.mixed(
                            colors[2], clamp01(-d / topBlend));
                        pixel.mixIn(color, clamp01(profile * curtain.strength));
                    }
                    line[column] = pixel.toRgb();
                }
            }
            break;
        }

        case ThemeEffectKind::Drift: {
            // Three soft blobs wandering across the whole window
            const auto windowWidth = static_cast<double>(frame.window.width());
            const auto windowHeight = static_cast<double>(frame.window.height());
            const auto radius = std::max(windowWidth, windowHeight) * 0.55;
            std::array<std::vector<double>, 3> falloffX;
            std::array<std::vector<double>, 3> falloffY;
            for (int k = 0; k < 3; ++k)
            {
                const auto blobX =
                    windowWidth *
                    (0.5 + 0.45 * std::sin(t * (0.050 + k * 0.013) + k * 2.2));
                const auto blobY =
                    windowHeight *
                    (0.5 + 0.45 * std::cos(t * (0.041 + k * 0.017) + k * 1.4));
                falloffX[k].resize(columns);
                falloffY[k].resize(rows);
                for (int column = 0; column < columns; ++column)
                {
                    const auto dx = (columnX(column) - blobX) / radius;
                    falloffX[k][column] = std::exp(-dx * dx * 2.4);
                }
                for (int row = 0; row < rows; ++row)
                {
                    const auto dy = (frame.origin.y() + row * FIELD_STEP -
                                     blobY) /
                                    radius;
                    falloffY[k][row] = std::exp(-dy * dy * 2.4);
                }
            }
            for (int row = 0; row < rows; ++row)
            {
                auto *line = reinterpret_cast<QRgb *>(this->field_.scanLine(row));
                for (int column = 0; column < columns; ++column)
                {
                    auto pixel = base;
                    for (int k = 0; k < 3; ++k)
                    {
                        pixel.mixIn(colors[k], intensity * 0.36 *
                                                   falloffX[k][column] *
                                                   falloffY[k][row]);
                    }
                    line[column] = pixel.toRgb();
                }
            }
            break;
        }

        case ThemeEffectKind::Starfield: {
            // A faint nebula behind the stars
            const auto windowWidth = static_cast<double>(frame.window.width());
            const auto windowHeight = static_cast<double>(frame.window.height());
            const auto radius = std::max(windowWidth, windowHeight) * 0.5;
            const std::array<std::array<double, 2>, 2> clouds{{
                {windowWidth * 0.25 + 30 * std::sin(t * 0.03 + 1),
                 windowHeight * 0.3},
                {windowWidth * 0.75 + 30 * std::sin(t * 0.03 + 2),
                 windowHeight * 0.7},
            }};
            for (int row = 0; row < rows; ++row)
            {
                auto *line = reinterpret_cast<QRgb *>(this->field_.scanLine(row));
                const auto y =
                    static_cast<double>(frame.origin.y() + row * FIELD_STEP);
                for (int column = 0; column < columns; ++column)
                {
                    auto pixel = base;
                    const auto x = columnX(column);
                    for (size_t k = 0; k < clouds.size(); ++k)
                    {
                        const auto dx = (x - clouds[k][0]) / radius;
                        const auto dy = (y - clouds[k][1]) / radius;
                        pixel.mixIn(colors[k + 1],
                                    intensity * 0.16 *
                                        std::exp(-(dx * dx + dy * dy) * 7));
                    }
                    line[column] = pixel.toRgb();
                }
            }
            break;
        }

        case ThemeEffectKind::Embers:
        case ThemeEffectKind::Rain: {
            // A warm glow (or a cool mist) rising from the bottom edge
            const auto embers = frame.effect.kind == ThemeEffectKind::Embers;
            const auto &tint = embers ? colors[0] : colors[1];
            const auto strength =
                embers ? 0.20 * (0.85 + 0.15 * std::sin(t * 0.7)) : 0.10;
            const auto start = height * (embers ? 0.5 : 0.55);
            for (int row = 0; row < rows; ++row)
            {
                auto pixel = base;
                const auto y = static_cast<double>(row * FIELD_STEP);
                pixel.mixIn(tint, intensity * strength *
                                      clamp01((y - start) / (height - start)));
                const auto value = pixel.toRgb();
                auto *line = reinterpret_cast<QRgb *>(this->field_.scanLine(row));
                std::fill(line, line + columns, value);
            }
            break;
        }

        case ThemeEffectKind::None:
            this->field_.fill(frame.surface);
            break;
    }

    // Each grid sample sits on a cell corner; centre the scaled image on them
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    const auto offset = FIELD_STEP / 2.0;
    painter.drawImage(QRectF(frame.target.x() - offset,
                             frame.target.y() - offset,
                             static_cast<double>(columns * FIELD_STEP),
                             static_cast<double>(rows * FIELD_STEP)),
                      this->field_);
    painter.restore();
}

void ThemeBackdrop::paintStars(QPainter &painter, const Frame &frame)
{
    struct Layer {
        int cell;
        double radius;
        double brightness;
        double drift;
    };
    static constexpr std::array<Layer, 3> layers{{
        {46, 0.6, 0.6, 1.5},
        {82, 0.9, 0.8, 3.0},
        {150, 1.3, 1.0, 5.5},
    }};

    this->ensureSprites(frame.effect);
    const auto t = frame.time;
    const auto &target = frame.target;
    const auto twinkleFloor = 0.55 + 0.45 * frame.intensity;

    for (int index = 0; index < static_cast<int>(layers.size()); ++index)
    {
        const auto &layer = layers[index];
        const auto offset = t * layer.drift;
        const auto firstColumn = static_cast<int>(std::floor(
                                     (frame.origin.x() + offset) / layer.cell)) -
                                 1;
        const auto lastColumn =
            static_cast<int>(std::floor((frame.origin.x() + offset +
                                         target.width()) /
                                        layer.cell)) +
            1;
        const auto firstRow = frame.origin.y() / layer.cell - 1;
        const auto lastRow =
            (frame.origin.y() + target.height()) / layer.cell + 1;
        const auto salt = index * 7;

        for (int column = firstColumn; column <= lastColumn; ++column)
        {
            for (int row = firstRow; row <= lastRow; ++row)
            {
                if (hash01(column, row, salt + 1) > 0.9)
                {
                    continue;
                }
                const QPointF center(
                    (column + hash01(column, row, salt + 2)) * layer.cell -
                        offset - frame.origin.x() + target.x(),
                    (row + hash01(column, row, salt + 3)) * layer.cell -
                        frame.origin.y() + target.y());
                const auto pick = hash01(column, row, salt + 4);
                const size_t colorIndex = pick < 0.7 ? 0 : pick < 0.88 ? 1 : 2;
                const auto frequency = 0.25 + hash01(column, row, salt + 5) * 0.9;
                const auto phase = hash01(column, row, salt + 6) * TAU;
                const auto twinkle = 0.5 + 0.5 * std::sin(t * frequency * 2.2 +
                                                          phase);
                const auto alpha = layer.brightness *
                                   (0.35 + 0.65 * twinkle * twinkle) *
                                   twinkleFloor;
                if (layer.radius > 0.8)
                {
                    this->drawGlow(painter, center, layer.radius * 4.5,
                                   colorIndex, alpha * 0.35);
                }
                painter.setBrush(
                    withAlpha(frame.effect.colors[colorIndex], alpha));
                painter.drawEllipse(center, layer.radius, layer.radius);
            }
        }
    }

    // An occasional shooting star
    constexpr double period = 13.0;
    const auto slot = static_cast<int>(std::floor(t / period));
    if (hash01(slot, 3, 9) >= 0.75)
    {
        return;
    }
    const auto start = hash01(slot, 4, 9) * (period - 1.2);
    const auto progress = (t - slot * period - start) / 0.9;
    if (progress <= 0 || progress >= 1)
    {
        return;
    }
    const auto windowWidth = static_cast<double>(frame.window.width());
    const auto length = std::min(windowWidth, 900.0) * 0.45;
    const QPointF head(
        windowWidth * (0.3 + 0.6 * hash01(slot, 5, 9)) - length * progress -
            frame.origin.x() + target.x(),
        frame.window.height() * 0.25 * hash01(slot, 6, 9) +
            length * 0.45 * progress - frame.origin.y() + target.y());
    const QPointF tail = head + QPointF(90, -40);
    QLinearGradient trail(head, tail);
    const auto alpha = std::sin(progress * TAU / 2) * 0.9;
    trail.setColorAt(0, withAlpha(frame.effect.colors[0], alpha));
    trail.setColorAt(1, withAlpha(frame.effect.colors[0], 0));
    painter.setPen(QPen(QBrush(trail), 1.4, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(head, tail);
    painter.setPen(Qt::NoPen);
}

void ThemeBackdrop::paintEmbers(QPainter &painter, const Frame &frame)
{
    constexpr int cell = 30;
    this->ensureSprites(frame.effect);
    const auto t = frame.time;
    const auto &target = frame.target;
    const auto height = static_cast<double>(target.height());
    const auto firstColumn = frame.origin.x() / cell - 1;
    const auto lastColumn = (frame.origin.x() + target.width()) / cell + 1;

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        for (int k = 0; k < 2; ++k)
        {
            const auto h = [column, k](int salt) {
                return hash01(column, k, salt);
            };
            const auto period = 7 + h(1) * 7;
            const auto progress = fract(t / period + h(2));
            const auto x = column * cell + h(3) * cell +
                           (6 + h(4) * 14) *
                               std::sin(t * (0.6 + h(5)) + h(6) * TAU) +
                           progress * 18;
            const QPointF center(x - frame.origin.x() + target.x(),
                                 height * (1.04 - 1.1 * progress) + target.y());
            const auto radius = 1.0 + h(7) * 1.5;
            const auto pick = h(8);
            const size_t colorIndex = pick < 0.55 ? 0 : pick < 0.85 ? 1 : 2;
            const auto flicker =
                0.65 + 0.35 * std::sin(t * (5 + h(9) * 4) + h(10) * 20);
            const auto life =
                clamp01(progress / 0.08) * std::pow(1 - progress, 1.6);
            const auto alpha =
                std::min(1.0, flicker * life * (0.75 + 0.6 * frame.intensity));
            if (alpha <= 0.01)
            {
                continue;
            }
            this->drawGlow(painter, center, radius * 5, colorIndex,
                           alpha * 0.45);
            painter.setBrush(withAlpha(frame.effect.colors[colorIndex], alpha));
            painter.drawEllipse(center, radius, radius);
        }
    }
}

void ThemeBackdrop::paintRain(QPainter &painter, const Frame &frame)
{
    constexpr int cell = 22;
    const auto t = frame.time;
    const auto &target = frame.target;
    const auto height = static_cast<double>(target.height());
    const auto firstColumn = frame.origin.x() / cell - 2;
    const auto lastColumn = (frame.origin.x() + target.width()) / cell + 1;

    for (int column = firstColumn; column <= lastColumn; ++column)
    {
        for (int k = 0; k < 2; ++k)
        {
            const auto h = [column, k](int salt) {
                return hash01(column, k, salt + 20);
            };
            const auto speed = 420 + h(1) * 260;
            const auto length = 12 + h(2) * 16;
            const auto span = height + length + 60;
            const auto y = fract(t * speed / span + h(3)) *
                               span -
                           length - 30;
            const auto x = column * cell + h(4) * cell + y * 0.12;
            const auto near = h(5);
            const auto alpha = (0.10 + 0.18 * near) * (0.5 + 0.7 * frame.intensity);
            const auto &color = frame.effect.colors[near > 0.8 ? 2 : 0];
            painter.setPen(QPen(withAlpha(color, alpha), 0.8 + near * 0.6,
                                Qt::SolidLine, Qt::RoundCap));
            const QPointF top(x - frame.origin.x() + target.x(),
                              y + target.y());
            painter.drawLine(top, top + QPointF(length * 0.12, length));
        }
    }
    painter.setPen(Qt::NoPen);
}

void ThemeBackdrop::drawGlow(QPainter &painter, QPointF center, double radius,
                             size_t colorIndex, double opacity)
{
    if (opacity <= 0.005)
    {
        return;
    }
    const auto previous = painter.opacity();
    painter.setOpacity(previous * clamp01(opacity));
    painter.drawPixmap(QRectF(center.x() - radius, center.y() - radius,
                              radius * 2, radius * 2),
                       this->sprites_[colorIndex],
                       QRectF(this->sprites_[colorIndex].rect()));
    painter.setOpacity(previous);
}

void ThemeBackdrop::ensureSprites(const ThemeEffect &effect)
{
    constexpr int size = 32;
    for (size_t index = 0; index < this->sprites_.size(); ++index)
    {
        const auto &color = effect.colors[index];
        if (!this->sprites_[index].isNull() &&
            this->spriteColors_[index] == color)
        {
            continue;
        }

        QPixmap sprite(size, size);
        sprite.fill(Qt::transparent);
        QPainter spritePainter(&sprite);
        QRadialGradient gradient(QPointF(size / 2.0, size / 2.0), size / 2.0);
        gradient.setColorAt(0, withAlpha(color, 1.0));
        gradient.setColorAt(0.35, withAlpha(color, 0.35));
        gradient.setColorAt(1, withAlpha(color, 0.0));
        spritePainter.fillRect(sprite.rect(), gradient);
        spritePainter.end();

        this->sprites_[index] = sprite;
        this->spriteColors_[index] = color;
    }
}

}  // namespace chatterino
