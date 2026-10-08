// Integration test of Pararam::Backend against pararam/mock_server.py.
//
// The backend is driven exactly like Telegram Desktop drives it: requests
// are serialized with the generated TL classes and responses are parsed
// with the generated response types. See pararam/tests/run_backend_test.sh.
#include "pararam/pararam_backend.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "base/unixtime.h"

#include <QtCore/QDateTime>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <cstdio>

// The backend only calls these two methods of MTP::Instance.
namespace {

std::map<mtpRequestId, mtpBuffer> Responses;
std::vector<mtpBuffer> Updates;

} // namespace

namespace MTP {

void Instance::processCallback(const Response &response) {
	Responses[response.requestId] = response.reply;
}

void Instance::processUpdate(const Response &message) {
	Updates.push_back(message.reply);
}

} // namespace MTP

// Minimal replacements for the parts of lib_base the backend uses.
namespace base {

void LogWriteMain(const QString &message) {
	std::printf("[log] %s\n", message.toUtf8().constData());
}

namespace assertion {

void log(const char *message, const char *file, int line) {
	std::printf("[assert] %s at %s:%d\n", message, file, line);
}

} // namespace assertion

namespace unixtime {

TimeId now() {
	return TimeId(QDateTime::currentSecsSinceEpoch());
}

} // namespace unixtime
} // namespace base

namespace {

int Failures = 0;
mtpRequestId LastRequestId = 0;
std::unique_ptr<Pararam::Backend> Backend;

void Check(bool condition, const char *what, int line) {
	if (!condition) {
		std::printf("FAIL line %d: %s\n", line, what);
		++Failures;
	}
}

#define CHECK(x) Check((x), #x, __LINE__)

void Wait(Fn<bool()> condition, int timeout = 10000) {
	auto timer = QElapsedTimer();
	timer.start();
	while (!condition() && timer.elapsed() < timeout) {
		QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
	}
}

template <typename Request>
mtpBuffer CallRaw(const Request &request) {
	auto body = mtpBuffer();
	request.write(body);
	const auto id = ++LastRequestId;
	Backend->handleRequest(id, std::move(body), 0);
	Wait([&] { return Responses.contains(id); });
	auto result = Responses.contains(id) ? Responses[id] : mtpBuffer();
	Responses.erase(id);
	return result;
}

bool IsError(const mtpBuffer &buffer, QString *type = nullptr) {
	if (buffer.isEmpty() || mtpTypeId(buffer[0]) != mtpc_rpc_error) {
		return false;
	}
	auto error = MTPRpcError();
	auto from = buffer.constData();
	if (error.read(from, from + buffer.size()) && type) {
		*type = qs(error.c_rpc_error().verror_message());
	}
	return true;
}

template <typename Result, typename Request>
std::optional<Result> Call(const Request &request, const char *name) {
	const auto buffer = CallRaw(request);
	auto type = QString();
	if (buffer.isEmpty()) {
		std::printf("FAIL %s: no response\n", name);
		++Failures;
		return std::nullopt;
	} else if (IsError(buffer, &type)) {
		std::printf("FAIL %s: error %s\n", name, type.toUtf8().constData());
		++Failures;
		return std::nullopt;
	}
	auto result = Result();
	auto from = buffer.constData();
	const auto end = from + buffer.size();
	if (!result.read(from, end) || from != end) {
		std::printf("FAIL %s: could not parse the response\n", name);
		++Failures;
		return std::nullopt;
	}
	std::printf("ok   %s\n", name);
	return result;
}

void Mock(const QString &path, const QByteArray &body) {
	static auto manager = QNetworkAccessManager();
	auto request = QNetworkRequest(QUrl(qEnvironmentVariable("PARARAM_API_URL") + path));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	const auto reply = manager.post(request, body);
	Wait([&] { return reply->isFinished(); });
	reply->deleteLater();
}

MTPInputPeer Channel(int64 id) {
	return MTP_inputPeerChannel(MTP_long(id), MTP_long(0));
}

MTPInputChannel InputChannel(int64 id) {
	return MTP_inputChannel(MTP_long(id), MTP_long(0));
}

MTPInputPeer User(int64 id) {
	return MTP_inputPeerUser(MTP_long(id), MTP_long(0));
}

QVector<MTPMessage> MessagesOf(const MTPmessages_Messages &result) {
	return result.match([](const MTPDmessages_messagesNotModified &) {
		return QVector<MTPMessage>();
	}, [](const auto &data) {
		return data.vmessages().v;
	});
}

int32 IdOf(const MTPMessage &message) {
	return message.match([](const auto &data) { return data.vid().v; });
}

QString TextOf(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) {
		return qs(data.vmessage());
	}, [](const auto &) {
		return QString();
	});
}

