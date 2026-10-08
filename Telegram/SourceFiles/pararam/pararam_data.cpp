/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_data.h"

#include <QtCore/QDateTime>
#include <QtCore/QRandomGenerator>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>

#include <algorithm>

namespace Pararam {
namespace {

void AppendIds(std::vector<int64> &to, const QJsonValue &value) {
	for (const auto id : JsonIds(value)) {
		if (id && std::find(to.begin(), to.end(), id) == to.end()) {
			to.push_back(id);
		}
	}
}

} // namespace

int64 JsonInt(const QJsonValue &value, int64 fallback) {
	if (value.isDouble()) {
		return int64(value.toDouble());
	} else if (value.isString()) {
		auto ok = false;
		const auto result = value.toString().toLongLong(&ok);
		return ok ? result : fallback;
	} else if (value.isBool()) {
		return value.toBool() ? 1 : 0;
	}
	return fallback;
}

int64 JsonInt(const QJsonObject &object, const QString &key, int64 fallback) {
	return JsonInt(object.value(key), fallback);
}

QString JsonString(const QJsonObject &object, const QString &key) {
	const auto value = object.value(key);
	if (value.isString()) {
		return value.toString();
	} else if (value.isDouble()) {
		return QString::number(int64(value.toDouble()));
	}
	return QString();
}

bool JsonBool(const QJsonObject &object, const QString &key, bool fallback) {
	const auto value = object.value(key);
	if (value.isBool()) {
		return value.toBool();
	} else if (value.isDouble()) {
		return value.toDouble() != 0.;
	} else if (value.isString()) {
		const auto string = value.toString().toLower();
		return (string == u"true"_q || string == u"1"_q);
	}
	return fallback;
}

std::vector<int64> JsonIds(const QJsonValue &value) {
	auto result = std::vector<int64>();
	if (value.isArray()) {
		const auto array = value.toArray();
		result.reserve(array.size());
		for (const auto &entry : array) {
			if (entry.isObject()) {
				const auto object = entry.toObject();
				const auto id = JsonInt(object, u"id"_q);
				if (id) {
					result.push_back(id);
				}
			} else if (const auto id = JsonInt(entry)) {
				result.push_back(id);
			}
		}
	}
	return result;
}

TimeId ParseTime(const QString &value) {
	if (value.isEmpty()) {
		return 0;
	}
	auto string = value.trimmed();
	// Python isoformat may have microseconds, Qt understands milliseconds.
	static const auto fraction = QRegularExpression(
		u"\\.(\\d{3})\\d*"_q);
	string.replace(fraction, u".\\1"_q);
	auto result = QDateTime::fromString(string, Qt::ISODateWithMs);
	if (!result.isValid()) {
		result = QDateTime::fromString(string, Qt::ISODate);
	}
	if (!result.isValid()) {
		return 0;
	}
	if (!string.endsWith('Z')
		&& !string.contains('+')
		&& string.lastIndexOf('-') < 8) {
		result.setTimeZone(QTimeZone::UTC);
	}
	return TimeId(result.toSecsSinceEpoch());
}

UserInfo UserInfo::Parse(const QJsonObject &object) {
	auto result = UserInfo();
	result.id = JsonInt(object, u"id"_q);
	if (!result.id) {
		result.id = JsonInt(object, u"user_id"_q);
	}
	result.name = JsonString(object, u"name"_q);
	result.uniqueName = JsonString(object, u"unique_name"_q);
	result.info = JsonString(object, u"info"_q);
	result.avatar = JsonString(object, u"avatar"_q);
	result.bot = JsonBool(object, u"is_bot"_q);
	result.deleted = JsonBool(object, u"deleted"_q);
	return result;
}

void UserInfo::mergeFrom(const UserInfo &other) {
	if (!other.name.isEmpty()) {
		name = other.name;
	}
	if (!other.uniqueName.isEmpty()) {
		uniqueName = other.uniqueName;
	}
	if (!other.info.isEmpty()) {
		info = other.info;
	}
	if (!other.avatar.isEmpty()) {
		avatar = other.avatar;
	}
	if (other.full) {
		bot = other.bot;
		deleted = other.deleted;
		full = true;
	} else if (other.bot) {
		bot = true;
	}
}

FileInfo FileInfo::Parse(const QJsonObject &object) {
	auto result = FileInfo();
	result.guid = JsonString(object, u"guid"_q);
	result.name = JsonString(object, u"name"_q);
	if (result.name.isEmpty()) {
		result.name = JsonString(object, u"filename"_q);
	}
	result.mimeType = JsonString(object, u"mime_type"_q);
	result.size = JsonInt(object, u"size"_q);
	for (const auto &key : { u"width"_q, u"w"_q }) {
		if (const auto value = JsonInt(object, key)) {
			result.width = int(value);
		}
	}
	for (const auto &key : { u"height"_q, u"h"_q }) {
		if (const auto value = JsonInt(object, key)) {
			result.height = int(value);
		}
	}
	return result;
}

bool FileInfo::isImage() const {
	const auto mime = mimeType.toLower();
	return mime == u"image/jpeg"_q
		|| mime == u"image/png"_q
		|| mime == u"image/webp"_q
		|| mime == u"image/gif"_q
		|| mime == u"image/bmp"_q;
}

PostInfo PostInfo::Parse(const QJsonObject &object) {
	auto result = PostInfo();
	result.chatId = JsonInt(object, u"chat_id"_q);
	if (!result.chatId) {
		result.chatId = JsonInt(object, u"thread_id"_q);
	}
	result.postNo = int(JsonInt(object, u"post_no"_q));
	result.userId = JsonInt(object, u"user_id"_q);
	result.uuid = JsonString(object, u"uuid"_q);
	result.text = JsonString(object, u"text"_q);
	result.replyNo = int(JsonInt(object, u"reply_no"_q));
	result.created = ParseTime(JsonString(object, u"time_created"_q));
	result.edited = ParseTime(JsonString(object, u"time_edited"_q));
	result.ver = int(JsonInt(object, u"ver"_q));
	result.deleted = JsonBool(object, u"is_deleted"_q);

	for (const auto &entry : object.value(u"text_parsed"_q).toArray()) {
		const auto part = entry.toObject();
		auto parsed = TextPart();
		parsed.type = JsonString(part, u"type"_q);
		parsed.value = JsonString(part, u"value"_q);
		parsed.link = JsonString(part, u"link"_q);
		if (parsed.link.isEmpty()) {
			parsed.link = JsonString(part, u"url"_q);
		}
		parsed.userId = JsonInt(part, u"id"_q);
		result.parsed.push_back(parsed);
	}

	const auto meta = object.value(u"meta"_q).toObject();
	if (meta.contains(u"file"_q)) {
		result.file = FileInfo::Parse(meta.value(u"file"_q).toObject());
	}
	for (const auto &guid : meta.value(u"attachments"_q).toArray()) {
		if (guid.isString()) {
			result.attachments.push_back(guid.toString());
		}
	}
	if (meta.contains(u"user"_q)) {
		result.metaUser = UserInfo::Parse(meta.value(u"user"_q).toObject());
	}
	if (!result.ver) {
		result.ver = int(JsonInt(meta, u"ver"_q));
	}
	const auto event = object.value(u"event"_q);
	if (event.isObject()) {
		const auto data = event.toObject();
		result.eventType = JsonString(data, u"type"_q);
		result.eventData = data.value(u"data"_q).toObject();
	}
	return result;
}

bool PostInfo::sameContent(const PostInfo &other) const {
	return (text == other.text)
		&& (deleted == other.deleted)
		&& (ver == other.ver)
		&& (file.guid == other.file.guid)
		&& (replyNo == other.replyNo);
}

ChatInfo ChatInfo::Parse(const QJsonObject &object) {
	auto result = ChatInfo();
	result.id = JsonInt(object, u"id"_q);
	if (!result.id) {
		result.id = JsonInt(object, u"chat_id"_q);
	}
	result.type = JsonString(object, u"type"_q);
	result.title = JsonString(object, u"title"_q);
	result.customTitle = JsonString(object, u"custom_title"_q);
	result.description = JsonString(object, u"description"_q);
	result.pm = (result.type == u"pm"_q)
		|| (result.type.isEmpty() && JsonBool(object, u"pm"_q));
	result.e2e = (result.type == u"e2e"_q) || JsonBool(object, u"e2e"_q);
	if (result.e2e) {
		result.pm = true;
	}
	result.readOnly = JsonBool(object, u"read_only"_q);
	result.favorite = JsonBool(object, u"is_favorite"_q);
	result.visible = JsonBool(object, u"tshow"_q, true);
	const auto mute = object.value(u"mute"_q);
	result.mute = mute.isString() ? mute.toString() : QString();
	result.postsCount = int(JsonInt(object, u"posts_count"_q));
	result.lastReadPostNo = int(JsonInt(object, u"last_read_post_no"_q));
	result.historyStart = int(JsonInt(object, u"history_start"_q));
	result.created = ParseTime(JsonString(object, u"time_created"_q));
	result.updated = ParseTime(JsonString(object, u"time_updated"_q));
	if (!result.updated) {
		result.updated = result.created;
	}
	result.authorId = JsonInt(object, u"author_id"_q);
	result.lastMsgAuthorId = JsonInt(object, u"last_msg_author_id"_q);
	result.lastMsg = JsonString(object, u"last_msg"_q);
	if (result.lastMsg.isEmpty()) {
		result.lastMsg = JsonString(object, u"last_msg_text"_q);
	}
	AppendIds(result.users, object.value(u"thread_users"_q));
	AppendIds(result.users, object.value(u"thread_users_all"_q));
	AppendIds(result.admins, object.value(u"thread_admins"_q));
	for (const auto id : JsonIds(object.value(u"pinned"_q))) {
		result.pinned.push_back(int(id));
	}
	result.organizationId = JsonInt(object, u"organization_id"_q);
	return result;
}

FormattedText ParseIncomingText(
		const QString &text,
		const std::vector<TextPart> &parsed) {
	auto result = FormattedText();
	result.text.reserve(text.size());
	const auto size = int(text.size());
	auto i = 0;
	const auto startsWith = [&](int position, QStringView marker) {
		return QStringView(text).mid(position).startsWith(marker);
	};
	while (i < size) {
		if (startsWith(i, u"```")) {
			const auto end = text.indexOf(u"```"_q, i + 3);
			if (end > i + 3) {
				auto content = text.mid(i + 3, end - i - 3);
				auto language = QString();
				const auto newline = content.indexOf('\n');
				if (newline > 0
					&& !content.left(newline).contains(' ')
					&& newline < 20) {
					language = content.left(newline);
					content = content.mid(newline + 1);
				} else if (newline == 0) {
					content = content.mid(1);
				}
				if (content.endsWith('\n')) {
					content.chop(1);
				}
				result.entities.push_back({
					.type = Entity::Type::Pre,
					.offset = int(result.text.size()),
					.length = int(content.size()),
					.url = language,
				});
				result.text.append(content);
				i = end + 3;
				continue;
			}
		} else if (text[i] == '`') {
			const auto end = text.indexOf('`', i + 1);
			const auto newline = text.indexOf('\n', i + 1);
			if (end > i + 1 && (newline < 0 || newline > end)) {
				const auto content = text.mid(i + 1, end - i - 1);
				result.entities.push_back({
					.type = Entity::Type::Code,
					.offset = int(result.text.size()),
					.length = int(content.size()),
				});
				result.text.append(content);
				i = end + 1;
				continue;
			}
		} else if (startsWith(i, u"**") || startsWith(i, u"~~")) {
			const auto marker = text.mid(i, 2);
			const auto end = text.indexOf(marker, i + 2);
			if (end > i + 2 && !text.mid(i + 2, end - i - 2).contains('\n')) {
				const auto content = text.mid(i + 2, end - i - 2);
				result.entities.push_back({
					.type = (marker == u"**"_q)
						? Entity::Type::Bold
						: Entity::Type::Strike,
					.offset = int(result.text.size()),
					.length = int(content.size()),
				});
				result.text.append(content);
				i = end + 2;
				continue;
			}
		}
		result.text.append(text[i]);
		++i;
	}

	auto cursor = 0;
	for (const auto &part : parsed) {
		if (part.value.isEmpty()) {
			continue;
		}
		const auto isLink = (part.type == u"link"_q || part.type == u"url"_q);
		const auto isMention = (part.type == u"mention"_q
			|| part.type == u"userlink"_q
			|| part.type == u"user_link"_q) && part.userId;
		if (!isLink && !isMention) {
			if (part.type == u"text"_q) {
				const auto found = result.text.indexOf(part.value, cursor);
				if (found >= 0) {
					cursor = found + part.value.size();
				}
			}
			continue;
		}
		const auto found = result.text.indexOf(part.value, cursor);
		if (found < 0) {
			continue;
		}
		cursor = found + part.value.size();
		if (isMention) {
			result.entities.push_back({
				.type = Entity::Type::MentionName,
				.offset = int(found),
				.length = int(part.value.size()),
				.userId = part.userId,
			});
		} else if (!part.link.isEmpty() && part.link != part.value) {
			result.entities.push_back({
				.type = Entity::Type::TextUrl,
				.offset = int(found),
				.length = int(part.value.size()),
				.url = part.link,
			});
		} else {
			result.entities.push_back({
				.type = Entity::Type::Url,
				.offset = int(found),
				.length = int(part.value.size()),
			});
		}
	}
	std::stable_sort(
		result.entities.begin(),
		result.entities.end(),
		[](const Entity &a, const Entity &b) { return a.offset < b.offset; });
	return result;
}

QString FormatOutgoingText(const FormattedText &text) {
	struct Insertion {
		int position = 0;
		int order = 0;
		QString value;
	};
	auto insertions = std::vector<Insertion>();
	auto order = 0;
	const auto size = int(text.text.size());
	for (const auto &entity : text.entities) {
		const auto from = std::clamp(entity.offset, 0, size);
		const auto till = std::clamp(entity.offset + entity.length, from, size);
		if (from == till) {
			continue;
		}
		const auto wrap = [&](const QString &open, const QString &close) {
			insertions.push_back({ from, ++order, open });
			insertions.push_back({ till, -(++order), close });
		};
		switch (entity.type) {
		case Entity::Type::Bold: wrap(u"**"_q, u"**"_q); break;
		case Entity::Type::Strike: wrap(u"~~"_q, u"~~"_q); break;
		case Entity::Type::Code: wrap(u"`"_q, u"`"_q); break;
		case Entity::Type::Pre:
			wrap(u"```"_q + entity.url + '\n', u"\n```"_q);
			break;
		case Entity::Type::TextUrl:
			if (text.text.mid(from, till - from) != entity.url) {
				insertions.push_back({ till, -(++order), u" ("_q + entity.url + ')' });
			}
			break;
		default: break;
		}
	}
	// Apply from the end so that positions stay valid. Closing markers
	// (negative order) at the same position go before opening ones.
	std::sort(insertions.begin(), insertions.end(), [](
			const Insertion &a,
			const Insertion &b) {
		if (a.position != b.position) {
			return a.position > b.position;
		}
		return a.order > b.order;
	});
	auto result = text.text;
	for (const auto &insertion : insertions) {
		result.insert(insertion.position, insertion.value);
	}
	return result;
}

QString RandomUuid() {
	auto result = QString();
	result.reserve(32);
	auto *generator = QRandomGenerator::global();
	for (auto i = 0; i != 4; ++i) {
		result.append(QString::number(generator->generate(), 16)
			.rightJustified(8, '0'));
	}
	return result;
}

} // namespace Pararam
