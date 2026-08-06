// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

#include <functional>
#include <memory>

namespace chatterino {

class SeventvBrowserAuth final
{
public:
    using SuccessCallback = std::function<void(const QString &)>;
    using ErrorCallback = std::function<void(const QString &)>;

    static SeventvBrowserAuth &instance();

    ~SeventvBrowserAuth();

    bool isRunning() const;
    void start(SuccessCallback onSuccess, ErrorCallback onError);
    void cancel();

private:
    SeventvBrowserAuth();

    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chatterino
