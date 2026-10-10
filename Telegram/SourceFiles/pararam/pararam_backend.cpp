/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_backend.h"

#include "pararam/pararam_tl_reader.h"
#include "pararam/pararam_calls.h"
#include "pararam/pararam_reaction_icons.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "mtproto/details/mtproto_dump_to_text.h"
#include "scheme-dump_to_text.h"
#include "base/unixtime.h"
#include "logs.h"

#include <QtCore/QBuffer>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QDateTime>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTimeZone>
#include <QtGui/QImageReader>

#include <algorithm>
#include <limits>

namespace Pararam {
namespace {

constexpr auto kSerializeTag = quint32(0x50524231); // 'PRB1'
constexpr auto kDcId = 2;
constexpr auto kPmSlotShift = 20;
constexpr auto kPmPostMask = (1 << kPmSlotShift) - 1;
constexpr auto kMaxSlots = 2047;
constexpr auto kLogLimit = 3000;
constexpr auto kPollInterval = crl::time(3000);
constexpr auto kPollRetryInterval = crl::time(10000);
constexpr auto kMinPollInterval = crl::time(1000);
constexpr auto kMaxPostsPerRequest = 99;
constexpr auto kChatsPerRequest = 50;
constexpr auto kUsersPerRequest = 100;
constexpr auto kParallelRequests = 6;
constexpr auto kHistoryAttempts = 6;
constexpr auto kEditCheckDepth = 30;
constexpr auto kMaxCachedBytes = int64(256 * 1024 * 1024);
constexpr auto kChannelDifferenceTimeout = 5;
constexpr auto kArchiveFolderId = 1;
constexpr auto kSearchLimit = 50;
constexpr auto kFirstFilterId = 2;
constexpr auto kImageProbeBytes = 256 * 1024;
constexpr auto kMaxProbedImageSize = int64(50 * 1024 * 1024);
constexpr auto kPreviewSide = 400; // The ?s= preview of the file server.
constexpr auto kConvertedSide = 2000; // HEIC converted by the file server.
constexpr auto kAvatarSmall = 200;
constexpr auto kAvatarLarge = 400;
constexpr auto kReactionDocumentBase = uint64(0x7FFFFFFF00000000ULL);
constexpr auto kMaxRecentReactions = 50;
constexpr auto kReadOutboxCheckInterval = crl::time(15000);
constexpr auto kReadMarkExpirePeriod = 10 * 365 * 86400;
constexpr auto kReadMarkSizeThreshold = 1000;

base::flat_map<const MTP::Instance*, Backend*> &Registry() {
	static auto result = base::flat_map<const MTP::Instance*, Backend*>();
	return result;
}

[[nodiscard]] int64 AccessHash(int64 id) {
	return int64(uint64(id) * 0x9E3779B97F4A7C15ULL) & 0x7FFFFFFFFFFFFFFFLL;
}

[[nodiscard]] uint64 HashId(const QString &key) {
	const auto hash = QCryptographicHash::hash(
		key.toUtf8(),
		QCryptographicHash::Sha1);
	auto result = uint64();
	memcpy(&result, hash.constData(), sizeof(result));
	return (result & 0x7FFFFFFFFFFFFFFFULL) | 1;
}

[[nodiscard]] MTPReaction ReactionToMTP(const QString &emoji) {
	return MTP_reactionEmoji(MTP_string(StandardReaction(emoji)));
}

[[nodiscard]] QSize FitPreview(QSize size) {
	if (size.width() <= kPreviewSide && size.height() <= kPreviewSide) {
		return size;
	}
	return size.scaled(kPreviewSide, kPreviewSide, Qt::KeepAspectRatio);
}

[[nodiscard]] TimeId Now() {
	return base::unixtime::now();
}

[[nodiscard]] QString FormatSyncTime(TimeId time) {
	return QDateTime::fromSecsSinceEpoch(time, QTimeZone::UTC).toString(
		u"yyyy-MM-ddTHH:mm:ss.zzz000Z"_q);
}

// Responses must be boxed (start with the constructor id), while the
// generated MTP_*() functions return bare values.
template <typename Type>
[[nodiscard]] mtpBuffer Serialize(const Type &value) {
	if constexpr (tl::is_boxed_v<Type>) {
		auto result = mtpBuffer();
		result.reserve(tl::count_length(value) >> 2);
		value.write(result);
		return result;
	} else {
		return Serialize(tl::boxed<Type>(value));
	}
}

[[nodiscard]] QString JoinIds(const std::vector<int64> &ids) {
	auto result = QStringList();
	result.reserve(ids.size());
	for (const auto id : ids) {
		result.push_back(QString::number(id));
	}
	return result.join(',');
}

[[nodiscard]] std::vector<int64> Unique(std::vector<int64> ids) {
	std::sort(ids.begin(), ids.end());
	ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
	ids.erase(std::remove(ids.begin(), ids.end(), int64(0)), ids.end());
	return ids;
}

[[nodiscard]] std::vector<PostInfo> ParsePosts(const QJsonValue &value) {
	auto result = std::vector<PostInfo>();
	const auto object = value.toObject();
	const auto list = object.contains(u"posts"_q)
		? object.value(u"posts"_q).toArray()
		: value.toArray();
	result.reserve(list.size());
	for (const auto &entry : list) {
		auto post = PostInfo::Parse(entry.toObject());
		if (post.chatId && post.postNo > 0) {
			result.push_back(std::move(post));
		}
	}
	return result;
}

[[nodiscard]] MTPChatBannedRights DefaultBannedRights(bool readOnly) {
	using Flag = MTPDchatBannedRights::Flag;
	auto flags = Flag::f_send_polls
		| Flag::f_send_games
		| Flag::f_send_inline
		| Flag::f_send_stickers
		| Flag::f_send_gifs
		| Flag::f_change_info
		| Flag::f_invite_users
		| Flag::f_pin_messages
		| Flag::f_manage_topics;
	if (readOnly) {
		flags |= Flag::f_send_messages
			| Flag::f_send_media
			| Flag::f_send_photos
			| Flag::f_send_videos
			| Flag::f_send_roundvideos
			| Flag::f_send_audios
			| Flag::f_send_voices
			| Flag::f_send_docs
			| Flag::f_send_plain
			| Flag::f_embed_links;
	}
	return MTP_chatBannedRights(MTP_flags(flags), MTP_int(0));
}

[[nodiscard]] MTPChatAdminRights AdminRights() {
	using Flag = MTPDchatAdminRights::Flag;
	return MTP_chatAdminRights(MTP_flags(Flag::f_change_info
		| Flag::f_post_messages
		| Flag::f_edit_messages
		| Flag::f_delete_messages
		| Flag::f_ban_users
		| Flag::f_invite_users
		| Flag::f_pin_messages
		| Flag::f_other));
}

[[nodiscard]] std::vector<Entity> EntitiesFromMTP(
		const MTPVector<MTPMessageEntity> &entities) {
	auto result = std::vector<Entity>();
	const auto simple = [&](Entity::Type type, const auto &data) {
		result.push_back({
			.type = type,
			.offset = data.voffset().v,
			.length = data.vlength().v,
		});
	};
	for (const auto &entity : entities.v) {
		entity.match([&](const MTPDmessageEntityBold &data) {
			simple(Entity::Type::Bold, data);
		}, [&](const MTPDmessageEntityItalic &data) {
			simple(Entity::Type::Italic, data);
		}, [&](const MTPDmessageEntityStrike &data) {
			simple(Entity::Type::Strike, data);
		}, [&](const MTPDmessageEntityCode &data) {
			simple(Entity::Type::Code, data);
		}, [&](const MTPDmessageEntityPre &data) {
			result.push_back({
				.type = Entity::Type::Pre,
				.offset = data.voffset().v,
				.length = data.vlength().v,
				.url = qs(data.vlanguage()),
			});
		}, [&](const MTPDmessageEntityTextUrl &data) {
			result.push_back({
				.type = Entity::Type::TextUrl,
				.offset = data.voffset().v,
				.length = data.vlength().v,
				.url = qs(data.vurl()),
			});
		}, [](const auto &) {
		});
	}
	return result;
}

[[nodiscard]] MTPVector<MTPMessageEntity> EntitiesToMTP(
		const std::vector<Entity> &entities) {
	auto result = QVector<MTPMessageEntity>();
	result.reserve(entities.size());
	for (const auto &entity : entities) {
		const auto offset = MTP_int(entity.offset);
		const auto length = MTP_int(entity.length);
		switch (entity.type) {
		case Entity::Type::Bold:
			result.push_back(MTP_messageEntityBold(offset, length));
			break;
		case Entity::Type::Italic:
			result.push_back(MTP_messageEntityItalic(offset, length));
			break;
		case Entity::Type::Strike:
			result.push_back(MTP_messageEntityStrike(offset, length));
			break;
		case Entity::Type::Code:
			result.push_back(MTP_messageEntityCode(offset, length));
			break;
		case Entity::Type::Pre:
			result.push_back(MTP_messageEntityPre(
				offset,
				length,
				MTP_string(entity.url)));
			break;
		case Entity::Type::Url:
			result.push_back(MTP_messageEntityUrl(offset, length));
			break;
		case Entity::Type::TextUrl:
			result.push_back(MTP_messageEntityTextUrl(
				offset,
				length,
				MTP_string(entity.url)));
			break;
		case Entity::Type::MentionName:
			result.push_back(MTP_messageEntityMentionName(
				offset,
				length,
				MTP_long(entity.userId)));
			break;
		case Entity::Type::Spoiler:
			result.push_back(MTP_messageEntitySpoiler(offset, length));
			break;
		case Entity::Type::Blockquote:
			result.push_back(MTP_messageEntityBlockquote(
				MTP_flags(0),
				offset,
				length));
			break;
		case Entity::Type::Email:
			result.push_back(MTP_messageEntityEmail(offset, length));
			break;
		case Entity::Type::Hashtag:
			result.push_back(MTP_messageEntityHashtag(offset, length));
			break;
		case Entity::Type::Phone:
			result.push_back(MTP_messageEntityPhone(offset, length));
			break;
		}
	}
	return MTP_vector<MTPMessageEntity>(std::move(result));
}

[[nodiscard]] QString DescribeEvent(const PostInfo &post) {
	if (!post.text.trimmed().isEmpty()) {
		return post.text.trimmed();
	}
	const auto &type = post.eventType;
	if (type == u"ENTER_TO_THREAD"_q) {
		return u"joined the chat"_q;
	} else if (type == u"GROUP_LEAVED"_q) {
		return u"left the group"_q;
	} else if (type == u"CHAT_TITLE"_q || type == u"EDIT_THREAD"_q) {
		const auto title = JsonString(post.eventData, u"title"_q);
		return title.isEmpty()
			? u"changed the chat"_q
			: (u"changed the chat title to \""_q + title + '"');
	} else if (type == u"NEW_THREAD"_q) {
		return u"created the chat"_q;
	} else if (type == u"POST_PINNED"_q) {
		return u"pinned a message"_q;
	} else if (type == u"CALL"_q) {
		return u"call"_q;
	}
	return type.toLower().replace('_', ' ');
}

// pararam.io writes a CALL event post into a chat for every call:
// {users: [{id, join, leave}], silent, is_open}.
[[nodiscard]] bool IsCallEvent(const PostInfo &post) {
	return (post.eventType == u"CALL"_q);
}

[[nodiscard]] MTPMessageAction PhoneCallAction(const PostInfo &post) {
	using Flag = MTPDmessageActionPhoneCall::Flag;
	const auto users = post.eventData.value(u"users"_q).toArray();
	auto joined = 0;
	auto start = TimeId(0);
	auto finish = TimeId(0);
	for (const auto &value : users) {
		const auto user = value.toObject();
		const auto join = TimeId(JsonInt(user, u"join"_q));
		const auto leave = TimeId(JsonInt(user, u"leave"_q));
		if (join) {
			++joined;
			start = start ? std::min(start, join) : join;
		}
		finish = std::max(finish, leave);
	}
	const auto open = JsonBool(post.eventData, u"is_open"_q);
	const auto missed = !open && (joined < 2);
	const auto duration = (!open && joined >= 2 && finish > start)
		? int(finish - start)
		: 0;
	return MTP_messageActionPhoneCall(
		MTP_flags((missed ? Flag::f_reason : Flag())
			| (duration ? Flag::f_duration : Flag())),
		MTP_long((uint64(post.chatId) << 24) ^ uint64(post.postNo)),
		MTP_phoneCallDiscardReasonMissed(),
		MTP_int(duration));
}

[[nodiscard]] HttpError LocalError(const QString &type) {
	auto result = HttpError();
	result.status = 400;
	result.type = type;
	result.message = type;
	return result;
}

} // namespace

struct Backend::Download {
	QByteArray bytes;
	bool loaded = false;
	bool loading = false;
	crl::time lastUsed = 0;
	std::vector<Fn<void(QByteArray)>> callbacks;
};

struct Backend::Upload {
	base::flat_map<int, QByteArray> parts;
};

struct Backend::HistoryQuery {
	int64 chatId = 0;
	int32 offsetId = 0;
	int32 addOffset = 0;
	int32 limit = 0;
	int32 maxId = 0;
	int32 minId = 0;
	int lo = 0;
	int hi = 0;
	int attempts = 0;
};

Backend::Backend(not_null<MTP::Instance*> instance)
: _instance(instance)
, _downloads(std::make_unique<base::flat_map<QString, Download>>())
, _uploads(std::make_unique<base::flat_map<uint64, Upload>>()) {
	Registry().emplace(instance.get(), this);
	_api.setChangedCallback([=] {
		sessionChanged();
	});
	_pollTimer.setSingleShot(true);
	QObject::connect(&_pollTimer, &QTimer::timeout, this, [=] { poll(); });
	_calls = std::make_unique<CallManager>(this);
}

Backend::~Backend() {
	_calls = nullptr;
	Registry().remove(_instance.get());
}

Backend *Backend::For(not_null<const MTP::Instance*> instance) {
	const auto &registry = Registry();
	const auto i = registry.find(instance.get());
	return (i != end(registry)) ? i->second : nullptr;
}

bool Backend::authorized() const {
	return _api.authorized() && _selfId != 0;
}

QByteArray Backend::serialize() const {
	if (!authorized()) {
		return QByteArray();
	}
	auto result = QByteArray();
	{
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		stream
			<< kSerializeTag
			<< _api.serialize()
			<< qint64(_selfId)
			<< _self.name
			<< _self.uniqueName
			<< qint32(_slots.size());
		for (const auto &[chatId, slot] : _slots) {
			stream << qint64(chatId) << qint32(slot);
		}
		stream << qint32(_localMute.size());
		for (const auto &[chatId, muted] : _localMute) {
			stream << qint64(chatId) << qint32(muted ? 1 : 0);
		}
	}
	return result;
}

void Backend::restore(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return;
	}
	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto tag = quint32();
	auto api = QByteArray();
	auto selfId = qint64();
	auto name = QString();
	auto uniqueName = QString();
	auto count = qint32();
	stream >> tag;
	if (tag != kSerializeTag) {
		return;
	}
	stream >> api >> selfId >> name >> uniqueName >> count;
	if (stream.status() != QDataStream::Ok || count < 0 || count > 65536) {
		return;
	}
	auto slots = base::flat_map<int64, int>();
	for (auto i = 0; i != count; ++i) {
		auto chatId = qint64();
		auto slot = qint32();
		stream >> chatId >> slot;
		slots.emplace(chatId, slot);
	}
	auto mutes = base::flat_map<int64, bool>();
	auto mutesCount = qint32();
	stream >> mutesCount;
	for (auto i = 0; i < mutesCount && i < 65536; ++i) {
		auto chatId = qint64();
		auto muted = qint32();
		stream >> chatId >> muted;
		mutes.emplace(chatId, muted != 0);
	}
	if (stream.status() != QDataStream::Ok || !_api.restore(api)) {
		return;
	}
	_localMute = std::move(mutes);
	_selfId = selfId;
	_self.id = selfId;
	_self.name = name;
	_self.uniqueName = uniqueName;
	_users[_selfId] = _self;
	_slots = std::move(slots);
	_chatBySlot.clear();
	for (const auto &[chatId, slot] : _slots) {
		_chatBySlot.emplace(slot, chatId);
	}
}

void Backend::setSessionChangedCallback(std::function<void()> callback) {
	_sessionChanged = std::move(callback);
}

void Backend::sessionChanged() {
	if (_sessionChanged) {
		_sessionChanged();
	}
}

void Backend::resetState() {
	if (_calls) {
		_calls->stop();
	}
	_pollTimer.stop();
	_loaded = _loading = _polling = false;
	_syncBroken = false;
	_loadedWaiters.clear();
	_loadFailWaiters.clear();
	_chats.clear();
	_chatByUser.clear();
	_documents.clear();
	_documentByGuid.clear();
	_downloads->clear();
	_uploads->clear();
	_cachedBytes = 0;
	_commonLog.clear();
	_commonPts = 1;
	_syncTime = QString();
}

void Backend::login(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult, MTPUser)> done) {
	_api.login(email, password, code, [=](LoginResult result) {
		if (result.status != LoginResult::Status::Success) {
			done(result, MTPUser());
			return;
		}
		resetState();
		_self = UserInfo::Parse(result.profile);
		_self.full = true;
		_selfId = _self.id;
		_users.clear();
		_users.emplace(_selfId, _self);
		_slots.clear();
		_chatBySlot.clear();
		_localMute.clear();
		_tags.clear();
		_tagById.clear();
		sessionChanged();
		done(result, userToMTP(_selfId));
	});
}

// Requests.

mtpBuffer Backend::ErrorBuffer(int code, const QString &type) {
	return Serialize(MTP_rpc_error(MTP_int(code), MTP_string(type)));
}

void Backend::handleRequest(
		mtpRequestId requestId,
		mtpBuffer &&body,
		mtpRequestId afterRequestId) {
	if (afterRequestId && _pending.contains(afterRequestId)) {
		auto copy = std::make_shared<mtpBuffer>(std::move(body));
		_after[afterRequestId].push_back([=] {
			handleRequest(requestId, std::move(*copy), 0);
		});
		return;
	}
	_cancelled.remove(requestId);
	_pending.emplace(requestId);
	if (body.isEmpty()) {
		replyError(requestId, 400, u"INPUT_REQUEST_EMPTY"_q);
		return;
	}
	auto reader = Reader(body.constData(), body.constData() + body.size());
	const auto type = reader.typeId();
	dispatch(requestId, type, reader, body);
}

void Backend::cancelRequest(mtpRequestId requestId) {
	if (_pending.remove(requestId)) {
		_cancelled.emplace(requestId);
		runAfter(requestId);
	}
}

void Backend::runAfter(mtpRequestId requestId) {
	const auto i = _after.find(requestId);
	if (i == end(_after)) {
		return;
	}
	auto list = std::move(i->second);
	_after.erase(i);
	for (const auto &callback : list) {
		callback();
	}
}

void Backend::finishRequest(mtpRequestId requestId, mtpBuffer &&reply) {
	if (!_pending.remove(requestId)) {
		_cancelled.remove(requestId);
		runAfter(requestId);
		return;
	}
	const auto instance = _instance;
	auto response = MTP::Response{
		.reply = std::move(reply),
		.outerMsgId = 0,
		.requestId = requestId,
	};
	QMetaObject::invokeMethod(this, [=, response = std::move(response)] {
		instance->processCallback(response);
	}, Qt::QueuedConnection);
	runAfter(requestId);
}

template <typename Type>
void Backend::reply(mtpRequestId requestId, const Type &value) {
	finishRequest(requestId, Serialize(value));
}

void Backend::replyError(
		mtpRequestId requestId,
		int code,
		const QString &type) {
	finishRequest(requestId, ErrorBuffer(code, type));
}

void Backend::replyHttpError(mtpRequestId requestId, const HttpError &error) {
	if (error.unauthorized()) {
		LOG(("Pararam: session expired, logging out."));
		replyError(requestId, 401, u"AUTH_KEY_UNREGISTERED"_q);
		return;
	}
	if (error.status == 0 || error.status >= 500 || error.status == 429) {
		// Telegram Desktop retries such requests automatically.
		LOG(("Pararam: server error %1 (%2), will retry."
			).arg(error.status
			).arg(error.message));
		replyError(requestId, 500, u"PARARAM_SERVER_ERROR"_q);
		return;
	}
	auto type = error.type.toUpper();
	static const auto bad = QRegularExpression(u"[^A-Z0-9_]"_q);
	type.replace(bad, u"_"_q);
	LOG(("Pararam: request failed with %1 %2 (%3)."
		).arg(error.status
		).arg(type
		).arg(error.message));
	replyError(
		requestId,
		400,
		type.isEmpty() ? u"PARARAM_ERROR"_q : (u"PARARAM_"_q + type));
}

void Backend::replyTrue(mtpRequestId requestId) {
	reply(requestId, MTP_boolTrue());
}

auto Backend::failer(mtpRequestId requestId) -> Fail {
	return [=](const HttpError &error) {
		replyHttpError(requestId, error);
	};
}

void Backend::pushUpdates(const MTPUpdates &updates) {
	auto response = MTP::Response{ .reply = Serialize(updates) };
	const auto instance = _instance;
	QMetaObject::invokeMethod(this, [=, response = std::move(response)] {
		instance->processUpdate(response);
	}, Qt::QueuedConnection);
}

