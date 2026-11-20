#include "widgets/dialogs/TwitchLoginPage.hpp"

#include "Application.hpp"
#include "common/AccountAccess.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/twitch/TwitchAccountManager.hpp"
#include "singletons/Settings.hpp"
#include "util/HttpServer.hpp"

#include <pajlada/settings/setting.hpp>
#include <QApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QStringBuilder>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <cassert>
#include <cstring>
#include <functional>
#include <utility>

using namespace Qt::Literals;

namespace {

using namespace chatterino;

const QString TWITCH_CLIENT_ID = u"98jqwhwtt8a26bsqrq75f1cdfpgl9g"_s;
const QString REDIRECT_URL = u"http://localhost:38276"_s;
constexpr uint16_t SERVER_PORT = 38276;
constexpr auto HTML_CONTENT_TYPE = "text/html; charset=utf-8";
constexpr auto JSON_CONTENT_TYPE = "application/json; charset=utf-8";
constexpr qsizetype STATE_BYTES = 32;
constexpr int AUTHORIZATION_TIMEOUT_MS = 5 * 60 * 1000;
constexpr int NETWORK_STEP_TIMEOUT_MS = 30 * 1000;

QByteArray generateRandomBytes(qsizetype size)
{
    assert((size % 4) == 0);
    QByteArray bytes;
    bytes.resize(size);
    auto *gen = QRandomGenerator::system();
    for (qsizetype i = 0; i < bytes.size() / 4; i++)
    {
        quint32 v = gen->generate();
        std::memcpy(bytes.data() + (i * 4), &v, 4);
    }
    return bytes;
}

QByteArray generateState()
{
    return generateRandomBytes(STATE_BYTES).toBase64(
        QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString requestPath(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? target.left(queryIdx) : target;
}

QUrlQuery targetQuery(const QString &target)
{
    const auto queryIdx = target.indexOf('?');
    return queryIdx >= 0 ? QUrlQuery(target.mid(queryIdx + 1)) : QUrlQuery{};
}

QUrlQuery formQuery(const QByteArray &body)
{
    return QUrlQuery(QString::fromUtf8(body));
}

QByteArray renderPage(const QString &title, const QString &body,
                      const QString &scripts = {})
{
    auto page = QStringLiteral(R"(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>%1</title>
<style>
    :root {
        color-scheme: dark;
        font-family: "Inter", "Segoe UI", system-ui, sans-serif;
        background: #111217;
        color: #f3f3f5;
    }
    body {
        min-height: 100vh;
        margin: 0;
        display: grid;
        place-items: center;
    }
    main {
        width: min(460px, calc(100vw - 32px));
        border: 1px solid #2b2d36;
        background: #181a22;
        padding: 28px;
        box-sizing: border-box;
    }
    h1 {
        margin: 0 0 12px;
        font-size: 22px;
        font-weight: 700;
    }
    p {
        margin: 0;
        color: #c9cbd3;
        line-height: 1.45;
    }
    .error {
        color: #ff9f9f;
    }
</style>
</head>
<body>
<main>%2</main>
%3
</body>
</html>)")
                    .arg(title.toHtmlEscaped(), body, scripts);
    return page.toUtf8();
}

QByteArray renderCallbackPage()
{
    return renderPage(
        u"Mergerino Twitch Login"_s,
        uR"(<h1>Finishing Twitch login</h1>
<p id="status">Sending the Twitch token to Mergerino...</p>)"_s,
        uR"(<script>
(async () => {
    const status = document.getElementById("status");
    const hash = window.location.hash.startsWith("#") ? window.location.hash.slice(1) : "";
    const query = window.location.search.startsWith("?") ? window.location.search.slice(1) : "";
    const params = new URLSearchParams(hash || query);
    if (params.has("error")) {
        status.className = "error";
        status.textContent = params.get("error_description") || "Twitch login was cancelled.";
        return;
    }
    if (!params.has("access_token")) {
        status.className = "error";
        status.textContent = "Twitch did not return a login token.";
        return;
    }

    const response = await fetch("/token", {
        method: "POST",
        headers: {"Content-Type": "application/x-www-form-urlencoded"},
        body: params.toString()
    });
    if (!response.ok) {
        status.className = "error";
        status.textContent = await response.text();
        return;
    }

    const poll = async () => {
        const res = await fetch("/status", {cache: "no-store"});
        const data = await res.json();
        status.className = data.state === "error" ? "error" : "";
        status.textContent = data.message;
        if (data.state === "pending") {
            window.setTimeout(poll, 700);
        }
    };
    poll();
})();
</script>)"_s);
}

QByteArray renderWaitingPage()
{
    return renderPage(
        u"Mergerino Twitch Login"_s,
        uR"(<h1>Waiting for Twitch</h1>
<p>Complete the Twitch authorization page that opened in your browser.</p>)"_s);
}

HttpServer::Response htmlResponse(const QByteArray &body, unsigned status = 200)
{
    return {
        .status = status,
        .body = body,
        .contentType = HTML_CONTENT_TYPE,
    };
}

HttpServer::Response textResponse(const QString &body, unsigned status)
{
    return {
        .status = status,
        .body = body.toUtf8(),
        .contentType = "text/plain; charset=utf-8",
    };
}

HttpServer::Response jsonResponse(const QJsonObject &body, unsigned status = 200)
{
    return {
        .status = status,
        .body = QJsonDocument(body).toJson(QJsonDocument::Compact),
        .contentType = JSON_CONTENT_TYPE,
    };
}

QString formatAPIError(const NetworkResult &result)
{
    const auto json = result.parseJson();
    auto error = json["message"_L1].toString(json["error"_L1].toString());
    if (!error.isEmpty())
    {
        return u"Error: " % error % u" (" % result.formatError() % ')';
    }
    return u"Error: " % result.formatError() % u" (no further information)";
}

bool saveTwitchAccount(const QString &username, const QString &userID,
                       const QString &clientID, const QString &accessToken)
{
    if (!accountAccessError(ProviderId::Twitch, username).isEmpty() ||
        username.isEmpty() || userID.isEmpty() || clientID.isEmpty() ||
        accessToken.isEmpty())
    {
        return false;
    }

    std::string basePath = "/accounts/uid" + userID.toStdString();
    pajlada::Settings::Setting<QString>::set(basePath + "/username", username);
    pajlada::Settings::Setting<QString>::set(basePath + "/userID", userID);
    pajlada::Settings::Setting<QString>::set(basePath + "/clientID", clientID);
    pajlada::Settings::Setting<QString>::set(basePath + "/oauthToken",
                                             accessToken);

    getApp()->getAccounts()->twitch.reloadUsers();
    getApp()->getAccounts()->twitch.currentUsername = username;
    getSettings()->requestSave();
    return true;
}

class AuthSession final : public QObject
{
public:
    AuthSession(QWidget *ownerWindow, std::function<void()> onAuthenticated,
                QObject *parent)
        : QObject(parent)
        , ownerWindow_(ownerWindow)
        , state_(generateState())
        , onAuthenticated_(std::move(onAuthenticated))
    {
        this->authorizationTimer_.setSingleShot(true);
        QObject::connect(&this->authorizationTimer_, &QTimer::timeout, this,
                         [this] {
                             this->setBrowserState(
                                 u"error"_s,
                                 u"Twitch login timed out. Start the login "
                                 u"flow again."_s);
                             QTimer::singleShot(1000, this,
                                                &QObject::deleteLater);
                         });

        this->pendingTimer_.setSingleShot(true);
        QObject::connect(&this->pendingTimer_, &QTimer::timeout, this, [this] {
            this->setBrowserState(u"error"_s, this->pendingTimeoutMessage_);
        });
    }

    bool begin()
    {
        this->server_ = new HttpServer(SERVER_PORT, this);
        if (!this->server_->isListening())
        {
            this->showError(
                u"Unable to start the local Twitch login server on %1. %2"_s
                    .arg(REDIRECT_URL, this->server_->errorString()));
            this->deleteLater();
            return false;
        }

        this->server_->setHandler([this](const auto &request) {
            return this->handleRequest(request);
        });
        this->authorizationTimer_.start(AUTHORIZATION_TIMEOUT_MS);

        if (!QDesktopServices::openUrl(this->buildAuthUrl()))
        {
            this->showError(
                u"Unable to open Twitch login in your browser. Try again from "
                u"the accounts settings page."_s);
            this->deleteLater();
            return false;
        }

        return true;
    }

private:
    HttpServer::Response handleRequest(const HttpServer::Request &request)
    {
        const auto path = requestPath(request.target);
        if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) == 0 &&
            path == u"/"_s)
        {
            const auto query = targetQuery(request.target);
            if (query.hasQueryItem(u"error"_s))
            {
                this->setBrowserState(
                    u"error"_s,
                    query.queryItemValue(u"error_description"_s,
                                         QUrl::FullyDecoded));
            }
            return htmlResponse(renderCallbackPage());
        }

        if (request.method.compare(u"POST"_s, Qt::CaseInsensitive) == 0 &&
            path == u"/token"_s)
        {
            return this->handleTokenPost(formQuery(request.body));
        }

        if (request.method.compare(u"GET"_s, Qt::CaseInsensitive) == 0 &&
            path == u"/status"_s)
        {
            return jsonResponse({
                {"state", this->browserState_},
                {"message", this->browserMessage_},
            });
        }

        return htmlResponse(renderWaitingPage());
    }

