#include "providers/seventv/SeventvPaints.hpp"

#include "Application.hpp"
#include "messages/Image.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/paints/LinearGradientPaint.hpp"
#include "providers/seventv/paints/PaintDropShadow.hpp"
#include "providers/seventv/paints/RadialGradientPaint.hpp"
#include "providers/seventv/paints/SolidColorPaint.hpp"
#include "providers/seventv/paints/UrlPaint.hpp"
#include "singletons/WindowManager.hpp"
#include "util/DebugCount.hpp"
#include "util/PostToThread.hpp"
#include "util/Variant.hpp"

#include <QLinearGradient>
#include <QLineF>
#include <QPainter>
#include <QRadialGradient>
#include <QUrlQuery>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using namespace chatterino;
using namespace Qt::Literals;

struct PaintGradientLayer {
    QString function;
    QGradientStops stops;
    std::optional<QColor> solidColor;
    bool repeat = false;
    float angle = 0.0F;
    qreal opacity = 1.0;
    ImagePtr image;
};

QColor rgbaToQColor(const uint32_t color)
{
    auto red = (int)((color >> 24) & 0xFF);
    auto green = (int)((color >> 16) & 0xFF);
    auto blue = (int)((color >> 8) & 0xFF);
    auto alpha = (int)(color & 0xFF);

    return {red, green, blue, alpha};
}

std::optional<uint32_t> parseRgbaValue(const QJsonValue &color)
{
    if (color.isNull() || color.isUndefined())
    {
        return std::nullopt;
    }

    bool ok = false;
    qint64 parsed = 0;
    if (color.isString())
    {
        parsed = color.toString().toLongLong(&ok);
    }
    else if (color.isDouble())
    {
        const double number = color.toDouble();
        ok = number >=
                 static_cast<double>(std::numeric_limits<int32_t>::min()) &&
             number <=
                 static_cast<double>(std::numeric_limits<uint32_t>::max());
        if (ok)
        {
            parsed = static_cast<qint64>(number);
        }
    }

    if (!ok || parsed < std::numeric_limits<int32_t>::min() ||
        parsed > std::numeric_limits<uint32_t>::max())
    {
        return std::nullopt;
    }
    return static_cast<uint32_t>(parsed);
}

std::optional<QColor> parsePaintColor(const QJsonValue &color)
{
    const auto rgba = parseRgbaValue(color);
    if (!rgba)
    {
        return std::nullopt;
    }

    return rgbaToQColor(*rgba);
}

std::optional<QColor> parseHexRgbaColor(QString value)
{
    value = value.trimmed();
    if (value.startsWith('#'))
    {
        value.removeFirst();
    }
    if (value.size() != 6 && value.size() != 8)
    {
        return std::nullopt;
    }

    const auto parseByte = [&value](qsizetype offset, bool *ok) {
        return value.sliced(offset, 2).toInt(ok, 16);
    };
    bool redOK = false;
    bool greenOK = false;
    bool blueOK = false;
    bool alphaOK = true;
    const int red = parseByte(0, &redOK);
    const int green = parseByte(2, &greenOK);
    const int blue = parseByte(4, &blueOK);
    const int alpha = value.size() == 8 ? parseByte(6, &alphaOK) : 255;
    if (!redOK || !greenOK || !blueOK || !alphaOK)
    {
        return std::nullopt;
    }
    return QColor(red, green, blue, alpha);
}

QGradientStops parsePaintStops(const QJsonArray &stops)
{
    QGradientStops parsedStops;
    double lastStop = -1;

    for (const auto &stop : stops)
    {
        const auto stopObject = stop.toObject();

        const auto rgbaColor =
            parseRgbaValue(stopObject["color"]).value_or(0);
        auto position = stopObject["at"].toDouble();

        // HACK: qt does not support hard edges in gradients like css does
        // Setting a different color at the same position twice just overwrites
        // the previous color. So we have to shift the second point slightly
        // ahead, simulating an actual hard edge
        if (position <= lastStop)
        {
            position = lastStop + 0.0000001;
        }

        lastStop = position;
        parsedStops.append(QGradientStop(position, rgbaToQColor(rgbaColor)));
    }

    return parsedStops;
}