void Backend::dispatch(
		mtpRequestId requestId,
		mtpTypeId type,
		Reader &reader,
		const mtpBuffer &body) {
	switch (type) {
	// Wrappers.
	case mtpc_invokeWithoutUpdates:
		return dispatch(requestId, reader.typeId(), reader, body);
	case mtpc_invokeAfterMsg: {
		[[maybe_unused]] const auto msgId = reader.int64Value();
		return dispatch(requestId, reader.typeId(), reader, body);
	}
	case mtpc_invokeWithLayer: {
		[[maybe_unused]] const auto layer = reader.int32Value();
		return dispatch(requestId, reader.typeId(), reader, body);
	}

	// Configuration.
	case mtpc_help_getConfig: return replyConfig(requestId);
	case mtpc_help_getNearestDc: return replyNearestDc(requestId);
	case mtpc_help_getAppConfig: return replyAppConfig(requestId);
	case mtpc_help_getCdnConfig:
		return reply(requestId, MTP_cdnConfig(MTP_vector<MTPCdnPublicKey>()));

	// Account.
	case mtpc_auth_logOut: return logOut(requestId);
	case mtpc_account_updateStatus:
	case mtpc_account_registerDevice:
	case mtpc_account_unregisterDevice:
	case mtpc_messages_setTyping:
	case mtpc_messages_reorderPinnedDialogs:
	case mtpc_messages_saveDraft:
	case mtpc_messages_markDialogUnread:
	case mtpc_help_dismissSuggestion:
	case mtpc_help_saveAppLog:
	case mtpc_messages_hidePeerSettingsBar:
		return replyTrue(requestId);
	}
	if (!authorized()) {
		replyError(requestId, 401, u"AUTH_KEY_UNREGISTERED"_q);
		return;
	}
	if (_calls->handle(requestId, type, reader)) {
		return;
	}
	switch (type) {
	case mtpc_account_updateNotifySettings:
		return updateNotifySettings(requestId, reader);

	// Updates.
	case mtpc_updates_getState: return getState(requestId);
	case mtpc_updates_getDifference: return getDifference(requestId, reader);
	case mtpc_updates_getChannelDifference:
		return getChannelDifference(requestId, reader);

	// Dialogs.
	case mtpc_messages_getDialogs: return getDialogs(requestId, reader);
	case mtpc_messages_getPinnedDialogs:
		return getPinnedDialogs(requestId, reader);
	case mtpc_messages_getPeerDialogs:
		return getPeerDialogs(requestId, reader);
	case mtpc_messages_toggleDialogPin:
		return toggleDialogPin(requestId, reader);
	case mtpc_folders_editPeerFolders:
		return editPeerFolders(requestId, reader);
	case mtpc_messages_getDialogFilters:
		return getDialogFilters(requestId);
	case mtpc_messages_updateDialogFilter:
		return updateDialogFilter(requestId, reader);
	case mtpc_messages_updateDialogFiltersOrder:
		return replyTrue(requestId);
	case mtpc_messages_getSuggestedDialogFilters:
		return reply(
			requestId,
			MTP_vector<MTPDialogFilterSuggested>());

	// Messages.
	case mtpc_messages_getHistory: return getHistory(requestId, reader);
	case mtpc_messages_getMessages:
		return getMessages(requestId, reader, false);
	case mtpc_channels_getMessages:
		return getMessages(requestId, reader, true);
	case mtpc_messages_sendMessage: return sendMessage(requestId, reader);
	case mtpc_messages_sendMedia: return sendMedia(requestId, reader);
	case mtpc_messages_sendMultiMedia:
		return sendMultiMedia(requestId, reader);
	case mtpc_messages_forwardMessages:
		return forwardMessages(requestId, reader);
	case mtpc_messages_editMessage: return editMessage(requestId, reader);
	case mtpc_messages_deleteMessages:
		return deleteMessages(requestId, reader, false);
	case mtpc_channels_deleteMessages:
		return deleteMessages(requestId, reader, true);
	case mtpc_messages_readHistory:
		return readHistory(requestId, reader, false);
	case mtpc_channels_readHistory:
		return readHistory(requestId, reader, true);
	case mtpc_messages_search:
		return messagesSearch(requestId, reader, false);
	case mtpc_messages_searchGlobal:
		return messagesSearch(requestId, reader, true);
	case mtpc_messages_readMentions:
	case mtpc_messages_readReactions:
		return reply(requestId, MTP_messages_affectedHistory(
			MTP_int(_commonPts),
			MTP_int(0),
			MTP_int(0)));
	case mtpc_messages_readMessageContents:
		return reply(requestId, MTP_messages_affectedMessages(
			MTP_int(_commonPts),
			MTP_int(0)));
	case mtpc_channels_readMessageContents:
		return replyTrue(requestId);
	case mtpc_messages_getScheduledHistory:
	case mtpc_messages_getUnreadMentions:
	case mtpc_messages_getUnreadReactions:
		return reply(requestId, MTP_messages_messages(
			MTP_vector<MTPMessage>(),
			MTP_vector<MTPForumTopic>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	case mtpc_messages_getSearchCounters:
		return reply(requestId, MTP_vector<MTPmessages_SearchCounter>());

	// Users and chats.
	case mtpc_users_getUsers: return getUsers(requestId, reader);
	case mtpc_users_getFullUser: return getFullUser(requestId, reader);
	case mtpc_channels_getChannels: return getChannels(requestId, reader);
	case mtpc_channels_getFullChannel:
		return getFullChannel(requestId, reader);
	case mtpc_channels_getParticipants:
		return getParticipants(requestId, reader);
	case mtpc_channels_getParticipant:
		return getParticipant(requestId, reader);
	case mtpc_channels_joinChannel:
		return joinChannel(requestId, reader, true);
	case mtpc_channels_leaveChannel:
		return joinChannel(requestId, reader, false);
	case mtpc_contacts_getContacts: return getContacts(requestId);
	case mtpc_contacts_search: return contactsSearch(requestId, reader);
	case mtpc_contacts_resolveUsername:
		return resolveUsername(requestId, reader);
	case mtpc_messages_getPeerSettings:
		return reply(requestId, MTP_messages_peerSettings(
			MTP_peerSettings(
				MTP_flags(0),
				MTPint(),
				MTPstring(),
				MTPint(),
				MTPlong(),
				MTPstring(),
				MTPlong(),
				MTPstring(),
				MTPstring(),
				MTPint(),
				MTPint()),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	case mtpc_account_getNotifySettings:
		return reply(requestId, MTP_peerNotifySettings(
			MTP_flags(0),
			MTPBool(),
			MTPBool(),
			MTPint(),
			MTPNotificationSound(),
			MTPNotificationSound(),
			MTPNotificationSound(),
			MTPBool(),
			MTPBool(),
			MTPNotificationSound(),
			MTPNotificationSound(),
			MTPNotificationSound()));

	// Reactions.
	case mtpc_messages_sendReaction: return sendReaction(requestId, reader);
	case mtpc_messages_getMessagesReactions:
		return getMessagesReactions(requestId, reader);
	case mtpc_messages_getMessageReactionsList:
		return getMessageReactionsList(requestId, reader);

	// Read receipts.
	case mtpc_messages_getMessageReadParticipants:
		return getMessageReadParticipants(requestId, reader);
	case mtpc_messages_getOutboxReadDate:
		return getOutboxReadDate(requestId, reader);

	// Files.
	case mtpc_upload_getFile: return getFile(requestId, reader);
	case mtpc_photos_getUserPhotos: return getUserPhotos(requestId, reader);
	case mtpc_upload_saveFilePart:
		return saveFilePart(requestId, reader, false);
	case mtpc_upload_saveBigFilePart:
		return saveFilePart(requestId, reader, true);

	// Features pararam.io does not have: reply with empty data so that
	// the UI does not retry.
	case mtpc_messages_getAllStickers:
	case mtpc_messages_getMaskStickers:
	case mtpc_messages_getEmojiStickers:
		return reply(requestId, MTP_messages_allStickersNotModified());
	case mtpc_messages_getRecentStickers:
		return reply(requestId, MTP_messages_recentStickersNotModified());
	case mtpc_messages_getFeaturedStickers:
	case mtpc_messages_getFeaturedEmojiStickers:
		return reply(
			requestId,
			MTP_messages_featuredStickersNotModified(MTP_int(0)));
	case mtpc_messages_getSavedGifs:
		return reply(requestId, MTP_messages_savedGifsNotModified());
	case mtpc_messages_getFavedStickers:
		return reply(requestId, MTP_messages_favedStickersNotModified());
	case mtpc_messages_getStickers:
		return reply(requestId, MTP_messages_stickers(
			MTP_long(0),
			MTP_vector<MTPDocument>()));
	case mtpc_account_getWallPapers:
		return reply(requestId, MTP_account_wallPapersNotModified());
	case mtpc_help_getPeerColors:
	case mtpc_help_getPeerProfileColors:
		return reply(requestId, MTP_help_peerColorsNotModified());
	case mtpc_help_getTimezonesList:
		return reply(requestId, MTP_help_timezonesListNotModified());
	case mtpc_messages_getTopReactions:
	case mtpc_messages_getRecentReactions:
		return reply(requestId, reactionsList());
	case mtpc_messages_getDefaultTagReactions:
		return reply(requestId, MTP_messages_reactionsNotModified());
	case mtpc_messages_getAvailableReactions:
		return reply(requestId, availableReactions());
	case mtpc_messages_clearRecentReactions:
		return replyTrue(requestId);
	case mtpc_messages_getAvailableEffects:
		return reply(requestId, MTP_messages_availableEffectsNotModified());
	case mtpc_account_getDefaultEmojiStatuses:
	case mtpc_account_getRecentEmojiStatuses:
	case mtpc_account_getChannelDefaultEmojiStatuses:
	case mtpc_account_getCollectibleEmojiStatuses:
		return reply(requestId, MTP_account_emojiStatusesNotModified());
	case mtpc_account_getDefaultProfilePhotoEmojis:
	case mtpc_account_getDefaultGroupPhotoEmojis:
	case mtpc_account_getDefaultBackgroundEmojis:
	case mtpc_account_getChannelRestrictedStatusEmojis:
		return reply(requestId, MTP_emojiListNotModified());
	case mtpc_messages_getEmojiGroups:
	case mtpc_messages_getEmojiStatusGroups:
	case mtpc_messages_getEmojiProfilePhotoGroups:
	case mtpc_messages_getEmojiStickerGroups:
		return reply(requestId, MTP_messages_emojiGroupsNotModified());
	case mtpc_account_getThemes:
	case mtpc_account_getChatThemes:
		return reply(requestId, MTP_account_themesNotModified());
	case mtpc_messages_getSavedReactionTags:
		return reply(requestId, MTP_messages_savedReactionTagsNotModified());
	case mtpc_account_getSavedRingtones:
		return reply(requestId, MTP_account_savedRingtonesNotModified());
	case mtpc_help_getTermsOfServiceUpdate:
		return reply(requestId, MTP_help_termsOfServiceUpdateEmpty(
			MTP_int(Now() + 86400)));
	case mtpc_help_getPromoData:
		return reply(
			requestId,
			MTP_help_promoDataEmpty(MTP_int(Now() + 86400)));
	case mtpc_help_getCountriesList:
		return reply(requestId, MTP_help_countriesListNotModified());
	case mtpc_contacts_getTopPeers:
		return reply(requestId, MTP_contacts_topPeersDisabled());
	case mtpc_contacts_getBlocked:
		return reply(requestId, MTP_contacts_blocked(
			MTP_vector<MTPPeerBlocked>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	case mtpc_contacts_getBirthdays:
		return reply(requestId, MTP_contacts_contactBirthdays(
			MTP_vector<MTPContactBirthday>(),
			MTP_vector<MTPUser>()));
	case mtpc_account_getContentSettings:
		return reply(requestId, MTP_account_contentSettings(MTP_flags(0)));
	case mtpc_messages_getQuickReplies:
		return reply(requestId, MTP_messages_quickReplies(
			MTP_vector<MTPQuickReply>(),
			MTP_vector<MTPMessage>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	case mtpc_messages_getSavedDialogs:
	case mtpc_messages_getPinnedSavedDialogs:
		return reply(requestId, MTP_messages_savedDialogs(
			MTP_vector<MTPSavedDialog>(),
			MTP_vector<MTPMessage>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	case mtpc_messages_getSponsoredMessages:
		return reply(requestId, MTP_messages_sponsoredMessagesEmpty());
	case mtpc_account_getAuthorizations:
		return reply(requestId, MTP_account_authorizations(
			MTP_int(180),
			MTP_vector<MTPAuthorization>()));
	case mtpc_account_getWebAuthorizations:
		return reply(requestId, MTP_account_webAuthorizations(
			MTP_vector<MTPWebAuthorization>(),
			MTP_vector<MTPUser>()));
	}

	auto from = body.constData();
	auto dump = MTP::details::DumpToTextBuffer();
	[[maybe_unused]] const auto parsed = MTP::details::DumpToTextType(
		dump,
		from,
		body.constData() + body.size());
	const auto text = QString::fromUtf8(dump.p, dump.size);
	LOG(("Pararam: unsupported request %1"
		).arg(text.left(text.indexOf('\n')).left(200)));
	replyError(requestId, 400, u"PARARAM_NOT_SUPPORTED"_q);
}

void Backend::replyConfig(mtpRequestId requestId) {
	const auto now = Now();
	reply(requestId, MTP_config(
		MTP_flags(0),
		MTP_int(now),
		MTP_int(now + 3600),
		MTP_boolFalse(),
		MTP_int(kDcId),
		MTP_vector<MTPDcOption>(1, MTP_dcOption(
			MTP_flags(0),
			MTP_int(kDcId),
			MTP_string("127.0.0.1"),
			MTP_int(443),
			MTPbytes())),
		MTP_string(),
		MTP_int(200), // chat_size_max
		MTP_int(200000), // megagroup_size_max
		MTP_int(100), // forwarded_count_max
		MTP_int(120000), // online_update_period_ms
		MTP_int(5000), // offline_blur_timeout_ms
		MTP_int(30000), // offline_idle_timeout_ms
		MTP_int(300000), // online_cloud_timeout_ms
		MTP_int(30000), // notify_cloud_delay_ms
		MTP_int(1500), // notify_default_delay_ms
		MTP_int(60000), // push_chat_period_ms
		MTP_int(2), // push_chat_limit
		MTP_int(std::numeric_limits<int32>::max()), // edit_time_limit
		MTP_int(std::numeric_limits<int32>::max()), // revoke_time_limit
		MTP_int(std::numeric_limits<int32>::max()), // revoke_pm_time_limit
		MTP_int(2419200), // rating_e_decay
		MTP_int(30), // stickers_recent_limit
		MTP_int(604800), // channels_read_media_period
		MTPint(), // tmp_sessions
		MTP_int(20000), // call_receive_timeout_ms
		MTP_int(90000), // call_ring_timeout_ms
		MTP_int(30000), // call_connect_timeout_ms
		MTP_int(10000), // call_packet_timeout_ms
		MTP_string("https://app.pararam.io/"), // me_url_prefix
		MTPstring(), // autoupdate_url_prefix
		MTPstring(), // gif_search_username
		MTPstring(), // venue_search_username
		MTPstring(), // img_search_username
		MTPstring(), // static_maps_provider
		MTP_int(16384), // caption_length_max
		MTP_int(16384), // message_length_max
		MTP_int(kDcId), // webfile_dc_id
		MTPstring(), // suggested_lang_code
		MTPint(), // lang_pack_version
		MTPint(), // base_lang_pack_version
		MTPReaction(), // reactions_default
		MTPstring())); // autologin_token
}

void Backend::replyNearestDc(mtpRequestId requestId) {
	reply(requestId, MTP_nearestDc(
		MTP_string(),
		MTP_int(kDcId),
		MTP_int(kDcId)));
}

void Backend::replyAppConfig(mtpRequestId requestId) {
	const auto value = [](const QString &key, MTPJSONValue value) {
		return MTP_jsonObjectValue(MTP_string(key), std::move(value));
	};
	const auto boolean = [](bool value) {
		return MTP_jsonBool(value ? MTP_boolTrue() : MTP_boolFalse());
	};
	auto values = QVector<MTPJSONObjectValue>();
	values.push_back(value(u"stories_all_hidden"_q, boolean(true)));
	values.push_back(value(u"premium_purchase_blocked"_q, boolean(true)));
	values.push_back(value(
		u"stories_posting"_q,
		MTP_jsonString(MTP_string("disabled"))));
	// pararam.io allows one reaction per user from its own list.
	const auto number = [](int value) {
		return MTP_jsonNumber(MTP_double(value));
	};
	const auto count = int(ReactionKinds().size());
	// Who read: pararam.io keeps it for all posts of all chats.
	values.push_back(value(
		u"chat_read_mark_expire_period"_q,
		number(kReadMarkExpirePeriod)));
	values.push_back(value(
		u"pm_read_date_expire_period"_q,
		number(kReadMarkExpirePeriod)));
	values.push_back(value(
		u"chat_read_mark_size_threshold"_q,
		number(kReadMarkSizeThreshold)));
	values.push_back(value(u"reactions_user_max_default"_q, number(1)));
	values.push_back(value(u"reactions_user_max_premium"_q, number(1)));
	values.push_back(value(u"reactions_uniq_max"_q, number(count)));
	values.push_back(value(u"reactions_default"_q, MTP_jsonObject(
		MTP_vector<MTPJSONObjectValue>({
			value(u"_"_q, MTP_jsonString(MTP_string("reactionEmoji"))),
			value(u"emoticon"_q, MTP_jsonString(MTP_string(
				StandardReaction(
					QString::fromUtf16(ReactionKinds().front().emoji))))),
		}))));
	reply(requestId, MTP_help_appConfig(
		MTP_int(1),
		MTP_jsonObject(MTP_vector<MTPJSONObjectValue>(std::move(values)))));
}

void Backend::logOut(mtpRequestId requestId) {
	_api.logout([=] {
		resetState();
		_selfId = 0;
		_self = UserInfo();
		_users.clear();
		_slots.clear();
		_chatBySlot.clear();
		sessionChanged();
		reply(requestId, MTP_auth_loggedOut(MTP_flags(0), MTPbytes()));
	});
}

// Session data.

void Backend::ensureLoaded(Done done, Fail fail) {
	if (_loaded) {
		done();
		return;
	}
	_loadedWaiters.push_back(std::move(done));
	_loadFailWaiters.push_back(std::move(fail));
	if (_loading) {
		return;
	}
	_loading = true;
	const auto failed = [=](const HttpError &error) {
		_loading = false;
		auto waiters = base::take(_loadFailWaiters);
		_loadedWaiters.clear();
		for (const auto &waiter : waiters) {
			waiter(error);
		}
	};
	loadChatList([=] {
		_loading = false;
		_loaded = true;
		_loadFailWaiters.clear();
		auto waiters = base::take(_loadedWaiters);
		for (const auto &waiter : waiters) {
			waiter();
		}
		schedulePoll(kPollInterval);
		_calls->start();
		auto waiting = std::vector<int64>();
		for (const auto &[id, chat] : _chats) {
			if (waitsForRead(*chat)) {
				waiting.push_back(id);
			}
		}
		refreshReadOutboxes(std::move(waiting));
	}, failed);
}

void Backend::loadChatList(Done done, Fail fail) {
	_api.get(u"/core/chat/sync"_q, [=](const QJsonValue &result) {
		auto ids = JsonIds(result.toObject().value(u"chats"_q));
		if (ids.empty()) {
			ids = JsonIds(result);
		}
		loadChats(ids, [=] {
			auto chatIds = std::vector<int64>();
			for (const auto &[id, chat] : _chats) {
				chatIds.push_back(id);
			}
			loadLastPosts(chatIds, [=] {
				auto users = std::vector<int64>();
				auto files = std::vector<FileInfo>();
				for (const auto &[id, chat] : _chats) {
					if (chat->pmUserId) {
						users.push_back(chat->pmUserId);
					}
					if (const auto post = topPost(*chat)) {
						users.push_back(post->userId);
						const auto list = filesOf(*post);
						files.insert(files.end(), list.begin(), list.end());
					}
				}
				ensureImageSizes(files, [=] {
					ensureUsers(users, done);
				});
			});
		}, fail);
	}, fail);
}

void Backend::loadChats(
		const std::vector<int64> &ids,
		Done done,
		Fail fail) {
	if (ids.empty()) {
		done();
		return;
	}
	struct State {
		int left = 0;
		bool failed = false;
	};
	const auto state = std::make_shared<State>();
	auto batches = std::vector<std::vector<int64>>();
	for (auto i = 0; i < int(ids.size()); i += kChatsPerRequest) {
		const auto till = std::min(int(ids.size()), i + kChatsPerRequest);
		batches.emplace_back(ids.begin() + i, ids.begin() + till);
	}
	state->left = int(batches.size());
	for (const auto &batch : batches) {
		_api.get(u"/core/chat?ids="_q + JoinIds(batch), [=](
				const QJsonValue &result) {
			const auto object = result.toObject();
			const auto list = object.contains(u"chats"_q)
				? object.value(u"chats"_q).toArray()
				: result.toArray();
			for (const auto &entry : list) {
				const auto info = ChatInfo::Parse(entry.toObject());
				if (info.id && !info.e2e) {
					applyChat(info);
				}
			}
			if (!--state->left && !state->failed) {
				done();
			}
		}, [=](const HttpError &error) {
			if (!state->failed) {
				state->failed = true;
				fail(error);
			}
		});
	}
}

void Backend::loadLastPosts(const std::vector<int64> &chatIds, Done done) {
	struct State {
		std::vector<int64> queue;
		int running = 0;
		Done done;
	};
	const auto state = std::make_shared<State>();
	for (const auto id : chatIds) {
		if (const auto chat = chatById(id); chat && chat->info.postsCount > 0) {
			state->queue.push_back(id);
		}
	}
	state->done = std::move(done);
	const auto next = std::make_shared<Fn<void()>>();
	*next = [=] {
		while (state->running < kParallelRequests && !state->queue.empty()) {
			const auto id = state->queue.back();
			state->queue.pop_back();
			++state->running;
			const auto finished = [=] {
				--state->running;
				(*next)();
			};
			_api.get(
				u"/msg/post?chat_id=%1&range=-3x-1"_q.arg(id),
				[=](const QJsonValue &result) {
					if (const auto chat = chatById(id)) {
						for (const auto &post : ParsePosts(result)) {
							applyPost(*chat, post);
						}
					}
					finished();
				},
				[=](const HttpError &) { finished(); });
		}
		if (!state->running && state->queue.empty() && state->done) {
			base::take(state->done)();
			*next = nullptr;
		}
	};
	(*next)();
}

void Backend::ensureUsers(const std::vector<int64> &ids, Done done) {
	auto missing = std::vector<int64>();
	for (const auto id : Unique(ids)) {
		const auto i = _users.find(id);
		if ((i == end(_users) || !i->second.full)
			&& !_usersLoading.contains(id)
			&& !_usersFailed.contains(id)) {
			missing.push_back(id);
		}
	}
	if (missing.empty()) {
		done();
		return;
	}
	const auto left = std::make_shared<int>(0);
	for (auto i = 0; i < int(missing.size()); i += kUsersPerRequest) {
		const auto till = std::min(int(missing.size()), i + kUsersPerRequest);
		const auto batch = std::vector<int64>(
			missing.begin() + i,
			missing.begin() + till);
		for (const auto id : batch) {
			_usersLoading.emplace(id);
		}
		++*left;
		const auto finished = [=](bool failed) {
			for (const auto id : batch) {
				_usersLoading.remove(id);
				if (failed || !_users.contains(id) || !_users[id].full) {
					_usersFailed.emplace(id);
				}
			}
			if (!--*left) {
				done();
			}
		};
		_api.get(u"/user/list?ids="_q + JoinIds(batch), [=](
				const QJsonValue &result) {
			const auto object = result.toObject();
			const auto list = object.contains(u"users"_q)
				? object.value(u"users"_q).toArray()
				: result.toArray();
			for (const auto &entry : list) {
				auto user = UserInfo::Parse(entry.toObject());
				user.full = true;
				applyUser(user);
			}
			finished(false);
		}, [=](const HttpError &) {
			finished(true);
		});
	}
}

void Backend::applyUser(const UserInfo &user) {
	if (!user.id) {
		return;
	}
	auto &existing = _users[user.id];
	if (!existing.id) {
		existing = user;
	} else {
		existing.mergeFrom(user);
	}
	if (user.id == _selfId) {
		_self = existing;
	}
}

void Backend::applyChat(const ChatInfo &info) {
	auto &chat = _chats[info.id];
	const auto created = !chat;
	if (created) {
		chat = std::make_unique<Chat>();
		chat->fetchedPostNo = info.postsCount;
	}
	chat->info = info;
	chat->joined = true;
	if (info.pm) {
		chat->pmUserId = otherUser(info);
		chat->slot = assignSlot(info.id);
		_chatByUser[chat->pmUserId] = info.id;
	}
}

void Backend::applyPost(Chat &chat, const PostInfo &post) {
	if (post.metaUser.id) {
		applyUser(post.metaUser);
	}
	if (post.file.valid()) {
		[[maybe_unused]] const auto id = documentId(post.file);
	}
	chat.posts[post.postNo] = post;
	if (post.postNo > chat.info.postsCount) {
		chat.info.postsCount = post.postNo;
	}
}

void Backend::fetchPosts(
		int64 chatId,
		int from,
		int till,
		Done done,
		Fail fail) {
	from = std::max(from, 1);
	if (till < from) {
		done();
		return;
	}
	const auto left = std::make_shared<int>(0);
	const auto failed = std::make_shared<bool>(false);
	for (auto start = from; start <= till; start += kMaxPostsPerRequest) {
		const auto end = std::min(till, start + kMaxPostsPerRequest - 1);
		++*left;
		_api.get(
			u"/msg/post?chat_id=%1&range=%2x%3"_q.arg(chatId).arg(start).arg(end),
			[=](const QJsonValue &result) {
				if (const auto chat = chatById(chatId)) {
					for (const auto &post : ParsePosts(result)) {
						applyPost(*chat, post);
					}
				}
				if (!--*left && !*failed) {
					done();
				}
			},
			[=](const HttpError &error) {
				if (!*failed) {
					*failed = true;
					fail(error);
				}
			});
	}
}

// Ids.

auto Backend::chatById(int64 chatId) -> Chat* {
	const auto i = _chats.find(chatId);
	return (i != end(_chats)) ? i->second.get() : nullptr;
}

auto Backend::chatByUser(int64 userId) -> Chat* {
	const auto i = _chatByUser.find(userId);
	return (i != end(_chatByUser)) ? chatById(i->second) : nullptr;
}

auto Backend::chatByCommonMsgId(int32 msgId) -> Chat* {
	const auto slot = int(msgId >> kPmSlotShift);
	const auto i = _chatBySlot.find(slot);
	return (i != end(_chatBySlot)) ? chatById(i->second) : nullptr;
}

int64 Backend::userIdFromInputPeer(const MTPInputPeer &peer) const {
	return peer.match([&](const MTPDinputPeerUser &data) {
		return int64(data.vuser_id().v);
	}, [&](const MTPDinputPeerSelf &) {
		return _selfId;
	}, [&](const MTPDinputPeerUserFromMessage &data) {
		return int64(data.vuser_id().v);
	}, [](const auto &) {
		return int64(0);
	});
}

int64 Backend::userIdFromInputUser(const MTPInputUser &user) const {
	return user.match([&](const MTPDinputUser &data) {
		return int64(data.vuser_id().v);
	}, [&](const MTPDinputUserSelf &) {
		return _selfId;
	}, [&](const MTPDinputUserFromMessage &data) {
		return int64(data.vuser_id().v);
	}, [](const MTPDinputUserEmpty &) {
		return int64(0);
	});
}

auto Backend::chatByInputPeer(const MTPInputPeer &peer) -> Chat* {
	return peer.match([&](const MTPDinputPeerChannel &data) {
		return chatById(data.vchannel_id().v);
	}, [&](const MTPDinputPeerChannelFromMessage &data) {
		return chatById(data.vchannel_id().v);
	}, [&](const MTPDinputPeerChat &data) {
		return chatById(data.vchat_id().v);
	}, [&](const auto &) {
		const auto userId = userIdFromInputPeer(peer);
		return userId ? chatByUser(userId) : nullptr;
	});
}

auto Backend::chatByInputChannel(const MTPInputChannel &channel) -> Chat* {
	return channel.match([&](const MTPDinputChannel &data) {
		return chatById(data.vchannel_id().v);
	}, [&](const MTPDinputChannelFromMessage &data) {
		return chatById(data.vchannel_id().v);
	}, [](const MTPDinputChannelEmpty &) {
		return (Chat*)nullptr;
	});
}

int32 Backend::msgId(const Chat &chat, int postNo) const {
	if (!chat.info.pm) {
		return int32(postNo);
	}
	return int32((chat.slot << kPmSlotShift)
		| std::clamp(postNo, 0, kPmPostMask));
}

int Backend::postNo(const Chat &chat, int32 msgId) const {
	return chat.info.pm ? int(msgId & kPmPostMask) : int(msgId);
}

int Backend::assignSlot(int64 chatId) {
	if (const auto i = _slots.find(chatId); i != end(_slots)) {
		return i->second;
	}
	auto slot = 1;
	while (_chatBySlot.contains(slot) && slot <= kMaxSlots) {
		++slot;
	}
	if (slot > kMaxSlots) {
		// Should never happen, reuse a slot deterministically.
		slot = 1 + int(uint64(chatId) % kMaxSlots);
	}
	_slots[chatId] = slot;
	_chatBySlot[slot] = chatId;
	sessionChanged();
	return slot;
}

int64 Backend::otherUser(const ChatInfo &info) const {
	for (const auto id : info.users) {
		if (id != _selfId) {
			return id;
		}
	}
	if (info.authorId && info.authorId != _selfId) {
		return info.authorId;
	}
	return _selfId;
}

void Backend::withChatForPeer(
		const MTPInputPeer &peer,
		std::function<void(Chat*)> done,
		Fail fail,
		bool create) {
	if (const auto chat = chatByInputPeer(peer)) {
		done(chat);
		return;
	}
	const auto userId = userIdFromInputPeer(peer);
	if (!userId || !create) {
		done(nullptr);
		return;
	}
	_api.post(
		u"/core/chat/pm/%1"_q.arg(userId),
		QJsonObject(),
		[=](const QJsonValue &result) {
			const auto chatId = JsonInt(result.toObject(), u"chat_id"_q);
			if (!chatId) {
				fail(LocalError(u"PEER_ID_INVALID"_q));
				return;
			}
			loadChats({ chatId }, [=] {
				if (const auto chat = chatById(chatId)) {
					if (chat->info.pm && chat->pmUserId != userId) {
						chat->pmUserId = userId;
						_chatByUser[userId] = chatId;
					}
				}
				done(chatById(chatId));
			}, fail);
		},
		fail);
}

uint64 Backend::documentId(const FileInfo &file) {
	if (const auto i = _documentByGuid.find(file.guid)
		; i != end(_documentByGuid)) {
		auto &existing = _documents[i->second];
		if (existing.name.isEmpty()) {
			existing = file;
		}
		return i->second;
	}
	const auto hash = QCryptographicHash::hash(
		file.guid.toUtf8(),
		QCryptographicHash::Sha1);
	auto result = uint64();
	memcpy(&result, hash.constData(), sizeof(result));
	result &= 0x7FFFFFFFFFFFFFFFULL;
	_documentByGuid.emplace(file.guid, result);
	_documents.emplace(result, file);
	return result;
}

// Conversion.

MTPUser Backend::userToMTP(int64 userId) const {
	using Flag = MTPDuser::Flag;
	const auto i = _users.find(userId);
	auto user = (i != end(_users)) ? i->second : UserInfo();
	user.id = userId;
	if (user.name.isEmpty()) {
		user.name = user.uniqueName.isEmpty()
			? (u"User #"_q + QString::number(userId))
			: user.uniqueName;
	}
	const auto self = (userId == _selfId);
	auto flags = Flag::f_access_hash | Flag::f_status | Flag::f_photo;
	if (user.deleted) {
		flags |= Flag::f_deleted;
	} else {
		flags |= Flag::f_first_name;
		if (!user.uniqueName.isEmpty()) {
			flags |= Flag::f_username;
		}
	}
	if (self) {
		flags |= Flag::f_self;
	} else if (_chatByUser.contains(userId)) {
		flags |= Flag::f_contact | Flag::f_mutual_contact;
	}
	if (user.bot) {
		flags |= Flag::f_bot | Flag::f_bot_info_version | Flag::f_bot_nochats;
	}
	return MTP_user(
		MTP_flags(flags),
		MTP_long(userId),
		MTP_long(AccessHash(userId)),
		MTP_string(user.name),
		MTPstring(), // last_name
		MTP_string(user.uniqueName),
		MTPstring(), // phone
		(user.deleted
			? MTP_userProfilePhotoEmpty()
			: MTP_userProfilePhoto(
				MTP_flags(0),
				MTP_long(userAvatarId(userId)),
				MTPbytes(),
				MTP_int(kDcId))),
		(self
			? MTP_userStatusOnline(MTP_int(Now() + 300))
			: MTP_userStatusRecently(MTP_flags(0))),
		MTP_int(1), // bot_info_version
		MTPVector<MTPRestrictionReason>(),
		MTPstring(), // bot_inline_placeholder
		MTPstring(), // lang_code
		MTPEmojiStatus(),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(), // color
		MTPPeerColor(), // profile_color
		MTPint(), // bot_active_users
		MTPlong(), // bot_verification_icon
		MTPlong(), // send_paid_messages_stars
		MTPlong()); // linked_community_id
}

bool Backend::isAdmin(const Chat &chat) const {
	return (chat.info.authorId == _selfId)
		|| (std::find(
			chat.info.admins.begin(),
			chat.info.admins.end(),
			_selfId) != chat.info.admins.end());
}

MTPChat Backend::chatToMTP(const Chat &chat) const {
	using Flag = MTPDchannel::Flag;
	auto flags = Flag::f_access_hash
		| Flag::f_megagroup
		| Flag::f_participants_count
		| Flag::f_default_banned_rights;
	const auto admin = isAdmin(chat);
	if (_calls && _calls->groupCallActive(chat.info.id)) {
		flags |= Flag::f_call_active;
		if (_calls->groupCallNotEmpty(chat.info.id)) {
			flags |= Flag::f_call_not_empty;
		}
	}
	if (!chat.joined) {
		flags |= Flag::f_left;
	} else if (chat.info.authorId == _selfId) {
		flags |= Flag::f_creator | Flag::f_admin_rights;
	} else if (admin) {
		flags |= Flag::f_admin_rights;
	}
	const auto title = chat.info.displayTitle().isEmpty()
		? (u"Chat #"_q + QString::number(chat.info.id))
		: chat.info.displayTitle();
	return MTP_channel(
		MTP_flags(flags),
		MTP_long(chat.info.id),
		MTP_long(AccessHash(chat.info.id)),
		MTP_string(title),
		MTPstring(), // username
		MTP_chatPhoto(
			MTP_flags(0),
			MTP_long(chatAvatarId(chat)),
			MTPbytes(),
			MTP_int(kDcId)),
		MTP_int(chat.info.created ? chat.info.created : Now()),
		MTPVector<MTPRestrictionReason>(),
		(admin ? AdminRights() : MTPChatAdminRights()),
		MTPChatBannedRights(),
		DefaultBannedRights(chat.info.readOnly),
		MTP_int(std::max(int(chat.info.users.size()), 1)),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPEmojiStatus(),
		MTPint(), // level
		MTPint(), // subscription_until_date
		MTPlong(), // bot_verification_icon
		MTPlong(), // send_paid_messages_stars
		MTPlong(), // linked_monoforum_id
		MTPlong()); // linked_community_id
}

MTPPeer Backend::peerToMTP(const Chat &chat) const {
	return chat.info.pm
		? MTP_peerUser(MTP_long(chat.pmUserId))
		: MTP_peerChannel(MTP_long(chat.info.id));
}

MTPMessageMedia Backend::mediaToMTP(const FileInfo &original) {
	auto file = original;
	if (file.isImage() && (file.width <= 0 || file.height <= 0)) {
		const auto i = _imageSizes.find(file.guid);
		if (i != end(_imageSizes) && i->second.isValid()) {
			file.width = i->second.width();
			file.height = i->second.height();
		}
	}
	const auto id = documentId(file);
	const auto date = MTP_int(Now());
	const auto reference = MTP_bytes(file.guid.toUtf8());
	if (file.isImage() && file.width > 0 && file.height > 0) {
		// "m" is the preview resized by the file server, "y" the file.
		const auto preview = FitPreview(QSize(file.width, file.height));
		auto sizes = QVector<MTPPhotoSize>();
		sizes.push_back(MTP_photoSize(
			MTP_string("m"),
			MTP_int(preview.width()),
			MTP_int(preview.height()),
			MTP_int(0)));
		// The loader needs exact byte sizes, the converted one is unknown.
		sizes.push_back(MTP_photoSize(
			MTP_string("y"),
			MTP_int(file.width),
			MTP_int(file.height),
			MTP_int(file.needsConversion()
				? 0
				: int(std::min(
					file.size,
					int64(std::numeric_limits<int>::max()))))));
		return MTP_messageMediaPhoto(
			MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo),
			MTP_photo(
				MTP_flags(0),
				MTP_long(id),
				MTP_long(AccessHash(id)),
				reference,
				date,
				MTP_vector<MTPPhotoSize>(std::move(sizes)),
				MTPVector<MTPVideoSize>(),
				MTP_int(kDcId)),
			MTPint(),
			MTPDocument());
	}
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeFilename(MTP_string(
		file.name.isEmpty() ? u"file"_q : file.name)));
	if (file.width > 0 && file.height > 0) {
		attributes.push_back(MTP_documentAttributeImageSize(
			MTP_int(file.width),
			MTP_int(file.height)));
	}
	return MTP_messageMediaDocument(
		MTP_flags(MTPDmessageMediaDocument::Flag::f_document),
		MTP_document(
			MTP_flags(0),
			MTP_long(id),
			MTP_long(AccessHash(id)),
			reference,
			date,
			MTP_string(file.mimeType.isEmpty()
				? u"application/octet-stream"_q
				: file.mimeType),
			MTP_long(file.size),
			MTPVector<MTPPhotoSize>(),
			MTPVector<MTPVideoSize>(),
			MTP_int(kDcId),
			MTP_vector<MTPDocumentAttribute>(std::move(attributes))),
		MTPVector<MTPDocument>(),
		MTPPhoto(),
		MTPint(),
		MTPint());
}

MTPMessage Backend::messageToMTP(const Chat &chat, const PostInfo &post) {
	const auto id = MTP_int(msgId(chat, post.postNo));
	const auto out = (post.userId == _selfId);
	const auto from = MTP_peerUser(MTP_long(post.userId
		? post.userId
		: (chat.info.pm ? chat.pmUserId : _selfId)));
	const auto date = MTP_int(post.created ? post.created : Now());
	if (post.isService()) {
		using Flag = MTPDmessageService::Flag;
		return MTP_messageService(
			MTP_flags(Flag::f_from_id | (out ? Flag::f_out : Flag())),
			id,
			from,
			peerToMTP(chat),
			MTPPeer(),
			MTPMessageReplyHeader(),
			date,
			((chat.info.pm && IsCallEvent(post))
				? PhoneCallAction(post)
				: MTP_messageActionCustomAction(
					MTP_string(DescribeEvent(post)))),
			MTPMessageReactions(),
			MTPint());
	}
	using Flag = MTPDmessage::Flag;
	auto flags = Flag::f_from_id | (out ? Flag::f_out : Flag());
	auto formatted = ParseIncomingText(post.text, post.parsed);
	auto media = MTPMessageMedia();
	auto file = post.file;
	if (!file.valid()) {
		for (const auto &guid : post.attachments) {
			const auto i = _documentByGuid.find(guid);
			if (i != end(_documentByGuid)) {
				file = _documents[i->second];
				break;
			}
		}
	}
	if (file.valid()) {
		flags |= Flag::f_media;
		media = mediaToMTP(file);
	} else if (!post.attachments.empty()) {
		if (!formatted.text.isEmpty()) {
			formatted.text.append('\n');
		}
		formatted.text.append(post.attachments.size() == 1
			? u"[attachment]"_q
			: u"[%1 attachments]"_q.arg(post.attachments.size()));
	}
	auto reply = MTPMessageReplyHeader();
	if (post.replyNo > 0) {
		flags |= Flag::f_reply_to;
		reply = MTP_messageReplyHeader(
			MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id),
			MTP_int(msgId(chat, post.replyNo)),
			MTPPeer(),
			MTPMessageFwdHeader(),
			MTPMessageMedia(),
			MTPint(),
			MTPstring(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPbytes());
	}
	for (const auto &entity : formatted.entities) {
		if (entity.type == Entity::Type::MentionName
			&& entity.userId == _selfId
			&& !out) {
			flags |= Flag::f_mentioned;
		}
	}
	if (!formatted.entities.empty()) {
		flags |= Flag::f_entities;
	}
	if (post.edited && post.edited > post.created + 1) {
		flags |= Flag::f_edit_date;
	}
	if (!post.reactions.list.empty()) {
		flags |= Flag::f_reactions;
	}
	return MTP_message(
		MTP_flags(flags),
		id,
		from,
		MTPint(), // from_boosts_applied
		MTPstring(), // from_rank
		peerToMTP(chat),
		MTPPeer(), // saved_peer_id
		MTPMessageFwdHeader(),
		MTPlong(), // via_bot_id
		MTPlong(), // via_business_bot_id
		MTPPeer(), // guestchat_via_from
		reply,
		date,
		MTP_string(formatted.text),
		media,
		MTPReplyMarkup(),
		EntitiesToMTP(formatted.entities),
		MTPint(), // views
		MTPint(), // forwards
		MTPMessageReplies(),
		MTP_int(post.edited),
		MTPstring(), // post_author
		MTPlong(), // grouped_id
		(post.reactions.list.empty()
			? MTPMessageReactions()
			: reactionsToMTP(post)),
		MTPVector<MTPRestrictionReason>(),
		MTPint(), // ttl_period
		MTPint(), // quick_reply_shortcut_id
		MTPlong(), // effect
		MTPFactCheck(),
		MTPint(), // report_delivery_until_date
		MTPlong(), // paid_message_stars
		MTPSuggestedPost(),
		MTPint(), // schedule_repeat_period
		MTPstring(), // summary_from_language
		MTPRichMessage());
}

MTPPeerNotifySettings Backend::notifySettings(const Chat &chat) const {
	using Flag = MTPDpeerNotifySettings::Flag;
	const auto i = _localMute.find(chat.info.id);
	const auto muted = (i != end(_localMute)) ? i->second : chat.info.muted();
	return MTP_peerNotifySettings(
		MTP_flags(muted ? Flag::f_mute_until : Flag()),
		MTPBool(),
		MTPBool(),
		MTP_int(muted ? std::numeric_limits<int32>::max() : 0),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPBool(),
		MTPBool(),
		MTPNotificationSound(),
		MTPNotificationSound(),
		MTPNotificationSound());
}

const PostInfo *Backend::topPost(const Chat &chat) const {
	for (auto i = chat.posts.rbegin(); i != chat.posts.rend(); ++i) {
		if (!i->second.deleted) {
			return &i->second;
		}
	}
	return nullptr;
}

int Backend::unreadCount(const Chat &chat) const {
	const auto top = topPost(chat);
	if (top && top->userId == _selfId) {
		return 0;
	}
	return std::max(0, chat.info.postsCount - chat.info.lastReadPostNo);
}

MTPDialog Backend::dialogToMTP(const Chat &chat) const {
	using Flag = MTPDdialog::Flag;
	auto flags = MTPDdialog::Flags();
	if (!chat.info.pm) {
		flags |= Flag::f_pts;
	}
	if (chat.info.favorite) {
		flags |= Flag::f_pinned;
	}
	if (!chat.info.visible) {
		flags |= Flag::f_folder_id;
	}
	const auto top = topPost(chat);
	const auto unread = unreadCount(chat);
	const auto readInbox = unread
		? chat.info.lastReadPostNo
		: (top ? top->postNo : chat.info.lastReadPostNo);
	return MTP_dialog(
		MTP_flags(flags),
		peerToMTP(chat),
		MTP_int(top ? msgId(chat, top->postNo) : 0),
		MTP_int(msgId(chat, readInbox)),
		MTP_int(chat.readOutbox ? msgId(chat, chat.readOutbox) : 0),
		MTP_int(unread),
		MTP_int(0), // unread_mentions_count
		MTP_int(0), // unread_reactions_count
		MTP_int(0), // unread_poll_votes_count
		notifySettings(chat),
		MTP_int(chat.pts),
		MTPDraftMessage(),
		MTP_int(chat.info.visible ? 0 : kArchiveFolderId),
		MTPint());
}

MTPVector<MTPUser> Backend::usersToMTP(const std::vector<int64> &ids) const {
	auto result = QVector<MTPUser>();
	for (const auto id : Unique(ids)) {
		result.push_back(userToMTP(id));
	}
	return MTP_vector<MTPUser>(std::move(result));
}

MTPVector<MTPChat> Backend::chatsToMTP(const std::vector<int64> &ids) const {
	auto result = QVector<MTPChat>();
	for (const auto id : Unique(ids)) {
		const auto i = _chats.find(id);
		if (i != end(_chats) && !i->second->info.pm) {
			result.push_back(chatToMTP(*i->second));
		}
	}
	return MTP_vector<MTPChat>(std::move(result));
}

MTPupdates_State Backend::stateToMTP() const {
	return MTP_updates_state(
		MTP_int(_commonPts),
		MTP_int(0),
		MTP_int(Now()),
		MTP_int(0),
		MTP_int(0));
}

void Backend::collectPeers(
		const Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users) const {
	users.push_back(post.userId);
	if (chat.pmUserId) {
		users.push_back(chat.pmUserId);
	}
	auto reactors = 0;
	for (const auto &[emoji, list] : post.reactions.list) {
		for (const auto userId : list) {
			if (++reactors > kMaxRecentReactions) {
				return;
			}
			users.push_back(userId);
		}
	}
}

FormattedText Backend::textFromMTP(
		const QString &text,
		const MTPVector<MTPMessageEntity> *entities) const {
	auto result = FormattedText{ text };
	if (entities) {
		result.entities = EntitiesFromMTP(*entities);
	}
	return result;
}

void Backend::ensureImageSizes(std::vector<FileInfo> files, Done done) {
	auto wanted = std::vector<FileInfo>();
	for (const auto &file : files) {
		if (file.valid()
			&& file.isImage()
			&& (file.width <= 0 || file.height <= 0)
			&& !_imageSizes.contains(file.guid)
			&& std::none_of(wanted.begin(), wanted.end(), [&](
					const FileInfo &other) {
				return other.guid == file.guid;
			})) {
			wanted.push_back(file);
		}
	}
	if (wanted.empty()) {
		done();
		return;
	}
	struct State {
		int left = 0;
		Done done;
	};
	const auto state = std::make_shared<State>();
	state->left = int(wanted.size());
	state->done = std::move(done);
	for (const auto &file : wanted) {
		const auto guid = file.guid;
		_imageProbeWaiters[guid].push_back([=] {
			if (!--state->left && state->done) {
				base::take(state->done)();
			}
		});
		if (_imageProbeWaiters[guid].size() > 1) {
			continue; // Already probing.
		}
		const auto finish = [=](QSize size) {
			_imageSizes[guid] = size;
			auto waiters = _imageProbeWaiters.take(guid).value_or(
				std::vector<Done>());
			for (const auto &waiter : waiters) {
				waiter();
			}
		};
		const auto byPreview = [=] {
			probeImageByPreview(file, finish);
		};
		if (file.needsConversion() || file.size > kMaxProbedImageSize) {
			byPreview();
			continue;
		}
		_api.downloadHead(guid, file.name, kImageProbeBytes, [=](
				const QByteArray &bytes) {
			auto buffer = QBuffer();
			buffer.setData(bytes);
			buffer.open(QIODevice::ReadOnly);
			auto reader = QImageReader(&buffer);
			auto size = reader.size();
			if (size.isValid() && reader.transformation()
				& QImageIOHandler::TransformationRotate90) {
				size.transpose();
			}
			if (size.isValid() && !size.isEmpty()) {
				finish(size);
			} else {
				// Large metadata before the image header, use the preview.
				byPreview();
			}
		}, [=](const HttpError &) {
			byPreview();
		});
	}
}

void Backend::probeImageByPreview(
		const FileInfo &file,
		Fn<void(QSize)> done) {
	// The file server resizes images keeping the aspect ratio, the
	// preview is needed for the "m" photo size anyway.
	loadPath(FilePath(file, true), [=](const QByteArray &bytes) {
		auto buffer = QBuffer();
		buffer.setData(bytes);
		buffer.open(QIODevice::ReadOnly);
		auto reader = QImageReader(&buffer);
		reader.setAutoTransform(true);
		auto size = reader.size();
		if (size.isValid() && reader.transformation()
			& QImageIOHandler::TransformationRotate90) {
			size.transpose();
		}
		done((size.isValid() && !size.isEmpty()) ? size : QSize(-1, -1));
	});
}

QString Backend::FilePath(const FileInfo &file, bool preview) {
	// Same as the web client: '/', '?' and '&' are replaced in the name.
	auto name = file.name;
	if (name.isEmpty() || name == u"."_q || name == u".."_q) {
		name = u"file"_q;
	}
	name.replace(QRegularExpression(u"[/?&]"_q), u"-"_q);
	auto result = u"/download/"_q
		+ file.guid
		+ '/'
		+ QString::fromLatin1(QUrl::toPercentEncoding(name));
	if (preview) {
		result += u"?s=%1"_q.arg(kPreviewSide);
	} else if (file.needsConversion()) {
		result += u"?s=%1"_q.arg(kConvertedSide);
	}
	return result;
}

std::vector<FileInfo> Backend::filesOf(const PostInfo &post) const {
	auto result = std::vector<FileInfo>();
	if (post.file.valid()) {
		result.push_back(post.file);
	}
	for (const auto &guid : post.attachments) {
		const auto i = _documentByGuid.find(guid);
		if (i != end(_documentByGuid)) {
			const auto j = _documents.find(i->second);
			if (j != end(_documents)) {
				result.push_back(j->second);
			}
		}
	}
	return result;
}

// Avatars.

uint64 Backend::userAvatarId(int64 userId) const {
	// The web client shows /avatar/user/<id> for every user, the
	// server keeps a default picture for users without an avatar.
	const auto i = _users.find(userId);
	const auto stamp = (i != end(_users))
		? (i->second.updated
			? QString::number(i->second.updated)
			: i->second.avatar)
		: QString();
	return HashId(u"user:%1:%2"_q.arg(userId).arg(stamp));
}

uint64 Backend::chatAvatarId(const Chat &chat) const {
	// time_updated changes with every post, time_edited with the chat.
	const auto stamp = chat.info.edited ? chat.info.edited : chat.info.created;
	return HashId(u"chat:%1:%2"_q.arg(chat.info.id).arg(stamp));
}

QString Backend::userAvatarPath(int64 userId, bool big) const {
	const auto i = _users.find(userId);
	const auto stamp = (i != end(_users)) ? i->second.updated : TimeId();
	return u"/avatar/user/%1/%2?x=%3"_q
		.arg(userId)
		.arg(big ? kAvatarLarge : kAvatarSmall)
		.arg(qint64(stamp) * 1000);
}

QString Backend::chatAvatarPath(const Chat &chat, bool big) const {
	const auto stamp = chat.info.edited ? chat.info.edited : chat.info.created;
	return u"/avatar/chat/%1/%2?x=%3"_q
		.arg(chat.info.id)
		.arg(big ? kAvatarLarge : kAvatarSmall)
		.arg(qint64(stamp) * 1000);
}

MTPPhoto Backend::avatarPhoto(uint64 photoId, const QString &key) {
	_avatarPaths[photoId] = key;
	auto sizes = QVector<MTPPhotoSize>();
	sizes.push_back(MTP_photoSize(
		MTP_string("a"),
		MTP_int(kAvatarSmall),
		MTP_int(kAvatarSmall),
		MTP_int(0)));
	sizes.push_back(MTP_photoSize(
		MTP_string("c"),
		MTP_int(kAvatarLarge),
		MTP_int(kAvatarLarge),
		MTP_int(0)));
	return MTP_photo(
		MTP_flags(0),
		MTP_long(photoId),
		MTP_long(AccessHash(int64(photoId))),
		MTP_bytes(key.toUtf8()),
		MTP_int(Now()),
		MTP_vector<MTPPhotoSize>(std::move(sizes)),
		MTPVector<MTPVideoSize>(),
		MTP_int(kDcId));
}

// Reactions.

MTPMessageReactions Backend::reactionsToMTP(const PostInfo &post) const {
	using Flag = MTPDmessageReactions::Flag;
	const auto mine = post.reactions.of(_selfId);
	auto results = QVector<MTPReactionCount>();
	auto recent = QVector<MTPMessagePeerReaction>();
	for (const auto &[emoji, users] : post.reactions.list) {
		using CountFlag = MTPDreactionCount::Flag;
		const auto chosen = (emoji == mine);
		results.push_back(MTP_reactionCount(
			MTP_flags(chosen ? CountFlag::f_chosen_order : CountFlag()),
			chosen ? MTP_int(0) : MTPint(),
			ReactionToMTP(emoji),
			MTP_int(int(users.size()))));
		for (const auto userId : users) {
			if (recent.size() >= kMaxRecentReactions) {
				break;
			}
			using PeerFlag = MTPDmessagePeerReaction::Flag;
			recent.push_back(MTP_messagePeerReaction(
				MTP_flags((userId == _selfId)
					? PeerFlag::f_my
					: PeerFlag()),
				MTP_peerUser(MTP_long(userId)),
				MTP_int(post.created ? post.created : Now()),
				ReactionToMTP(emoji)));
		}
	}
	return MTP_messageReactions(
		MTP_flags(Flag::f_can_see_list
			| (recent.isEmpty() ? Flag() : Flag::f_recent_reactions)),
		MTP_vector<MTPReactionCount>(std::move(results)),
		MTP_vector<MTPMessagePeerReaction>(std::move(recent)),
		MTPVector<MTPMessageReactor>());
}

MTPUpdate Backend::reactionsUpdate(
		const Chat &chat,
		const PostInfo &post) const {
	return MTP_updateMessageReactions(
		MTP_flags(0),
		peerToMTP(chat),
		MTP_int(msgId(chat, post.postNo)),
		MTPint(), // top_msg_id
		MTPPeer(), // saved_peer_id
		reactionsToMTP(post));
}

MTPDocument Backend::reactionDocument(int index) {
	const auto &kinds = ReactionKinds();
	if (_generatedFiles.empty()) {
		auto emoji = std::vector<QString>();
		for (const auto &kind : kinds) {
			emoji.push_back(
				StandardReaction(QString::fromUtf16(kind.emoji)));
		}
		const auto icons = RenderReactionIcons(emoji);
		for (auto i = 0, count = int(icons.size()); i != count; ++i) {
			_generatedFiles[kReactionDocumentBase + i] = icons[i];
		}
	}
	const auto id = kReactionDocumentBase + index;
	const auto &bytes = _generatedFiles[id];
	auto attributes = QVector<MTPDocumentAttribute>();
	attributes.push_back(MTP_documentAttributeImageSize(
		MTP_int(kReactionIconSize),
		MTP_int(kReactionIconSize)));
	attributes.push_back(MTP_documentAttributeSticker(
		MTP_flags(0),
		MTP_string(
			StandardReaction(QString::fromUtf16(kinds[index].emoji))),
		MTP_inputStickerSetEmpty(),
		MTPMaskCoords()));
	attributes.push_back(MTP_documentAttributeFilename(
		MTP_string(u"reaction.webp"_q)));
	return MTP_document(
		MTP_flags(0),
		MTP_long(id),
		MTP_long(AccessHash(int64(id))),
		MTP_bytes(QByteArray::number(qulonglong(id))),
		MTP_int(1),
		MTP_string("image/webp"),
		MTP_long(bytes.size()),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(kDcId),
		MTP_vector<MTPDocumentAttribute>(std::move(attributes)));
}

MTPmessages_AvailableReactions Backend::availableReactions() {
	const auto &kinds = ReactionKinds();
	auto list = QVector<MTPAvailableReaction>();
	for (auto i = 0, count = int(kinds.size()); i != count; ++i) {
		const auto document = reactionDocument(i);
		if (_generatedFiles[kReactionDocumentBase + i].isEmpty()) {
			continue; // Emoji unknown to this emoji set.
		}
		list.push_back(MTP_availableReaction(
			MTP_flags(0),
			MTP_string(
				StandardReaction(QString::fromUtf16(kinds[i].emoji))),
			MTP_string(QString::fromUtf8(kinds[i].title)),
			document, // static_icon
			document, // appear_animation
			document, // select_animation
			document, // activate_animation
			document, // effect_animation
			MTPDocument(), // around_animation
			MTPDocument())); // center_icon
	}
	return MTP_messages_availableReactions(
		MTP_int(1),
		MTP_vector<MTPAvailableReaction>(std::move(list)));
}

MTPmessages_Reactions Backend::reactionsList() const {
	auto list = QVector<MTPReaction>();
	for (const auto &kind : ReactionKinds()) {
		list.push_back(ReactionToMTP(QString::fromUtf16(kind.emoji)));
	}
	return MTP_messages_reactions(
		MTP_long(1),
		MTP_vector<MTPReaction>(std::move(list)));
}

void Backend::collectChanges(
		Chat &chat,
		const base::flat_map<int, PostInfo> &old,
		QVector<MTPUpdate> &updates,
		std::vector<int64> &users,
		bool allReactions) {
	for (const auto &[no, post] : old) {
		const auto i = chat.posts.find(no);
		if (i == end(chat.posts) || i->second.deleted || post.deleted) {
			continue;
		}
		const auto edited = !i->second.sameContent(post);
		if (edited) {
			updates.push_back(editMessageUpdate(chat, i->second, users));
		}
		if (allReactions
			|| (!edited && !(i->second.reactions == post.reactions))) {
			collectPeers(chat, i->second, users);
			updates.push_back(reactionsUpdate(chat, i->second));
		}
	}
}

// Updates.

void Backend::logUpdate(
		Chat *chat,
		MTPUpdate update,
		int32 pts,
		int32 ptsCount,
		std::vector<int64> users) {
	auto &log = chat ? chat->log : _commonLog;
	log.push_back({
		.pts = pts,
		.ptsCount = ptsCount,
		.update = std::move(update),
		.users = std::move(users),
		.chats = (chat && !chat->info.pm)
			? std::vector<int64>{ chat->info.id }
			: std::vector<int64>(),
	});
	while (log.size() > kLogLimit) {
		log.pop_front();
	}
}

MTPUpdate Backend::newMessageUpdate(
		Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users) {
	collectPeers(chat, post, users);
	const auto message = messageToMTP(chat, post);
	auto peers = std::vector<int64>{ post.userId, chat.pmUserId };
	if (chat.info.pm) {
		const auto pts = ++_commonPts;
		auto update = MTP_updateNewMessage(message, MTP_int(pts), MTP_int(1));
		logUpdate(nullptr, update, pts, 1, std::move(peers));
		return update;
	}
	const auto pts = ++chat.pts;
	auto update = MTP_updateNewChannelMessage(
		message,
		MTP_int(pts),
		MTP_int(1));
	logUpdate(&chat, update, pts, 1, std::move(peers));
	return update;
}

MTPUpdate Backend::editMessageUpdate(
		Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users) {
	collectPeers(chat, post, users);
	const auto message = messageToMTP(chat, post);
	auto peers = std::vector<int64>{ post.userId, chat.pmUserId };
	if (chat.info.pm) {
		const auto pts = ++_commonPts;
		auto update = MTP_updateEditMessage(message, MTP_int(pts), MTP_int(1));
		logUpdate(nullptr, update, pts, 1, std::move(peers));
		return update;
	}
	const auto pts = ++chat.pts;
	auto update = MTP_updateEditChannelMessage(
		message,
		MTP_int(pts),
		MTP_int(1));
	logUpdate(&chat, update, pts, 1, std::move(peers));
	return update;
}

MTPUpdate Backend::deleteMessagesUpdate(
		Chat &chat,
		const std::vector<int> &postNos) {
	auto ids = QVector<MTPint>();
	for (const auto postNo : postNos) {
		ids.push_back(MTP_int(msgId(chat, postNo)));
		if (const auto i = chat.posts.find(postNo); i != end(chat.posts)) {
			i->second.deleted = true;
		}
	}
	if (chat.info.pm) {
		const auto pts = ++_commonPts;
		auto update = MTP_updateDeleteMessages(
			MTP_vector<MTPint>(std::move(ids)),
			MTP_int(pts),
			MTP_int(1));
		logUpdate(nullptr, update, pts, 1, {});
		return update;
	}
	const auto pts = ++chat.pts;
	auto update = MTP_updateDeleteChannelMessages(
		MTP_long(chat.info.id),
		MTP_vector<MTPint>(std::move(ids)),
		MTP_int(pts),
		MTP_int(1));
	logUpdate(&chat, update, pts, 1, {});
	return update;
}

MTPUpdates Backend::wrapUpdates(
		QVector<MTPUpdate> updates,
		std::vector<int64> users,
		const std::vector<int64> &chats) const {
	users.push_back(_selfId);
	return MTP_updates(
		MTP_vector<MTPUpdate>(std::move(updates)),
		usersToMTP(users),
		chatsToMTP(chats),
		MTP_int(Now()),
		MTP_int(0));
}

void Backend::pushReadUpdate(Chat &chat) {
	const auto unread = unreadCount(chat);
	const auto maxId = MTP_int(msgId(chat, chat.info.lastReadPostNo));
	auto users = std::vector<int64>{ chat.pmUserId };
	if (chat.info.pm) {
		const auto pts = ++_commonPts;
		auto update = MTP_updateReadHistoryInbox(
			MTP_flags(0),
			MTPint(),
			peerToMTP(chat),
			MTPint(),
			maxId,
			MTP_int(unread),
			MTP_int(pts),
			MTP_int(1));
		logUpdate(nullptr, update, pts, 1, users);
		pushUpdates(wrapUpdates({ update }, users, {}));
		return;
	}
	pushUpdates(wrapUpdates({ MTP_updateReadChannelInbox(
		MTP_flags(0),
		MTPint(),
		MTP_long(chat.info.id),
		maxId,
		MTP_int(unread),
		MTP_int(chat.pts)) }, users, { chat.info.id }));
}

// Polling.

void Backend::schedulePoll(crl::time delay) {
	if (!_pollTimer.isActive() || _pollTimer.remainingTime() > delay) {
		_pollTimer.start(int(delay));
	}
}

void Backend::pollFinished(crl::time delay) {
	_polling = false;
	schedulePoll(delay);
}

void Backend::poll() {
	if (!authorized() || !_loaded || _polling) {
		return;
	}
	const auto now = crl::now();
	if (_lastPoll && now - _lastPoll < kMinPollInterval) {
		schedulePoll(kMinPollInterval - (now - _lastPoll));
		return;
	}
	_polling = true;
	_lastPoll = now;
	if (_syncBroken) {
		pollByChats();
		return;
	}
	auto ids = QStringList();
	for (const auto &[id, chat] : _chats) {
		if (!chat->joined) {
			continue;
		}
		ids.push_back(u"%1-%2-%3"_q
			.arg(id)
			.arg(chat->info.postsCount)
			.arg(chat->info.lastReadPostNo));
	}
	auto body = QJsonObject();
	body.insert(u"ids"_q, ids.join('/'));
	if (!_syncTime.isEmpty()) {
		body.insert(u"sync_time"_q, _syncTime);
	}
	const auto started = FormatSyncTime(Now() - 5);
	_api.post(u"/core/chat/sync"_q, body, [=](const QJsonValue &result) {
		_syncTime = started;
		applySync(result.toObject());
		pollFinished(kPollInterval);
	}, [=](const HttpError &error) {
		if (error.unauthorized()) {
			LOG(("Pararam: session expired while polling."));
			_api.clear();
			_polling = false;
			return;
		} else if (error.status >= 400 && error.status < 500) {
			LOG(("Pararam: chat sync failed (%1), using fallback polling."
				).arg(error.status));
			_syncBroken = true;
		}
		pollFinished(kPollRetryInterval);
	});
}

void Backend::pollByChats() {
	struct Known {
		int postsCount = 0;
		int lastRead = 0;
	};
	auto known = base::flat_map<int64, Known>();
	for (const auto &[id, chat] : _chats) {
		if (chat->joined) {
			known.emplace(id, Known{
				chat->info.postsCount,
				chat->info.lastReadPostNo,
			});
		}
	}
	_api.get(u"/core/chat/sync"_q, [=](const QJsonValue &result) {
		auto ids = JsonIds(result.toObject().value(u"chats"_q));
		if (ids.empty()) {
			ids = JsonIds(result);
		}
		auto fresh = std::vector<int64>();
		for (const auto id : ids) {
			if (!known.contains(id)) {
				fresh.push_back(id);
			}
		}
		loadChats(ids, [=] {
			for (const auto &[id, was] : known) {
				if (const auto chat = chatById(id)) {
					const auto now = Known{
						chat->info.postsCount,
						chat->info.lastReadPostNo,
					};
					chat->info.postsCount = was.postsCount;
					chat->info.lastReadPostNo = was.lastRead;
					refreshChat(id, now.postsCount, now.lastRead);
				}
			}
			for (const auto id : fresh) {
				introduceChat(id);
			}
			pollFinished(kPollInterval);
		}, [=](const HttpError &) {
			pollFinished(kPollRetryInterval);
		});
	}, [=](const HttpError &error) {
		if (error.unauthorized()) {
			_api.clear();
			_polling = false;
			return;
		}
		pollFinished(kPollRetryInterval);
	});
}

void Backend::applySync(const QJsonObject &result) {
	const auto status = result.value(u"status"_q);
	if (status.isObject()) {
		const auto object = status.toObject();
		for (auto i = object.begin(); i != object.end(); ++i) {
			const auto chatId = i.key().toLongLong();
			const auto data = i.value().toObject();
			if (!chatId) {
				continue;
			}
			refreshChat(
				chatId,
				int(JsonInt(data, u"posts_count"_q, -1)),
				int(JsonInt(data, u"last_read_post_no"_q, -1)));
		}
	} else if (status.isArray()) {
		for (const auto &entry : status.toArray()) {
			const auto data = entry.toObject();
			const auto chatId = JsonInt(data, u"chat_id"_q, JsonInt(data, u"id"_q));
			if (chatId) {
				refreshChat(
					chatId,
					int(JsonInt(data, u"posts_count"_q, -1)),
					int(JsonInt(data, u"last_read_post_no"_q, -1)));
			}
		}
	}
	auto fresh = std::vector<int64>();
	for (const auto id : JsonIds(result.value(u"new"_q))) {
		if (!chatById(id)) {
			fresh.push_back(id);
		}
	}
	if (!fresh.empty()) {
		loadChats(fresh, [=] {
			for (const auto id : fresh) {
				introduceChat(id);
			}
		}, [](const HttpError &) {});
	}
	auto updated = std::vector<int64>();
	for (const auto id : JsonIds(result.value(u"updated"_q))) {
		if (chatById(id)) {
			updated.push_back(id);
		}
	}
	if (!updated.empty()) {
		struct Was {
			QString title;
			bool favorite = false;
			bool visible = true;
			int postsCount = 0;
			int lastRead = 0;
		};
		auto was = base::flat_map<int64, Was>();
		for (const auto id : updated) {
			const auto chat = chatById(id);
			was.emplace(id, Was{
				chat->info.displayTitle(),
				chat->info.favorite,
				chat->info.visible,
				chat->info.postsCount,
				chat->info.lastReadPostNo,
			});
		}
		loadChats(updated, [=] {
			auto channels = QVector<MTPUpdate>();
			auto chats = std::vector<int64>();
			for (const auto &[id, old] : was) {
				const auto chat = chatById(id);
				if (!chat) {
					continue;
				}
				const auto postsCount = chat->info.postsCount;
				const auto lastRead = chat->info.lastReadPostNo;
				chat->info.postsCount = old.postsCount;
				chat->info.lastReadPostNo = old.lastRead;
				if (!chat->info.pm
					&& old.title != chat->info.displayTitle()) {
					channels.push_back(MTP_updateChannel(MTP_long(id)));
					chats.push_back(id);
				}
				if (old.favorite != chat->info.favorite) {
					channels.push_back(MTP_updateDialogPinned(
						MTP_flags(chat->info.favorite
							? MTPDupdateDialogPinned::Flag::f_pinned
							: MTPDupdateDialogPinned::Flag()),
						MTPint(),
						MTP_dialogPeer(peerToMTP(*chat))));
					if (!chat->info.pm) {
						chats.push_back(id);
					}
				}
				if (postsCount == old.postsCount && lastRead == old.lastRead) {
					checkEdits(id);
				} else {
					refreshChat(id, postsCount, lastRead);
				}
				if (waitsForRead(*chat)) {
					refreshReadOutbox(id);
				}
			}
			if (!channels.isEmpty()) {
				pushUpdates(wrapUpdates(channels, {}, chats));
			}
		}, [](const HttpError &) {});
	}
	auto removed = QVector<MTPUpdate>();
	auto removedChats = std::vector<int64>();
	for (const auto id : JsonIds(result.value(u"removed"_q))) {
		if (const auto chat = chatById(id); chat && !chat->info.pm) {
			chat->joined = false;
			removed.push_back(MTP_updateChannel(MTP_long(id)));
			removedChats.push_back(id);
		}
	}
	if (!removed.isEmpty()) {
		pushUpdates(wrapUpdates(removed, {}, removedChats));
	}
}

void Backend::introduceChat(int64 chatId) {
	const auto chat = chatById(chatId);
	if (!chat) {
		return;
	}
	// Deliver the last post so that the new chat appears in the list.
	chat->fetchedPostNo = std::max(0, chat->info.postsCount - 1);
	if (!chat->info.pm) {
		pushUpdates(wrapUpdates(
			{ MTP_updateChannel(MTP_long(chatId)) },
			{},
			{ chatId }));
	}
	deliverNewPosts(chatId, chat->info.postsCount);
}

void Backend::refreshChat(int64 chatId, int postsCount, int lastRead) {
	const auto chat = chatById(chatId);
	if (!chat) {
		loadChats({ chatId }, [=] {
			introduceChat(chatId);
		}, [](const HttpError &) {});
		return;
	}
	if (postsCount > chat->info.postsCount) {
		chat->info.postsCount = postsCount;
	}
	if (lastRead >= 0 && lastRead != chat->info.lastReadPostNo) {
		chat->info.lastReadPostNo = lastRead;
		pushReadUpdate(*chat);
	}
	if (chat->info.postsCount > chat->fetchedPostNo) {
		deliverNewPosts(chatId, chat->info.postsCount);
	}
}

void Backend::deliverNewPosts(int64 chatId, int till) {
	const auto chat = chatById(chatId);
	if (!chat || till <= chat->fetchedPostNo) {
		return;
	} else if (chat->sending > 0) {
		chat->refreshAfterSend = true;
		return;
	} else if (chat->delivering) {
		chat->deliverAgain = true;
		return;
	}
	const auto from = chat->fetchedPostNo + 1;
	chat->delivering = true;
	const auto finish = [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			return;
		}
		chat->delivering = false;
		if (base::take(chat->deliverAgain)) {
			deliverNewPosts(chatId, chat->info.postsCount);
		}
	};
	fetchPosts(chatId, from, till, [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			return;
		}
		auto users = std::vector<int64>();
		auto files = std::vector<FileInfo>();
		for (auto no = from; no <= till; ++no) {
			const auto i = chat->posts.find(no);
			if (i != end(chat->posts)) {
				collectPeers(*chat, i->second, users);
				const auto list = filesOf(i->second);
				files.insert(files.end(), list.begin(), list.end());
			}
		}
		ensureImageSizes(files, [=] { ensureUsers(users, [=] {
			const auto chat = chatById(chatId);
			if (!chat) {
				return;
			}
			auto updates = QVector<MTPUpdate>();
			auto users = std::vector<int64>();
			for (auto no = from; no <= till; ++no) {
				if (chat->sentByUs.remove(no)) {
					continue;
				}
				const auto i = chat->posts.find(no);
				if (i == end(chat->posts) || i->second.deleted) {
					continue;
				}
				if (_sendingUuids.contains(i->second.uuid)) {
					chat->sentByUs.emplace(no);
					continue;
				}
				updates.push_back(newMessageUpdate(*chat, i->second, users));
			}
			chat->fetchedPostNo = std::max(chat->fetchedPostNo, till);
			if (!updates.isEmpty()) {
				pushUpdates(wrapUpdates(updates, users, { chatId }));
			}
			finish();
		}); });
	}, [=](const HttpError &) {
		finish();
	});
}

void Backend::checkEdits(int64 chatId) {
	const auto chat = chatById(chatId);
	if (!chat || chat->info.postsCount <= 0 || chat->checkingEdits) {
		return;
	}
	const auto till = std::min(chat->info.postsCount, chat->fetchedPostNo);
	const auto from = std::max(1, till - kEditCheckDepth + 1);
	auto old = base::flat_map<int, PostInfo>();
	for (auto i = chat->posts.lower_bound(from)
		; i != end(chat->posts) && i->first <= till
		; ) {
		old.emplace(i->first, i->second);
		i = chat->posts.erase(i);
	}
	if (old.empty()) {
		return;
	}
	chat->checkingEdits = true;
	fetchPosts(chatId, from, till, [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			return;
		}
		chat->checkingEdits = false;
		auto returned = false;
		for (const auto &[no, post] : old) {
			if (chat->posts.contains(no)) {
				returned = true;
				break;
			}
		}
		auto deleted = std::vector<int>();
		auto updates = QVector<MTPUpdate>();
		auto users = std::vector<int64>();
		for (const auto &[no, post] : old) {
			const auto i = chat->posts.find(no);
			if (i == end(chat->posts)) {
				if (returned && !post.deleted) {
					deleted.push_back(no);
				}
				chat->posts.emplace(no, post);
				chat->posts[no].deleted = returned || post.deleted;
				continue;
			}
			if (i->second.deleted && !post.deleted) {
				deleted.push_back(no);
			}
		}
		collectChanges(*chat, old, updates, users);
		if (!deleted.empty()) {
			updates.push_back(deleteMessagesUpdate(*chat, deleted));
		}
		if (!updates.isEmpty()) {
			ensureUsers(users, [=] {
				pushUpdates(wrapUpdates(updates, users, { chatId }));
			});
		}
	}, [=](const HttpError &) {
		if (const auto chat = chatById(chatId)) {
			chat->checkingEdits = false;
			for (const auto &[no, post] : old) {
				chat->posts.emplace(no, post);
			}
		}
	});
}

void Backend::postEdited(int64 chatId, int postNo) {
	const auto chat = chatById(chatId);
	if (!chat || postNo <= 0 || !chat->posts.contains(postNo)) {
		return;
	}
	auto old = base::flat_map<int, PostInfo>();
	old.emplace(postNo, chat->posts[postNo]);
	const auto finish = [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			return;
		}
		auto updates = QVector<MTPUpdate>();
		auto users = std::vector<int64>();
		collectChanges(*chat, old, updates, users);
		if (updates.isEmpty()) {
			return;
		}
		const auto chats = chat->info.pm
			? std::vector<int64>()
			: std::vector{ chatId };
		ensureUsers(users, [=] {
			pushUpdates(wrapUpdates(updates, users, chats));
		});
	};
	fetchPosts(chatId, postNo, postNo, finish, [](const HttpError &) {});
}

bool Backend::waitsForRead(const Chat &chat) const {
	// Only the own posts show read marks.
	auto checked = 0;
	for (auto i = chat.posts.rbegin()
		; i != chat.posts.rend() && checked < 50
		; ++i, ++checked) {
		if (i->first <= chat.readOutbox) {
			return false;
		} else if (!i->second.deleted
			&& !i->second.isService()
			&& i->second.userId == _selfId) {
			return true;
		}
	}
	return false;
}

void Backend::refreshReadOutbox(int64 chatId, bool force, Done done) {
	const auto chat = chatById(chatId);
	const auto now = crl::now();
	if (!chat
		|| chat->readOutboxLoading
		|| (!force
			&& chat->readOutboxChecked
			&& now - chat->readOutboxChecked < kReadOutboxCheckInterval)) {
		if (done) {
			done();
		}
		return;
	}
	chat->readOutboxLoading = true;
	chat->readOutboxChecked = now;
	const auto finish = [=](int postNo) {
		if (const auto chat = chatById(chatId)) {
			chat->readOutboxLoading = false;
			if (postNo > 0) {
				applyReadOutbox(chatId, postNo);
			}
		}
		if (done) {
			done();
		}
	};
	// { "<user id>": <last read post_no>, ... }
	_api.get(u"/msg/lastread/%1"_q.arg(chatId), [=](const QJsonValue &v) {
		const auto chat = chatById(chatId);
		auto result = 0;
		const auto object = v.toObject();
		for (auto i = object.begin(); chat && i != object.end(); ++i) {
			const auto userId = i.key().toLongLong();
			if (userId
				&& userId != _selfId
				&& (!chat->info.pm || userId == chat->pmUserId)) {
				result = std::max(result, int(JsonInt(i.value())));
			}
		}
		finish(result);
	}, [=](const HttpError &) {
		finish(0);
	});
}

void Backend::refreshReadOutboxes(std::vector<int64> chatIds) {
	struct State {
		std::vector<int64> queue;
		int running = 0;
	};
	const auto state = std::make_shared<State>();
	state->queue = std::move(chatIds);
	const auto next = std::make_shared<Fn<void()>>();
	*next = [=] {
		while (state->running < kParallelRequests
			&& !state->queue.empty()) {
			const auto id = state->queue.back();
			state->queue.pop_back();
			const auto chat = chatById(id);
			if (!chat || chat->readOutboxLoading) {
				continue; // Would finish right away, inside this loop.
			}
			++state->running;
			refreshReadOutbox(id, true, [=] {
				--state->running;
				(*next)();
			});
		}
		if (!state->running && state->queue.empty()) {
			*next = nullptr;
		}
	};
	(*next)();
}

void Backend::applyReadOutbox(int64 chatId, int postNo) {
	const auto chat = chatById(chatId);
	if (!chat) {
		return;
	}
	postNo = std::min(postNo, chat->info.postsCount);
	if (postNo <= chat->readOutbox) {
		return;
	}
	chat->readOutbox = postNo;
	const auto maxId = MTP_int(msgId(*chat, postNo));
	if (chat->info.pm) {
		const auto pts = ++_commonPts;
		auto update = MTP_updateReadHistoryOutbox(
			peerToMTP(*chat),
			maxId,
			MTP_int(pts),
			MTP_int(1));
		logUpdate(nullptr, update, pts, 1, { chat->pmUserId });
		pushUpdates(wrapUpdates({ update }, { chat->pmUserId }, {}));
	} else {
		pushUpdates(wrapUpdates(
			{ MTP_updateReadChannelOutbox(MTP_long(chatId), maxId) },
			{},
			{ chatId }));
	}
}

// Handlers.

void Backend::updateNotifySettings(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputNotifyPeer>();
	const auto settings = reader.read<MTPInputPeerNotifySettings>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	auto chat = (Chat*)nullptr;
	peer.match([&](const MTPDinputNotifyPeer &data) {
		chat = chatByInputPeer(data.vpeer());
	}, [](const auto &) {
	});
	if (chat) {
		const auto &data = settings.data();
		if (const auto until = data.vmute_until()) {
			// Kept locally: pararam.io mute settings are per account
			// on the server and are not exposed in a documented way.
			_localMute[chat->info.id] = (until->v > Now());
			sessionChanged();
		}
	}
	replyTrue(requestId);
}

void Backend::getState(mtpRequestId requestId) {
	reply(requestId, stateToMTP());
	ensureLoaded([] {}, [](const HttpError &) {});
}

void Backend::getDifference(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto pts = reader.int32Value();
	if (flags & (1U << 1)) {
		[[maybe_unused]] const auto ptsLimit = reader.int32Value();
	}
	if (flags & (1U << 0)) {
		[[maybe_unused]] const auto ptsTotalLimit = reader.int32Value();
	}
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	schedulePoll(kMinPollInterval);
	if (pts >= _commonPts) {
		reply(requestId, MTP_updates_differenceEmpty(
			MTP_int(Now()),
			MTP_int(0)));
		return;
	}
	auto messages = QVector<MTPMessage>();
	auto other = QVector<MTPUpdate>();
	auto users = std::vector<int64>{ _selfId };
	auto chats = std::vector<int64>();
	for (const auto &entry : _commonLog) {
		if (entry.pts <= pts) {
			continue;
		}
		if (entry.update.type() == mtpc_updateNewMessage) {
			messages.push_back(entry.update.c_updateNewMessage().vmessage());
		} else {
			other.push_back(entry.update);
		}
		users.insert(users.end(), entry.users.begin(), entry.users.end());
		chats.insert(chats.end(), entry.chats.begin(), entry.chats.end());
	}
	reply(requestId, MTP_updates_difference(
		MTP_vector<MTPMessage>(std::move(messages)),
		MTP_vector<MTPEncryptedMessage>(),
		MTP_vector<MTPUpdate>(std::move(other)),
		chatsToMTP(chats),
		usersToMTP(users),
		stateToMTP()));
}

void Backend::getChannelDifference(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto channel = reader.read<MTPInputChannel>();
	[[maybe_unused]] const auto filter = reader.read<MTPChannelMessagesFilter>();
	const auto pts = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	using EmptyFlag = MTPDupdates_channelDifferenceEmpty::Flag;
	const auto chat = chatByInputChannel(channel);
	if (!chat) {
		reply(requestId, MTP_updates_channelDifferenceEmpty(
			MTP_flags(EmptyFlag::f_final | EmptyFlag::f_timeout),
			MTP_int(pts),
			MTP_int(kChannelDifferenceTimeout)));
		return;
	}
	// The UI polls the opened chats, use it to deliver news faster.
	schedulePoll(kMinPollInterval);
	if (waitsForRead(*chat)) {
		refreshReadOutbox(chat->info.id);
	}
	if (pts >= chat->pts) {
		reply(requestId, MTP_updates_channelDifferenceEmpty(
			MTP_flags(EmptyFlag::f_final | EmptyFlag::f_timeout),
			MTP_int(chat->pts),
			MTP_int(kChannelDifferenceTimeout)));
		return;
	}
	auto messages = QVector<MTPMessage>();
	auto other = QVector<MTPUpdate>();
	auto users = std::vector<int64>{ _selfId };
	for (const auto &entry : chat->log) {
		if (entry.pts <= pts) {
			continue;
		}
		if (entry.update.type() == mtpc_updateNewChannelMessage) {
			messages.push_back(
				entry.update.c_updateNewChannelMessage().vmessage());
		} else {
			other.push_back(entry.update);
		}
		users.insert(users.end(), entry.users.begin(), entry.users.end());
	}
	using Flag = MTPDupdates_channelDifference::Flag;
	reply(requestId, MTP_updates_channelDifference(
		MTP_flags(Flag::f_final | Flag::f_timeout),
		MTP_int(chat->pts),
		MTP_int(kChannelDifferenceTimeout),
		MTP_vector<MTPMessage>(std::move(messages)),
		MTP_vector<MTPUpdate>(std::move(other)),
		chatsToMTP({ chat->info.id }),
		usersToMTP(users)));
}

void Backend::dialogsData(
		const std::vector<Chat*> &list,
		QVector<MTPDialog> &dialogs,
		QVector<MTPMessage> &messages,
		std::vector<int64> &users,
		std::vector<int64> &chats) {
	for (const auto chat : list) {
		const auto top = topPost(*chat);
		if (!top) {
			continue;
		}
		dialogs.push_back(dialogToMTP(*chat));
		messages.push_back(messageToMTP(*chat, *top));
		collectPeers(*chat, *top, users);
		if (!chat->info.pm) {
			chats.push_back(chat->info.id);
		}
	}
}

void Backend::getDialogs(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto folderId = (flags & (1U << 1)) ? reader.int32Value() : 0;
	const auto offsetDate = reader.int32Value();
	const auto offsetId = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	ensureLoaded([=] {
		auto list = std::vector<Chat*>();
		const auto excludePinned = (flags & 1U) != 0;
		if (!offsetDate && !offsetId) {
			for (const auto &[id, chat] : _chats) {
				const auto archived = !chat->info.visible;
				if (chat->joined
					&& archived == (folderId == kArchiveFolderId)
					&& !(excludePinned && chat->info.favorite)) {
					list.push_back(chat.get());
				}
			}
		}
		std::sort(list.begin(), list.end(), [&](Chat *a, Chat *b) {
			const auto topA = topPost(*a);
			const auto topB = topPost(*b);
			return (topA ? topA->created : 0) > (topB ? topB->created : 0);
		});
		auto dialogs = QVector<MTPDialog>();
		auto messages = QVector<MTPMessage>();
		auto users = std::vector<int64>{ _selfId };
		auto chats = std::vector<int64>();
		dialogsData(list, dialogs, messages, users, chats);
		if (!offsetDate && !offsetId && folderId != kArchiveFolderId) {
			// Hidden pararam.io chats are shown in the Archive folder.
			auto archived = std::vector<Chat*>();
			for (const auto &[id, chat] : _chats) {
				if (chat->joined && !chat->info.visible && topPost(*chat)) {
					archived.push_back(chat.get());
				}
			}
			if (!archived.empty()) {
				const auto newest = *std::max_element(
					archived.begin(),
					archived.end(),
					[&](Chat *a, Chat *b) {
						return topPost(*a)->created < topPost(*b)->created;
					});
				auto unreadPeers = 0;
				auto unreadMessages = 0;
				for (const auto chat : archived) {
					if (const auto count = unreadCount(*chat)) {
						++unreadPeers;
						unreadMessages += count;
					}
				}
				const auto top = topPost(*newest);
				dialogs.push_back(MTP_dialogFolder(
					MTP_flags(0),
					MTP_folder(
						MTP_flags(0),
						MTP_int(kArchiveFolderId),
						MTP_string("Archived Chats"),
						MTPChatPhoto()),
					peerToMTP(*newest),
					MTP_int(msgId(*newest, top->postNo)),
					MTP_int(0),
					MTP_int(unreadPeers),
					MTP_int(0),
					MTP_int(unreadMessages)));
				messages.push_back(messageToMTP(*newest, *top));
				collectPeers(*newest, *top, users);
				if (!newest->info.pm) {
					chats.push_back(newest->info.id);
				}
			}
		}
		reply(requestId, MTP_messages_dialogs(
			MTP_vector<MTPDialog>(std::move(dialogs)),
			MTP_vector<MTPMessage>(std::move(messages)),
			chatsToMTP(chats),
			usersToMTP(users)));
	}, failer(requestId));
}

void Backend::getPinnedDialogs(mtpRequestId requestId, Reader &reader) {
	const auto folderId = reader.int32Value();
	ensureLoaded([=] {
		auto list = std::vector<Chat*>();
		for (const auto &[id, chat] : _chats) {
			const auto archived = !chat->info.visible;
			if (chat->joined
				&& chat->info.favorite
				&& archived == (folderId == kArchiveFolderId)) {
				list.push_back(chat.get());
			}
		}
		auto dialogs = QVector<MTPDialog>();
		auto messages = QVector<MTPMessage>();
		auto users = std::vector<int64>{ _selfId };
		auto chats = std::vector<int64>();
		dialogsData(list, dialogs, messages, users, chats);
		reply(requestId, MTP_messages_peerDialogs(
			MTP_vector<MTPDialog>(std::move(dialogs)),
			MTP_vector<MTPMessage>(std::move(messages)),
			chatsToMTP(chats),
			usersToMTP(users),
			stateToMTP()));
	}, failer(requestId));
}

void Backend::getPeerDialogs(mtpRequestId requestId, Reader &reader) {
	const auto peers = reader.read<MTPVector<MTPInputDialogPeer>>();
	ensureLoaded([=] {
		auto list = std::vector<Chat*>();
		for (const auto &peer : peers.v) {
			peer.match([&](const MTPDinputDialogPeer &data) {
				if (const auto chat = chatByInputPeer(data.vpeer())) {
					list.push_back(chat);
				}
			}, [](const auto &) {
			});
		}
		auto dialogs = QVector<MTPDialog>();
		auto messages = QVector<MTPMessage>();
		auto users = std::vector<int64>{ _selfId };
		auto chats = std::vector<int64>();
		dialogsData(list, dialogs, messages, users, chats);
		reply(requestId, MTP_messages_peerDialogs(
			MTP_vector<MTPDialog>(std::move(dialogs)),
			MTP_vector<MTPMessage>(std::move(messages)),
			chatsToMTP(chats),
			usersToMTP(users),
			stateToMTP()));
	}, failer(requestId));
}

void Backend::toggleDialogPin(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputDialogPeer>();
	const auto pinned = (flags & 1U) != 0;
	auto chat = (Chat*)nullptr;
	peer.match([&](const MTPDinputDialogPeer &data) {
		chat = chatByInputPeer(data.vpeer());
	}, [](const auto &) {
	});
	if (!chat) {
		replyError(requestId, 400, u"PEER_ID_INVALID"_q);
		return;
	}
	const auto chatId = chat->info.id;
	_api.post(
		u"/core/chat/%1/%2"_q.arg(chatId).arg(pinned
			? u"favorite"_q
			: u"unfavorite"_q),
		QJsonObject(),
		[=](const QJsonValue &) {
			if (const auto chat = chatById(chatId)) {
				chat->info.favorite = pinned;
			}
			replyTrue(requestId);
		},
		failer(requestId));
}

void Backend::editPeerFolders(mtpRequestId requestId, Reader &reader) {
	const auto list = reader.read<MTPVector<MTPInputFolderPeer>>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	struct Change {
		int64 chatId = 0;
		bool archive = false;
	};
	auto changes = std::vector<Change>();
	for (const auto &entry : list.v) {
		const auto &data = entry.data();
		if (const auto chat = chatByInputPeer(data.vpeer())) {
			changes.push_back({
				chat->info.id,
				(data.vfolder_id().v == kArchiveFolderId),
			});
		}
	}
	if (changes.empty()) {
		replyError(requestId, 400, u"PEER_ID_INVALID"_q);
		return;
	}
	const auto left = std::make_shared<int>(int(changes.size()));
	const auto finish = [=] {
		if (--*left) {
			return;
		}
		auto peers = QVector<MTPFolderPeer>();
		auto users = std::vector<int64>();
		auto chats = std::vector<int64>();
		for (const auto &change : changes) {
			if (const auto chat = chatById(change.chatId)) {
				peers.push_back(MTP_folderPeer(
					peerToMTP(*chat),
					MTP_int(chat->info.visible ? 0 : kArchiveFolderId)));
				if (chat->info.pm) {
					users.push_back(chat->pmUserId);
				} else {
					chats.push_back(chat->info.id);
				}
			}
		}
		const auto pts = ++_commonPts;
		auto update = MTP_updateFolderPeers(
			MTP_vector<MTPFolderPeer>(std::move(peers)),
			MTP_int(pts),
			MTP_int(1));
		logUpdate(nullptr, update, pts, 1, users);
		reply(requestId, wrapUpdates({ update }, users, chats));
	};
	for (const auto &change : changes) {
		const auto chatId = change.chatId;
		const auto archive = change.archive;
		auto body = QJsonObject();
		body.insert(u"chat_id"_q, chatId);
		_api.post(
			u"/core/chat/%1/%2"_q.arg(chatId).arg(archive
				? u"hide"_q
				: u"show"_q),
			body,
			[=](const QJsonValue &) {
				if (const auto chat = chatById(chatId)) {
					chat->info.visible = !archive;
				}
				finish();
			},
			[=](const HttpError &) {
				finish();
			});
	}
}

MTPInputPeer Backend::inputPeerToMTP(const Chat &chat) const {
	return chat.info.pm
		? MTP_inputPeerUser(
			MTP_long(chat.pmUserId),
			MTP_long(AccessHash(chat.pmUserId)))
		: MTP_inputPeerChannel(
			MTP_long(chat.info.id),
			MTP_long(AccessHash(chat.info.id)));
}

void Backend::getDialogFilters(mtpRequestId requestId) {
	const auto finish = [=] {
		auto filters = QVector<MTPDialogFilter>();
		filters.push_back(MTP_dialogFilterDefault());
		for (const auto &[id, name] : _tagById) {
			auto peers = QVector<MTPInputPeer>();
			const auto i = _tags.find(name);
			if (i != end(_tags)) {
				for (const auto chatId : i->second) {
					if (const auto chat = chatById(chatId)) {
						peers.push_back(inputPeerToMTP(*chat));
					}
				}
			}
			filters.push_back(MTP_dialogFilter(
				MTP_flags(0),
				MTP_int(id),
				MTP_textWithEntities(
					MTP_string(name),
					MTP_vector<MTPMessageEntity>()),
				MTPstring(), // emoticon
				MTPint(), // color
				MTP_vector<MTPInputPeer>(),
				MTP_vector<MTPInputPeer>(std::move(peers)),
				MTP_vector<MTPInputPeer>()));
		}
		reply(requestId, MTP_messages_dialogFilters(
			MTP_flags(0),
			MTP_vector<MTPDialogFilter>(std::move(filters))));
	};
	ensureLoaded([=] {
		// pararam.io chat tags are shown as chat folders.
		_api.get(u"/user/chat/tags"_q, [=](const QJsonValue &result) {
			_tags.clear();
			const auto object = result.toObject();
			const auto list = object.contains(u"chats_tags"_q)
				? object.value(u"chats_tags"_q).toArray()
				: result.toArray();
			for (const auto &entry : list) {
				const auto data = entry.toObject();
				const auto name = JsonString(data, u"tag"_q);
				if (!name.isEmpty()) {
					_tags[name] = JsonIds(data.value(u"chat_ids"_q));
				}
			}
			for (auto i = begin(_tagById); i != end(_tagById);) {
				if (_tags.contains(i->second)) {
					++i;
				} else {
					i = _tagById.erase(i);
				}
			}
			for (const auto &[name, chats] : _tags) {
				const auto known = ranges::any_of(_tagById, [&](
						const auto &pair) {
					return pair.second == name;
				});
				if (!known) {
					auto id = kFirstFilterId;
					while (_tagById.contains(id)) {
						++id;
					}
					_tagById.emplace(id, name);
				}
			}
			finish();
		}, [=](const HttpError &) {
			finish();
		});
	}, [=](const HttpError &) {
		finish();
	});
}

void Backend::updateDialogFilter(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto id = reader.int32Value();
	const auto filter = (flags & 1U)
		? std::make_optional(reader.read<MTPDialogFilter>())
		: std::nullopt;
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	const auto i = _tagById.find(id);
	const auto oldName = (i != end(_tagById)) ? i->second : QString();
	const auto oldChats = _tags.contains(oldName)
		? _tags[oldName]
		: std::vector<int64>();
	auto newName = QString();
	auto newChats = std::vector<int64>();
	if (filter) {
		filter->match([&](const MTPDdialogFilter &data) {
			newName = qs(data.vtitle().data().vtext()).trimmed();
			for (const auto &peer : data.vinclude_peers().v) {
				if (const auto chat = chatByInputPeer(peer)) {
					newChats.push_back(chat->info.id);
				}
			}
			for (const auto &peer : data.vpinned_peers().v) {
				if (const auto chat = chatByInputPeer(peer)) {
					newChats.push_back(chat->info.id);
				}
			}
		}, [](const auto &) {
		});
		newChats = Unique(newChats);
		if (newName.isEmpty()) {
			replyError(requestId, 400, u"FILTER_TITLE_EMPTY"_q);
			return;
		}
	}
	// Tag operations: remove everything from the old tag that is not in
	// the new one with the same name, add what is missing.
	struct Operation {
		QString name;
		int64 chatId = 0;
		bool add = false;
	};
	auto operations = std::vector<Operation>();
	const auto contains = [](const std::vector<int64> &list, int64 id) {
		return std::find(list.begin(), list.end(), id) != list.end();
	};
	for (const auto chatId : oldChats) {
		if (newName != oldName || !contains(newChats, chatId)) {
			operations.push_back({ oldName, chatId, false });
		}
	}
	for (const auto chatId : newChats) {
		if (newName != oldName || !contains(oldChats, chatId)) {
			operations.push_back({ newName, chatId, true });
		}
	}
	if (oldName.isEmpty()) {
		operations.erase(
			std::remove_if(operations.begin(), operations.end(), [](
					const Operation &operation) {
				return !operation.add;
			}),
			operations.end());
	}
	_tags.remove(oldName);
	_tagById.remove(id);
	if (!newName.isEmpty()) {
		_tags[newName] = newChats;
		_tagById[id] = newName;
	}
	if (operations.empty()) {
		replyTrue(requestId);
		return;
	}
	struct State {
		int left = 0;
		std::optional<HttpError> error;
	};
	const auto state = std::make_shared<State>();
	state->left = int(operations.size());
	const auto finished = [=] {
		if (--state->left) {
			return;
		} else if (state->error) {
			replyHttpError(requestId, *state->error);
		} else {
			replyTrue(requestId);
		}
	};
	for (const auto &operation : operations) {
		const auto url = u"/user/chat/tags?name=%1&chat_id=%2"_q
			.arg(QString::fromLatin1(QUrl::toPercentEncoding(operation.name)))
			.arg(operation.chatId);
		const auto done = [=](const QJsonValue &) { finished(); };
		const auto fail = [=](const HttpError &error) {
			state->error = error;
			finished();
		};
		if (operation.add) {
			_api.put(url, QJsonObject(), done, fail);
		} else {
			_api.del(url, done, fail);
		}
	}
}

void Backend::getHistory(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputPeer>();
	auto query = std::make_shared<HistoryQuery>();
	query->offsetId = reader.int32Value();
	[[maybe_unused]] const auto offsetDate = reader.int32Value();
	query->addOffset = reader.int32Value();
	query->limit = std::clamp(reader.int32Value(), 1, 200);
	query->maxId = reader.int32Value();
	query->minId = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	ensureLoaded([=] {
		withChatForPeer(peer, [=](Chat *chat) {
			if (!chat) {
				reply(requestId, MTP_messages_messages(
					MTP_vector<MTPMessage>(),
					MTP_vector<MTPForumTopic>(),
					MTP_vector<MTPChat>(),
					MTP_vector<MTPUser>()));
				return;
			}
			query->chatId = chat->info.id;
			const auto total = chat->info.postsCount;
			const auto first = std::max(1, chat->info.historyStart);
			const auto offsetPost = query->offsetId
				? postNo(*chat, query->offsetId)
				: 0;
			const auto index = query->offsetId
				? (total - offsetPost + 1)
				: 0;
			auto start = index + query->addOffset;
			auto limit = query->limit;
			if (start < 0) {
				limit += start;
				start = 0;
			}
			auto hi = total - start;
			auto lo = hi - limit + 1;
			if (query->maxId) {
				hi = std::min(hi, postNo(*chat, query->maxId) - 1);
			}
			if (query->minId) {
				lo = std::max(lo, postNo(*chat, query->minId) + 1);
			}
			query->lo = std::max(lo, first);
			query->hi = hi;
			query->limit = std::max(limit, 0);
			loadHistory(requestId, query);
			refreshReadOutbox(chat->info.id);
		}, failer(requestId), false);
	}, failer(requestId));
}

void Backend::loadHistory(
		mtpRequestId requestId,
		std::shared_ptr<HistoryQuery> query) {
	if (query->limit <= 0 || query->hi < query->lo) {
		replyHistory(requestId, query);
		return;
	}
	fetchPosts(query->chatId, query->lo, query->hi, [=] {
		replyHistory(requestId, query);
	}, failer(requestId));
}

void Backend::replyHistory(
		mtpRequestId requestId,
		std::shared_ptr<HistoryQuery> query) {
	const auto chat = chatById(query->chatId);
	if (!chat) {
		replyError(requestId, 400, u"PEER_ID_INVALID"_q);
		return;
	}
	auto numbers = std::vector<int>();
	if (query->limit > 0 && query->hi >= query->lo) {
		for (auto i = chat->posts.lower_bound(query->lo)
			; i != end(chat->posts) && i->first <= query->hi
			; ++i) {
			if (!i->second.deleted) {
				numbers.push_back(i->first);
			}
		}
	}
	const auto first = std::max(1, chat->info.historyStart);
	const auto loadingOlder = (query->addOffset >= 0);
	if (loadingOlder
		&& int(numbers.size()) < query->limit
		&& query->lo > first
		&& query->attempts < kHistoryAttempts) {
		// Some posts were deleted, load more to fill the page.
		++query->attempts;
		const auto missing = query->limit - int(numbers.size());
		const auto till = query->lo - 1;
		query->lo = std::max(first, query->lo - missing - 10);
		fetchPosts(query->chatId, query->lo, till, [=] {
			replyHistory(requestId, query);
		}, failer(requestId));
		return;
	}
	std::reverse(numbers.begin(), numbers.end());
	if (loadingOlder && int(numbers.size()) > query->limit) {
		numbers.resize(query->limit);
	}
	auto users = std::vector<int64>{ _selfId };
	auto files = std::vector<FileInfo>();
	for (const auto number : numbers) {
		const auto &post = chat->posts[number];
		collectPeers(*chat, post, users);
		const auto list = filesOf(post);
		files.insert(files.end(), list.begin(), list.end());
	}
	ensureImageSizes(files, [=] { ensureUsers(users, [=] {
		const auto chat = chatById(query->chatId);
		if (!chat) {
			replyError(requestId, 400, u"PEER_ID_INVALID"_q);
			return;
		}
		auto messages = QVector<MTPMessage>();
		for (const auto number : numbers) {
			const auto i = chat->posts.find(number);
			if (i != end(chat->posts)) {
				messages.push_back(messageToMTP(*chat, i->second));
			}
		}
		const auto count = MTP_int(std::max(
			chat->info.postsCount,
			int(messages.size())));
		if (chat->info.pm) {
			reply(requestId, MTP_messages_messagesSlice(
				MTP_flags(0),
				count,
				MTPint(),
				MTPint(),
				MTPSearchPostsFlood(),
				MTP_vector<MTPMessage>(std::move(messages)),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				usersToMTP(users)));
		} else {
			reply(requestId, MTP_messages_channelMessages(
				MTP_flags(0),
				MTP_int(chat->pts),
				count,
				MTPint(),
				MTP_vector<MTPMessage>(std::move(messages)),
				MTP_vector<MTPForumTopic>(),
				chatsToMTP({ chat->info.id }),
				usersToMTP(users)));
		}
	}); });
}

void Backend::getMessages(
		mtpRequestId requestId,
		Reader &reader,
		bool channel) {
	auto channelChat = (Chat*)nullptr;
	if (channel) {
		channelChat = chatByInputChannel(reader.read<MTPInputChannel>());
	}
	const auto ids = reader.read<MTPVector<MTPInputMessage>>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	struct Wanted {
		int64 chatId = 0;
		int postNo = 0;
		int32 id = 0;
	};
	auto wanted = std::make_shared<std::vector<Wanted>>();
	for (const auto &input : ids.v) {
		input.match([&](const MTPDinputMessageID &data) {
			const auto id = data.vid().v;
			const auto chat = channelChat
				? channelChat
				: chatByCommonMsgId(id);
			if (chat) {
				wanted->push_back({ chat->info.id, postNo(*chat, id), id });
			}
		}, [&](const MTPDinputMessageReplyTo &data) {
			const auto id = data.vid().v;
			const auto chat = channelChat
				? channelChat
				: chatByCommonMsgId(id);
			if (!chat) {
				return;
			}
			const auto i = chat->posts.find(postNo(*chat, id));
			if (i != end(chat->posts) && i->second.replyNo > 0) {
				wanted->push_back({
					chat->info.id,
					i->second.replyNo,
					msgId(*chat, i->second.replyNo),
				});
			}
		}, [](const auto &) {
		});
	}
	const auto finish = [=] {
		auto messages = QVector<MTPMessage>();
		auto users = std::vector<int64>{ _selfId };
		auto chats = std::vector<int64>();
		for (const auto &entry : *wanted) {
			const auto chat = chatById(entry.chatId);
			if (!chat) {
				continue;
			}
			const auto i = chat->posts.find(entry.postNo);
			if (i == end(chat->posts) || i->second.deleted) {
				messages.push_back(MTP_messageEmpty(
					MTP_flags(MTPDmessageEmpty::Flag::f_peer_id),
					MTP_int(entry.id),
					peerToMTP(*chat)));
				continue;
			}
			collectPeers(*chat, i->second, users);
			if (!chat->info.pm) {
				chats.push_back(chat->info.id);
			}
			messages.push_back(messageToMTP(*chat, i->second));
		}
		ensureUsers(users, [=] {
			if (channelChat) {
				reply(requestId, MTP_messages_channelMessages(
					MTP_flags(0),
					MTP_int(channelChat->pts),
					MTP_int(messages.size()),
					MTPint(),
					MTP_vector<MTPMessage>(messages),
					MTP_vector<MTPForumTopic>(),
					chatsToMTP(chats),
					usersToMTP(users)));
			} else {
				reply(requestId, MTP_messages_messages(
					MTP_vector<MTPMessage>(messages),
					MTP_vector<MTPForumTopic>(),
					chatsToMTP(chats),
					usersToMTP(users)));
			}
		});
	};
	const auto left = std::make_shared<int>(1);
	const auto done = [=] {
		if (!--*left) {
			finish();
		}
	};
	for (const auto &entry : *wanted) {
		const auto chat = chatById(entry.chatId);
		if (chat && !chat->posts.contains(entry.postNo) && entry.postNo > 0) {
			++*left;
			fetchPosts(entry.chatId, entry.postNo, entry.postNo, done, [=](
					const HttpError &) {
				done();
			});
		}
	}
	done();
}

int Backend::replyToPost(const Chat &chat, const MTPInputReplyTo &replyTo) const {
	return replyTo.match([&](const MTPDinputReplyToMessage &data) {
		return postNo(chat, data.vreply_to_msg_id().v);
	}, [](const auto &) {
		return 0;
	});
}

void Backend::sendMessage(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto replyTo = (flags & (1U << 0))
		? std::make_optional(reader.read<MTPInputReplyTo>())
		: std::nullopt;
	const auto message = reader.string();
	const auto randomId = reader.int64Value();
	if (flags & (1U << 2)) {
		[[maybe_unused]] const auto markup = reader.read<MTPReplyMarkup>();
	}
	const auto entities = (flags & (1U << 3))
		? std::make_optional(reader.read<MTPVector<MTPMessageEntity>>())
		: std::nullopt;
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	} else if (flags & (1U << 10)) {
		replyError(requestId, 400, u"SCHEDULE_DATE_INVALID"_q);
		return;
	}
	const auto text = FormatOutgoingText(textFromMTP(
		message,
		entities ? &*entities : nullptr));
	ensureLoaded([=] {
		withChatForPeer(peer, [=](Chat *chat) {
			if (!chat) {
				replyError(requestId, 400, u"PEER_ID_INVALID"_q);
				return;
			}
			const auto replyNo = replyTo ? replyToPost(*chat, *replyTo) : 0;
			createPost(chat->info.id, text, replyNo, randomId, {}, {}, [=](
					QVector<MTPUpdate> updates,
					std::vector<int64> users) {
				reply(requestId, wrapUpdates(updates, users, { chat->info.id }));
			}, failer(requestId));
		}, failer(requestId), true);
	}, failer(requestId));
}

void Backend::createPost(
		int64 chatId,
		const QString &text,
		int replyNo,
		uint64 randomId,
		std::vector<QString> attachments,
		std::optional<FileInfo> file,
		UpdatesDone done,
		Fail fail) {
	const auto chat = chatById(chatId);
	if (!chat) {
		fail(LocalError(u"PEER_ID_INVALID"_q));
		return;
	}
	const auto uuid = RandomUuid();
	auto body = QJsonObject();
	body.insert(u"uuid"_q, uuid);
	body.insert(u"text"_q, text);
	body.insert(u"quote"_q, QJsonValue::Null);
	body.insert(
		u"reply_no"_q,
		replyNo > 0 ? QJsonValue(replyNo) : QJsonValue(QJsonValue::Null));
	if (!attachments.empty()) {
		auto list = QJsonArray();
		for (const auto &guid : attachments) {
			list.push_back(guid);
		}
		body.insert(u"attachments"_q, list);
	}
	++chat->sending;
	_sendingUuids.emplace(uuid);
	const auto finished = [=] {
		_sendingUuids.remove(uuid);
		const auto chat = chatById(chatId);
		if (!chat) {
			return;
		}
		--chat->sending;
		if (!chat->sending && base::take(chat->refreshAfterSend)) {
			deliverNewPosts(chatId, chat->info.postsCount);
		}
	};
	_api.post(u"/msg/post/%1"_q.arg(chatId), body, [=](
			const QJsonValue &result) {
		const auto chat = chatById(chatId);
		const auto object = result.toObject();
		const auto number = int(JsonInt(object, u"post_no"_q));
		if (!chat || number <= 0) {
			finished();
			fail(LocalError(u"PARARAM_BAD_RESPONSE"_q));
			return;
		}
		auto post = PostInfo();
		post.chatId = chatId;
		post.postNo = number;
		post.userId = _selfId;
		post.uuid = uuid;
		post.text = text;
		post.replyNo = replyNo;
		post.created = Now();
		post.attachments = attachments;
		if (file) {
			post.file = *file;
		}
		applyPost(*chat, post);
		if (number > chat->fetchedPostNo) {
			chat->sentByUs.emplace(number);
		}
		chat->info.lastReadPostNo = std::max(
			chat->info.lastReadPostNo,
			number);
		auto users = std::vector<int64>{ _selfId };
		auto updates = QVector<MTPUpdate>();
		if (randomId) {
			updates.push_back(MTP_updateMessageID(
				MTP_int(msgId(*chat, number)),
				MTP_long(randomId)));
		}
		updates.push_back(newMessageUpdate(*chat, chat->posts[number], users));
		finished();
		done(std::move(updates), std::move(users));
	}, [=](const HttpError &error) {
		finished();
		fail(error);
	});
}

QByteArray Backend::takeUploaded(const MTPInputFile &file, QString *name) {
	auto id = uint64();
	file.match([&](const MTPDinputFile &data) {
		id = data.vid().v;
		*name = qs(data.vname());
	}, [&](const MTPDinputFileBig &data) {
		id = data.vid().v;
		*name = qs(data.vname());
	}, [](const auto &) {
	});
	const auto i = _uploads->find(id);
	if (i == end(*_uploads)) {
		return QByteArray();
	}
	auto result = QByteArray();
	for (const auto &[index, part] : i->second.parts) {
		result.append(part);
	}
	_uploads->erase(i);
	return result;
}

void Backend::uploadMedia(
		int64 chatId,
		const MTPInputMedia &media,
		const QString &caption,
		int replyNo,
		uint64 randomId,
		UpdatesDone done,
		Fail fail) {
	auto name = QString();
	auto mime = QString();
	auto bytes = QByteArray();
	auto existing = std::optional<FileInfo>();
	media.match([&](const MTPDinputMediaUploadedPhoto &data) {
		bytes = takeUploaded(data.vfile(), &name);
		mime = u"image/jpeg"_q;
		if (name.isEmpty() || !name.contains('.')) {
			name = u"photo.jpg"_q;
		}
	}, [&](const MTPDinputMediaUploadedDocument &data) {
		bytes = takeUploaded(data.vfile(), &name);
		mime = qs(data.vmime_type());
		if (const auto thumb = data.vthumb()) {
			// pararam.io makes its own previews, drop the thumbnail.
			auto thumbName = QString();
			[[maybe_unused]] const auto dropped = takeUploaded(
				*thumb,
				&thumbName);
		}
		for (const auto &attribute : data.vattributes().v) {
			attribute.match([&](const MTPDdocumentAttributeFilename &data) {
				name = qs(data.vfile_name());
			}, [](const auto &) {
			});
		}
	}, [&](const MTPDinputMediaDocument &data) {
		data.vid().match([&](const MTPDinputDocument &data) {
			const auto i = _documents.find(data.vid().v);
			if (i != end(_documents)) {
				existing = i->second;
			}
		}, [](const auto &) {
		});
	}, [&](const MTPDinputMediaPhoto &data) {
		data.vid().match([&](const MTPDinputPhoto &data) {
			const auto i = _documents.find(data.vid().v);
			if (i != end(_documents)) {
				existing = i->second;
			}
		}, [](const auto &) {
		});
	}, [](const auto &) {
	});
	if (existing) {
		createPost(
			chatId,
			caption,
			replyNo,
			randomId,
			{ existing->guid },
			existing,
			done,
			fail);
		return;
	} else if (bytes.isEmpty()) {
		fail(LocalError(u"MEDIA_INVALID"_q));
		return;
	}
	if (name.isEmpty()) {
		name = u"file"_q;
	}
	auto fields = std::vector<UploadField>{
		{ u"filename"_q, name },
		{ u"size"_q, QString::number(bytes.size()) },
		{ u"chat_id"_q, QString::number(chatId) },
	};
	if (replyNo > 0) {
		fields.push_back({ u"reply_no"_q, QString::number(replyNo) });
	}
	const auto size = int64(bytes.size());
	_api.upload(fields, name, mime, bytes, [=](const QJsonValue &result) {
		const auto object = result.toObject();
		auto file = FileInfo::Parse(object);
		if (!file.valid()) {
			fail(LocalError(u"PARARAM_UPLOAD_FAILED"_q));
			return;
		}
		if (file.name.isEmpty()) {
			file.name = name;
		}
		if (file.mimeType.isEmpty()) {
			file.mimeType = mime;
		}
		if (!file.size) {
			file.size = size;
		}
		if (file.isImage() && !_imageSizes.contains(file.guid)) {
			auto buffer = QBuffer();
			buffer.setData(bytes);
			buffer.open(QIODevice::ReadOnly);
			const auto dimensions = QImageReader(&buffer).size();
			_imageSizes[file.guid] = dimensions.isValid()
				? dimensions
				: QSize(-1, -1);
		}
		[[maybe_unused]] const auto id = documentId(file);
		const auto chat = chatById(chatId);
		const auto filePostNo = int(JsonInt(object, u"post_no"_q));
		if (!chat || filePostNo <= 0) {
			// The upload did not create a post, attach the file to one.
			createPost(
				chatId,
				caption,
				replyNo,
				randomId,
				{ file.guid },
				file,
				done,
				fail);
			return;
		}
		// The upload created a post with the file already.
		auto post = PostInfo();
		post.chatId = chatId;
		post.postNo = filePostNo;
		post.userId = _selfId;
		post.replyNo = replyNo;
		post.created = Now();
		post.file = file;
		applyPost(*chat, post);
		if (filePostNo > chat->fetchedPostNo) {
			chat->sentByUs.emplace(filePostNo);
		}
		auto users = std::vector<int64>{ _selfId };
		auto updates = QVector<MTPUpdate>();
		updates.push_back(MTP_updateMessageID(
			MTP_int(msgId(*chat, filePostNo)),
			MTP_long(randomId)));
		updates.push_back(newMessageUpdate(*chat, chat->posts[filePostNo], users));
		if (caption.trimmed().isEmpty()) {
			done(std::move(updates), std::move(users));
			return;
		}
		createPost(chatId, caption, replyNo, 0, {}, {}, [=](
				QVector<MTPUpdate> more,
				std::vector<int64> moreUsers) mutable {
			updates.append(more);
			users.insert(users.end(), moreUsers.begin(), moreUsers.end());
			done(std::move(updates), std::move(users));
		}, [=](const HttpError &) {
			done(updates, users);
		});
	}, fail);
}

void Backend::sendMedia(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto replyTo = (flags & (1U << 0))
		? std::make_optional(reader.read<MTPInputReplyTo>())
		: std::nullopt;
	const auto media = reader.read<MTPInputMedia>();
	const auto message = reader.string();
	const auto randomId = reader.int64Value();
	if (flags & (1U << 2)) {
		[[maybe_unused]] const auto markup = reader.read<MTPReplyMarkup>();
	}
	const auto entities = (flags & (1U << 3))
		? std::make_optional(reader.read<MTPVector<MTPMessageEntity>>())
		: std::nullopt;
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	} else if (flags & (1U << 10)) {
		replyError(requestId, 400, u"SCHEDULE_DATE_INVALID"_q);
		return;
	}
	const auto caption = FormatOutgoingText(textFromMTP(
		message,
		entities ? &*entities : nullptr));
	ensureLoaded([=] {
		withChatForPeer(peer, [=](Chat *chat) {
			if (!chat) {
				replyError(requestId, 400, u"PEER_ID_INVALID"_q);
				return;
			}
			const auto chatId = chat->info.id;
			const auto replyNo = replyTo ? replyToPost(*chat, *replyTo) : 0;
			uploadMedia(chatId, media, caption, replyNo, randomId, [=](
					QVector<MTPUpdate> updates,
					std::vector<int64> users) {
				reply(requestId, wrapUpdates(updates, users, { chatId }));
			}, failer(requestId));
		}, failer(requestId), true);
	}, failer(requestId));
}

void Backend::sendMultiMedia(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto replyTo = (flags & (1U << 0))
		? std::make_optional(reader.read<MTPInputReplyTo>())
		: std::nullopt;
	const auto list = reader.read<MTPVector<MTPInputSingleMedia>>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	ensureLoaded([=] {
		withChatForPeer(peer, [=](Chat *chat) {
			if (!chat) {
				replyError(requestId, 400, u"PEER_ID_INVALID"_q);
				return;
			}
			const auto chatId = chat->info.id;
			const auto replyNo = replyTo ? replyToPost(*chat, *replyTo) : 0;
			struct State {
				int index = 0;
				QVector<MTPUpdate> updates;
				std::vector<int64> users;
			};
			const auto state = std::make_shared<State>();
			const auto next = std::make_shared<Fn<void()>>();
			*next = [=] {
				if (state->index >= list.v.size()) {
					reply(requestId, wrapUpdates(
						state->updates,
						state->users,
						{ chatId }));
					*next = nullptr;
					return;
				}
				const auto &single = list.v[state->index++].data();
				const auto caption = FormatOutgoingText(textFromMTP(
					qs(single.vmessage()),
					single.ventities()));
				uploadMedia(
					chatId,
					single.vmedia(),
					caption,
					replyNo,
					single.vrandom_id().v,
					[=](QVector<MTPUpdate> updates, std::vector<int64> users) {
						state->updates.append(updates);
						state->users.insert(
							state->users.end(),
							users.begin(),
							users.end());
						(*next)();
					},
					[=](const HttpError &error) {
						replyHttpError(requestId, error);
						*next = nullptr;
					});
			};
			(*next)();
		}, failer(requestId), true);
	}, failer(requestId));
}

void Backend::forwardMessages(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto fromPeer = reader.read<MTPInputPeer>();
	const auto ids = reader.read<MTPVector<MTPint>>();
	const auto randomIds = reader.read<MTPVector<MTPlong>>();
	const auto toPeer = reader.read<MTPInputPeer>();
	if (!reader.ok() || ids.v.size() != randomIds.v.size()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	ensureLoaded([=] {
		const auto from = chatByInputPeer(fromPeer);
		if (!from) {
			replyError(requestId, 400, u"PEER_ID_INVALID"_q);
			return;
		}
		withChatForPeer(toPeer, [=](Chat *to) {
			if (!to) {
				replyError(requestId, 400, u"PEER_ID_INVALID"_q);
				return;
			}
			struct State {
				int index = 0;
				QVector<MTPUpdate> updates;
				std::vector<int64> users;
			};
			const auto fromId = from->info.id;
			const auto toId = to->info.id;
			const auto state = std::make_shared<State>();
			const auto next = std::make_shared<Fn<void()>>();
			*next = [=] {
				const auto from = chatById(fromId);
				if (!from || state->index >= ids.v.size()) {
					reply(requestId, wrapUpdates(
						state->updates,
						state->users,
						{ toId }));
					*next = nullptr;
					return;
				}
				const auto index = state->index++;
				const auto number = postNo(*from, ids.v[index].v);
				const auto i = from->posts.find(number);
				if (i == end(from->posts) || i->second.deleted) {
					(*next)();
					return;
				}
				const auto &post = i->second;
				const auto author = _users.contains(post.userId)
					? _users[post.userId].name
					: QString();
				auto text = post.text;
				if (post.userId != _selfId && !author.isEmpty()) {
					text = u"Forwarded from %1:\n"_q.arg(author) + text;
				}
				auto attachments = post.attachments;
				auto file = std::optional<FileInfo>();
				if (post.file.valid()) {
					attachments = { post.file.guid };
					file = post.file;
				}
				createPost(
					toId,
					text,
					0,
					randomIds.v[index].v,
					attachments,
					file,
					[=](QVector<MTPUpdate> updates, std::vector<int64> users) {
						state->updates.append(updates);
						state->users.insert(
							state->users.end(),
							users.begin(),
							users.end());
						(*next)();
					},
					[=](const HttpError &error) {
						replyHttpError(requestId, error);
						*next = nullptr;
					});
			};
			(*next)();
		}, failer(requestId), true);
	}, failer(requestId));
}

void Backend::editMessage(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto id = reader.int32Value();
	const auto message = (flags & (1U << 11))
		? std::make_optional(reader.string())
		: std::nullopt;
	if (flags & (1U << 14)) {
		[[maybe_unused]] const auto media = reader.read<MTPInputMedia>();
	}
	if (flags & (1U << 2)) {
		[[maybe_unused]] const auto markup = reader.read<MTPReplyMarkup>();
	}
	const auto entities = (flags & (1U << 3))
		? std::make_optional(reader.read<MTPVector<MTPMessageEntity>>())
		: std::nullopt;
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	const auto chat = chatByInputPeer(peer);
	if (!chat) {
		replyError(requestId, 400, u"PEER_ID_INVALID"_q);
		return;
	} else if (!message) {
		replyError(requestId, 400, u"MESSAGE_NOT_MODIFIED"_q);
		return;
	}
	const auto chatId = chat->info.id;
	const auto number = postNo(*chat, id);
	const auto i = chat->posts.find(number);
	const auto existing = (i != end(chat->posts))
		? i->second
		: PostInfo();
	const auto text = FormatOutgoingText(textFromMTP(
		*message,
		entities ? &*entities : nullptr));
	auto body = QJsonObject();
	body.insert(
		u"uuid"_q,
		existing.uuid.isEmpty() ? RandomUuid() : existing.uuid);
	body.insert(u"text"_q, text);
	body.insert(u"quote"_q, QJsonValue::Null);
	body.insert(
		u"reply_no"_q,
		(existing.replyNo > 0
			? QJsonValue(existing.replyNo)
			: QJsonValue(QJsonValue::Null)));
	_api.put(
		u"/msg/post/%1/%2"_q.arg(chatId).arg(number),
		body,
		[=](const QJsonValue &result) {
			const auto chat = chatById(chatId);
			if (!chat) {
				replyError(requestId, 400, u"PEER_ID_INVALID"_q);
				return;
			}
			auto post = existing;
			post.chatId = chatId;
			post.postNo = number;
			if (!post.userId) {
				post.userId = _selfId;
			}
			post.text = text;
			post.parsed.clear();
			post.edited = Now();
			if (!post.created) {
				post.created = post.edited - 2;
			}
			post.ver = int(JsonInt(result.toObject(), u"ver"_q, post.ver + 1));
			chat->posts[number] = post;
			auto users = std::vector<int64>{ _selfId };
			auto updates = QVector<MTPUpdate>();
			updates.push_back(editMessageUpdate(*chat, post, users));
			reply(requestId, wrapUpdates(updates, users, { chatId }));
		},
		failer(requestId));
}

void Backend::deleteMessages(
		mtpRequestId requestId,
		Reader &reader,
		bool channel) {
	auto channelChat = (Chat*)nullptr;
	if (channel) {
		channelChat = chatByInputChannel(reader.read<MTPInputChannel>());
	} else {
		[[maybe_unused]] const auto flags = reader.flags();
	}
	const auto ids = reader.read<MTPVector<MTPint>>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	auto byChat = std::make_shared<base::flat_map<int64, std::vector<int>>>();
	for (const auto &id : ids.v) {
		const auto chat = channelChat
			? channelChat
			: chatByCommonMsgId(id.v);
		if (chat) {
			(*byChat)[chat->info.id].push_back(postNo(*chat, id.v));
		}
	}
	if (byChat->empty()) {
		reply(requestId, MTP_messages_affectedMessages(
			MTP_int(channelChat ? channelChat->pts : _commonPts),
			MTP_int(0)));
		return;
	}
	struct State {
		int left = 0;
		std::optional<HttpError> error;
		base::flat_map<int64, std::vector<int>> deleted;
	};
	const auto state = std::make_shared<State>();
	const auto finish = [=] {
		auto affected = MTP_messages_affectedMessages(
			MTP_int(_commonPts),
			MTP_int(0));
		for (const auto &[chatId, list] : state->deleted) {
			const auto chat = chatById(chatId);
			if (!chat || list.empty()) {
				continue;
			}
			[[maybe_unused]] const auto update = deleteMessagesUpdate(
				*chat,
				list);
			affected = MTP_messages_affectedMessages(
				MTP_int(chat->info.pm ? _commonPts : chat->pts),
				MTP_int(1));
		}
		if (state->deleted.empty() && state->error) {
			replyHttpError(requestId, *state->error);
		} else {
			reply(requestId, affected);
		}
	};
	for (const auto &[chatId, list] : *byChat) {
		for (const auto number : list) {
			++state->left;
			_api.del(
				u"/msg/post/%1/%2"_q.arg(chatId).arg(number),
				[=](const QJsonValue &) {
					state->deleted[chatId].push_back(number);
					if (!--state->left) {
						finish();
					}
				},
				[=](const HttpError &error) {
					state->error = error;
					if (!--state->left) {
						finish();
					}
				});
		}
	}
}

void Backend::readHistory(
		mtpRequestId requestId,
		Reader &reader,
		bool channel) {
	const auto chat = channel
		? chatByInputChannel(reader.read<MTPInputChannel>())
		: chatByInputPeer(reader.read<MTPInputPeer>());
	const auto maxId = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	if (chat) {
		const auto number = std::min(
			postNo(*chat, maxId),
			chat->info.postsCount);
		if (number > chat->info.lastReadPostNo) {
			chat->info.lastReadPostNo = number;
			auto body = QJsonObject();
			body.insert(u"post_no"_q, number);
			_api.post(
				u"/msg/lastread/%1"_q.arg(chat->info.id),
				body,
				[](const QJsonValue &) {},
				[](const HttpError &) {});
		}
	}
	if (channel) {
		replyTrue(requestId);
	} else {
		reply(requestId, MTP_messages_affectedMessages(
			MTP_int(_commonPts),
			MTP_int(0)));
	}
}

void Backend::getUsers(mtpRequestId requestId, Reader &reader) {
	const auto list = reader.read<MTPVector<MTPInputUser>>();
	auto ids = std::vector<int64>();
	for (const auto &user : list.v) {
		if (const auto id = userIdFromInputUser(user)) {
			ids.push_back(id);
		}
	}
	ensureUsers(ids, [=] {
		auto result = QVector<MTPUser>();
		for (const auto id : ids) {
			result.push_back(userToMTP(id));
		}
		reply(requestId, MTP_vector<MTPUser>(std::move(result)));
	});
}

void Backend::getFullUser(mtpRequestId requestId, Reader &reader) {
	const auto userId = userIdFromInputUser(reader.read<MTPInputUser>());
	if (!userId) {
		replyError(requestId, 400, u"USER_ID_INVALID"_q);
		return;
	}
	ensureUsers({ userId }, [=] {
		using Flag = MTPDuserFull::Flag;
		const auto i = _users.find(userId);
		const auto about = (i != end(_users)) ? i->second.info : QString();
		const auto chat = chatByUser(userId);
		const auto calls = (userId != _selfId)
			? (Flag::f_phone_calls_available | Flag::f_video_calls_available)
			: Flag();
		const auto deleted = (i != end(_users)) && i->second.deleted;
		const auto photo = deleted
			? MTPPhoto()
			: avatarPhoto(userAvatarId(userId), u"user:%1"_q.arg(userId));
		reply(requestId, MTP_users_userFull(
			MTP_userFull(
				MTP_flags((about.isEmpty() ? Flag() : Flag::f_about)
					| calls
					| (deleted ? Flag() : Flag::f_profile_photo)),
				MTP_long(userId),
				MTP_string(about),
				MTP_peerSettings(
					MTP_flags(0),
					MTPint(),
					MTPstring(),
					MTPint(),
					MTPlong(),
					MTPstring(),
					MTPlong(),
					MTPstring(),
					MTPstring(),
					MTPint(),
					MTPint()),
				MTPPhoto(), // personal_photo
				photo, // profile_photo
				MTPPhoto(), // fallback_photo
				(chat
					? notifySettings(*chat)
					: MTP_peerNotifySettings(
						MTP_flags(0),
						MTPBool(),
						MTPBool(),
						MTPint(),
						MTPNotificationSound(),
						MTPNotificationSound(),
						MTPNotificationSound(),
						MTPBool(),
						MTPBool(),
						MTPNotificationSound(),
						MTPNotificationSound(),
						MTPNotificationSound())),
				MTPBotInfo(),
				MTPint(), // pinned_msg_id
				MTP_int(0), // common_chats_count
				MTPint(), // folder_id
				MTPint(), // ttl_period
				MTPChatTheme(),
				MTPstring(), // private_forward_name
				MTPChatAdminRights(),
				MTPChatAdminRights(),
				MTPWallPaper(),
				MTPPeerStories(),
				MTPBusinessWorkHours(),
				MTPBusinessLocation(),
				MTPBusinessGreetingMessage(),
				MTPBusinessAwayMessage(),
				MTPBusinessIntro(),
				MTPBirthday(),
				MTPlong(), // personal_channel_id
				MTPint(), // personal_channel_message
				MTPint(), // stargifts_count
				MTPStarRefProgram(),
				MTPBotVerification(),
				MTPlong(), // send_paid_messages_stars
				MTPDisallowedGiftsSettings(),
				MTPStarsRating(),
				MTPStarsRating(),
				MTPint(), // stars_my_pending_rating_date
				MTPProfileTab(),
				MTPDocument(), // saved_music
				MTPTextWithEntities(), // note
				MTPlong()), // bot_manager_id
			MTP_vector<MTPChat>(),
			usersToMTP({ userId, _selfId })));
	});
}

void Backend::getChannels(mtpRequestId requestId, Reader &reader) {
	const auto list = reader.read<MTPVector<MTPInputChannel>>();
	auto ids = std::vector<int64>();
	for (const auto &channel : list.v) {
		if (const auto chat = chatByInputChannel(channel)) {
			ids.push_back(chat->info.id);
		}
	}
	reply(requestId, MTP_messages_chats(chatsToMTP(ids)));
}

void Backend::getFullChannel(mtpRequestId requestId, Reader &reader) {
	const auto chat = chatByInputChannel(reader.read<MTPInputChannel>());
	if (!chat) {
		replyError(requestId, 400, u"CHANNEL_INVALID"_q);
		return;
	}
	const auto chatId = chat->info.id;
	auto members = chat->info.users;
	if (members.size() > 200) {
		members.resize(200);
	}
	ensureUsers(members, [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			replyError(requestId, 400, u"CHANNEL_INVALID"_q);
			return;
		}
		using Flag = MTPDchannelFull::Flag;
		auto flags = Flag::f_participants_count
			| Flag::f_can_view_participants
			| Flag::f_available_reactions;
		const auto pinned = chat->info.pinned.empty()
			? 0
			: chat->info.pinned.back();
		if (pinned) {
			flags |= Flag::f_pinned_msg_id;
		}
		if (_calls->groupCallActive(chatId)) {
			flags |= Flag::f_call;
		}
		const auto top = topPost(*chat);
		const auto unread = unreadCount(*chat);
		auto users = members;
		users.push_back(_selfId);
		reply(requestId, MTP_messages_chatFull(
			MTP_channelFull(
				MTP_flags(flags),
				MTP_long(chatId),
				MTP_string(chat->info.description),
				MTP_int(std::max(int(chat->info.users.size()), 1)),
				MTPint(), // admins_count
				MTPint(), // kicked_count
				MTPint(), // banned_count
				MTPint(), // online_count
				MTP_int(unread
					? chat->info.lastReadPostNo
					: (top ? top->postNo : chat->info.lastReadPostNo)),
				MTP_int(chat->readOutbox
					? msgId(*chat, chat->readOutbox)
					: 0),
				MTP_int(unread),
				avatarPhoto(chatAvatarId(*chat), u"chat:%1"_q.arg(chatId)),
				notifySettings(*chat),
				MTPExportedChatInvite(),
				MTP_vector<MTPBotInfo>(),
				MTPlong(), // migrated_from_chat_id
				MTPint(), // migrated_from_max_id
				MTP_int(pinned),
				MTPStickerSet(),
				MTPint(), // available_min_id
				MTPint(), // folder_id
				MTPlong(), // linked_chat_id
				MTPChannelLocation(),
				MTPint(), // slowmode_seconds
				MTPint(), // slowmode_next_send_date
				MTPint(), // stats_dc
				MTP_int(chat->pts),
				_calls->groupCallInput(chatId),
				MTPint(), // ttl_period
				MTPVector<MTPstring>(), // pending_suggestions
				MTPPeer(), // groupcall_default_join_as
				MTPstring(), // theme_emoticon
				MTPint(), // requests_pending
				MTPVector<MTPlong>(), // recent_requesters
				MTPPeer(), // default_send_as
				MTP_chatReactionsAll(MTP_flags(0)),
				MTPint(), // reactions_limit, distinct ones on a message
				MTPPeerStories(),
				MTPWallPaper(),
				MTPint(), // boosts_applied
				MTPint(), // boosts_unrestrict
				MTPStickerSet(), // emojiset
				MTPBotVerification(),
				MTPint(), // stargifts_count
				MTPlong(), // send_paid_messages_stars
				MTPProfileTab(),
				MTPlong()), // guard_bot_id
			chatsToMTP({ chatId }),
			usersToMTP(users)));
	});
}

MTPChannelParticipant Backend::participantToMTP(
		const Chat &chat,
		int64 userId) const {
	const auto date = MTP_int(chat.info.created ? chat.info.created : Now());
	if (userId == chat.info.authorId) {
		return MTP_channelParticipantCreator(
			MTP_flags(0),
			MTP_long(userId),
			AdminRights(),
			MTPstring());
	}
	const auto admin = std::find(
		chat.info.admins.begin(),
		chat.info.admins.end(),
		userId) != chat.info.admins.end();
	const auto inviter = MTP_long(chat.info.authorId
		? chat.info.authorId
		: _selfId);
	if (admin) {
		using Flag = MTPDchannelParticipantAdmin::Flag;
		const auto self = (userId == _selfId);
		return MTP_channelParticipantAdmin(
			MTP_flags(self ? Flag::f_self : Flag()),
			MTP_long(userId),
			self ? inviter : MTPlong(),
			inviter,
			date,
			AdminRights(),
			MTPstring());
	} else if (userId == _selfId) {
		return MTP_channelParticipantSelf(
			MTP_flags(0),
			MTP_long(userId),
			inviter,
			date,
			MTPint(),
			MTPstring());
	}
	return MTP_channelParticipant(
		MTP_flags(0),
		MTP_long(userId),
		date,
		MTPint(),
		MTPstring());
}

void Backend::getParticipants(mtpRequestId requestId, Reader &reader) {
	const auto chat = chatByInputChannel(reader.read<MTPInputChannel>());
	const auto filter = reader.read<MTPChannelParticipantsFilter>();
	const auto offset = std::max(reader.int32Value(), 0);
	const auto limit = std::clamp(reader.int32Value(), 0, 200);
	if (!chat) {
		replyError(requestId, 400, u"CHANNEL_INVALID"_q);
		return;
	}
	auto query = QString();
	auto adminsOnly = false;
	auto none = false;
	filter.match([&](const MTPDchannelParticipantsSearch &data) {
		query = qs(data.vq());
	}, [&](const MTPDchannelParticipantsMentions &data) {
		query = data.vq().value_or_empty();
	}, [&](const MTPDchannelParticipantsContacts &data) {
		query = qs(data.vq());
	}, [&](const MTPDchannelParticipantsAdmins &) {
		adminsOnly = true;
	}, [&](const MTPDchannelParticipantsRecent &) {
	}, [&](const auto &) {
		none = true; // Kicked, banned, bots.
	});
	const auto chatId = chat->info.id;
	auto members = none ? std::vector<int64>() : chat->info.users;
	if (adminsOnly) {
		members = chat->info.admins;
		if (chat->info.authorId) {
			members.insert(members.begin(), chat->info.authorId);
		}
		members = Unique(members);
	}
	ensureUsers(members, [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			replyError(requestId, 400, u"CHANNEL_INVALID"_q);
			return;
		}
		auto filtered = std::vector<int64>();
		const auto lower = query.trimmed().toLower();
		for (const auto id : members) {
			if (!lower.isEmpty()) {
				const auto i = _users.find(id);
				if (i == end(_users)
					|| (!i->second.name.toLower().contains(lower)
						&& !i->second.uniqueName.toLower().contains(lower))) {
					continue;
				}
			}
			filtered.push_back(id);
		}
		auto participants = QVector<MTPChannelParticipant>();
		auto users = std::vector<int64>{ _selfId };
		for (auto i = offset
			; i < int(filtered.size()) && participants.size() < limit
			; ++i) {
			participants.push_back(participantToMTP(*chat, filtered[i]));
			users.push_back(filtered[i]);
		}
		reply(requestId, MTP_channels_channelParticipants(
			MTP_int(filtered.size()),
			MTP_vector<MTPChannelParticipant>(std::move(participants)),
			MTP_vector<MTPChat>(),
			usersToMTP(users)));
	});
}

void Backend::getParticipant(mtpRequestId requestId, Reader &reader) {
	const auto chat = chatByInputChannel(reader.read<MTPInputChannel>());
	const auto userId = userIdFromInputPeer(reader.read<MTPInputPeer>());
	if (!chat || !userId) {
		replyError(requestId, 400, u"USER_NOT_PARTICIPANT"_q);
		return;
	}
	const auto member = (userId == _selfId)
		? chat->joined
		: (std::find(
			chat->info.users.begin(),
			chat->info.users.end(),
			userId) != chat->info.users.end());
	if (!member) {
		replyError(requestId, 400, u"USER_NOT_PARTICIPANT"_q);
		return;
	}
	const auto chatId = chat->info.id;
	ensureUsers({ userId }, [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			replyError(requestId, 400, u"CHANNEL_INVALID"_q);
			return;
		}
		reply(requestId, MTP_channels_channelParticipant(
			participantToMTP(*chat, userId),
			chatsToMTP({ chatId }),
			usersToMTP({ userId, _selfId })));
	});
}