template <typename Update>
std::vector<MTPUpdate> UpdatesOf(const MTPUpdates &updates) {
	auto result = std::vector<MTPUpdate>();
	updates.match([&](const MTPDupdates &data) {
		for (const auto &update : data.vupdates().v) {
			result.push_back(update);
		}
	}, [](const auto &) {
	});
	return result;
}

} // namespace

int main(int argc, char *argv[]) {
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);

	alignas(64) static char fakeInstance[4096] = { 0 };
	const auto instance = reinterpret_cast<MTP::Instance*>(fakeInstance);
	Backend = std::make_unique<Pararam::Backend>(instance);
	CHECK(Pararam::Backend::For(instance) == Backend.get());

	// Before login only configuration requests work.
	CHECK(Call<MTPConfig>(MTPhelp_GetConfig(), "help.getConfig").has_value());
	{
		auto type = QString();
		CHECK(IsError(CallRaw(MTPupdates_GetState()), &type));
		CHECK(type == u"AUTH_KEY_UNREGISTERED"_q);
	}

	// Login with a wrong password, then 2FA, then a plain login.
	{
		auto status = std::optional<Pararam::LoginResult::Status>();
		Backend->login(u"alice@example.com"_q, u"wrong"_q, QString(), [&](
				Pararam::LoginResult result,
				MTPUser) {
			status = result.status;
		});
		Wait([&] { return status.has_value(); });
		CHECK(status == Pararam::LoginResult::Status::Failed);
	}
	{
		auto status = std::optional<Pararam::LoginResult::Status>();
		Backend->login(u"bob@example.com"_q, u"password"_q, QString(), [&](
				Pararam::LoginResult result,
				MTPUser) {
			status = result.status;
		});
		Wait([&] { return status.has_value(); });
		CHECK(status == Pararam::LoginResult::Status::NeedTwoFactorCode);
		status = std::nullopt;
		auto self = MTPUser();
		Backend->login(u"bob@example.com"_q, u"password"_q, u"123456"_q, [&](
				Pararam::LoginResult result,
				MTPUser user) {
			status = result.status;
			self = user;
		});
		Wait([&] { return status.has_value(); });
		CHECK(status == Pararam::LoginResult::Status::Success);
		CHECK(self.type() == mtpc_user && self.c_user().vid().v == 2);
	}
	{
		auto status = std::optional<Pararam::LoginResult::Status>();
		auto self = MTPUser();
		Backend->login(u"alice@example.com"_q, u"password"_q, QString(), [&](
				Pararam::LoginResult result,
				MTPUser user) {
			status = result.status;
			self = user;
		});
		Wait([&] { return status.has_value(); });
		CHECK(status == Pararam::LoginResult::Status::Success);
		CHECK(self.type() == mtpc_user);
		CHECK(self.c_user().is_self());
		CHECK(qs(self.c_user().vfirst_name().value_or_empty())
			== u"Alice Example"_q);
	}

	// Session persistence.
	{
		const auto serialized = Backend->serialize();
		CHECK(!serialized.isEmpty());
		alignas(64) static char otherInstance[4096] = { 0 };
		auto restored = Pararam::Backend(
			reinterpret_cast<MTP::Instance*>(otherInstance));
		restored.restore(serialized);
		CHECK(restored.authorized());
	}

	const auto state = Call<MTPupdates_State>(
		MTPupdates_GetState(),
		"updates.getState");
	CHECK(state.has_value());

	// Dialogs.
	using DialogsFlag = MTPmessages_GetDialogs::Flag;
	const auto dialogs = Call<MTPmessages_Dialogs>(MTPmessages_GetDialogs(
		MTP_flags(DialogsFlag::f_exclude_pinned | DialogsFlag::f_folder_id),
		MTP_int(0),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTP_int(100),
		MTP_long(0)), "messages.getDialogs");
	if (dialogs) {
		CHECK(dialogs->type() == mtpc_messages_dialogs);
		const auto &data = dialogs->c_messages_dialogs();
		auto peers = QStringList();
		auto folders = 0;
		for (const auto &dialog : data.vdialogs().v) {
			dialog.match([&](const MTPDdialog &data) {
				data.vpeer().match([&](const MTPDpeerChannel &peer) {
					peers.push_back(u"channel%1"_q.arg(peer.vchannel_id().v));
					if (peer.vchannel_id().v == 10) {
						// 125 posts, read till 100, one deleted.
						CHECK(data.vunread_count().v == 25);
						CHECK(data.vtop_message().v == 125);
						CHECK(data.vread_inbox_max_id().v == 100);
					}
				}, [&](const MTPDpeerUser &peer) {
					peers.push_back(u"user%1"_q.arg(peer.vuser_id().v));
				}, [](const auto &) {});
			}, [&](const MTPDdialogFolder &data) {
				++folders;
				CHECK(data.vfolder().c_folder().vid().v == 1);
			}, [](const auto &) {});
		}
		std::printf("     dialogs: %s, folders: %d\n",
			peers.join(' ').toUtf8().constData(),
			folders);
		CHECK(peers.contains(u"channel10"_q));
		CHECK(peers.contains(u"channel11"_q));
		CHECK(!peers.contains(u"user3"_q)); // Pinned, excluded.
		CHECK(!peers.contains(u"user2"_q)); // Hidden, archived.
		CHECK(folders == 1);
		CHECK(data.vchats().v.size() >= 2);
		auto names = QStringList();
		for (const auto &user : data.vusers().v) {
			if (user.type() == mtpc_user) {
				names.push_back(qs(user.c_user().vfirst_name().value_or_empty()));
			}
		}
		CHECK(names.contains(u"Carol Tester"_q));
	}
	const auto pinned = Call<MTPmessages_PeerDialogs>(
		MTPmessages_GetPinnedDialogs(MTP_int(0)),
		"messages.getPinnedDialogs");
	if (pinned) {
		const auto &list = pinned->c_messages_peerDialogs().vdialogs().v;
		CHECK(list.size() == 1);
		CHECK(list.size() == 1
			&& list[0].c_dialog().vpeer().type() == mtpc_peerUser
			&& list[0].c_dialog().is_pinned());
	}
	const auto archived = Call<MTPmessages_Dialogs>(MTPmessages_GetDialogs(
		MTP_flags(DialogsFlag::f_exclude_pinned | DialogsFlag::f_folder_id),
		MTP_int(1),
		MTP_int(0),
		MTP_int(0),
		MTP_inputPeerEmpty(),
		MTP_int(100),
		MTP_long(0)), "messages.getDialogs (archive)");
	if (archived) {
		CHECK(archived->c_messages_dialogs().vdialogs().v.size() == 1);
	}
	const auto filters = Call<MTPmessages_DialogFilters>(
		MTPmessages_GetDialogFilters(),
		"messages.getDialogFilters");
	if (filters) {
		const auto &list = filters->c_messages_dialogFilters().vfilters().v;
		CHECK(list.size() == 2);
		if (list.size() == 2) {
			const auto &filter = list[1].c_dialogFilter();
			CHECK(qs(filter.vtitle().c_textWithEntities().vtext()) == u"Work"_q);
			CHECK(filter.vinclude_peers().v.size() == 2);
		}
	}

	// History of a group: newest page, older page, around a message.
	const auto history = [&](
			MTPInputPeer peer,
			int offsetId,
			int addOffset,
			int limit,
			const char *name) {
		return Call<MTPmessages_Messages>(MTPmessages_GetHistory(
			peer,
			MTP_int(offsetId),
			MTP_int(0),
			MTP_int(addOffset),
			MTP_int(limit),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)), name);
	};
	if (const auto page = history(Channel(10), 0, 0, 20, "messages.getHistory")) {
		CHECK(page->type() == mtpc_messages_channelMessages);
		const auto list = MessagesOf(*page);
		CHECK(list.size() == 20);
		CHECK(!list.isEmpty() && IdOf(list.front()) == 125);
		CHECK(!list.isEmpty() && IdOf(list.back()) == 106);
	}
	if (const auto page = history(Channel(10), 10, 0, 10, "messages.getHistory (older)")) {
		const auto list = MessagesOf(*page);
		// Posts 1..9 without the deleted post 4.
		CHECK(list.size() == 8);
		auto ids = std::vector<int32>();
		for (const auto &message : list) {
			ids.push_back(IdOf(message));
		}
		CHECK(std::find(ids.begin(), ids.end(), 4) == ids.end());
		CHECK(!list.isEmpty() && IdOf(list.front()) == 9);
		for (const auto &message : list) {
			if (IdOf(message) == 3) {
				const auto &data = message.c_message();
				CHECK(TextOf(message) == u"Hello @bob, code and strike"_q);
				CHECK(data.vreply_to()
					&& data.vreply_to()->c_messageReplyHeader().vreply_to_msg_id().value_or_empty() == 2);
				CHECK(data.ventities() && data.ventities()->v.size() == 3);
				CHECK(data.is_out());
			}
		}
	}
	if (const auto page = history(Channel(10), 50, -5, 10, "messages.getHistory (around)")) {
		const auto list = MessagesOf(*page);
		CHECK(list.size() == 10);
		CHECK(!list.isEmpty() && IdOf(list.front()) == 54);
		CHECK(!list.isEmpty() && IdOf(list.back()) == 45);
	}
	auto pmTopId = 0;
	if (const auto page = history(User(3), 0, 0, 20, "messages.getHistory (private)")) {
		CHECK(page->type() == mtpc_messages_messagesSlice);
		const auto list = MessagesOf(*page);
		CHECK(list.size() == 3);
		if (!list.isEmpty()) {
			pmTopId = IdOf(list.front());
			CHECK((pmTopId & 0xFFFFF) == 3);
			CHECK((pmTopId >> 20) > 0);
			CHECK(list.front().c_message().vpeer_id().type() == mtpc_peerUser);
		}
	}

	// The file posted in General is a document.
	auto documentId = uint64();
	if (const auto page = history(Channel(10), 0, 0, 1, "messages.getHistory (file)")) {
		const auto list = MessagesOf(*page);
		if (!list.isEmpty() && list[0].type() == mtpc_message) {
			const auto media = list[0].c_message().vmedia();
			CHECK(media && media->type() == mtpc_messageMediaDocument);
			if (media && media->type() == mtpc_messageMediaDocument) {
				const auto &document = media->c_messageMediaDocument().vdocument()->c_document();
				documentId = document.vid().v;
				CHECK(document.vsize().v == 1700);
			}
		}
	}
	if (documentId) {
		const auto file = Call<MTPupload_File>(MTPupload_GetFile(
			MTP_flags(0),
			MTP_inputDocumentFileLocation(
				MTP_long(documentId),
				MTP_long(0),
				MTP_bytes(),
				MTP_string()),
			MTP_long(0),
			MTP_int(512 * 1024)), "upload.getFile");
		if (file) {
			const auto bytes = file->c_upload_file().vbytes().v;
			CHECK(bytes.size() == 1700);
			CHECK(bytes.startsWith("Quarterly report"));
		}
	}

	// Sending with formatting and a reply.
	auto sentId = 0;
	{
		using Flag = MTPmessages_SendMessage::Flag;
		const auto result = Call<MTPUpdates>(MTPmessages_SendMessage(
			MTP_flags(Flag::f_reply_to | Flag::f_entities),
			Channel(10),
			MTP_inputReplyToMessage(
				MTP_flags(0),
				MTP_int(2),
				MTPint(),
				MTPInputPeer(),
				MTPstring(),
				MTPVector<MTPMessageEntity>(),
				MTPint(),
				MTPInputPeer(),
				MTPint(),
				MTPbytes()),
			MTP_string("bold and plain"),
			MTP_long(777),
			MTPReplyMarkup(),
			MTP_vector<MTPMessageEntity>(1, MTP_messageEntityBold(
				MTP_int(0),
				MTP_int(4))),
			MTPint(),
			MTPint(),
			MTPInputPeer(),
			MTPInputQuickReplyShortcut(),
			MTPlong(),
			MTPlong(),
			MTPSuggestedPost(),
			MTPInputRichMessage()), "messages.sendMessage");
		if (result) {
			const auto list = UpdatesOf<void>(*result);
			CHECK(list.size() == 2);
			if (list.size() == 2) {
				CHECK(list[0].type() == mtpc_updateMessageID);
				CHECK(list[0].c_updateMessageID().vrandom_id().v == 777);
				CHECK(list[1].type() == mtpc_updateNewChannelMessage);
				const auto &message = list[1].c_updateNewChannelMessage().vmessage();
				sentId = IdOf(message);
				CHECK(sentId == 126);
				CHECK(TextOf(message) == u"bold and plain"_q);
				CHECK(message.c_message().is_out());
			}
		}
	}
	// Editing and deleting it.
	if (sentId) {
		using Flag = MTPmessages_EditMessage::Flag;
		const auto result = Call<MTPUpdates>(MTPmessages_EditMessage(
			MTP_flags(Flag::f_message),
			Channel(10),
			MTP_int(sentId),
			MTP_string("edited"),
			MTPInputMedia(),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPint(),
			MTPInputRichMessage()), "messages.editMessage");
		if (result) {
			const auto list = UpdatesOf<void>(*result);
			CHECK(list.size() == 1
				&& list[0].type() == mtpc_updateEditChannelMessage);
		}
		const auto deleted = Call<MTPmessages_AffectedMessages>(
			MTPchannels_DeleteMessages(
				InputChannel(10),
				MTP_vector<MTPint>(1, MTP_int(sentId))),
			"channels.deleteMessages");
		CHECK(deleted.has_value()
			&& deleted->c_messages_affectedMessages().vpts_count().v == 1);
	}

	// Sending to a private chat and reading it.
	{
		const auto result = Call<MTPUpdates>(MTPmessages_SendMessage(
			MTP_flags(0),
			User(3),
			MTPInputReplyTo(),
			MTP_string("hi Carol"),
			MTP_long(778),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPInputPeer(),
			MTPInputQuickReplyShortcut(),
			MTPlong(),
			MTPlong(),
			MTPSuggestedPost(),
			MTPInputRichMessage()), "messages.sendMessage (private)");
		if (result) {
			const auto list = UpdatesOf<void>(*result);
			CHECK(list.size() == 2 && list[1].type() == mtpc_updateNewMessage);
		}
		CHECK(Call<MTPmessages_AffectedMessages>(MTPmessages_ReadHistory(
			User(3),
			MTP_int(pmTopId)), "messages.readHistory").has_value());
	}

	// A private chat with a new user is created when sending.
	{
		const auto result = Call<MTPUpdates>(MTPmessages_SendMessage(
			MTP_flags(0),
			User(4),
			MTPInputReplyTo(),
			MTP_string("hello bot"),
			MTP_long(779),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPInputPeer(),
			MTPInputQuickReplyShortcut(),
			MTPlong(),
			MTPlong(),
			MTPSuggestedPost(),
			MTPInputRichMessage()), "messages.sendMessage (new chat)");
		if (result) {
			const auto list = UpdatesOf<void>(*result);
			CHECK(list.size() == 2);
			if (list.size() == 2) {
				const auto &message = list[1].c_updateNewMessage().vmessage();
				CHECK(message.c_message().vpeer_id().c_peerUser().vuser_id().v == 4);
			}
		}
	}

	// Uploading a file.
	{
		CHECK(Call<MTPBool>(MTPupload_SaveFilePart(
			MTP_long(42),
			MTP_int(0),
			MTP_bytes("hello ")), "upload.saveFilePart").has_value());
		CHECK(Call<MTPBool>(MTPupload_SaveFilePart(
			MTP_long(42),
			MTP_int(1),
			MTP_bytes("file")), "upload.saveFilePart").has_value());
		const auto result = Call<MTPUpdates>(MTPmessages_SendMedia(
			MTP_flags(0),
			Channel(10),
			MTPInputReplyTo(),
			MTP_inputMediaUploadedDocument(
				MTP_flags(0),
				MTP_inputFile(
					MTP_long(42),
					MTP_int(2),
					MTP_string("notes.txt"),
					MTP_string()),
				MTPInputFile(),
				MTP_string("text/plain"),
				MTP_vector<MTPDocumentAttribute>(
					1,
					MTP_documentAttributeFilename(MTP_string("notes.txt"))),
				MTPVector<MTPInputDocument>(),
				MTPInputPhoto(),
				MTPint(),
				MTPint()),
			MTP_string("caption"),
			MTP_long(780),
			MTPReplyMarkup(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPint(),
			MTPInputPeer(),
			MTPInputQuickReplyShortcut(),
			MTPlong(),
			MTPlong(),
			MTPSuggestedPost()), "messages.sendMedia");
		if (result) {
			const auto list = UpdatesOf<void>(*result);
			CHECK(list.size() == 2);
			if (list.size() == 2) {
				const auto &message = list[1].c_updateNewChannelMessage().vmessage().c_message();
				CHECK(qs(message.vmessage()) == u"caption"_q);
				CHECK(message.vmedia()
					&& message.vmedia()->type() == mtpc_messageMediaDocument);
			}
		}
	}

	// Live updates: someone posts, edits and deletes.
	Updates.clear();
	Mock(u"/_mock/incoming"_q, R"({"chat_id": 10, "user_id": 3, "text": "live!"})");
	Wait([] { return !Updates.empty(); }, 15000);
	CHECK(!Updates.empty());
	if (!Updates.empty()) {
		auto updates = MTPUpdates();
		auto from = Updates[0].constData();
		CHECK(updates.read(from, from + Updates[0].size()));
		const auto list = UpdatesOf<void>(updates);
		CHECK(list.size() == 1
			&& list[0].type() == mtpc_updateNewChannelMessage
			&& TextOf(list[0].c_updateNewChannelMessage().vmessage()) == u"live!"_q);
		std::printf("ok   live new message\n");
	}
	Updates.clear();
	Mock(u"/_mock/edit"_q, R"({"chat_id": 10, "post_no": 125, "text": "changed"})");
	Wait([] { return !Updates.empty(); }, 15000);
	CHECK(!Updates.empty());
	if (!Updates.empty()) {
		auto updates = MTPUpdates();
		auto from = Updates[0].constData();
		CHECK(updates.read(from, from + Updates[0].size()));
		const auto list = UpdatesOf<void>(updates);
		CHECK(!list.empty() && list[0].type() == mtpc_updateEditChannelMessage);
		std::printf("ok   live edit\n");
	}

	// Channel difference contains the logged updates.
	{
		const auto difference = Call<MTPupdates_ChannelDifference>(
			MTPupdates_GetChannelDifference(
				MTP_flags(0),
				InputChannel(10),
				MTP_channelMessagesFilterEmpty(),
				MTP_int(1),
				MTP_int(100)),
			"updates.getChannelDifference");
		if (difference) {
			CHECK(difference->type() == mtpc_updates_channelDifference);
		}
	}

	// Members, profiles, search.
	if (const auto full = Call<MTPmessages_ChatFull>(
			MTPchannels_GetFullChannel(InputChannel(10)),
			"channels.getFullChannel")) {
		CHECK(full->c_messages_chatFull().vfull_chat().type() == mtpc_channelFull);
	}
	if (const auto members = Call<MTPchannels_ChannelParticipants>(
			MTPchannels_GetParticipants(
				InputChannel(10),
				MTP_channelParticipantsRecent(),
				MTP_int(0),
				MTP_int(100),
				MTP_long(0)),
			"channels.getParticipants")) {
		CHECK(members->c_channels_channelParticipants().vcount().v == 4);
	}
	CHECK(Call<MTPusers_UserFull>(
		MTPusers_GetFullUser(MTP_inputUser(MTP_long(3), MTP_long(0))),
		"users.getFullUser").has_value());
	if (const auto found = Call<MTPcontacts_Found>(MTPcontacts_Search(
			MTP_flags(0),
			MTP_string("car"),
			MTP_int(10)), "contacts.search")) {
		CHECK(found->c_contacts_found().vmy_results().v.size() == 1);
	}
	if (const auto found = Call<MTPmessages_Messages>(MTPmessages_Search(
			MTP_flags(0),
			Channel(10),
			MTP_string("number 1"),
			MTPInputPeer(),
			MTPInputPeer(),
			MTPVector<MTPReaction>(),
			MTPint(),
			MTP_inputMessagesFilterEmpty(),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(0),
			MTP_int(20),
			MTP_int(0),
			MTP_int(0),
			MTP_long(0)), "messages.search")) {
		CHECK(MessagesOf(*found).size() > 5);
	}
	CHECK(Call<MTPcontacts_ResolvedPeer>(MTPcontacts_ResolveUsername(
		MTP_flags(0),
		MTP_string("carol"),
		MTPstring()), "contacts.resolveUsername").has_value());

	// Pin / archive / folders write back.
	CHECK(Call<MTPBool>(MTPmessages_ToggleDialogPin(
		MTP_flags(MTPmessages_ToggleDialogPin::Flag::f_pinned),
		MTP_inputDialogPeer(Channel(11))), "messages.toggleDialogPin").has_value());
	CHECK(Call<MTPUpdates>(MTPfolders_EditPeerFolders(
		MTP_vector<MTPInputFolderPeer>(1, MTP_inputFolderPeer(
			Channel(11),
			MTP_int(1)))), "folders.editPeerFolders").has_value());
	CHECK(Call<MTPBool>(MTPmessages_UpdateDialogFilter(
		MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
		MTP_int(2),
		MTP_dialogFilter(
			MTP_flags(0),
			MTP_int(2),
			MTP_textWithEntities(MTP_string("Work"), MTPVector<MTPMessageEntity>()),
			MTPstring(),
			MTPint(),
			MTPVector<MTPInputPeer>(),
			MTP_vector<MTPInputPeer>(1, Channel(10)),
			MTPVector<MTPInputPeer>())), "messages.updateDialogFilter").has_value());

	// Unknown requests fail gracefully.
	{
		auto type = QString();
		CHECK(IsError(CallRaw(MTPpayments_GetStarsStatus(
			MTP_flags(0),
			MTP_inputPeerSelf())), &type));
		CHECK(type == u"PARARAM_NOT_SUPPORTED"_q);
	}

	CHECK(Call<MTPauth_LoggedOut>(MTPauth_LogOut(), "auth.logOut").has_value());
	CHECK(!Backend->authorized());

	std::printf(Failures ? "%d FAILED\n" : "ALL OK\n", Failures);
	Backend = nullptr;
	return Failures ? 1 : 0;
}