std::vector<PaintDropShadow> parseDropShadows(const QJsonArray &dropShadows)
{
    std::vector<PaintDropShadow> parsedDropShadows;

    for (const auto &shadow : dropShadows)
    {
        const auto shadowObject = shadow.toObject();

        const auto xOffset = shadowObject["x_offset"].toDouble();
        const auto yOffset = shadowObject["y_offset"].toDouble();
        const auto radius = shadowObject["radius"].toDouble();
        const auto rgbaColor =
            parseRgbaValue(shadowObject["color"]).value_or(0);

        parsedDropShadows.emplace_back(xOffset, yOffset, radius,
                                       rgbaToQColor(rgbaColor));
    }

    return parsedDropShadows;
}

bool isLinearGradient(const QString &function)
{
    return function == "LINEAR_GRADIENT" || function == "linear-gradient";
}

bool isRadialGradient(const QString &function)
{
    return function == "RADIAL_GRADIENT" || function == "radial-gradient";
}

bool isUrlGradient(const QString &function)
{
    return function == "URL" || function == "url";
}

std::optional<std::shared_ptr<Paint>> parsePaint(const QJsonObject &paintJson)
{
    const QString name = paintJson["name"].toString();
    const QString id = paintJson["id"].toString();

    const auto color = parsePaintColor(paintJson["color"]);
    const bool repeat = paintJson["repeat"].toBool();
    const float angle = (float)paintJson["angle"].toDouble();

    const QGradientStops stops = parsePaintStops(paintJson["stops"].toArray());

    const auto shadows = parseDropShadows(paintJson["shadows"].toArray());

    const QString function = paintJson["function"].toString();
    if (isLinearGradient(function))
    {
        return std::make_shared<LinearGradientPaint>(name, id, color, stops,
                                                     repeat, angle, shadows);
    }

    if (isRadialGradient(function))
    {
        return std::make_shared<RadialGradientPaint>(name, id, stops, repeat,
                                                     shadows);
    }

    if (isUrlGradient(function))
    {
        const QString url = paintJson["image_url"].toString();
        const ImagePtr image = Image::fromUrl({url}, 1);
        if (image == nullptr)
        {
            return std::nullopt;
        }

        return std::make_shared<UrlPaint>(name, id, image, shadows);
    }

    if (color || !shadows.empty())
    {
        return std::make_shared<SolidColorPaint>(name, id, color, shadows);
    }

    return std::nullopt;
}

QString browserPaintColor(const QColor &color)
{
    if (color.alpha() == 255)
    {
        return color.name(QColor::HexRgb);
    }

    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(QString::number(color.alphaF(), 'f', 3));
}

QString browserGradientStops(const QGradientStops &stops)
{
    QStringList serialized;
    serialized.reserve(stops.size());
    for (const auto &[position, color] : stops)
    {
        serialized.append(
            QStringLiteral("%1 %2%")
                .arg(browserPaintColor(color))
                .arg(QString::number(position * 100.0, 'f', 4)));
    }
    return serialized.join(QStringLiteral(", "));
}

QString browserGradientImage(const PaintGradientLayer &layer)
{
    const auto stops = browserGradientStops(layer.stops);
    if (stops.isEmpty())
    {
        return {};
    }

    if (isLinearGradient(layer.function))
    {
        return QStringLiteral("%1linear-gradient(%2deg, %3)")
            .arg(layer.repeat ? QStringLiteral("repeating-") : QString())
            .arg(QString::number(layer.angle, 'f', 3))
            .arg(stops);
    }

    if (isRadialGradient(layer.function))
    {
        return QStringLiteral("%1radial-gradient(circle, %2)")
            .arg(layer.repeat ? QStringLiteral("repeating-") : QString())
            .arg(stops);
    }

    return {};
}

class LayeredPaint final : public Paint
{
public:
    LayeredPaint(QString name, QString id, std::optional<QColor> color,
                 std::vector<PaintGradientLayer> layers,
                 std::vector<PaintDropShadow> dropShadows)
        : Paint(std::move(id))
        , name_(std::move(name))
        , color_(color)
        , layers_(std::move(layers))
        , dropShadows_(std::move(dropShadows))
    {
    }

    QBrush asBrush(QColor userColor, QRectF drawingRect) const override
    {
        const QSize targetSize =
            drawingRect.size().toSize().expandedTo(QSize(1, 1));
        QRectF targetRect(QPointF{}, QSizeF(targetSize));

        QPixmap target(targetSize);
        target.fill(this->color_.value_or(userColor));

        QPainter painter(&target);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        for (auto it = this->layers_.rbegin(); it != this->layers_.rend();
             ++it)
        {
            painter.setOpacity(std::clamp(it->opacity, 0.0, 1.0));
            if (it->solidColor)
            {
                painter.fillRect(targetRect, *it->solidColor);
                continue;
            }
            if (isUrlGradient(it->function))
            {
                this->drawUrlLayer(painter, target, *it);
                continue;
            }

            if (auto brush = this->makeGradientBrush(*it, targetRect))
            {
                painter.fillRect(targetRect, *brush);
            }
        }

        painter.setOpacity(1.0);

        painter.end();
        return {target};
    }