void Backend::joinChannel(mtpRequestId requestId, Reader &reader, bool join) {
	const auto chat = chatByInputChannel(reader.read<MTPInputChannel>());
	if (!chat) {
		replyError(requestId, 400, u"CHANNEL_INVALID"_q);
		return;
	}
	const auto chatId = chat->info.id;
	_api.post(
		u"/core/chat/%1/%2"_q.arg(chatId).arg(join ? u"enter"_q : u"quit"_q),
		QJsonObject(),
		[=](const QJsonValue &) {
			if (const auto chat = chatById(chatId)) {
				chat->joined = join;
			}
			reply(requestId, wrapUpdates(
				{ MTP_updateChannel(MTP_long(chatId)) },
				{},
				{ chatId }));
		},
		failer(requestId));
}

void Backend::getContacts(mtpRequestId requestId) {
	ensureLoaded([=] {
		auto contacts = QVector<MTPContact>();
		auto users = std::vector<int64>{ _selfId };
		for (const auto &[userId, chatId] : _chatByUser) {
			if (userId != _selfId) {
				contacts.push_back(MTP_contact(MTP_long(userId), MTP_boolTrue()));
				users.push_back(userId);
			}
		}
		reply(requestId, MTP_contacts_contacts(
			MTP_vector<MTPContact>(std::move(contacts)),
			MTP_int(0),
			usersToMTP(users)));
	}, failer(requestId));
}

