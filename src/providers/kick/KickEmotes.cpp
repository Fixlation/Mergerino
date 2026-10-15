#include "providers/kick/KickEmotes.hpp"

#include "messages/Image.hpp"

#include <boost/unordered/unordered_flat_map.hpp>

namespace {

using namespace chatterino;

struct StringViewHash : std::hash<QStringView> {
    using is_transparent = std::true_type;
};

template <typename T>
using StringMap =
    boost::unordered_flat_map<QString, T, StringViewHash, std::equal_to<>>;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables, clazy-non-pod-global-static)
StringMap<std::weak_ptr<const Emote>> CACHE;

}  // namespace

namespace chatterino {

EmotePtr KickEmotes::emoteForID(QStringView id, QStringView name)
{
    return emoteForID(id, name, {});
}

EmotePtr KickEmotes::emoteForID(QStringView id, QStringView name,
                              const QString &imageUrl)
{
    auto resolvedUrl = imageUrl;
    auto it = CACHE.find(id);
    if (it != CACHE.end())
    {
        if (auto cached = it->second.lock())
        {
            if (cached->name.string == name &&
                (resolvedUrl.isEmpty() ||
                 cached->images.getImage1()->url().string == resolvedUrl))
            {
                return cached;
            }
            if (resolvedUrl.isEmpty())
            {
                resolvedUrl = cached->images.getImage1()->url().string;
            }
        }
    }
    if (resolvedUrl.isEmpty())
    {
        resolvedUrl = u"https://files.kick.com/emotes/" % id % u"/fullsize";
    }

    auto nameStr = name.toString();
    auto idStr = id.toString();
    QString tooltip = nameStr.toHtmlEscaped() % u"<br>Kick Emote";
    auto emote = std::make_shared<const Emote>(Emote{
        .name = {std::move(nameStr)},
        .images = ImageSet(
            Image::fromBoundedAutoscaledUrl({resolvedUrl}, {56, 28}, {28, 28})),
        .tooltip = {std::move(tooltip)},
        .id = {idStr},
    });
    CACHE[idStr] = emote;
    static size_t insertions = 0;
    if (++insertions % 256 == 0)
    {
        for (auto iter = CACHE.begin(); iter != CACHE.end();)
        {
            if (iter->second.expired())
            {
                CACHE.erase(iter++);
            }
            else
            {
                ++iter;
            }
        }
    }
    return emote;
}

}  // namespace chatterino
