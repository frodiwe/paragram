/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "pararam/pararam_api.h"
#include "pararam/pararam_data.h"
#include "mtproto/core_types.h"
#include "base/flat_map.h"
#include "base/flat_set.h"

#include <QtCore/QObject>
#include <QtCore/QSize>
#include <QtCore/QTimer>

#include <deque>
#include <functional>
#include <memory>
#include <optional>

namespace MTP {
class Instance;
} // namespace MTP

namespace Pararam {

class Reader;
class CallManager;

// Translates MTProto requests issued by the Telegram Desktop UI
// into pararam.io REST API calls and produces MTProto responses
// and updates from pararam.io data. No Telegram servers are involved.
//
// Mapping:
// - pararam users          -> Telegram users (same ids).
// - pararam private chats  -> Telegram private chats with the other user.
// - pararam group chats    -> Telegram supergroups (same ids),
//                             read only chats forbid sending to non-admins.
// - pararam post numbers   -> message ids (supergroups: post_no,
//                             private chats: slot << 20 | post_no).
// - pararam favorites      -> pinned dialogs.
// - pararam hidden chats   -> archived dialogs.
class Backend final : public QObject {
public:
	explicit Backend(not_null<MTP::Instance*> instance);
	~Backend();

	// Returns the backend serving the given MTP instance.
	// Every MTP instance in Pararam Desktop has one.
	[[nodiscard]] static Backend *For(not_null<const MTP::Instance*> instance);

	// Main thread. Body is the serialized TL request without the header.
	void handleRequest(
		mtpRequestId requestId,
		mtpBuffer &&body,
		mtpRequestId afterRequestId);
	void cancelRequest(mtpRequestId requestId);

	void login(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult, MTPUser)> done);

	[[nodiscard]] bool authorized() const;
	[[nodiscard]] QByteArray serialize() const;
	void restore(const QByteArray &serialized);
	void setSessionChangedCallback(std::function<void()> callback);

	[[nodiscard]] Api &api() {
		return _api;
	}

	[[nodiscard]] static mtpBuffer ErrorBuffer(int code, const QString &type);

private:
	friend class CallManager;

	struct Download;
	struct Upload;
	struct HistoryQuery;
	using Done = std::function<void()>;
	using Fail = std::function<void(const HttpError&)>;
	using UpdatesDone = std::function<void(
		QVector<MTPUpdate> updates,
		std::vector<int64> users)>;

	struct LoggedUpdate {
		int32 pts = 0;
		int32 ptsCount = 0;
		MTPUpdate update;
		std::vector<int64> users;
		std::vector<int64> chats;
	};

	struct Chat {
		ChatInfo info;
		int64 pmUserId = 0;
		int slot = 0;
		int32 pts = 1;
		int fetchedPostNo = 0;
		bool joined = true;
		base::flat_map<int, PostInfo> posts;
		base::flat_set<int> sentByUs;
		std::deque<LoggedUpdate> log;
		int sending = 0;
		bool refreshAfterSend = false;
		bool delivering = false;
		bool deliverAgain = false;
		bool checkingEdits = false;
		int readOutbox = 0; // The last post read by somebody else.
		crl::time readOutboxChecked = 0;
		bool readOutboxLoading = false;
	};

	void sessionChanged();
	void resetState();

	// Requests.
	void dispatch(
		mtpRequestId requestId,
		mtpTypeId type,
		Reader &reader,
		const mtpBuffer &body);
	void runAfter(mtpRequestId requestId);
	void finishRequest(mtpRequestId requestId, mtpBuffer &&reply);
	template <typename Type>
	void reply(mtpRequestId requestId, const Type &value);
	void replyError(
		mtpRequestId requestId,
		int code,
		const QString &type);
	void replyHttpError(mtpRequestId requestId, const HttpError &error);
	void replyTrue(mtpRequestId requestId);
	[[nodiscard]] Fail failer(mtpRequestId requestId);
	void pushUpdates(const MTPUpdates &updates);

	void replyConfig(mtpRequestId requestId);
	void replyNearestDc(mtpRequestId requestId);
	void replyAppConfig(mtpRequestId requestId);
	void logOut(mtpRequestId requestId);

