/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <vector>

namespace Pararam {

inline constexpr auto kReactionIconSize = 72;

// PNGs of the emoji from the bundled emoji set, used as the static
// stickers of the pararam.io reactions, empty for unknown emoji.
[[nodiscard]] std::vector<QByteArray> RenderReactionIcons(
	const std::vector<QString> &emoji);

} // namespace Pararam
