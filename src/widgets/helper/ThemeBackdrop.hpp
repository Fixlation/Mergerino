// SPDX-License-Identifier: MIT

#pragma once

#include "singletons/ThemeEffect.hpp"

#include <QColor>
#include <QImage>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QSize>

#include <array>

class QPainter;

namespace chatterino {

/// Paints the animated background of an animated theme.
///
/// Smooth effects are evaluated on a coarse grid and scaled up, particles are
/// drawn from cached glow sprites. Positions are expressed in window
/// coordinates so that neighbouring splits share one continuous backdrop.
class ThemeBackdrop
{
public:
    /// @param target The area to fill, in painter coordinates
    /// @param windowOrigin The top-left of `target` in window coordinates
    /// @param windowSize The size of the window `target` lives in
    /// @param surface The opaque background colour the effect is drawn over
    /// @param seconds The animation clock, before the effect's speed is applied
    void paint(QPainter &painter, const QRect &target, QPoint windowOrigin,
               QSize windowSize, const ThemeEffect &effect,
               const QColor &surface, double seconds);

private:
    struct Frame {
        QRect target;
        QPoint origin;
        QSize window;
        const ThemeEffect &effect;
        QColor surface;
        double time;
        double intensity;
    };

    void paintField(QPainter &painter, const Frame &frame);
    void paintStars(QPainter &painter, const Frame &frame);
    void paintEmbers(QPainter &painter, const Frame &frame);
    void paintRain(QPainter &painter, const Frame &frame);

    void drawGlow(QPainter &painter, QPointF center, double radius,
                  size_t colorIndex, double opacity);
    void ensureSprites(const ThemeEffect &effect);

    QImage field_;
    std::array<QPixmap, 3> sprites_;
    std::array<QColor, 3> spriteColors_;
};

}  // namespace chatterino