	// Session data.
	void ensureLoaded(Done done, Fail fail);
	void loadChatList(Done done, Fail fail);
	void loadChats(const std::vector<int64> &ids, Done done, Fail fail);
	void loadLastPosts(const std::vector<int64> &chatIds, Done done);
	void ensureUsers(const std::vector<int64> &ids, Done done);
	void applyUser(const UserInfo &user);
	void applyChat(const ChatInfo &info);
	void applyPost(Chat &chat, const PostInfo &post);
	void fetchPosts(int64 chatId, int from, int till, Done done, Fail fail);

	// Ids.
	[[nodiscard]] Chat *chatById(int64 chatId);
	[[nodiscard]] Chat *chatByUser(int64 userId);
	[[nodiscard]] Chat *chatByCommonMsgId(int32 msgId);
	[[nodiscard]] Chat *chatByInputPeer(const MTPInputPeer &peer);
	[[nodiscard]] Chat *chatByInputChannel(const MTPInputChannel &channel);
	[[nodiscard]] int64 userIdFromInputPeer(const MTPInputPeer &peer) const;
	[[nodiscard]] int64 userIdFromInputUser(const MTPInputUser &user) const;
	[[nodiscard]] int32 msgId(const Chat &chat, int postNo) const;
	[[nodiscard]] int postNo(const Chat &chat, int32 msgId) const;
	[[nodiscard]] int assignSlot(int64 chatId);
	[[nodiscard]] int64 otherUser(const ChatInfo &info) const;
	void withChatForPeer(
		const MTPInputPeer &peer,
		std::function<void(Chat*)> done,
		Fail fail,
		bool create);
	[[nodiscard]] uint64 documentId(const FileInfo &file);
	[[nodiscard]] int replyToPost(
		const Chat &chat,
		const MTPInputReplyTo &replyTo) const;

	// Conversion.
	[[nodiscard]] MTPUser userToMTP(int64 userId) const;
	[[nodiscard]] bool isAdmin(const Chat &chat) const;
	[[nodiscard]] MTPChat chatToMTP(const Chat &chat) const;
	[[nodiscard]] MTPPeer peerToMTP(const Chat &chat) const;
	[[nodiscard]] MTPInputPeer inputPeerToMTP(const Chat &chat) const;
	[[nodiscard]] MTPMessageMedia mediaToMTP(const FileInfo &original);
	void ensureImageSizes(std::vector<FileInfo> files, Done done);
	void probeImageByPreview(const FileInfo &file, Fn<void(QSize)> done);
	[[nodiscard]] static QString FilePath(
		const FileInfo &file,
		bool preview);

	// Avatars.
	[[nodiscard]] uint64 userAvatarId(int64 userId) const;
	[[nodiscard]] uint64 chatAvatarId(const Chat &chat) const;
	[[nodiscard]] QString userAvatarPath(int64 userId, bool big) const;
	[[nodiscard]] QString chatAvatarPath(const Chat &chat, bool big) const;
	[[nodiscard]] MTPPhoto avatarPhoto(uint64 photoId, const QString &key);

	// Reactions.
	[[nodiscard]] MTPMessageReactions reactionsToMTP(
		const PostInfo &post) const;
	[[nodiscard]] MTPUpdate reactionsUpdate(
		const Chat &chat,
		const PostInfo &post) const;
	[[nodiscard]] MTPDocument reactionDocument(int index);
	[[nodiscard]] MTPmessages_AvailableReactions availableReactions();
	[[nodiscard]] MTPmessages_Reactions reactionsList() const;
	void collectChanges(
		Chat &chat,
		const base::flat_map<int, PostInfo> &old,
		QVector<MTPUpdate> &updates,
		std::vector<int64> &users,
		bool allReactions = false);
	[[nodiscard]] std::vector<FileInfo> filesOf(const PostInfo &post) const;
	[[nodiscard]] MTPMessage messageToMTP(
		const Chat &chat,
		const PostInfo &post);
	[[nodiscard]] MTPPeerNotifySettings notifySettings(const Chat &chat) const;
	[[nodiscard]] const PostInfo *topPost(const Chat &chat) const;
	[[nodiscard]] int unreadCount(const Chat &chat) const;
	[[nodiscard]] MTPDialog dialogToMTP(const Chat &chat) const;
	[[nodiscard]] MTPVector<MTPUser> usersToMTP(
		const std::vector<int64> &ids) const;
	[[nodiscard]] MTPVector<MTPChat> chatsToMTP(
		const std::vector<int64> &ids) const;
	[[nodiscard]] MTPupdates_State stateToMTP() const;
	[[nodiscard]] MTPChannelParticipant participantToMTP(
		const Chat &chat,
		int64 userId) const;
	void collectPeers(
		const Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users) const;
	[[nodiscard]] FormattedText textFromMTP(
		const QString &text,
		const MTPVector<MTPMessageEntity> *entities) const;
	void dialogsData(
		const std::vector<Chat*> &list,
		QVector<MTPDialog> &dialogs,
		QVector<MTPMessage> &messages,
		std::vector<int64> &users,
		std::vector<int64> &chats);

