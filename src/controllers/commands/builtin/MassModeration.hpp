// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "controllers/moderation/MassModerationQueue.hpp"
#include "ForwardDecl.hpp"
#include "util/Expected.hpp"

namespace chatterino {

struct CommandContext;

/// Opens the mass ban or unban window for a Twitch or Kick channel, reusing
/// one that is already open.
void openMassModerationDialog(const ChannelPtr &channel, bool ban);

ExpectedStr<MassModerationQueue::Action> makeMassModerationAction(
    const ChannelPtr &channel, bool ban, const QString &reason,
    QObject *receiver);

namespace commands {
QString massBan(const CommandContext &ctx);
QString massUnban(const CommandContext &ctx);
}  // namespace commands
}  // namespace chatterino