    HttpServer::Response handleTokenPost(const QUrlQuery &query)
    {
        if (query.queryItemValue(u"state"_s) != QString::fromUtf8(this->state_))
        {
            const auto message =
                u"Twitch login state did not match. Start the login flow "
                u"again."_s;
            this->setBrowserState(u"error"_s, message);
            return textResponse(message, 400);
        }

        const auto token = query.queryItemValue(u"access_token"_s);
        if (token.isEmpty())
        {
            const auto message = u"Twitch did not return a login token."_s;
            this->setBrowserState(u"error"_s, message);
            return textResponse(message, 400);
        }

        this->setBrowserState(u"pending"_s,
                              u"Validating the Twitch account..."_s);
        this->startPendingTimeout(
            u"Twitch token validation timed out. Start the login flow again."_s,
            NETWORK_STEP_TIMEOUT_MS);
        this->validateToken(token);
        return jsonResponse({
            {"state", this->browserState_},
            {"message", this->browserMessage_},
        });
    }

    QUrl buildAuthUrl() const
    {
        QStringList scopes;
        scopes.reserve(static_cast<int>(AUTH_SCOPES.size()) + 1);
        for (const auto scope : AUTH_SCOPES)
        {
            scopes.push_back(scope.toString());
        }
        // Optional for existing accounts. Twitch also requires channel access.
        scopes.push_back(u"moderator:read:followers"_s);

        QUrlQuery query{
            {"response_type", "token"},
            {"client_id", TWITCH_CLIENT_ID},
            {"redirect_uri", REDIRECT_URL},
            {"scope", scopes.join(' ')},
            {"state", QString::fromUtf8(this->state_)},
        };
        return QUrl(u"https://id.twitch.tv/oauth2/authorize?"_s %
                    query.toString(QUrl::FullyEncoded));
    }