    const std::vector<PaintDropShadow> &getDropShadows() const override
    {
        return this->dropShadows_;
    }

    bool animated() const override
    {
        for (const auto &layer : this->layers_)
        {
            if (layer.image != nullptr && layer.image->animated())
            {
                return true;
            }
        }
        return false;
    }

    QString animationUrl() const
    {
        for (const auto &layer : this->layers_)
        {
            if (layer.image != nullptr && layer.image->animated())
            {
                return layer.image->url().string;
            }
        }
        return {};
    }

    QJsonArray browserLayers(const QColor &userColor) const
    {
        QJsonArray serializedLayers;
        serializedLayers.append(QJsonObject{
            {QStringLiteral("backgroundColor"),
             browserPaintColor(this->color_.value_or(userColor))},
            {QStringLiteral("opacity"), 1.0},
        });

        for (auto it = this->layers_.rbegin(); it != this->layers_.rend();
             ++it)
        {
            QJsonObject serialized{
                {QStringLiteral("opacity"),
                 std::clamp(it->opacity, 0.0, 1.0)},
            };
            if (it->solidColor)
            {
                serialized.insert(QStringLiteral("backgroundColor"),
                                  browserPaintColor(*it->solidColor));
            }
            else if (it->image != nullptr)
            {
                serialized.insert(QStringLiteral("imageUrl"),
                                  it->image->url().string);
            }
            else if (const auto gradient = browserGradientImage(*it);
                     !gradient.isEmpty())
            {
                serialized.insert(QStringLiteral("backgroundImage"),
                                  gradient);
            }
            else
            {
                continue;
            }
            serializedLayers.append(serialized);
        }

        return serializedLayers;
    }

private:
    static qreal repeatingStopPosition(qreal position,
                                       const QGradientStops &stops)
    {
        const qreal gradientStart = stops.first().first;
        const qreal gradientEnd = stops.last().first;
        const qreal gradientLength = gradientEnd - gradientStart;
        if (gradientLength == 0.0)
        {
            return position;
        }

        return (position - gradientStart) / gradientLength;
    }

    static void setGradientStops(QGradient &gradient,
                                 const PaintGradientLayer &layer)
    {
        for (const auto &[position, color] : layer.stops)
        {
            const auto offsetPosition =
                layer.repeat ? repeatingStopPosition(position, layer.stops)
                             : position;
            gradient.setColorAt(offsetPosition, color);
        }
    }

    std::optional<QBrush> makeGradientBrush(
        const PaintGradientLayer &layer, const QRectF &drawingRect) const
    {
        if (layer.stops.empty())
        {
            return std::nullopt;
        }

        if (isLinearGradient(layer.function))
        {
            return this->makeLinearGradientBrush(layer, drawingRect);
        }

        if (isRadialGradient(layer.function))
        {
            return this->makeRadialGradientBrush(layer, drawingRect);
        }

        return std::nullopt;
    }

    QBrush makeLinearGradientBrush(const PaintGradientLayer &layer,
                                   const QRectF &drawingRect) const
    {
        QPointF startPoint = drawingRect.bottomLeft();
        QPointF endPoint = drawingRect.topRight();

        const int angleStep = int(layer.angle / 90) % 4;
        if (angleStep == 1)
        {
            startPoint = drawingRect.topLeft();
            endPoint = drawingRect.bottomRight();
        }
        if (angleStep == 2)
        {
            startPoint = drawingRect.topRight();
            endPoint = drawingRect.bottomLeft();
        }
        if (angleStep == 3)
        {
            startPoint = drawingRect.bottomRight();
            endPoint = drawingRect.topLeft();
        }

        QLineF gradientAxis;
        gradientAxis.setP1(drawingRect.center());
        gradientAxis.setAngle(90.0F - layer.angle);

        QLineF colorStartAxis;
        colorStartAxis.setP1(startPoint);
        colorStartAxis.setAngle(-layer.angle);

        QLineF colorStopAxis;
        colorStopAxis.setP1(endPoint);
        colorStopAxis.setAngle(-layer.angle);

        QPointF gradientStart;
        QPointF gradientEnd;
        gradientAxis.intersects(colorStartAxis, &gradientStart);
        gradientAxis.intersects(colorStopAxis, &gradientEnd);

        if (layer.repeat)
        {
            QLineF gradientLine(gradientStart, gradientEnd);
            gradientStart = gradientLine.pointAt(layer.stops.front().first);
            gradientEnd = gradientLine.pointAt(layer.stops.back().first);
        }

        QLinearGradient gradient(gradientStart, gradientEnd);
        gradient.setSpread(layer.repeat ? QGradient::RepeatSpread
                                        : QGradient::PadSpread);
        setGradientStops(gradient, layer);
        return {gradient};
    }

