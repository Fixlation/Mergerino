// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/HttpServer.hpp"

#include <QObject>

#include <cstdint>
#include <memory>

namespace chatterino {

class MergerinoExtensionBridge final : public QObject
{
public:
    static constexpr uint16_t PORT = 29381;

    explicit MergerinoExtensionBridge(QObject *parent = nullptr);

private:
    std::unique_ptr<HttpServer> server_;
    QString bridgeToken_;

    QByteArray workspaceJson() const;
    QByteArray accountsJson() const;
    QByteArray seventvJson() const;
    QByteArray seventvAuthPage() const;
    HttpServer::Response handlePost(const QString &path,
                                    const QByteArray &body) const;
    bool hasValidToken(const QString &token) const;
};

}  // namespace chatterino
