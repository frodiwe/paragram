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
#include <optional>

namespace Pararam {
namespace {

void AppendIds(std::vector<int64> &to, const QJsonValue &value) {
	for (const auto id : JsonIds(value)) {
		if (id && std::find(to.begin(), to.end(), id) == to.end()) {
			to.push_back(id);
		}
	}
}

[[nodiscard]] QString MimeByName(const QString &name) {
	const auto dot = name.lastIndexOf('.');
	const auto ext = (dot >= 0) ? name.mid(dot + 1).toLower() : QString();
	if (ext == u"jpg"_q || ext == u"jpeg"_q || ext == u"jfif"_q) {
		return u"image/jpeg"_q;
	} else if (ext == u"png"_q) {
		return u"image/png"_q;
	} else if (ext == u"gif"_q) {
		return u"image/gif"_q;
	} else if (ext == u"webp"_q) {
		return u"image/webp"_q;
	} else if (ext == u"bmp"_q) {
		return u"image/bmp"_q;
	} else if (ext == u"heic"_q) {
		return u"image/heic"_q;
	} else if (ext == u"avif"_q) {
		return u"image/avif"_q;
	}
	return QString();
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
	result.updated = ParseTime(JsonString(object, u"time_updated"_q));
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
	if (other.updated) {
		updated = other.updated;
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
	if (result.mimeType.isEmpty()) {
		result.mimeType = JsonString(object, u"type"_q);
	}
	if (result.mimeType.isEmpty() || result.mimeType.indexOf('/') < 0) {
		result.mimeType = MimeByName(result.name);
	}
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
	// The image types the pararam.io web client shows inline.
	const auto mime = mimeType.toLower();
	return mime == u"image/jpeg"_q
		|| mime == u"image/jpg"_q
		|| mime == u"image/pjpeg"_q
		|| mime == u"image/png"_q
		|| mime == u"image/webp"_q
		|| mime == u"image/gif"_q
		|| mime == u"image/bmp"_q
		|| mime == u"image/avif"_q
		|| needsConversion();
}

bool FileInfo::needsConversion() const {
	return (mimeType.toLower() == u"image/heic"_q)
		|| name.endsWith(u".heic"_q, Qt::CaseInsensitive);
}

Reactions Reactions::Parse(const QJsonValue &value) {
	auto result = Reactions();
	const auto object = value.toObject();
	for (auto i = object.begin(); i != object.end(); ++i) {
		auto users = JsonIds(i.value());
		if (users.empty()) {
			continue;
		}
		// The same reaction may come with or without U+FE0F.
		const auto emoji = PararamReaction(i.key());
		const auto j = std::find_if(
			result.list.begin(),
			result.list.end(),
			[&](const auto &entry) { return entry.first == emoji; });
		if (j == result.list.end()) {
			result.list.emplace_back(emoji, std::move(users));
		} else {
			for (const auto id : users) {
				if (std::find(j->second.begin(), j->second.end(), id)
					== j->second.end()) {
					j->second.push_back(id);
				}
			}
		}
	}
	// Keep the picker order, so that the list is stable between updates.
	std::stable_sort(result.list.begin(), result.list.end(), [](
			const auto &a,
			const auto &b) {
		const auto i = ReactionIndex(a.first);
		const auto j = ReactionIndex(b.first);
		return (i < 0 ? 1000 : i) < (j < 0 ? 1000 : j);
	});
	return result;
}

QString Reactions::of(int64 userId) const {
	for (const auto &[emoji, users] : list) {
		if (std::find(users.begin(), users.end(), userId) != users.end()) {
			return emoji;
		}
	}
	return QString();
}

int Reactions::count() const {
	auto result = 0;
	for (const auto &[emoji, users] : list) {
		result += int(users.size());
	}
	return result;
}

void Reactions::set(int64 userId, const QString &emoji) {
	for (auto i = list.begin(); i != list.end();) {
		auto &users = i->second;
		users.erase(
			std::remove(users.begin(), users.end(), userId),
			users.end());
		i = users.empty() ? list.erase(i) : (i + 1);
	}
	if (emoji.isEmpty()) {
		return;
	}
	for (auto &[already, users] : list) {
		if (already == emoji) {
			users.push_back(userId);
			return;
		}
	}
	const auto index = ReactionIndex(emoji);
	auto position = list.begin();
	while (position != list.end()
		&& ReactionIndex(position->first) >= 0
		&& ReactionIndex(position->first) < index) {
		++position;
	}
	list.insert(position, { emoji, { userId } });
}

const std::vector<ReactionKind> &ReactionKinds() {
	static const auto result = std::vector<ReactionKind>{
		{ u"\U0001F44D", "Like" },
		{ u"\U0001F525", "Fire" },
		{ u"\u2714", "Check" },
		{ u"\U0001F4A1", "Idea" },
		{ u"\u26A0\uFE0F", "Attention" },
		{ u"\U0001F4AF", "100%" },
		{ u"\U0001F44E", "Dislike" },
		{ u"\u2753", "Question" },
		{ u"\U0001F937", "Don't know" },
		{ u"\U0001F468\u200D\U0001F4BB", "Working on it" },
		{ u"\U0001F973", "Congratulations" },
		{ u"\U0001FAE1", "Yes" },
		{ u"\U0001F602", "Tears of joy" },
		{ u"\U0001F926", "Facepalm" },
		{ u"\U0001F6E0\uFE0F", "Fixing" },
		{ u"\U0001FAB2", "Bug" },
		{ u"\U0001F480", "Skull" },
		{ u"\U0001F4DE", "Call" },
		{ u"\U0001F50D", "Search" },
		{ u"\U0001F44F", "Clap" },
		{ u"\U0001F4A9", "Poop" },
		{ u"\u2764", "Heart" },
		{ u"\U0001F622", "Sad" },
		{ u"\U0001F440", "Look" },
		{ u"\u23F0", "Alarm" },
		{ u"\U0001F62E", "Oops" },
		{ u"\U0001F642", "Smile" },
		{ u"\U0001F37B", "Beer" },
		{ u"\U0001F37E", "Champagne" },
		{ u"\U0001F92C", "Swears" },
		{ u"\U0001F621", "Angry" },
		{ u"\U0001F91D", "Handshake" },
		{ u"\U0001F44B", "Wave" },
		{ u"\U0001F596", "Vulcan salute" },
		{ u"\U0001FAF0", "Fingers" },
		{ u"\U0001F44C", "OK" },
		{ u"\u270C\uFE0F", "Peace" },
		{ u"\U0001F918", "Rock" },
		{ u"\U0001F4AA", "Biceps" },
		{ u"\U0001F922", "Nauseated" },
		{ u"\U0001F385", "Santa" },
		{ u"\U0001F384", "Tree" },
		{ u"\U0001F52E", "Ball" },
		{ u"\U0001F381", "Gift" },
		{ u"\u2795", "Plus" },
		{ u"\u2796", "Minus" },
		{ u"\U0001F921", "Clown" },
	};
	return result;
}

int ReactionIndex(const QString &emoji) {
	const auto strip = [](QString value) {
		return value.remove(QChar(0xFE0F));
	};
	const auto wanted = strip(emoji);
	const auto &list = ReactionKinds();
	for (auto i = 0, count = int(list.size()); i != count; ++i) {
		if (wanted == strip(QString::fromUtf16(list[i].emoji))) {
			return i;
		}
	}
	return -1;
}

QString PararamReaction(const QString &emoji) {
	const auto index = ReactionIndex(emoji);
	return (index >= 0)
		? QString::fromUtf16(ReactionKinds()[index].emoji)
		: emoji;
}

QString StandardReaction(const QString &emoji) {
	// pararam.io uses the text style U+2714 and U+2764, the standard
	// (fully qualified) emoji have U+FE0F after them.
	const auto value = PararamReaction(emoji);
	if (value.size() == 1
		&& (value[0] == QChar(0x2714) || value[0] == QChar(0x2764))) {
		return value + QChar(0xFE0F);
	}
	return value;
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
		parsed.title = JsonString(part, u"title"_q);
		parsed.text = JsonString(part, u"text"_q);
		parsed.description = JsonString(part, u"description"_q);
		parsed.lang = JsonString(part, u"lang"_q);
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
	result.reactions = Reactions::Parse(meta.value(u"reactions"_q));
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
	result.edited = ParseTime(JsonString(object, u"time_edited"_q));
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

namespace {

// Builds the text the way the pararam.io web client renders text_parsed.
[[nodiscard]] FormattedText FromParsed(const std::vector<TextPart> &parsed) {
	auto result = FormattedText();
	const auto append = [&](
			const QString &value,
			std::optional<Entity> entity = std::nullopt) {
		if (entity && !value.isEmpty()) {
			entity->offset = int(result.text.size());
			entity->length = int(value.size());
			result.entities.push_back(*entity);
		}
		result.text.append(value);
	};
	const auto entity = [](Entity::Type type) {
		return Entity{ .type = type };
	};
	for (const auto &part : parsed) {
		const auto &type = part.type;
		if (type == u"br"_q) {
			result.text.append('\n');
		} else if (type == u"bold"_q) {
			append(part.value, entity(Entity::Type::Bold));
		} else if (type == u"strike"_q) {
			append(part.value, entity(Entity::Type::Strike));
		} else if (type == u"blockquote1"_q) {
			// Code block, the value starts with the language if lang is set.
			auto code = part.value;
			auto language = QString();
			if (!part.lang.isEmpty()) {
				static const auto separator = QRegularExpression(
					u"[\r\n ]"_q);
				const auto position = int(code.indexOf(separator));
				if (position >= 0) {
					language = (part.lang == u"code"_q)
						? QString()
						: part.lang;
					code = code.mid(position + 1);
				}
			}
			while (code.startsWith('\n') || code.startsWith('\r')) {
				code = code.mid(1);
			}
			while (code.endsWith('\n') || code.endsWith('\r')) {
				code.chop(1);
			}
			if (!result.text.isEmpty() && !result.text.endsWith('\n')) {
				result.text.append('\n');
			}
			append(code, Entity{
				.type = Entity::Type::Pre,
				.url = language,
			});
		} else if (type == u"blockquote2"_q) {
			append(part.value, entity(Entity::Type::Code));
		} else if (type == u"blockquote3"_q || type == u"blockquote4"_q) {
			append(part.value, entity(Entity::Type::Blockquote));
		} else if (type == u"link"_q) {
			const auto title = !part.title.isEmpty()
				? part.title
				: !part.value.isEmpty()
				? part.value
				: part.link;
			const auto url = part.link.isEmpty() ? part.value : part.link;
			if (url.isEmpty()) {
				append(title);
			} else if (title == url) {
				append(title, entity(Entity::Type::Url));
			} else {
				append(title, Entity{
					.type = Entity::Type::TextUrl,
					.url = url,
				});
			}
		} else if (type == u"mention"_q
			|| type == u"userlink"_q
			|| type == u"user_link"_q) {
			if (part.userId) {
				append(part.value, Entity{
					.type = Entity::Type::MentionName,
					.userId = part.userId,
				});
			} else {
				append(part.value, entity(Entity::Type::Bold));
			}
		} else if (type == u"mention_special"_q
			|| type == u"mention_group"_q) {
			append(part.value, entity(Entity::Type::Bold));
		} else if (type == u"email"_q) {
			append(part.value, entity(Entity::Type::Email));
		} else if (type == u"tag"_q) {
			append(part.value, entity(Entity::Type::Hashtag));
		} else if (type == u"phone"_q) {
			append(
				part.text.isEmpty() ? part.value : part.text,
				entity(Entity::Type::Phone));
		} else if (type == u"spoiler"_q) {
			append(
				part.text.isEmpty() ? part.value : part.text,
				entity(Entity::Type::Spoiler));
		} else if (type == u"textcolor"_q) {
			append(part.text.isEmpty() ? part.value : part.text);
		} else if (type == u"tooltip"_q) {
			append(part.description.isEmpty()
				? (part.text.isEmpty() ? part.value : part.text)
				: part.description);
		} else if (type == u"checkbox"_q) {
			if (!result.text.isEmpty() && !result.text.endsWith('\n')) {
				result.text.append('\n');
			}
			const auto checked = !part.description.trimmed().isEmpty();
			result.text.append(checked ? u"\u2611 "_q : u"\u2610 "_q);
			append(part.text.isEmpty() ? part.value : part.text);
		} else if (type == u"bot_link"_q) {
			append(part.title.isEmpty() ? part.value : part.title);
		} else {
			// text, time, datetime, ipv4, hint, table and unknown ones.
			append(!part.value.isEmpty()
				? part.value
				: !part.text.isEmpty()
				? part.text
				: part.title);
		}
	}
	return result;
}

} // namespace

FormattedText ParseIncomingText(
		const QString &text,
		const std::vector<TextPart> &parsed) {
	if (!parsed.empty()) {
		auto result = FromParsed(parsed);
		if (!result.text.trimmed().isEmpty() || text.trimmed().isEmpty()) {
			return result;
		}
	}
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
				wrap(u"["_q, u"]("_q + entity.url + ')');
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
