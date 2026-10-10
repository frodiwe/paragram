/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_reaction_icons.h"

#include "ui/emoji_config.h"

#include <QtCore/QBuffer>
#include <QtGui/QImage>
#include <QtGui/QPainter>

namespace Pararam {

std::vector<QByteArray> RenderReactionIcons(
		const std::vector<QString> &emoji) {
	auto result = std::vector<QByteArray>(emoji.size());
	const auto &shared = Ui::Emoji::SourceImages();
	if (!shared) {
		return result;
	}
	// A separate copy, the shared one is managed by the application.
	auto images = Ui::Emoji::UniversalImages(shared->id());
	if (!images.ensureLoaded()) {
		return result;
	}
	for (auto i = 0, count = int(emoji.size()); i != count; ++i) {
		const auto found = Ui::Emoji::Find(emoji[i]);
		if (!found) {
			continue;
		}
		auto image = QImage(
			kReactionIconSize,
			kReactionIconSize,
			QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::transparent);
		{
			auto p = QPainter(&image);
			images.draw(p, found, kReactionIconSize, 0, 0);
		}
		auto buffer = QBuffer(&result[i]);
		buffer.open(QIODevice::WriteOnly);
		image.save(&buffer, "PNG");
	}
	return result;
}

} // namespace Pararam
