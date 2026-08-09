#include "providers/kick/KickApi.hpp"
#include "util/BoostJsonWrap.hpp"

#include <gtest/gtest.h>

#include <boost/json.hpp>

using namespace chatterino;

TEST(KickApi, PreservesChannelSlugSeparators)
{
    EXPECT_EQ(KickApi::slugify(QStringLiteral("channel_with_underscores")),
              QStringLiteral("channel_with_underscores"));
    EXPECT_EQ(KickApi::slugify(QStringLiteral("hyphen-channel")),
              QStringLiteral("hyphen-channel"));
}

TEST(KickApi, ParsesAuthoritativeChannelRolesWithoutSelectedBadges)
{
    const boost::json::object payload{
        {"id", 123},
        {"username", "Fixlation"},
        {"is_moderator", true},
        {"is_channel_owner", false},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_EQ(info.userID, 123);
    EXPECT_EQ(info.username, QStringLiteral("Fixlation"));
    EXPECT_TRUE(info.isModerator);
    EXPECT_FALSE(info.isChannelOwner);
}

TEST(KickApi, ParsesChannelOwnerSeparatelyFromModerator)
{
    const boost::json::object payload{
        {"id", 456},
        {"username", "Broadcaster"},
        {"is_moderator", false},
        {"is_channel_owner", true},
        {"badges", boost::json::array{}},
        {"badges_v2", boost::json::array{}},
    };

    const KickPrivateUserInChannelInfo info{BoostJsonObject(payload)};
    EXPECT_FALSE(info.isModerator);
    EXPECT_TRUE(info.isChannelOwner);
}