	// Updates.
	void logUpdate(
		Chat *chat,
		MTPUpdate update,
		int32 pts,
		int32 ptsCount,
		std::vector<int64> users);
	[[nodiscard]] MTPUpdate newMessageUpdate(
		Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users);
	[[nodiscard]] MTPUpdate editMessageUpdate(
		Chat &chat,
		const PostInfo &post,
		std::vector<int64> &users);
	[[nodiscard]] MTPUpdate deleteMessagesUpdate(
		Chat &chat,
		const std::vector<int> &postNos);
	[[nodiscard]] MTPUpdates wrapUpdates(
		QVector<MTPUpdate> updates,
		std::vector<int64> users,
		const std::vector<int64> &chats) const;
	void pushReadUpdate(Chat &chat);

	// Polling.
	void schedulePoll(crl::time delay);
	void pollFinished(crl::time delay);
	void poll();
	void pollByChats();
	void applySync(const QJsonObject &result);
	void introduceChat(int64 chatId);
	void refreshChat(int64 chatId, int postsCount, int lastRead);
	void deliverNewPosts(int64 chatId, int till);
	void checkEdits(int64 chatId);
	void postEdited(int64 chatId, int postNo); // From the main socket.

	// Read receipts, from /msg/lastread and the main socket.
	void refreshReadOutbox(
		int64 chatId,
		bool force = false,
		Done done = nullptr);
	void refreshReadOutboxes(std::vector<int64> chatIds);
	void applyReadOutbox(int64 chatId, int postNo);
	[[nodiscard]] bool waitsForRead(const Chat &chat) const;

	// Handlers.
	void updateNotifySettings(mtpRequestId requestId, Reader &reader);
	void getState(mtpRequestId requestId);
	void getDifference(mtpRequestId requestId, Reader &reader);
	void getChannelDifference(mtpRequestId requestId, Reader &reader);
	void getDialogs(mtpRequestId requestId, Reader &reader);
	void getPinnedDialogs(mtpRequestId requestId, Reader &reader);
	void getPeerDialogs(mtpRequestId requestId, Reader &reader);
	void toggleDialogPin(mtpRequestId requestId, Reader &reader);
	void editPeerFolders(mtpRequestId requestId, Reader &reader);
	void getDialogFilters(mtpRequestId requestId);
	void updateDialogFilter(mtpRequestId requestId, Reader &reader);
	void getHistory(mtpRequestId requestId, Reader &reader);
	void loadHistory(
		mtpRequestId requestId,
		std::shared_ptr<HistoryQuery> query);
	void replyHistory(
		mtpRequestId requestId,
		std::shared_ptr<HistoryQuery> query);
	void getMessages(mtpRequestId requestId, Reader &reader, bool channel);
	void sendMessage(mtpRequestId requestId, Reader &reader);
	void sendMedia(mtpRequestId requestId, Reader &reader);
	void sendMultiMedia(mtpRequestId requestId, Reader &reader);
	void forwardMessages(mtpRequestId requestId, Reader &reader);
	void editMessage(mtpRequestId requestId, Reader &reader);
	void deleteMessages(mtpRequestId requestId, Reader &reader, bool channel);
	void readHistory(mtpRequestId requestId, Reader &reader, bool channel);
	void getUsers(mtpRequestId requestId, Reader &reader);
	void getFullUser(mtpRequestId requestId, Reader &reader);
	void getChannels(mtpRequestId requestId, Reader &reader);
	void getFullChannel(mtpRequestId requestId, Reader &reader);
	void getParticipants(mtpRequestId requestId, Reader &reader);
	void getParticipant(mtpRequestId requestId, Reader &reader);
	void joinChannel(mtpRequestId requestId, Reader &reader, bool join);
	void getContacts(mtpRequestId requestId);
	void contactsSearch(mtpRequestId requestId, Reader &reader);
	void resolveUsername(mtpRequestId requestId, Reader &reader);
	void messagesSearch(mtpRequestId requestId, Reader &reader, bool global);
	void getFile(mtpRequestId requestId, Reader &reader);
	void getUserPhotos(mtpRequestId requestId, Reader &reader);
	void sendReaction(mtpRequestId requestId, Reader &reader);
	void getMessagesReactions(mtpRequestId requestId, Reader &reader);
	void getMessageReactionsList(mtpRequestId requestId, Reader &reader);
	void getMessageReadParticipants(mtpRequestId requestId, Reader &reader);
	void getOutboxReadDate(mtpRequestId requestId, Reader &reader);
	void saveFilePart(mtpRequestId requestId, Reader &reader, bool big);