void Backend::resolveUsername(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto username = reader.string().trimmed().toLower();
	if (!reader.ok() || username.isEmpty()) {
		replyError(requestId, 400, u"USERNAME_INVALID"_q);
		return;
	}
	const auto found = [=](int64 userId) {
		reply(requestId, MTP_contacts_resolvedPeer(
			MTP_peerUser(MTP_long(userId)),
			MTP_vector<MTPChat>(),
			usersToMTP({ userId, _selfId })));
	};
	for (const auto &[id, user] : _users) {
		if (user.uniqueName.toLower() == username) {
			found(id);
			return;
		}
	}
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(username));
	_api.get(u"/user/search?flt="_q + encoded, [=](const QJsonValue &result) {
		const auto object = result.toObject();
		const auto list = object.contains(u"users"_q)
			? object.value(u"users"_q).toArray()
			: result.toArray();
		for (const auto &entry : list) {
			const auto user = UserInfo::Parse(entry.toObject());
			if (user.id && user.uniqueName.toLower() == username) {
				applyUser(user);
				found(user.id);
				return;
			}
		}
		replyError(requestId, 400, u"USERNAME_NOT_OCCUPIED"_q);
	}, failer(requestId));
}

void Backend::contactsSearch(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto query = reader.string().trimmed();
	if (query.isEmpty()) {
		reply(requestId, MTP_contacts_found(
			MTP_vector<MTPPeer>(),
			MTP_vector<MTPPeer>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
		return;
	}
	struct State {
		int left = 2;
		std::vector<int64> users;
		std::vector<int64> chats;
	};
	const auto state = std::make_shared<State>();
	const auto finish = [=] {
		if (--state->left) {
			return;
		}
		auto mine = QVector<MTPPeer>();
		auto other = QVector<MTPPeer>();
		auto users = std::vector<int64>{ _selfId };
		auto chats = std::vector<int64>();
		for (const auto id : Unique(state->users)) {
			if (id == _selfId) {
				continue;
			}
			(_chatByUser.contains(id) ? mine : other).push_back(
				MTP_peerUser(MTP_long(id)));
			users.push_back(id);
		}
		for (const auto id : Unique(state->chats)) {
			const auto chat = chatById(id);
			if (!chat || chat->info.pm) {
				continue;
			}
			(chat->joined ? mine : other).push_back(
				MTP_peerChannel(MTP_long(id)));
			chats.push_back(id);
		}
		reply(requestId, MTP_contacts_found(
			MTP_vector<MTPPeer>(std::move(mine)),
			MTP_vector<MTPPeer>(std::move(other)),
			chatsToMTP(chats),
			usersToMTP(users)));
	};
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(query));
	_api.get(u"/user/search?flt="_q + encoded, [=](const QJsonValue &result) {
		const auto object = result.toObject();
		const auto list = object.contains(u"users"_q)
			? object.value(u"users"_q).toArray()
			: result.toArray();
		for (const auto &entry : list) {
			const auto data = entry.toObject();
			auto user = UserInfo::Parse(data);
			if (!user.id) {
				continue;
			}
			applyUser(user);
			state->users.push_back(user.id);
			const auto pmChatId = JsonInt(data, u"pm_thread_id"_q);
			if (pmChatId && !_chatByUser.contains(user.id)) {
				// Known private chat that is not loaded yet.
				loadChats({ pmChatId }, [] {}, [](const HttpError &) {});
			}
		}
		finish();
	}, [=](const HttpError &) {
		finish();
	});
	_api.get(
		u"/core/chat/search?flt=%1&type=all&visibility=all"_q.arg(encoded),
		[=](const QJsonValue &result) {
			const auto object = result.toObject();
			const auto list = object.contains(u"threads"_q)
				? object.value(u"threads"_q).toArray()
				: result.toArray();
			for (const auto &entry : list) {
				const auto info = ChatInfo::Parse(entry.toObject());
				if (!info.id || info.pm || info.e2e) {
					continue;
				}
				const auto known = (chatById(info.id) != nullptr);
				applyChat(info);
				if (!known) {
					const auto self = std::find(
						info.users.begin(),
						info.users.end(),
						_selfId);
					chatById(info.id)->joined = (self != info.users.end());
				}
				state->chats.push_back(info.id);
			}
			finish();
		},
		[=](const HttpError &) {
			finish();
		});
}

void Backend::messagesSearch(
		mtpRequestId requestId,
		Reader &reader,
		bool global) {
	const auto empty = [=] {
		reply(requestId, MTP_messages_messages(
			MTP_vector<MTPMessage>(),
			MTP_vector<MTPForumTopic>(),
			MTP_vector<MTPChat>(),
			MTP_vector<MTPUser>()));
	};
	const auto flags = reader.flags();
	auto chat = (Chat*)nullptr;
	auto query = QString();
	auto filter = MTPMessagesFilter();
	auto offset = int32(0);
	if (global) {
		if (flags & (1U << 0)) {
			[[maybe_unused]] const auto folderId = reader.int32Value();
		}
		if (flags & (1U << 4)) {
			[[maybe_unused]] const auto community = reader.read<MTPInputChannel>();
		}
		query = reader.string();
		filter = reader.read<MTPMessagesFilter>();
		[[maybe_unused]] const auto minDate = reader.int32Value();
		[[maybe_unused]] const auto maxDate = reader.int32Value();
		const auto offsetRate = reader.int32Value();
		[[maybe_unused]] const auto offsetPeer = reader.read<MTPInputPeer>();
		offset = reader.int32Value() + offsetRate;
	} else {
		chat = chatByInputPeer(reader.read<MTPInputPeer>());
		query = reader.string();
		if (flags & (1U << 0)) {
			[[maybe_unused]] const auto from = reader.read<MTPInputPeer>();
		}
		if (flags & (1U << 2)) {
			[[maybe_unused]] const auto saved = reader.read<MTPInputPeer>();
		}
		if (flags & (1U << 3)) {
			[[maybe_unused]] const auto reactions
				= reader.read<MTPVector<MTPReaction>>();
		}
		if (flags & (1U << 1)) {
			[[maybe_unused]] const auto topMsgId = reader.int32Value();
		}
		filter = reader.read<MTPMessagesFilter>();
		[[maybe_unused]] const auto minDate = reader.int32Value();
		[[maybe_unused]] const auto maxDate = reader.int32Value();
		offset = reader.int32Value();
	}
	if (reader.ok()
		&& chat
		&& !global
		&& !offset
		&& filter.type() == mtpc_inputMessagesFilterPinned) {
		const auto chatId = chat->info.id;
		auto pinned = chat->info.pinned;
		std::sort(pinned.begin(), pinned.end(), std::greater<>());
		const auto left = std::make_shared<int>(int(pinned.size()) + 1);
		const auto finish = [=] {
			if (--*left) {
				return;
			}
			const auto chat = chatById(chatId);
			if (!chat) {
				empty();
				return;
			}
			auto messages = QVector<MTPMessage>();
			auto users = std::vector<int64>{ _selfId };
			for (const auto number : pinned) {
				const auto i = chat->posts.find(number);
				if (i != end(chat->posts) && !i->second.deleted) {
					collectPeers(*chat, i->second, users);
					messages.push_back(messageToMTP(*chat, i->second));
				}
			}
			ensureUsers(users, [=] {
				reply(requestId, MTP_messages_messages(
					MTP_vector<MTPMessage>(messages),
					MTP_vector<MTPForumTopic>(),
					chatsToMTP({ chatId }),
					usersToMTP(users)));
			});
		};
		for (const auto number : pinned) {
			if (chat->posts.contains(number)) {
				finish();
			} else {
				fetchPosts(chatId, number, number, finish, [=](
						const HttpError &) {
					finish();
				});
			}
		}
		finish();
		return;
	}
	if (reader.ok()
		&& !chat
		&& filter.type() == mtpc_inputMessagesFilterPhoneCalls) {
		// The calls list: CALL events of the private chats we know.
		if (offset) {
			empty();
			return;
		}
		struct Found {
			Chat *chat = nullptr;
			const PostInfo *post = nullptr;
		};
		auto found = std::vector<Found>();
		for (const auto &[id, chat] : _chats) {
			if (!chat->info.pm) {
				continue;
			}
			for (const auto &[number, post] : chat->posts) {
				if (IsCallEvent(post) && !post.deleted) {
					found.push_back({ chat.get(), &post });
				}
			}
		}
		ranges::sort(found, std::greater<>(), [](const Found &entry) {
			return entry.post->created;
		});
		if (found.size() > kSearchLimit) {
			found.resize(kSearchLimit);
		}
		auto messages = QVector<MTPMessage>();
		auto users = std::vector<int64>{ _selfId };
		for (const auto &entry : found) {
			collectPeers(*entry.chat, *entry.post, users);
			messages.push_back(messageToMTP(*entry.chat, *entry.post));
		}
		ensureUsers(users, [=] {
			reply(requestId, MTP_messages_messages(
				MTP_vector<MTPMessage>(messages),
				MTP_vector<MTPForumTopic>(),
				MTP_vector<MTPChat>(),
				usersToMTP(users)));
		});
		return;
	}
	if (!reader.ok()
		|| filter.type() != mtpc_inputMessagesFilterEmpty
		|| query.trimmed().isEmpty()
		|| offset != 0
		|| (!global && !chat)) {
		empty();
		return;
	}
	auto url = u"/posts/search?q=%1&order_type=time&page=1&limit=%2"_q
		.arg(QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed())))
		.arg(kSearchLimit);
	if (chat) {
		url += u"&chat_ids=%1"_q.arg(chat->info.id);
	}
	_api.get(url, [=](const QJsonValue &result) {
		auto found = std::vector<std::pair<int64, int>>();
		auto users = std::vector<int64>{ _selfId };
		for (const auto &post : ParsePosts(result)) {
			const auto chat = chatById(post.chatId);
			if (!chat) {
				continue;
			}
			applyPost(*chat, post);
			found.emplace_back(post.chatId, post.postNo);
			collectPeers(*chat, post, users);
		}
		ensureUsers(users, [=] {
			auto messages = QVector<MTPMessage>();
			auto chats = std::vector<int64>();
			for (const auto &[chatId, number] : found) {
				const auto chat = chatById(chatId);
				const auto i = chat
					? chat->posts.find(number)
					: decltype(chat->posts.end())();
				if (!chat || i == end(chat->posts)) {
					continue;
				}
				messages.push_back(messageToMTP(*chat, i->second));
				if (!chat->info.pm) {
					chats.push_back(chatId);
				}
			}
			reply(requestId, MTP_messages_messages(
				MTP_vector<MTPMessage>(std::move(messages)),
				MTP_vector<MTPForumTopic>(),
				chatsToMTP(chats),
				usersToMTP(users)));
		});
	}, [=](const HttpError &) {
		empty();
	});
}