    QBrush makeRadialGradientBrush(const PaintGradientLayer &layer,
                                   const QRectF &drawingRect) const
    {
        const double x = drawingRect.x() + (drawingRect.width() / 2);
        const double y = drawingRect.y() + (drawingRect.height() / 2);

        double radius = std::max(drawingRect.width(), drawingRect.height()) / 2;
        radius = layer.repeat ? radius * layer.stops.back().first : radius;

        QRadialGradient gradient(x, y, radius);
        gradient.setSpread(layer.repeat ? QGradient::RepeatSpread
                                        : QGradient::PadSpread);
        setGradientStops(gradient, layer);
        return {gradient};
    }

    void drawUrlLayer(QPainter &painter, const QPixmap &target,
                      const PaintGradientLayer &layer) const
    {
        if (layer.image == nullptr)
        {
            return;
        }

        if (const auto paintPixmap = layer.image->pixmapOrLoad())
        {
            painter.drawPixmap(target.rect(), *paintPixmap,
                               paintPixmap->rect());
        }
    }

    const QString name_;
    const std::optional<QColor> color_;
    const std::vector<PaintGradientLayer> layers_;
    const std::vector<PaintDropShadow> dropShadows_;
};

QGradientStops parseV4PaintStops(const QJsonArray &stops)
{
    QGradientStops parsedStops;
    double lastStop = -1.0;
    for (const auto &value : stops)
    {
        const auto stop = value.toObject();
        const auto color = parseHexRgbaColor(
            stop.value(QStringLiteral("color"))
                .toObject()
                .value(QStringLiteral("hex"))
                .toString());
        if (!color)
        {
            continue;
        }

        auto position = stop.value(QStringLiteral("at")).toDouble();
        if (position <= lastStop)
        {
            position = lastStop + 0.0000001;
        }
        lastStop = position;
        parsedStops.append({position, *color});
    }
    return parsedStops;
}

std::vector<PaintDropShadow> parseV4PaintShadows(const QJsonArray &shadows)
{
    std::vector<PaintDropShadow> parsed;
    for (const auto &value : shadows)
    {
        const auto shadow = value.toObject();
        const auto color = parseHexRgbaColor(
            shadow.value(QStringLiteral("color"))
                .toObject()
                .value(QStringLiteral("hex"))
                .toString());
        if (!color)
        {
            continue;
        }
        parsed.emplace_back(
            shadow.value(QStringLiteral("offsetX")).toDouble(),
            shadow.value(QStringLiteral("offsetY")).toDouble(),
            shadow.value(QStringLiteral("blur")).toDouble(), *color);
    }
    return parsed;
}

QString selectV4PaintImage(const QJsonArray &images)
{
    if (images.isEmpty())
    {
        return {};
    }

    const bool animated = std::ranges::any_of(images, [](const auto &value) {
        return value.toObject()
                   .value(QStringLiteral("frameCount"))
                   .toInt() > 1;
    });
    QJsonObject selected;
    for (const auto &value : images)
    {
        const auto image = value.toObject();
        const bool matchesAnimation =
            (image.value(QStringLiteral("frameCount")).toInt() > 1) ==
            animated;
        if (image.value(QStringLiteral("scale")).toInt() == 1 &&
            matchesAnimation)
        {
            selected = image;
            break;
        }
        if (selected.isEmpty() && matchesAnimation)
        {
            selected = image;
        }
    }
    if (selected.isEmpty())
    {
        selected = images.first().toObject();
    }

    auto url = selected.value(QStringLiteral("url")).toString();
    if (url.startsWith(QStringLiteral("//")))
    {
        url.prepend(QStringLiteral("https:"));
    }
    return url;
}

