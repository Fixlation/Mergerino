#pragma once

#include <QColor>
#include <QMarginsF>

class QPixmapDropShadowFilter;

namespace chatterino {

class PaintDropShadow
{
public:
    PaintDropShadow(float xOffset, float yOffset, float radius, QColor color);

    bool isValid() const;
    PaintDropShadow scaled(float scale) const;
    QMarginsF padding(float scale) const;
    void apply(QPixmapDropShadowFilter &effect) const;

    float xOffset() const
    {
        return this->xOffset_;
    }

    float yOffset() const
    {
        return this->yOffset_;
    }

    float radius() const
    {
        return this->radius_;
    }

    const QColor &color() const
    {
        return this->color_;
    }

private:
    const float xOffset_;
    const float yOffset_;
    const float radius_;
    const QColor color_;
};

}  // namespace chatterino