// Files.

void Backend::getUserPhotos(mtpRequestId requestId, Reader &reader) {
	const auto userId = userIdFromInputUser(reader.read<MTPInputUser>());
	const auto offset = reader.int32Value();
	auto photos = QVector<MTPPhoto>();
	if (userId && offset <= 0) {
		const auto i = _users.find(userId);
		if (i == end(_users) || !i->second.deleted) {
			photos.push_back(avatarPhoto(
				userAvatarId(userId),
				u"user:%1"_q.arg(userId)));
		}
	}
	reply(requestId, MTP_photos_photos(
		MTP_vector<MTPPhoto>(std::move(photos)),
		usersToMTP({ userId })));
}

void Backend::sendReaction(mtpRequestId requestId, Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto id = reader.int32Value();
	const auto list = (flags & (1U << 0))
		? reader.read<MTPVector<MTPReaction>>()
		: MTPVector<MTPReaction>();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	auto wanted = QString();
	for (const auto &reaction : list.v) {
		reaction.match([&](const MTPDreactionEmoji &data) {
			if (wanted.isEmpty()) {
				wanted = PararamReaction(qs(data.vemoticon()));
			}
		}, [](const auto &) {
		});
	}
	const auto chat = chatByInputPeer(peer);
	const auto number = chat ? postNo(*chat, id) : 0;
	if (!chat || !chat->posts.contains(number)) {
		replyError(requestId, 400, u"MESSAGE_ID_INVALID"_q);
		return;
	} else if (!wanted.isEmpty() && ReactionIndex(wanted) < 0) {
		replyError(requestId, 400, u"REACTION_INVALID"_q);
		return;
	}
	const auto chatId = chat->info.id;
	const auto replyCurrent = [=] {
		const auto chat = chatById(chatId);
		if (!chat || !chat->posts.contains(number)) {
			replyError(requestId, 400, u"MESSAGE_ID_INVALID"_q);
			return;
		}
		const auto &post = chat->posts[number];
		auto users = std::vector<int64>();
		collectPeers(*chat, post, users);
		reply(requestId, wrapUpdates(
			{ reactionsUpdate(*chat, post) },
			users,
			chat->info.pm ? std::vector<int64>() : std::vector{ chatId }));
	};
	const auto mine = chat->posts[number].reactions.of(_selfId);
	if (wanted == mine) {
		replyCurrent();
		return;
	}
	// The server toggles: the same reaction again removes it.
	auto body = QJsonObject();
	body.insert(u"reaction"_q, wanted.isEmpty() ? mine : wanted);
	_api.post(
		u"/msg/post/%1/%2/reaction"_q.arg(chatId).arg(number),
		body,
		[=](const QJsonValue &result) {
			if (const auto chat = chatById(chatId)) {
				const auto i = chat->posts.find(number);
				if (i != end(chat->posts)) {
					const auto object = result.toObject();
					const auto mine = object.contains(u"my_reaction"_q)
						? PararamReaction(
							JsonString(object, u"my_reaction"_q))
						: wanted;
					i->second.reactions.set(_selfId, mine);
				}
			}
			replyCurrent();
		},
		failer(requestId));
}