std::optional<PaintGradientLayer> parseV4PaintLayer(
    const QJsonObject &layerJson)
{
    PaintGradientLayer layer;
    layer.opacity = std::clamp(
        layerJson.value(QStringLiteral("opacity")).toDouble(1.0), 0.0, 1.0);
    const auto type = layerJson.value(QStringLiteral("ty")).toObject();
    const auto typeName =
        type.value(QStringLiteral("__typename")).toString();

    if (typeName == QStringLiteral("PaintLayerTypeSingleColor"))
    {
        layer.solidColor = parseHexRgbaColor(
            type.value(QStringLiteral("color"))
                .toObject()
                .value(QStringLiteral("hex"))
                .toString());
        return layer.solidColor ? std::optional{layer} : std::nullopt;
    }
    if (typeName == QStringLiteral("PaintLayerTypeLinearGradient"))
    {
        layer.function = QStringLiteral("LINEAR_GRADIENT");
        layer.angle =
            static_cast<float>(type.value(QStringLiteral("angle")).toDouble());
        layer.repeat = type.value(QStringLiteral("repeating")).toBool();
        layer.stops =
            parseV4PaintStops(type.value(QStringLiteral("stops")).toArray());
        return layer.stops.isEmpty() ? std::nullopt : std::optional{layer};
    }
    if (typeName == QStringLiteral("PaintLayerTypeRadialGradient"))
    {
        layer.function = QStringLiteral("RADIAL_GRADIENT");
        layer.repeat = type.value(QStringLiteral("repeating")).toBool();
        layer.stops =
            parseV4PaintStops(type.value(QStringLiteral("stops")).toArray());
        return layer.stops.isEmpty() ? std::nullopt : std::optional{layer};
    }
    if (typeName == QStringLiteral("PaintLayerTypeImage"))
    {
        const auto url =
            selectV4PaintImage(type.value(QStringLiteral("images")).toArray());
        if (url.isEmpty())
        {
            return std::nullopt;
        }
        layer.function = QStringLiteral("URL");
        layer.image = Image::fromUrl({url}, 1);
        return layer.image == nullptr ? std::nullopt : std::optional{layer};
    }
    return std::nullopt;
}

std::optional<std::shared_ptr<Paint>> parseV4Paint(
    const QJsonObject &paintJson)
{
    const auto id = paintJson.value(QStringLiteral("id")).toString();
    const auto data = paintJson.value(QStringLiteral("data")).toObject();
    if (id.isEmpty() || data.isEmpty())
    {
        return std::nullopt;
    }

    std::vector<PaintGradientLayer> layers;
    for (const auto &value :
         data.value(QStringLiteral("layers")).toArray())
    {
        if (auto layer = parseV4PaintLayer(value.toObject()))
        {
            layers.emplace_back(std::move(*layer));
        }
    }
    if (layers.empty())
    {
        return std::nullopt;
    }

    return std::make_shared<LayeredPaint>(
        paintJson.value(QStringLiteral("name")).toString(), id, std::nullopt,
        std::move(layers), parseV4PaintShadows(
                               data.value(QStringLiteral("shadows")).toArray()));
}

QString normalizeUsername(const QString &userName)
{
    return userName.toLower();
}

bool setIfDifferent(auto &map, auto &&key, const auto &value)
{
    auto it = map.find(key);
    if (it == map.end())
    {
        map.emplace(std::forward<decltype(key)>(key), value);
        return true;
    }

    if (it->second != value)
    {
        it->second = value;
        return true;
    }

    return false;
}

QJsonArray graphQLPaintShadows(const QJsonObject &paint)
{
    QJsonArray mergedShadows = paint["shadows"].toArray();
    const auto text = paint["text"].toObject();
    for (const auto &shadow : text["shadows"].toArray())
    {
        mergedShadows.append(shadow);
    }
    return mergedShadows;
}

QJsonObject graphQLPaintToLegacyPaint(const QJsonObject &paint)
{
    QJsonObject legacy{
        {"id", paint["id"].toString()},
        {"name", paint["name"].toString()},
        {"color", paint["color"]},
    };

    legacy["shadows"] = graphQLPaintShadows(paint);

    const auto gradients = paint["gradients"].toArray();
    if (!gradients.isEmpty())
    {
        const auto gradient = gradients.first().toObject();
        legacy["function"] = gradient["function"].toString();
        legacy["repeat"] = gradient["repeat"].toBool();
        legacy["angle"] = gradient["angle"].toDouble();
        legacy["stops"] = gradient["stops"].toArray();
        legacy["image_url"] = gradient["image_url"].toString();
    }

    return legacy;
}

