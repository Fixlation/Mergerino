// SPDX-FileCopyrightText: 2019 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>

namespace chatterino {

class Modes
{
public:
    Modes();

    static const Modes &instance();

    bool isPortable{};

    /// Marked by the line `externally-packaged`
    ///
    /// The externally packaged mode comes with the following changes:
    ///  - No shortcuts are created by default
    bool isExternallyPackaged{};

    /// Optional application data root, resolved relative to the executable
    /// directory when the value is not absolute.
    QString dataRoot;
};

}  // namespace chatterino