void Backend::getMessagesReactions(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputPeer>();
	const auto ids = reader.read<MTPVector<MTPint>>();
	const auto chat = reader.ok() ? chatByInputPeer(peer) : nullptr;
	if (!chat) {
		reply(requestId, wrapUpdates({}, {}, {}));
		return;
	}
	auto numbers = std::vector<int>();
	for (const auto &id : ids.v) {
		const auto number = postNo(*chat, id.v);
		if (number > 0 && chat->posts.contains(number)) {
			numbers.push_back(number);
		}
	}
	if (numbers.empty()) {
		reply(requestId, wrapUpdates({}, {}, {}));
		return;
	}
	ranges::sort(numbers);
	const auto till = numbers.back();
	const auto from = std::max(numbers.front(), till - kEditCheckDepth * 4);
	const auto chatId = chat->info.id;
	// Older posts are not refreshed, their known state is replied.
	auto old = base::flat_map<int, PostInfo>();
	for (const auto number : numbers) {
		old.emplace(number, chat->posts[number]);
	}
	const auto finish = [=] {
		const auto chat = chatById(chatId);
		if (!chat) {
			reply(requestId, wrapUpdates({}, {}, {}));
			return;
		}
		// The UI clears reactions of the messages missing in the reply.
		auto updates = QVector<MTPUpdate>();
		auto users = std::vector<int64>();
		collectChanges(*chat, old, updates, users, true);
		const auto chats = chat->info.pm
			? std::vector<int64>()
			: std::vector{ chatId };
		ensureUsers(users, [=] {
			reply(requestId, wrapUpdates(updates, users, chats));
		});
	};
	fetchPosts(chatId, from, till, finish, [=](const HttpError &) {
		finish();
	});
}