    void validateToken(const QString &accessToken)
    {
        NetworkRequest(u"https://id.twitch.tv/oauth2/validate"_s,
                       NetworkRequestType::Get)
            .header("Authorization", u"OAuth " % accessToken)
            .timeout(20'000)
            .caller(this)
            .onError([this](const NetworkResult &result) {
                this->clearPendingTimeout();
                auto error = formatAPIError(result);
                qCWarning(chatterinoTwitch)
                    << "Validating Twitch login failed" << error;
                this->setBrowserState(u"error"_s, error);
            })
            .onSuccess([this, accessToken](const NetworkResult &result) {
                this->clearPendingTimeout();
                const auto json = result.parseJson();
                const auto clientID = json["client_id"_L1].toString();
                const auto username = json["login"_L1].toString();
                const auto userID = json["user_id"_L1].toString();

                if (clientID != TWITCH_CLIENT_ID)
                {
                    const auto error =
                        u"Twitch returned a token for a different app."_s;
                    qCWarning(chatterinoTwitch) << error << json;
                    this->setBrowserState(u"error"_s, error);
                    return;
                }

                if (const auto error =
                        accountAccessError(ProviderId::Twitch, username);
                    !error.isEmpty())
                {
                    this->authorizationTimer_.stop();
                    this->setBrowserState(u"error"_s, error);
                    return;
                }

                if (!saveTwitchAccount(username, userID, clientID, accessToken))
                {
                    const auto error =
                        u"Twitch did not return enough account information."_s;
                    qCWarning(chatterinoTwitch) << error << json;
                    this->setBrowserState(u"error"_s, error);
                    return;
                }

                this->authorizationTimer_.stop();
                this->setBrowserState(u"success"_s,
                                      u"Twitch account connected. You can "
                                      u"close this browser tab."_s);
                if (this->onAuthenticated_)
                {
                    this->onAuthenticated_();
                    this->onAuthenticated_ = {};
                }
                QTimer::singleShot(15000, this, &QObject::deleteLater);
            })
            .execute();
    }

    void setBrowserState(const QString &state, const QString &message)
    {
        this->browserState_ = state;
        this->browserMessage_ = message;
    }

    void startPendingTimeout(const QString &message, int timeoutMs)
    {
        this->pendingTimeoutMessage_ = message;
        this->pendingTimer_.start(timeoutMs);
    }

    void clearPendingTimeout()
    {
        this->pendingTimer_.stop();
        this->pendingTimeoutMessage_.clear();
    }

    void showError(const QString &message)
    {
        this->setBrowserState(u"error"_s, message);
        if (this->ownerWindow_)
        {
            QMessageBox::critical(this->ownerWindow_, u"Twitch login"_s,
                                  message);
        }
    }

    HttpServer *server_{};
    QPointer<QWidget> ownerWindow_;
    QByteArray state_;
    std::function<void()> onAuthenticated_;
    QTimer authorizationTimer_;
    QTimer pendingTimer_;
    QString pendingTimeoutMessage_;
    QString browserState_ = u"pending"_s;
    QString browserMessage_ =
        u"Complete the Twitch authorization page in your browser."_s;
};

bool startTwitchLoginFlow(QWidget *parent,
                          std::function<void()> onAuthenticated)
{
    static QPointer<AuthSession> activeSession;
    if (activeSession)
    {
        delete activeSession.data();
        activeSession.clear();
    }

    auto *session = new AuthSession(parent ? parent->window() : nullptr,
                                    std::move(onAuthenticated), qApp);
    activeSession = session;
    QObject::connect(session, &QObject::destroyed, qApp, [session] {
        if (activeSession == session)
        {
            activeSession.clear();
        }
    });
    return session->begin();
}

}  // namespace

namespace chatterino {

bool TwitchLoginPage::startLoginFlow(QWidget *parent,
                                     std::function<void()> onAuthenticated)
{
    return startTwitchLoginFlow(parent, std::move(onAuthenticated));
}

TwitchLoginPage::TwitchLoginPage()
{
    auto *root = new QVBoxLayout(this);

    auto *topLabel = new QLabel(
        "Twitch sign-in opens Twitch in your browser, then Mergerino catches "
        "the login locally and saves the account on this PC.");
    topLabel->setWordWrap(true);
    root->addWidget(topLabel);

    auto *startButton = new QPushButton("Log in with Twitch");
    startButton->setDefault(true);
    root->addWidget(startButton, 0, Qt::AlignLeft);
    root->addStretch(1);

    QObject::connect(startButton, &QPushButton::clicked, this, [this] {
        startTwitchLoginFlow(
            this,
            [window = QPointer<QWidget>(this->window())] {
                if (window)
                {
                    window->close();
                }
            });
    });
}

}  // namespace chatterino