std::optional<PaintGradientLayer> parseGraphQLGradientLayer(
    const QJsonObject &gradient)
{
    PaintGradientLayer layer;
    layer.function = gradient["function"].toString();
    layer.repeat = gradient["repeat"].toBool();
    layer.angle = gradient["angle"].toDouble();
    layer.stops = parsePaintStops(gradient["stops"].toArray());

    if (isLinearGradient(layer.function) || isRadialGradient(layer.function))
    {
        if (layer.stops.empty())
        {
            return std::nullopt;
        }
        return layer;
    }

    if (isUrlGradient(layer.function))
    {
        const QString url = gradient["image_url"].toString();
        layer.image = Image::fromUrl({url}, 1);
        if (layer.image == nullptr)
        {
            return std::nullopt;
        }
        return layer;
    }

    return std::nullopt;
}

std::optional<std::shared_ptr<Paint>> parseGraphQLPaint(
    const QJsonObject &paintJson)
{
    std::vector<PaintGradientLayer> layers;
    for (const auto &value : paintJson["gradients"].toArray())
    {
        if (auto layer = parseGraphQLGradientLayer(value.toObject()))
        {
            layers.emplace_back(std::move(*layer));
        }
    }

    if (layers.size() <= 1)
    {
        return parsePaint(graphQLPaintToLegacyPaint(paintJson));
    }

    const auto paintID = paintJson["id"].toString();
    if (paintID.isEmpty())
    {
        return std::nullopt;
    }

    return std::make_shared<LayeredPaint>(
        paintJson["name"].toString(), paintID,
        parsePaintColor(paintJson["color"]), std::move(layers),
        parseDropShadows(graphQLPaintShadows(paintJson)));
}

}  // namespace