	void createPost(
		int64 chatId,
		const QString &text,
		int replyNo,
		uint64 randomId,
		std::vector<QString> attachments,
		std::optional<FileInfo> file,
		UpdatesDone done,
		Fail fail);
	[[nodiscard]] QByteArray takeUploaded(
		const MTPInputFile &file,
		QString *name);
	void uploadMedia(
		int64 chatId,
		const MTPInputMedia &media,
		const QString &caption,
		int replyNo,
		uint64 randomId,
		UpdatesDone done,
		Fail fail);
	void servePath(
		mtpRequestId requestId,
		const QString &path,
		int64 offset,
		int limit);
	void loadPath(const QString &path, Fn<void(QByteArray)> done);
	void trimDownloads(const QString &keep);

	const not_null<MTP::Instance*> _instance;
	Api _api;

	int64 _selfId = 0;
	UserInfo _self;
	bool _loaded = false;
	bool _loading = false;
	std::vector<Done> _loadedWaiters;
	std::vector<Fail> _loadFailWaiters;

	base::flat_map<int64, UserInfo> _users;
	base::flat_set<int64> _usersLoading;
	base::flat_set<int64> _usersFailed;
	base::flat_map<int64, std::unique_ptr<Chat>> _chats;
	base::flat_map<int64, int64> _chatByUser;
	base::flat_map<int64, int> _slots;
	base::flat_map<int64, bool> _localMute;
	base::flat_map<QString, std::vector<int64>> _tags;
	base::flat_map<int, QString> _tagById;
	base::flat_map<int, int64> _chatBySlot;
	base::flat_map<uint64, FileInfo> _documents;
	base::flat_map<QString, uint64> _documentByGuid;
	base::flat_map<QString, QSize> _imageSizes;
	base::flat_map<uint64, QString> _avatarPaths; // Photo id -> avatar key.
	base::flat_map<uint64, QByteArray> _generatedFiles;
	base::flat_map<QString, std::vector<Done>> _imageProbeWaiters;

	int32 _commonPts = 1;
	std::deque<LoggedUpdate> _commonLog;

	base::flat_set<mtpRequestId> _pending;
	base::flat_set<mtpRequestId> _cancelled;
	base::flat_map<mtpRequestId, std::vector<std::function<void()>>> _after;
	base::flat_set<QString> _sendingUuids;

	std::unique_ptr<base::flat_map<QString, Download>> _downloads;
	std::unique_ptr<base::flat_map<uint64, Upload>> _uploads;
	int64 _cachedBytes = 0;

	QTimer _pollTimer;
	bool _polling = false;
	bool _syncBroken = false;
	crl::time _lastPoll = 0;
	QString _syncTime;

	std::function<void()> _sessionChanged;

	std::unique_ptr<CallManager> _calls;

};

} // namespace Pararam