void Backend::getMessageReactionsList(
		mtpRequestId requestId,
		Reader &reader) {
	const auto flags = reader.flags();
	const auto peer = reader.read<MTPInputPeer>();
	const auto id = reader.int32Value();
	const auto filter = (flags & (1U << 0))
		? std::make_optional(reader.read<MTPReaction>())
		: std::nullopt;
	if (flags & (1U << 1)) {
		[[maybe_unused]] const auto offset = reader.string();
	}
	[[maybe_unused]] const auto limit = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	auto only = QString();
	if (filter) {
		filter->match([&](const MTPDreactionEmoji &data) {
			only = PararamReaction(qs(data.vemoticon()));
		}, [](const auto &) {
		});
	}
	const auto chat = chatByInputPeer(peer);
	const auto number = chat ? postNo(*chat, id) : 0;
	if (!chat || !chat->posts.contains(number)) {
		replyError(requestId, 400, u"MESSAGE_ID_INVALID"_q);
		return;
	}
	const auto &post = chat->posts[number];
	auto list = QVector<MTPMessagePeerReaction>();
	auto users = std::vector<int64>();
	for (const auto &[emoji, reacted] : post.reactions.list) {
		if (!only.isEmpty() && emoji != only) {
			continue;
		}
		for (const auto userId : reacted) {
			using Flag = MTPDmessagePeerReaction::Flag;
			list.push_back(MTP_messagePeerReaction(
				MTP_flags((userId == _selfId) ? Flag::f_my : Flag()),
				MTP_peerUser(MTP_long(userId)),
				MTP_int(post.created ? post.created : Now()),
				ReactionToMTP(emoji)));
			users.push_back(userId);
		}
	}
	ensureUsers(users, [=] {
		reply(requestId, MTP_messages_messageReactionsList(
			MTP_flags(0),
			MTP_int(list.size()),
			MTP_vector<MTPMessagePeerReaction>(list),
			MTP_vector<MTPChat>(),
			usersToMTP(users),
			MTPstring()));
	});
}