namespace chatterino {

QString seventvPaintAnimationUrl(const std::shared_ptr<Paint> &paint)
{
    if (!paint || !paint->animated())
    {
        return {};
    }

    if (const auto urlPaint = std::dynamic_pointer_cast<UrlPaint>(paint))
    {
        return urlPaint->animationUrl();
    }
    if (const auto layeredPaint =
            std::dynamic_pointer_cast<LayeredPaint>(paint))
    {
        return layeredPaint->animationUrl();
    }
    return {};
}

QJsonArray seventvPaintBrowserLayers(const std::shared_ptr<Paint> &paint,
                                     const QColor &userColor)
{
    if (!paint || !paint->animated())
    {
        return {};
    }

    if (const auto urlPaint = std::dynamic_pointer_cast<UrlPaint>(paint))
    {
        return {
            QJsonObject{
                {QStringLiteral("backgroundColor"),
                 browserPaintColor(userColor)},
                {QStringLiteral("opacity"), 1.0},
            },
            QJsonObject{
                {QStringLiteral("imageUrl"), urlPaint->animationUrl()},
                {QStringLiteral("opacity"), 1.0},
            },
        };
    }
    if (const auto layeredPaint =
            std::dynamic_pointer_cast<LayeredPaint>(paint))
    {
        return layeredPaint->browserLayers(userColor);
    }
    return {};
}

std::shared_ptr<Paint> makeSeventvPaintFromGraphQL(
    const QJsonObject &paintJson)
{
    if (paintJson.value(QStringLiteral("data"))
            .toObject()
            .contains(QStringLiteral("layers")))
    {
        return parseV4Paint(paintJson).value_or(nullptr);
    }
    return parseGraphQLPaint(paintJson).value_or(nullptr);
}

SeventvPaints::SeventvPaints() = default;

std::shared_ptr<Paint> SeventvPaints::getPaint(const QString &userName,
                                               bool kick) const
{
    std::shared_lock lock(this->mutex_);
    const auto normalized = normalizeUsername(userName);

    if (kick)
    {
        const auto it = this->kickPaintMap_.find(normalized);
        if (it != this->kickPaintMap_.end())
        {
            return it->second;
        }
    }
    else
    {
        const auto it = this->twitchPaintMap_.find(normalized);
        if (it != this->twitchPaintMap_.end())
        {
            return it->second;
        }
    }
    return nullptr;
}

std::optional<QColor> SeventvPaints::getUserStyleColor(const QString &userName,
                                                       bool kick) const
{
    std::shared_lock lock(this->mutex_);
    const auto normalized = normalizeUsername(userName);

    const auto &map =
        kick ? this->kickPaintUserColors_ : this->twitchPaintUserColors_;
    const auto it = map.find(normalized);
    if (it == map.end())
    {
        return std::nullopt;
    }
    return it->second;
}

void SeventvPaints::addPaint(const QJsonObject &paintJson)
{
    const auto paintID = paintJson["id"].toString();
    auto paint = parsePaint(paintJson);
    if (!paint)
    {
        return;
    }

    this->addParsedPaint(paintID, *paint);
}

void SeventvPaints::addParsedPaint(const QString &paintID,
                                   std::shared_ptr<Paint> paint)
{
    bool changed = false;
    int64_t nAdded = 0;

    {
        std::unique_lock lock(this->mutex_);

        const bool isNewPaint = !this->knownPaints_.contains(paintID);
        if (isNewPaint)
        {
            DebugCount::increase(DebugObject::SeventvPaints);
        }
        this->knownPaints_[paintID] = std::move(paint);
        const auto &knownPaint = this->knownPaints_.at(paintID);

        for (auto it = this->pendingTwitchPaintAssignments_.begin();
             it != this->pendingTwitchPaintAssignments_.end();)
        {
            if (it->second != paintID)
            {
                ++it;
                continue;
            }

            const bool inserted = !this->twitchPaintMap_.contains(it->first);
            if (setIfDifferent(this->twitchPaintMap_, it->first, knownPaint))
            {
                changed = true;
                if (inserted)
                {
                    nAdded++;
                }
            }
            it = this->pendingTwitchPaintAssignments_.erase(it);
        }

        for (auto it = this->pendingKickPaintAssignments_.begin();
             it != this->pendingKickPaintAssignments_.end();)
        {
            if (it->second != paintID)
            {
                ++it;
                continue;
            }

            const bool inserted = !this->kickPaintMap_.contains(it->first);
            if (setIfDifferent(this->kickPaintMap_, it->first, knownPaint))
            {
                changed = true;
                if (inserted)
                {
                    nAdded++;
                }
            }
            it = this->pendingKickPaintAssignments_.erase(it);
        }

        for (auto &[userName, assignedPaint] : this->twitchPaintMap_)
        {
            if (assignedPaint->id == paintID && assignedPaint != knownPaint)
            {
                assignedPaint = knownPaint;
                changed = true;
            }
        }

        for (auto &[userName, assignedPaint] : this->kickPaintMap_)
        {
            if (assignedPaint->id == paintID && assignedPaint != knownPaint)
            {
                assignedPaint = knownPaint;
                changed = true;
            }
        }
    }

    if (nAdded > 0)
    {
        DebugCount::increase(DebugObject::SeventvPaintAssignments, nAdded);
    }

    if (changed)
    {
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::addPaintFromGraphQL(const QJsonObject &paintJson)
{
    const auto paint = parseGraphQLPaint(paintJson);
    if (paint)
    {
        this->addParsedPaint((*paint)->id, *paint);
    }
}

void SeventvPaints::removePaint(const QString &paintID)
{
    int64_t nRemoved = 0;
    bool removedKnownPaint = false;

    {
        std::unique_lock lock(this->mutex_);

        removedKnownPaint = this->knownPaints_.erase(paintID) > 0;
        this->requestedPaintIDs_.erase(paintID);

        for (auto it = this->twitchPaintMap_.begin();
             it != this->twitchPaintMap_.end();)
        {
            if (it->second->id == paintID)
            {
                this->twitchPaintUserColors_.erase(it->first);
                it = this->twitchPaintMap_.erase(it);
                nRemoved++;
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->kickPaintMap_.begin();
             it != this->kickPaintMap_.end();)
        {
            if (it->second->id == paintID)
            {
                this->kickPaintUserColors_.erase(it->first);
                it = this->kickPaintMap_.erase(it);
                nRemoved++;
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingTwitchPaintAssignments_.begin();
             it != this->pendingTwitchPaintAssignments_.end();)
        {
            if (it->second == paintID)
            {
                it = this->pendingTwitchPaintAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto it = this->pendingKickPaintAssignments_.begin();
             it != this->pendingKickPaintAssignments_.end();)
        {
            if (it->second == paintID)
            {
                it = this->pendingKickPaintAssignments_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (removedKnownPaint)
    {
        DebugCount::decrease(DebugObject::SeventvPaints);
    }

    if (nRemoved > 0)
    {
        DebugCount::decrease(DebugObject::SeventvPaintAssignments, nRemoved);
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::assignPaintToUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users,
    std::optional<QColor> styleColor)
{
    bool changed = false;
    int64_t nAdded = 0;
    bool missingPaint = false;

    {
        std::unique_lock lock(this->mutex_);

        const auto paintIt = this->knownPaints_.find(paintID);
        const bool hasKnownPaint = paintIt != this->knownPaints_.end();
        auto addToMap = [&](auto &map, auto &pendingMap, auto &styleColorMap,
                            const QString &username) {
            const auto normalized = normalizeUsername(username);
            if (styleColor && styleColor->isValid() && styleColor->alpha() > 0)
            {
                const auto styleIt = styleColorMap.find(normalized);
                if (styleIt == styleColorMap.end())
                {
                    styleColorMap.emplace(normalized, *styleColor);
                    changed = true;
                }
                else if (styleIt->second != *styleColor)
                {
                    styleIt->second = *styleColor;
                    changed = true;
                }
            }

            if (hasKnownPaint)
            {
                const bool inserted = !map.contains(normalized);
                if (setIfDifferent(map, normalized, paintIt->second))
                {
                    changed = true;
                    if (inserted)
                    {
                        nAdded++;
                    }
                }
                pendingMap.erase(normalized);
            }
            else
            {
                pendingMap[normalized] = paintID;
                missingPaint = true;
            }
        };
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                            [&](const seventv::eventapi::TwitchUser &u) {
                                addToMap(this->twitchPaintMap_,
                                         this->pendingTwitchPaintAssignments_,
                                         this->twitchPaintUserColors_,
                                         u.userName);
                            },
                            [&](const seventv::eventapi::KickUser &u) {
                                addToMap(this->kickPaintMap_,
                                         this->pendingKickPaintAssignments_,
                                         this->kickPaintUserColors_,
                                         u.userName);
                            },
                       },
                       user);
        }
    }

    if (missingPaint)
    {
        this->ensurePaintLoaded(paintID);
    }

    if (nAdded > 0)
    {
        DebugCount::increase(DebugObject::SeventvPaintAssignments, nAdded);
    }

    if (changed)
    {
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::clearPaintFromUsers(
    const QString &paintID, std::span<const seventv::eventapi::User> users)
{
    int64_t nRemoved = 0;

    {
        std::unique_lock lock(this->mutex_);

        auto removeFromMap = [&](auto &map, auto &pendingMap,
                                 auto &styleColorMap,
                                 const QString &username) {
            const auto normalized = normalizeUsername(username);
            const auto it = map.find(normalized);
            if (it != map.end() && it->second->id == paintID)
            {
                map.erase(it);
                styleColorMap.erase(normalized);
                nRemoved++;
            }

            const auto pendingIt = pendingMap.find(normalized);
            if (pendingIt != pendingMap.end() && pendingIt->second == paintID)
            {
                pendingMap.erase(pendingIt);
            }
        };
        for (const auto &user : users)
        {
            std::visit(variant::Overloaded{
                            [&](const seventv::eventapi::TwitchUser &u) {
                                removeFromMap(this->twitchPaintMap_,
                                              this->pendingTwitchPaintAssignments_,
                                              this->twitchPaintUserColors_,
                                              u.userName);
                            },
                            [&](const seventv::eventapi::KickUser &u) {
                                removeFromMap(this->kickPaintMap_,
                                              this->pendingKickPaintAssignments_,
                                              this->kickPaintUserColors_,
                                              u.userName);
                            },
                       },
                       user);
        }
    }

    if (nRemoved > 0)
    {
        DebugCount::decrease(DebugObject::SeventvPaintAssignments, nRemoved);
        postToThread([] {
            getApp()->getWindows()->invalidateChannelViewBuffers();
        });
    }
}

void SeventvPaints::ensurePaintLoaded(const QString &paintID)
{
    if (paintID.isEmpty())
    {
        return;
    }

    {
        std::shared_lock lock(this->mutex_);
        if (this->knownPaints_.contains(paintID) ||
            this->requestedPaintIDs_.contains(paintID))
        {
            return;
        }
    }

    {
        std::unique_lock lock(this->mutex_);
        if (this->knownPaints_.contains(paintID) ||
            !this->requestedPaintIDs_.emplace(paintID).second)
        {
            return;
        }
    }

    auto *api = getApp()->getSeventvAPI();
    if (!api)
    {
        std::unique_lock lock(this->mutex_);
        this->requestedPaintIDs_.erase(paintID);
        return;
    }

    api->getCosmeticsByIDs(
        {paintID},
        [this, paintID](const QJsonObject &cosmetics) {
            const auto paints = cosmetics["paints"].toArray();
            for (const auto &paintValue : paints)
            {
                getApp()->getSeventvPaints()->addPaintFromGraphQL(
                    paintValue.toObject());
            }

            std::unique_lock lock(this->mutex_);
            this->requestedPaintIDs_.erase(paintID);
        },
        [this, paintID](const auto &) {
            std::unique_lock lock(this->mutex_);
            this->requestedPaintIDs_.erase(paintID);
        });
}

}  // namespace chatterino
