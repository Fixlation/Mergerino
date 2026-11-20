// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "common/AccountAccess.hpp"

#include "controllers/accounts/AccountController.hpp"
#include "mocks/BaseApplication.hpp"
#include "mocks/Helix.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "widgets/dialogs/LoginDialog.hpp"

#include <gtest/gtest.h>
#include <QApplication>
#include <QMessageBox>
#include <QTimer>

using namespace chatterino;

namespace {

QString primaryIdentity()
{
    return qEnvironmentVariable("MERGERINO_TEST_RESTRICTED_PRIMARY").trimmed();
}

QString aliasIdentity()
{
    return qEnvironmentVariable("MERGERINO_TEST_RESTRICTED_KICK_ALIAS").trimmed();
}

}  // namespace

TEST(AccountAccess, RejectsListedIdentitiesWithExactGenericError)
{
    const auto primary = primaryIdentity();
    const auto alias = aliasIdentity();
    if (primary.isEmpty() || alias.isEmpty())
    {
        GTEST_SKIP() << "Private identity fixtures are supplied through the environment.";
    }
    for (const auto &name : {primary, primary.toUpper(), QStringLiteral(" %1 ").arg(primary)})
    {
        EXPECT_EQ(accountAccessError(ProviderId::Twitch, name),
                  QStringLiteral("Error"));
        EXPECT_EQ(accountAccessError(ProviderId::Kick, name),
                  QStringLiteral("Error"));
    }
    for (const auto &name : {alias, alias.toUpper(), QStringLiteral(" %1 ").arg(alias)})
    {
        EXPECT_EQ(accountAccessError(ProviderId::Kick, name),
                  QStringLiteral("Error"));
    }
}

TEST(AccountAccess, KeepsRestrictionsSpecificToTheListedPlatform)
{
    const auto primary = primaryIdentity();
    const auto alias = aliasIdentity();
    if (primary.isEmpty() || alias.isEmpty())
    {
        GTEST_SKIP() << "Private identity fixtures are supplied through the environment.";
    }
    EXPECT_TRUE(accountAccessError(ProviderId::Twitch, alias).isEmpty());
    EXPECT_TRUE(accountAccessError(ProviderId::YouTube, primary).isEmpty());
    EXPECT_TRUE(accountAccessError(ProviderId::YouTube, alias).isEmpty());
}

TEST(AccountAccess, AllowsAnonymousUnrelatedAndSimilarNames)
{
    for (const auto provider : {ProviderId::Twitch, ProviderId::Kick})
    {
        for (const auto *name :
             {"", "allowed_user", "example2", "xexample", "example_", "example_user"})
        {
            SCOPED_TRACE(name);
            EXPECT_TRUE(accountAccessError(provider, QString(name)).isEmpty());
        }
    }
}

TEST(AccountAccess, SavedTwitchAccountCannotBeLoadedOrSelected)
{
    const auto identity = primaryIdentity();
    if (identity.isEmpty())
    {
        GTEST_SKIP() << "Private identity fixtures are supplied through the environment.";
    }
    mock::BaseApplication app(QStringLiteral(R"({"accounts":{
      "current":"%1",
      "uid1000":{"username":"%1","userID":"1000","clientID":"test-client","oauthToken":"blocked-token"},
      "uid1001":{"username":"allowed_user","userID":"1001","clientID":"test-client","oauthToken":"allowed-token"}
    }})").arg(identity.toUpper()));
    static testing::NiceMock<mock::Helix> helix;
    initializeHelix(&helix);
    AccountController accounts;
    ASSERT_EQ(accounts.twitch.currentUsername.getValue(), identity.toUpper());
    accounts.twitch.currentUserChanged.disconnect_all_slots();
    EXPECT_CALL(helix, update(QStringLiteral("test-client"),
                              QStringLiteral("allowed-token")));
    accounts.twitch.load();
    EXPECT_TRUE(testing::Mock::VerifyAndClearExpectations(&helix));
    EXPECT_EQ(accounts.twitch.getUsernames(),
              std::vector<QString>{QStringLiteral("allowed_user")});
    EXPECT_EQ(accounts.twitch.findUserByUsername(identity), nullptr);
    EXPECT_FALSE(accounts.twitch.isLoggedIn());
    EXPECT_TRUE(accounts.twitch.getCurrent()->isAnon());
    EXPECT_EQ(pajlada::Settings::Setting<QString>::get(
                  "/accounts/uid1000/oauthToken"),
              QStringLiteral("blocked-token"));
}

TEST(AccountAccess, SavedKickAccountsCannotBeLoadedOrSelected)
{
    const auto primary = primaryIdentity();
    const auto alias = aliasIdentity();
    if (primary.isEmpty() || alias.isEmpty())
    {
        GTEST_SKIP() << "Private identity fixtures are supplied through the environment.";
    }
    mock::BaseApplication app(QStringLiteral(R"({"kickAccounts":{
      "current":"%2",
      "uid1000":{"username":"%1","userID":1000,"clientID":"test-client","clientSecret":"test-secret","authToken":"test-token","refreshToken":"test-refresh","expiresAt":"2099-01-01T00:00:00Z"},
      "uid1001":{"username":"%2","userID":1001,"clientID":"test-client","clientSecret":"test-secret","authToken":"test-token","refreshToken":"test-refresh","expiresAt":"2099-01-01T00:00:00Z"}
    }})").arg(primary, alias.toUpper()));
    KickAccountManager accounts;
    ASSERT_EQ(accounts.currentUsername.getValue(), alias.toUpper());
    accounts.load();
    EXPECT_TRUE(accounts.current()->isAnonymous());
    accounts.currentUsername = primary;
    EXPECT_TRUE(accounts.current()->isAnonymous());
    EXPECT_TRUE(accounts.usernames().empty());
    EXPECT_EQ(accounts.findUserByUsername(primary), nullptr);
    EXPECT_EQ(accounts.findUserByUsername(alias), nullptr);
    EXPECT_FALSE(accounts.isLoggedIn());
    EXPECT_NE(KickAccountData::loadRaw("uid1000"), std::nullopt);
    EXPECT_NE(KickAccountData::loadRaw("uid1001"), std::nullopt);
}

TEST(AccountAccess, ManualSignInShowsOnlyErrorAndDoesNotSaveCredentials)
{
    const auto identity = primaryIdentity();
    if (identity.isEmpty())
    {
        GTEST_SKIP() << "Private identity fixtures are supplied through the environment.";
    }
    mock::BaseApplication app;
    AdvancedLoginWidget login;
    login.ui_.usernameInput.setText(identity);
    login.ui_.userIDInput.setText("test-restricted-user");
    login.ui_.clientIDInput.setText("test-client");
    login.ui_.oauthTokenInput.setText("test-token");
    bool sawError = false;
    QTimer::singleShot(0, [&] {
        for (auto *widget : QApplication::topLevelWidgets())
        {
            if (auto *message = qobject_cast<QMessageBox *>(widget))
            {
                sawError = true;
                EXPECT_EQ(message->windowTitle(), QStringLiteral("Error"));
                EXPECT_EQ(message->text(), QStringLiteral("Error"));
                message->accept();
            }
        }
    });
    login.ui_.buttonUpperRow.addUserButton.click();
    EXPECT_TRUE(sawError);
    EXPECT_TRUE(pajlada::Settings::Setting<QString>::get(
                    "/accounts/uidtest-restricted-user/oauthToken")
                    .isEmpty());
}