void Backend::getMessageReadParticipants(
		mtpRequestId requestId,
		Reader &reader) {
	const auto peer = reader.read<MTPInputPeer>();
	const auto id = reader.int32Value();
	const auto chat = reader.ok() ? chatByInputPeer(peer) : nullptr;
	const auto number = chat ? postNo(*chat, id) : 0;
	if (!chat || number <= 0) {
		replyError(requestId, 400, u"MSG_ID_INVALID"_q);
		return;
	}
	// { "<user id>": "<time read>", ... }
	_api.get(
		u"/activity/who-read?thread_id=%1&post_no=%2"_q
			.arg(chat->info.id)
			.arg(number),
		[=](const QJsonValue &result) {
			auto list = QVector<MTPReadParticipantDate>();
			const auto object = result.toObject();
			for (auto i = object.begin(); i != object.end(); ++i) {
				const auto userId = i.key().toLongLong();
				if (!userId || userId == _selfId) {
					continue;
				}
				const auto date = ParseTime(i.value().toString());
				list.push_back(MTP_readParticipantDate(
					MTP_long(userId),
					MTP_int(date ? date : Now())));
			}
			reply(requestId, MTP_vector<MTPReadParticipantDate>(list));
		},
		failer(requestId));
}

void Backend::getOutboxReadDate(mtpRequestId requestId, Reader &reader) {
	const auto peer = reader.read<MTPInputPeer>();
	const auto id = reader.int32Value();
	const auto chat = reader.ok() ? chatByInputPeer(peer) : nullptr;
	const auto number = chat ? postNo(*chat, id) : 0;
	if (!chat || !chat->info.pm || number <= 0) {
		replyError(requestId, 400, u"MSG_ID_INVALID"_q);
		return;
	}
	const auto other = chat->pmUserId;
	_api.get(
		u"/activity/who-read?thread_id=%1&post_no=%2"_q
			.arg(chat->info.id)
			.arg(number),
		[=](const QJsonValue &result) {
			const auto object = result.toObject();
			const auto value = object.value(QString::number(other));
			if (value.isUndefined() || value.isNull()) {
				replyError(requestId, 400, u"MESSAGE_NOT_READ_YET"_q);
				return;
			}
			const auto date = ParseTime(value.toString());
			reply(requestId, MTP_outboxReadDate(MTP_int(date ? date : Now())));
		},
		failer(requestId));
}

void Backend::getFile(mtpRequestId requestId, Reader &reader) {
	[[maybe_unused]] const auto flags = reader.flags();
	const auto location = reader.read<MTPInputFileLocation>();
	const auto offset = int64(reader.int64Value());
	const auto limit = reader.int32Value();
	if (!reader.ok()) {
		replyError(requestId, 400, u"INPUT_REQUEST_INVALID"_q);
		return;
	}
	const auto avatar = [&](const QString &key, bool big) {
		const auto colon = key.indexOf(':');
		const auto id = key.mid(colon + 1).toLongLong();
		if (key.startsWith(u"user:"_q)) {
			return userAvatarPath(id, big);
		} else if (const auto chat = chatById(id)) {
			return chatAvatarPath(*chat, big);
		}
		return QString();
	};
	auto id = uint64();
	auto thumb = QString();
	auto path = QString();
	location.match([&](const MTPDinputDocumentFileLocation &data) {
		id = data.vid().v;
		thumb = qs(data.vthumb_size());
	}, [&](const MTPDinputPhotoFileLocation &data) {
		id = data.vid().v;
		thumb = qs(data.vthumb_size());
		if (const auto i = _avatarPaths.find(id); i != end(_avatarPaths)) {
			path = avatar(i->second, (thumb != u"a"_q));
		}
	}, [&](const MTPDinputPeerPhotoFileLocation &data) {
		const auto big = data.is_big();
		data.vpeer().match([&](const MTPDinputPeerChannel &data) {
			path = avatar(
				u"chat:%1"_q.arg(data.vchannel_id().v),
				big);
		}, [&](const MTPDinputPeerChannelFromMessage &data) {
			path = avatar(
				u"chat:%1"_q.arg(data.vchannel_id().v),
				big);
		}, [&](const auto &) {
			if (const auto userId = userIdFromInputPeer(data.vpeer())) {
				path = userAvatarPath(userId, big);
			}
		});
	}, [](const auto &) {
	});
	if (!path.isEmpty()) {
		servePath(requestId, path, offset, limit);
		return;
	}
	if (const auto i = _generatedFiles.find(id); i != end(_generatedFiles)) {
		const auto &bytes = i->second;
		const auto from = std::clamp(offset, int64(0), int64(bytes.size()));
		reply(requestId, MTP_upload_file(
			MTP_storage_filePng(),
			MTP_int(0),
			MTP_bytes(bytes.mid(int(from), std::max(limit, 0)))));
		return;
	}
	const auto i = _documents.find(id);
	if (!id || i == end(_documents)) {
		replyError(requestId, 400, u"LOCATION_INVALID"_q);
		return;
	}
	const auto preview = (thumb == u"m"_q) && i->second.isImage();
	servePath(requestId, FilePath(i->second, preview), offset, limit);
}

void Backend::loadPath(const QString &path, Fn<void(QByteArray)> done) {
	auto &download = (*_downloads)[path];
	download.lastUsed = crl::now();
	if (download.loaded) {
		done(download.bytes);
		return;
	}
	download.callbacks.push_back(std::move(done));
	if (download.loading) {
		return;
	}
	download.loading = true;
	const auto finish = [=](QByteArray bytes, bool loaded) {
		const auto i = _downloads->find(path);
		if (i == end(*_downloads)) {
			return;
		}
		auto callbacks = base::take(i->second.callbacks);
		if (loaded) {
			auto &download = i->second;
			download.loading = false;
			download.loaded = true;
			download.bytes = bytes;
			_cachedBytes += bytes.size();
		} else {
			_downloads->erase(i);
		}
		for (const auto &callback : callbacks) {
			callback(bytes);
		}
		if (loaded) {
			trimDownloads(path);
		}
	};
	_api.downloadPath(path, [=](const QByteArray &bytes) {
		finish(bytes, true);
	}, [=](const HttpError &error) {
		LOG(("Pararam: could not load %1 (%2)").arg(path).arg(error.status));
		finish(QByteArray(), false);
	});
}

void Backend::servePath(
		mtpRequestId requestId,
		const QString &path,
		int64 offset,
		int limit) {
	loadPath(path, [=](const QByteArray &bytes) {
		if (bytes.isEmpty()) {
			replyError(requestId, 400, u"LOCATION_INVALID"_q);
			return;
		}
		const auto from = std::clamp(offset, int64(0), int64(bytes.size()));
		reply(requestId, MTP_upload_file(
			MTP_storage_fileUnknown(),
			MTP_int(0),
			MTP_bytes(bytes.mid(int(from), std::max(limit, 0)))));
	});
}

void Backend::trimDownloads(const QString &keep) {
	while (_cachedBytes > kMaxCachedBytes) {
		auto oldest = end(*_downloads);
		for (auto i = begin(*_downloads); i != end(*_downloads); ++i) {
			if (i->second.loaded
				&& i->first != keep
				&& (oldest == end(*_downloads)
					|| i->second.lastUsed < oldest->second.lastUsed)) {
				oldest = i;
			}
		}
		if (oldest == end(*_downloads)) {
			return;
		}
		_cachedBytes -= oldest->second.bytes.size();
		_downloads->erase(oldest);
	}
}

void Backend::saveFilePart(mtpRequestId requestId, Reader &reader, bool big) {
	const auto fileId = reader.int64Value();
	const auto part = reader.int32Value();
	if (big) {
		[[maybe_unused]] const auto total = reader.int32Value();
	}
	const auto bytes = reader.bytes();
	if (!reader.ok() || part < 0) {
		replyError(requestId, 400, u"FILE_PART_INVALID"_q);
		return;
	}
	(*_uploads)[fileId].parts[part] = bytes;
	replyTrue(requestId);
}

} // namespace Pararam

namespace Pararam {

// Used by CallManager.
template void Backend::reply<MTPdataJSON>(
	mtpRequestId,
	const MTPdataJSON&);
template void Backend::reply<MTPmessages_dhConfig>(
	mtpRequestId,
	const MTPmessages_dhConfig&);
template void Backend::reply<MTPphone_phoneCall>(
	mtpRequestId,
	const MTPphone_phoneCall&);
template void Backend::reply<MTPUpdates>(
	mtpRequestId,
	const MTPUpdates&);
template void Backend::reply<MTPphone_groupCall>(
	mtpRequestId,
	const MTPphone_groupCall&);
template void Backend::reply<MTPphone_groupParticipants>(
	mtpRequestId,
	const MTPphone_groupParticipants&);
template void Backend::reply<MTPphone_joinAsPeers>(
	mtpRequestId,
	const MTPphone_joinAsPeers&);
template void Backend::reply<MTPVector<MTPint>>(
	mtpRequestId,
	const MTPVector<MTPint>&);

} // namespace Pararam
