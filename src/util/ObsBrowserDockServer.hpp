// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "util/HttpServer.hpp"

#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <memory>

namespace chatterino {

class Split;
class SplitContainer;
class Window;

class ObsBrowserDockServer final : public QObject
{
public:
    static constexpr uint16_t PORT = 38479;

    explicit ObsBrowserDockServer(QObject *parent = nullptr);

    static QString dockUrl(const QString &view = QStringLiteral("chat"),
                           int tabIndex = -1);
    static QString overlayUrl();
    static QStringList availableOverlayTabNames();

private:
    std::unique_ptr<HttpServer> server_;

    Window *dockWindow() const;
    SplitContainer *selectedPage(int tabIndex = -1) const;
    SplitContainer *findOverlayPage(const QString &tabName) const;
    SplitContainer *ensureOverlayPage(const QString &tabName) const;
    SplitContainer *resolveOverlayPage(bool createIfMissing) const;
    Split *resolveSplit(SplitContainer *page, const QString &view) const;
    Split *resolveOverlaySplit(SplitContainer *page) const;

    QByteArray dockPageHtml() const;
    QByteArray dockStateJson(const QString &view, int requestedTabIndex) const;
    QByteArray overlayPageHtml() const;
    QByteArray overlayStateJson() const;
};

}  // namespace chatterino
