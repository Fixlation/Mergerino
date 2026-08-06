// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "util/MergerinoExtensionBridge.hpp"

#include "Application.hpp"
#include "common/Channel.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "providers/kick/KickAccount.hpp"
#include "providers/kick/KickChatServer.hpp"
#include "providers/seventv/SeventvAccountManager.hpp"
#include "providers/twitch/TwitchAccount.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/HttpServer.hpp"
#include "widgets/dialogs/KickLoginPage.hpp"
#include "widgets/dialogs/TwitchLoginPage.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <algorithm>
#include <tuple>
#include <vector>

namespace {

using namespace chatterino;

HttpServer::Response jsonResponse(const QJsonObject &body,
                                  unsigned status = 200)
{
    return HttpServer::Response{
        .status = status,
        .body = QJsonDocument(body).toJson(QJsonDocument::Compact),
        .contentType = QByteArrayLiteral("application/json; charset=utf-8"),
    };
}

HttpServer::Response errorResponse(unsigned status, const QString &message)
{
    return jsonResponse(
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("mergerino.error")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("error"), message},
        },
        status);
}

HttpServer::Response okResponse(const QJsonObject &data = {})
{
    return jsonResponse(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("mergerino.result")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("data"), data},
    });
}

QString normalizedLogin(QString value, bool kick)
{
    value = value.trimmed();
    while (value.startsWith('@'))
    {
        value.removeFirst();
    }
    if (kick)
    {
        value.replace('_', '-');
    }
    return value.toLower();
}

