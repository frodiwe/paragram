/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtCore/QString>

#include <vector>

namespace Pararam {

using TimeId = int32;

[[nodiscard]] int64 JsonInt(const QJsonValue &value, int64 fallback = 0);
[[nodiscard]] int64 JsonInt(
	const QJsonObject &object,
	const QString &key,
	int64 fallback = 0);
[[nodiscard]] QString JsonString(const QJsonObject &object, const QString &key);
[[nodiscard]] bool JsonBool(
	const QJsonObject &object,
	const QString &key,
	bool fallback = false);
[[nodiscard]] std::vector<int64> JsonIds(const QJsonValue &value);
[[nodiscard]] TimeId ParseTime(const QString &value);

struct UserInfo {
	int64 id = 0;
	QString name;
	QString uniqueName;
	QString info;
	QString avatar;
	TimeId updated = 0;
	bool bot = false;
	bool deleted = false;
	bool full = false; // Loaded from /user/list, not only from post meta.

	[[nodiscard]] static UserInfo Parse(const QJsonObject &object);
	void mergeFrom(const UserInfo &other);
};

struct FileInfo {
	QString guid;
	QString name;
	QString mimeType;
	int64 size = 0;
	int width = 0;
	int height = 0;

	[[nodiscard]] static FileInfo Parse(const QJsonObject &object);
	[[nodiscard]] bool valid() const {
		return !guid.isEmpty();
	}
	[[nodiscard]] bool isImage() const;
	// HEIC is shown through the server side conversion only.
	[[nodiscard]] bool needsConversion() const;
};

// Emoji -> users who reacted, in the pararam.io reactions order.
struct Reactions {
	std::vector<std::pair<QString, std::vector<int64>>> list;

	[[nodiscard]] static Reactions Parse(const QJsonValue &value);
	[[nodiscard]] QString of(int64 userId) const;
	[[nodiscard]] int count() const;
	void set(int64 userId, const QString &emoji);
	friend inline bool operator==(
		const Reactions &a,
		const Reactions &b) = default;
};

// The reactions pararam.io offers, in the order of its picker.
struct ReactionKind {
	const char16_t *emoji = nullptr;
	const char *title = nullptr;
};
[[nodiscard]] const std::vector<ReactionKind> &ReactionKinds();
[[nodiscard]] int ReactionIndex(const QString &emoji);

struct TextPart {
	QString type;
	QString value;
	QString link;
	QString title;
	QString text;
	QString description;
	QString lang;
	int64 userId = 0;
};

struct PostInfo {
	int64 chatId = 0;
	int postNo = 0;
	int64 userId = 0;
	QString uuid;
	QString text;
	std::vector<TextPart> parsed;
	int replyNo = 0;
	TimeId created = 0;
	TimeId edited = 0;
	int ver = 0;
	bool deleted = false;
	FileInfo file;
	std::vector<QString> attachments;
	Reactions reactions;
	QString eventType;
	QJsonObject eventData;
	UserInfo metaUser;

	[[nodiscard]] static PostInfo Parse(const QJsonObject &object);
	[[nodiscard]] bool isService() const {
		return !eventType.isEmpty();
	}
	[[nodiscard]] bool sameContent(const PostInfo &other) const;
};

struct ChatInfo {
	int64 id = 0;
	QString type;
	QString title;
	QString customTitle;
	QString description;
	bool pm = false;
	bool e2e = false;
	bool readOnly = false;
	bool favorite = false;
	bool visible = true;
	QString mute;
	int postsCount = 0;
	int lastReadPostNo = 0;
	int historyStart = 0;
	TimeId created = 0;
	TimeId updated = 0;
	TimeId edited = 0;
	int64 authorId = 0;
	int64 lastMsgAuthorId = 0;
	QString lastMsg;
	std::vector<int64> users;
	std::vector<int64> admins;
	std::vector<int> pinned;
	int64 organizationId = 0;

	[[nodiscard]] static ChatInfo Parse(const QJsonObject &object);
	[[nodiscard]] QString displayTitle() const {
		return customTitle.isEmpty() ? title : customTitle;
	}
	[[nodiscard]] bool muted() const {
		return !mute.isEmpty() && mute != u"none"_q;
	}
};

struct Entity {
	enum class Type {
		Bold,
		Italic,
		Code,
		Pre,
		Url,
		TextUrl,
		MentionName,
		Strike,
		Spoiler,
		Blockquote,
		Email,
		Hashtag,
		Phone,
	};
	Type type = Type::Bold;
	int offset = 0;
	int length = 0;
	QString url;
	int64 userId = 0;
};

struct FormattedText {
	QString text;
	std::vector<Entity> entities;
};

// Converts a pararam post to plain text with entities. The server
// parsed text_parsed is used when present, the raw text otherwise.
[[nodiscard]] FormattedText ParseIncomingText(
	const QString &text,
	const std::vector<TextPart> &parsed);

// Converts plain text with entities to pararam markdown.
[[nodiscard]] QString FormatOutgoingText(const FormattedText &text);

[[nodiscard]] QString RandomUuid();

} // namespace Pararam
