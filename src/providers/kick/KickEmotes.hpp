#pragma once

#include "messages/Emote.hpp"

namespace chatterino {

class KickEmotes
{
public:
    static EmotePtr emoteForID(QStringView id, QStringView name);
    static EmotePtr emoteForID(QStringView id, QStringView name,
                              const QString &imageUrl);
};

}  // namespace chatterino