bool layoutContainsTarget(const QJsonValue &value, const QString &platform,
                          const QString &login)
{
    if (value.isArray())
    {
        for (const auto &entry : value.toArray())
        {
            if (layoutContainsTarget(entry, platform, login))
            {
                return true;
            }
        }
        return false;
    }

    if (!value.isObject())
    {
        return false;
    }

    const auto object = value.toObject();
    const auto type =
        object.value(QStringLiteral("type")).toString().trimmed().toLower();
    if (type == QStringLiteral("twitch") &&
        platform == QStringLiteral("TWITCH"))
    {
        return normalizedLogin(
                   object.value(QStringLiteral("name")).toString(), false) ==
               login;
    }
    if (type == QStringLiteral("kick") && platform == QStringLiteral("KICK"))
    {
        return normalizedLogin(
                   object.value(QStringLiteral("name")).toString(), true) ==
               login;
    }
    if (type == QStringLiteral("merged"))
    {
        const auto fallback =
            object.value(QStringLiteral("name")).toString();
        if (platform == QStringLiteral("TWITCH") &&
            object.value(QStringLiteral("twitchEnabled")).toBool(true))
        {
            auto target =
                object.value(QStringLiteral("twitchChannel")).toString();
            if (target.trimmed().isEmpty())
            {
                target = fallback;
            }
            if (normalizedLogin(target, false) == login)
            {
                return true;
            }
        }
        if (platform == QStringLiteral("KICK") &&
            object.value(QStringLiteral("kickEnabled")).toBool(true))
        {
            auto target =
                object.value(QStringLiteral("kickChannel")).toString();
            if (target.trimmed().isEmpty())
            {
                target = fallback;
            }
            if (normalizedLogin(target, true) == login)
            {
                return true;
            }
        }
    }

    for (auto it = object.constBegin(); it != object.constEnd(); ++it)
    {
        if (layoutContainsTarget(it.value(), platform, login))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

namespace chatterino {

MergerinoExtensionBridge::MergerinoExtensionBridge(QObject *parent)
    : QObject(parent)
    , server_(std::make_unique<HttpServer>(MergerinoExtensionBridge::PORT, this))
    , bridgeToken_(QUuid::createUuid().toString(QUuid::WithoutBraces))
{
    this->server_->setHandler([this](const HttpServer::Request &request) {
        const auto requestUrl =
            QUrl(QStringLiteral("http://127.0.0.1") + request.target);
        const auto path = requestUrl.path();
        const QUrlQuery query(requestUrl);

        if (request.method.compare(QStringLiteral("GET"),
                                   Qt::CaseInsensitive) == 0)
        {
            if (path == QStringLiteral("/extension/workspace"))
            {
                return HttpServer::Response{
                    .body = this->workspaceJson(),
                    .contentType =
                        QByteArrayLiteral("application/json; charset=utf-8"),
                };
            }

            if (path == QStringLiteral("/extension/accounts"))
            {
                return HttpServer::Response{
                    .body = this->accountsJson(),
                    .contentType =
                        QByteArrayLiteral("application/json; charset=utf-8"),
                };
            }

            if (path == QStringLiteral("/extension/7tv/auth"))
            {
                return HttpServer::Response{
                    .body = this->seventvAuthPage(),
                    .contentType =
                        QByteArrayLiteral("text/html; charset=utf-8"),
                };
            }

            if (path == QStringLiteral("/extension/7tv"))
            {
                if (!this->hasValidToken(
                        query.queryItemValue(QStringLiteral("token"))))
                {
                    return errorResponse(
                        403, QStringLiteral("Invalid bridge token"));
                }
                return HttpServer::Response{
                    .body = this->seventvJson(),
                    .contentType =
                        QByteArrayLiteral("application/json; charset=utf-8"),
                };
            }

            return errorResponse(404, QStringLiteral("Not Found"));
        }

        if (request.method.compare(QStringLiteral("POST"),
                                   Qt::CaseInsensitive) == 0)
        {
            if (!this->hasValidToken(
                    query.queryItemValue(QStringLiteral("token"))))
            {
                return errorResponse(403,
                                     QStringLiteral("Invalid bridge token"));
            }

            return this->handlePost(path, request.body);
        }

        return errorResponse(405, QStringLiteral("Method Not Allowed"));
    });
}

QByteArray MergerinoExtensionBridge::workspaceJson() const
{
    QJsonObject windowLayout;

    if (auto *app = tryGetApp())
    {
        if (auto *windows = app->getWindows())
        {
            windowLayout = windows->currentWindowLayoutJson();
        }
    }

    const QJsonObject root{
        {QStringLiteral("type"), QStringLiteral("mergerino.workspace")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("data"),
         QJsonObject{
             {QStringLiteral("windowLayout"), windowLayout},
         }},
    };

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray MergerinoExtensionBridge::accountsJson() const
{
    QJsonArray providers;

    if (auto *app = tryGetApp())
    {
        if (auto *accounts = app->getAccounts())
        {
            QJsonArray twitchAccounts;
            for (const auto &username : accounts->twitch.getUsernames())
            {
                const auto account =
                    accounts->twitch.findUserByUsername(username);
                twitchAccounts.append(QJsonObject{
                    {QStringLiteral("id"),
                     account ? account->getUserId() : QString{}},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("displayName"), username},
                });
            }

            const auto currentTwitch = accounts->twitch.getCurrent();
            const auto currentTwitchUsername =
                currentTwitch && !currentTwitch->isAnon()
                    ? currentTwitch->getUserName()
                    : QString{};
            providers.append(QJsonObject{
                {QStringLiteral("platform"), QStringLiteral("TWITCH")},
                {QStringLiteral("loggedIn"),
                 !currentTwitchUsername.isEmpty()},
                {QStringLiteral("currentUsername"),
                 currentTwitchUsername},
                {QStringLiteral("accounts"), twitchAccounts},
            });

            QJsonArray kickAccounts;
            for (const auto &username : accounts->kick.usernames())
            {
                const auto account =
                    accounts->kick.findUserByUsername(username);
                kickAccounts.append(QJsonObject{
                    {QStringLiteral("id"),
                     account ? QString::number(account->userID()) : QString{}},
                    {QStringLiteral("username"), username},
                    {QStringLiteral("displayName"), username},
                });
            }

            const auto currentKick = accounts->kick.current();
            const auto currentKickUsername =
                currentKick && !currentKick->isAnonymous()
                    ? currentKick->username()
                    : QString{};
            providers.append(QJsonObject{
                {QStringLiteral("platform"), QStringLiteral("KICK")},
                {QStringLiteral("loggedIn"), !currentKickUsername.isEmpty()},
                {QStringLiteral("currentUsername"), currentKickUsername},
                {QStringLiteral("accounts"), kickAccounts},
            });
        }
    }

    const QJsonObject root{
        {QStringLiteral("type"), QStringLiteral("mergerino.accounts")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("data"),
         QJsonObject{
             {QStringLiteral("bridgeToken"), this->bridgeToken_},
             {QStringLiteral("providers"), providers},
         }},
    };

    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray MergerinoExtensionBridge::seventvJson() const
{
    const QJsonObject root{
        {QStringLiteral("type"), QStringLiteral("mergerino.seventv")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("data"),
         SeventvAccountManager::instance().exportSyncState()},
    };
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

QByteArray MergerinoExtensionBridge::seventvAuthPage() const
{
    auto page = QStringLiteral(R"html(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Connect 7TV to Mergerino</title>
<style>
html{color-scheme:dark}body{margin:0;min-height:100vh;display:grid;place-items:center;background:#121315;color:#f2f3f5;font:16px system-ui,sans-serif}
main{width:min(440px,calc(100vw - 48px));padding:32px;border:1px solid #393c42;border-radius:12px;background:#202226;box-shadow:0 18px 60px #0008}.brand{display:flex;align-items:center;justify-content:center;width:58px;height:42px;margin-bottom:20px;border:1px solid #44484f;border-radius:9px;background:#2a2d31}.brand svg{width:38px;height:auto;fill:#f4f5f6}
h1{margin:0 0 10px;font-size:25px}p{color:#b8bec8;line-height:1.5}button{width:100%;margin-top:14px;padding:12px 16px;border:1px solid #d8dade;border-radius:7px;background:#f0f1f2;color:#1b1d20;font-weight:750;font-size:15px;cursor:pointer}button:hover{background:#fff}button:disabled{opacity:.55;cursor:default}#status{min-height:24px;margin-top:15px;color:#aeb5c2}.ok{color:#74e39b!important}.error{color:#ff8c8c!important}
</style></head><body><main>
<div class="brand" aria-label="7TV"><svg viewBox="0 0 33 23.551" role="img"><path d="M2.383,0,0,4.127,1.473,6.676H11.7L3.426,21,4.9,23.551H9.66Q14.532,15.113,19.4,6.676L15.549,0ZM18.492,0l3.856,6.676h2.945l2.381-4.125L26.2,0Zm2.383,9.225L17.021,15.9l4.417,7.649H26.2L33,11.775l-1.473-2.55H26.764l-2.944,5.1Z"/></svg></div>
<h1>Connect your account</h1>
<p>Mergerino will use your 7TV session to manage your owned paints, badges and emote set. Your session is sent directly back to Mergerino on this PC.</p>
<button id="connect">Continue with 7TV</button><div id="status"></div>
<script>
const bridgeToken="__BRIDGE_TOKEN__";
const button=document.querySelector('#connect');const status=document.querySelector('#status');let popup=null,timer=null;
const setStatus=(text,kind='')=>{status.textContent=text;status.className=kind};
const startSignIn=()=>{if(popup&&!popup.closed){popup.focus();return}popup=window.open('https://7tv.app/extension/auth','7tv-auth','width=400,height=600');if(!popup){button.disabled=false;setStatus('Brave blocked the automatic sign-in window. Click Continue with 7TV.','error');return}button.disabled=true;setStatus('Waiting for 7TV…');timer=setInterval(()=>{if(!popup||popup.closed){clearInterval(timer);button.disabled=false;setStatus('Sign-in was closed.','error');return}popup.postMessage('7tv-token-request','https://7tv.app')},100)};
button.addEventListener('click',startSignIn);
window.addEventListener('message',async event=>{if(event.origin!=='https://7tv.app'||event.source!==popup||event.data?.type!=='7tv-token'||!event.data.token)return;clearInterval(timer);popup?.close();setStatus('Finishing sign-in…');try{const response=await fetch('/extension/7tv/session?token='+encodeURIComponent(bridgeToken),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({token:String(event.data.token)})});const body=await response.json();if(!response.ok)throw new Error(body.error||'Mergerino rejected the session');setStatus('Connected. You can close this page and return to Mergerino.','ok')}catch(error){button.disabled=false;setStatus(error.message||'Unable to finish sign-in.','error')}});
if(new URLSearchParams(location.search).get('auto')==='1'){setStatus('Opening 7TV sign-in…');startSignIn()}
</script></main></body></html>)html");
    page.replace(QStringLiteral("__BRIDGE_TOKEN__"), this->bridgeToken_);
    return page.toUtf8();
}

HttpServer::Response MergerinoExtensionBridge::handlePost(
    const QString &path, const QByteArray &body) const
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
    {
        return errorResponse(400, QStringLiteral("Invalid JSON request"));
    }

    auto *app = tryGetApp();
    if (!app || !app->getAccounts())
    {
        return errorResponse(503, QStringLiteral("Mergerino is not ready"));
    }

    const auto request = document.object();
    const auto platform =
        request.value(QStringLiteral("platform")).toString().trimmed().toUpper();

    if (path == QStringLiteral("/extension/7tv/session"))
    {
        QString error;
        if (!SeventvAccountManager::instance().acceptSessionToken(
                request.value(QStringLiteral("token")).toString(), &error))
        {
            return errorResponse(400, error);
        }
        return okResponse(
            SeventvAccountManager::instance().exportSyncState());
    }

    if (path == QStringLiteral("/extension/7tv/sync"))
    {
        const auto state = request.value(QStringLiteral("state")).toObject();
        QString error;
        if (!SeventvAccountManager::instance().importSyncState(state, &error))
        {
            return errorResponse(400, error);
        }
        return okResponse(
            SeventvAccountManager::instance().exportSyncState());
    }

    if (path == QStringLiteral("/extension/accounts/select"))
    {
        const auto username =
            request.value(QStringLiteral("username")).toString().trimmed();

        if (platform == QStringLiteral("TWITCH"))
        {
            if (!username.isEmpty() &&
                !app->getAccounts()->twitch.userExists(username))
            {
                return errorResponse(404,
                                     QStringLiteral("Twitch account not found"));
            }

            app->getAccounts()->twitch.currentUsername = username;
        }
        else if (platform == QStringLiteral("KICK"))
        {
            if (!username.isEmpty() &&
                !app->getAccounts()->kick.userExists(username))
            {
                return errorResponse(404,
                                     QStringLiteral("Kick account not found"));
            }

            app->getAccounts()->kick.currentUsername = username;
        }
        else
        {
            return errorResponse(400, QStringLiteral("Unsupported platform"));
        }

        std::ignore = getSettings()->requestSave();
        return HttpServer::Response{
            .body = this->accountsJson(),
            .contentType =
                QByteArrayLiteral("application/json; charset=utf-8"),
        };
    }

    if (path == QStringLiteral("/extension/accounts/login"))
    {
        if (platform == QStringLiteral("TWITCH"))
        {
            if (!TwitchLoginPage::startLoginFlow())
            {
                return errorResponse(
                    409, QStringLiteral("Twitch sign-in could not be started"));
            }
        }
        else if (platform == QStringLiteral("KICK"))
        {
            KickLoginPage::startLoginFlow();
        }
        else
        {
            return errorResponse(400, QStringLiteral("Unsupported platform"));
        }

        return okResponse(QJsonObject{
            {QStringLiteral("platform"), platform},
            {QStringLiteral("started"), true},
        });
    }

    if (path == QStringLiteral("/extension/chat/send"))
    {
        const auto message =
            request.value(QStringLiteral("message")).toString().trimmed();
        const auto rawTargets = request.value(QStringLiteral("targets")).toArray();
        if (message.isEmpty())
        {
            return errorResponse(400, QStringLiteral("Message is empty"));
        }
        if (message.size() > 2000)
        {
            return errorResponse(400, QStringLiteral("Message is too long"));
        }
        if (rawTargets.isEmpty() || rawTargets.size() > 2)
        {
            return errorResponse(400, QStringLiteral("Invalid send targets"));
        }

        struct SendTarget {
            QString platform;
            QString login;
            ChannelPtr channel;
        };
        std::vector<SendTarget> sendTargets;
        sendTargets.reserve(static_cast<size_t>(rawTargets.size()));
        const auto windowLayout = app->getWindows()
                                      ? app->getWindows()->currentWindowLayoutJson()
                                      : QJsonObject{};

        for (const auto &rawTarget : rawTargets)
        {
            const auto target = rawTarget.toObject();
            const auto targetPlatform =
                target.value(QStringLiteral("platform"))
                    .toString()
                    .trimmed()
                    .toUpper();
            const bool isKick = targetPlatform == QStringLiteral("KICK");
            const auto login = normalizedLogin(
                target.value(QStringLiteral("login")).toString(), isKick);
            if (login.isEmpty())
            {
                return errorResponse(400,
                                     QStringLiteral("Missing channel login"));
            }
            if (!layoutContainsTarget(windowLayout, targetPlatform, login))
            {
                return errorResponse(
                    403,
                    QStringLiteral(
                        "The requested channel is not in the Mergerino workspace"));
            }

            const auto duplicate = std::ranges::find_if(
                sendTargets, [&](const auto &existing) {
                    return existing.platform == targetPlatform &&
                           existing.login == login;
                });
            if (duplicate != sendTargets.end())
            {
                continue;
            }

            if (targetPlatform == QStringLiteral("TWITCH"))
            {
                if (!app->getAccounts()->twitch.isLoggedIn())
                {
                    return errorResponse(
                        409, QStringLiteral("Log in to Twitch in Mergerino"));
                }

                auto channel = app->getTwitch()->getOrAddChannel(login);
                if (!channel)
                {
                    return errorResponse(
                        404, QStringLiteral("Twitch channel is unavailable"));
                }
                sendTargets.push_back({targetPlatform, login, channel});
                continue;
            }

            if (targetPlatform == QStringLiteral("KICK"))
            {
                if (!app->getAccounts()->kick.isLoggedIn())
                {
                    return errorResponse(
                        409, QStringLiteral("Log in to Kick in Mergerino"));
                }

                auto channel = app->getKickChatServer()->findBySlug(login);
                if (!channel)
                {
                    return errorResponse(
                        409,
                        QStringLiteral(
                            "Open this Kick channel in Mergerino before sending"));
                }
                sendTargets.push_back({targetPlatform, login, channel});
                continue;
            }

            return errorResponse(400, QStringLiteral("Unsupported platform"));
        }

        if (sendTargets.empty())
        {
            return errorResponse(400, QStringLiteral("No send targets"));
        }

        for (const auto &target : sendTargets)
        {
            target.channel->sendMessage(message);
        }

        QJsonArray acceptedTargets;
        for (const auto &target : sendTargets)
        {
            acceptedTargets.append(QJsonObject{
                {QStringLiteral("platform"), target.platform},
                {QStringLiteral("login"), target.login},
            });
        }
        return okResponse(QJsonObject{
            {QStringLiteral("accepted"), true},
            {QStringLiteral("targets"), acceptedTargets},
        });
    }

    return errorResponse(404, QStringLiteral("Not Found"));
}

bool MergerinoExtensionBridge::hasValidToken(const QString &token) const
{
    return !token.isEmpty() && token == this->bridgeToken_;
}

}  // namespace chatterino
