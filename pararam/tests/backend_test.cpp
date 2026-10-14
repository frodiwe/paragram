// Integration test of Pararam::Backend against pararam/mock_server.py.
//
// The backend is driven exactly like Telegram Desktop drives it: requests
// are serialized with the generated TL classes and responses are parsed
// with the generated response types. See pararam/tests/run_backend_test.sh.
#include "pararam/pararam_backend.h"
#include "pararam/pararam_reaction_icons.h"
#include "pararam/calls/pararam_media_signaling.h"
#include "mtproto/mtp_instance.h"
#include "mtproto/mtproto_response.h"
#include "base/unixtime.h"

#include <QtCore/QDateTime>
#include <QtCore/QSize>
#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>

#include <cstdio>
#include <map>
#include <optional>

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
namespace Pararam::Media {

void SetPendingSignaling(std::shared_ptr<Signaling> signaling) {
}

std::shared_ptr<Signaling> TakePendingSignaling() {
	return nullptr;
}

} // namespace Pararam::Media

namespace Pararam {

// The real icons are drawn with the emoji set of lib_ui.
std::vector<QByteArray> RenderReactionIcons(
		const std::vector<QString> &emoji) {
	return std::vector<QByteArray>(emoji.size(), QByteArray("icon"));
}

} // namespace Pararam

namespace base {

bool LogSkipDebug() {
	return true;
}

void LogWriteDebug(const QString &message, const char *file, int line) {
}

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

void http_invalidate() {
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
		Backend->login(u"bob@example.com"_q, u"password"_q, u"111111"_q, [&](
				Pararam::LoginResult result,
				MTPUser) {
			status = result.status;
		});
		Wait([&] { return status.has_value(); });
		CHECK(status == Pararam::LoginResult::Status::Failed);
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

		// Bob is only in Acme, it still has its folder.
		if (const auto filters = Call<MTPmessages_DialogFilters>(
				MTPmessages_GetDialogFilters(),
				"messages.getDialogFilters (one organization)")) {
			const auto &list = filters->c_messages_dialogFilters().vfilters().v;
			CHECK(list.size() == 2);
			CHECK(list.size() == 2
				&& qs(list[1].c_dialogFilter().vtitle(
					).c_textWithEntities().vtext()) == u"Acme & Co"_q);
		}
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
		// All chats, two organizations, then the "Work" tag.
		CHECK(list.size() == 4);
		if (list.size() == 4) {
			const auto title = [&](int index) {
				return qs(list[index].c_dialogFilter().vtitle(
					).c_textWithEntities().vtext());
			};
			const auto peers = [&](int index) {
				return list[index].c_dialogFilter().vinclude_peers().v.size();
			};
			CHECK(list[1].c_dialogFilter().vid().v == 2);
			CHECK(title(1) == u"Acme & Co"_q);
			CHECK(qs(list[1].c_dialogFilter().vemoticon().value_or_empty())
				== QString::fromUtf8("\xF0\x9F\x92\xBC"));
			CHECK(peers(1) == 3); // General, Carol and Bob.
			CHECK(list[2].c_dialogFilter().vid().v == 3);
			CHECK(title(2) == u"Side Project"_q);
			CHECK(peers(2) == 2); // Releases and Carol.
			CHECK(list[3].c_dialogFilter().vid().v == 4);
			CHECK(title(3) == u"Work"_q);
			CHECK(peers(3) == 2);
		}
		// Organization folders show the logo of the organization.
		auto logo = std::optional<QByteArray>();
		CHECK(Backend->loadFolderLogo(2, [&](QByteArray bytes) {
			logo = bytes;
		}));
		Wait([&] { return logo.has_value(); });
		CHECK(logo && logo->startsWith("\x89PNG"));
		CHECK(!Backend->loadFolderLogo(4, [](QByteArray) {}));
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

	// Read receipts: Carol read the private chat, all read Alice's post 3.
	if (pmTopId) {
		Wait([&] { return false; }, 1500); // /msg/lastread after loading.
		if (const auto dialogs = Call<MTPmessages_PeerDialogs>(
				MTPmessages_GetPeerDialogs(MTP_vector<MTPInputDialogPeer>(
					1,
					MTP_inputDialogPeer(User(3)))),
				"messages.getPeerDialogs (read outbox)")) {
			const auto &list = dialogs->c_messages_peerDialogs().vdialogs().v;
			CHECK(list.size() == 1
				&& list[0].c_dialog().vread_outbox_max_id().v == pmTopId);
		}
		if (const auto date = Call<MTPOutboxReadDate>(
				MTPmessages_GetOutboxReadDate(User(3), MTP_int(pmTopId - 1)),
				"messages.getOutboxReadDate")) {
			CHECK(date->c_outboxReadDate().vdate().v > 0);
		}
	}
	if (const auto read = Call<MTPVector<MTPReadParticipantDate>>(
			MTPmessages_GetMessageReadParticipants(Channel(10), MTP_int(3)),
			"messages.getMessageReadParticipants")) {
		auto users = std::vector<int64>();
		for (const auto &entry : read->v) {
			users.push_back(entry.c_readParticipantDate().vuser_id().v);
		}
		ranges::sort(users);
		CHECK(users == (std::vector<int64>{ 2, 3, 4 }));
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
	// Images are shown as photos with a server side resized preview.
	const auto photoOf = [](const MTPMessage &message) {
		const auto media = (message.type() == mtpc_message)
			? message.c_message().vmedia()
			: nullptr;
		return (media && media->type() == mtpc_messageMediaPhoto)
			? &media->c_messageMediaPhoto().vphoto()->c_photo()
			: nullptr;
	};
	const auto sizeOf = [](const MTPDphoto &photo, const char *type) {
		for (const auto &size : photo.vsizes().v) {
			if (size.type() == mtpc_photoSize
				&& qs(size.c_photoSize().vtype()) == QString(type)) {
				const auto &data = size.c_photoSize();
				return QSize(data.vw().v, data.vh().v);
			}
		}
		return QSize();
	};
	const auto fileOf = [](const MTPDphoto &photo, const char *type, const char *name) {
		const auto file = Call<MTPupload_File>(MTPupload_GetFile(
			MTP_flags(0),
			MTP_inputPhotoFileLocation(
				photo.vid(),
				photo.vaccess_hash(),
				photo.vfile_reference(),
				MTP_string(type)),
			MTP_long(0),
			MTP_int(512 * 1024)), name);
		return file ? file->c_upload_file().vbytes().v : QByteArray();
	};
	auto reactedId = 0;
	if (const auto page = history(Channel(11), 0, 0, 4, "messages.getHistory (photos)")) {
		const auto list = MessagesOf(*page);
		CHECK(list.size() == 4);
		for (const auto &message : list) {
			const auto id = IdOf(message);
			const auto photo = photoOf(message);
			if (id == 3) {
				// Size read from the file header.
				CHECK(photo != nullptr);
				if (photo) {
					CHECK(sizeOf(*photo, "y") == QSize(40, 30));
					CHECK(sizeOf(*photo, "m") == QSize(40, 30));
					CHECK(fileOf(*photo, "y", "upload.getFile (photo)").startsWith("\x89PNG"));
				}
			} else if (id == 4) {
				// HEIC, size taken from the preview of the file server.
				CHECK(photo != nullptr);
				if (photo) {
					CHECK(sizeOf(*photo, "y") == QSize(400, 300));
					const auto preview = fileOf(*photo, "m", "upload.getFile (HEIC preview)");
					CHECK(preview.startsWith("\x89PNG"));
				}
			} else if (id == 5) {
				// No mime type, a .jpg name.
				CHECK(photo != nullptr);
				if (photo) {
					CHECK(sizeOf(*photo, "y") == QSize(60, 120));
				}
			} else if (id == 6) {
				reactedId = id;
				const auto &data = message.c_message();
				CHECK(TextOf(message) == u"Notes are in the release notes and bold works"_q);
				auto textUrl = false;
				for (const auto &entity : data.ventities() ? data.ventities()->v : QVector<MTPMessageEntity>()) {
					if (entity.type() == mtpc_messageEntityTextUrl) {
						textUrl = (qs(entity.c_messageEntityTextUrl().vurl()) == u"https://pararam.io/notes"_q);
					}
				}
				CHECK(textUrl);
				CHECK(data.vreactions() != nullptr);
				if (data.vreactions()) {
					const auto &results = data.vreactions()->c_messageReactions().vresults().v;
					CHECK(results.size() == 2);
					CHECK(!results.isEmpty() && results[0].c_reactionCount().vcount().v == 2);
				}
			}
		}
		// Every user has an avatar from the file server.
		auto withPhoto = 0;
		for (const auto &user : page->c_messages_channelMessages().vusers().v) {
			if (user.type() == mtpc_user
				&& user.c_user().vphoto()
				&& user.c_user().vphoto()->type() == mtpc_userProfilePhoto) {
				++withPhoto;
				const auto file = Call<MTPupload_File>(MTPupload_GetFile(
					MTP_flags(0),
					MTP_inputPeerPhotoFileLocation(
						MTP_flags(0),
						User(user.c_user().vid().v),
						user.c_user().vphoto()->c_userProfilePhoto().vphoto_id()),
					MTP_long(0),
					MTP_int(512 * 1024)), "upload.getFile (avatar)");
				CHECK(file && file->c_upload_file().vbytes().v.startsWith("\x89PNG"));
			}
		}
		CHECK(withPhoto > 0);
	}

	// Reactions: the list, toggling, polling and who reacted.
	if (const auto available = Call<MTPmessages_AvailableReactions>(
			MTPmessages_GetAvailableReactions(MTP_int(0)),
			"messages.getAvailableReactions")) {
		CHECK(available->type() == mtpc_messages_availableReactions);
		if (available->type() == mtpc_messages_availableReactions) {
			const auto &list = available->c_messages_availableReactions().vreactions().v;
			CHECK(list.size() == 47);
			// Standard fully qualified emoji, pararam.io sends U+2714 alone.
			CHECK(list.size() > 2
				&& qs(list[2].c_availableReaction().vreaction()) == u"\u2714\uFE0F"_q);
			if (!list.isEmpty()) {
				const auto &icon = list[0].c_availableReaction().vstatic_icon();
				const auto file = Call<MTPupload_File>(MTPupload_GetFile(
					MTP_flags(0),
					MTP_inputDocumentFileLocation(
						icon.c_document().vid(),
						icon.c_document().vaccess_hash(),
						icon.c_document().vfile_reference(),
						MTP_string()),
					MTP_long(0),
					MTP_int(512 * 1024)), "upload.getFile (reaction icon)");
				CHECK(file && file->c_upload_file().vbytes().v == "icon");
			}
		}
	}
	const auto reactionsIn = [](const MTPUpdates &updates) {
		auto result = std::map<QString, std::pair<int, bool>>();
		for (const auto &update : UpdatesOf<MTPUpdate>(updates)) {
			if (update.type() != mtpc_updateMessageReactions) {
				continue;
			}
			const auto &reactions = update.c_updateMessageReactions().vreactions();
			for (const auto &count : reactions.c_messageReactions().vresults().v) {
				const auto &data = count.c_reactionCount();
				result[qs(data.vreaction().c_reactionEmoji().vemoticon())] = {
					data.vcount().v,
					data.vchosen_order().has_value(),
				};
			}
		}
		return result;
	};
	const auto fire = QString::fromUtf8("\xF0\x9F\x94\xA5");
	const auto joy = QString::fromUtf8("\xF0\x9F\x98\x82");
	if (reactedId) {
		const auto react = [&](const QString &emoji, const char *name) {
			return Call<MTPUpdates>(MTPmessages_SendReaction(
				MTP_flags(MTPmessages_SendReaction::Flag::f_reaction),
				Channel(11),
				MTP_int(reactedId),
				MTP_vector<MTPReaction>(emoji.isEmpty()
					? QVector<MTPReaction>()
					: QVector<MTPReaction>{ MTP_reactionEmoji(MTP_string(emoji)) })),
				name);
		};
		if (const auto result = react(fire, "messages.sendReaction")) {
			const auto counts = reactionsIn(*result);
			CHECK(counts.contains(fire) && counts.at(fire) == std::make_pair(2, true));
		}
		if (const auto result = react(QString(), "messages.sendReaction (remove)")) {
			const auto counts = reactionsIn(*result);
			CHECK(counts.contains(fire) && counts.at(fire) == std::make_pair(1, false));
		}
		// The standard heart is sent to pararam.io as its U+2764.
		const auto heart = u"\u2764\uFE0F"_q;
		if (const auto result = react(heart, "messages.sendReaction (heart)")) {
			const auto counts = reactionsIn(*result);
			CHECK(counts.contains(heart) && counts.at(heart) == std::make_pair(1, true));
		}
		if (const auto result = react(QString(), "messages.sendReaction (remove heart)")) {
			CHECK(!reactionsIn(*result).contains(heart));
		}
		Mock(u"/_mock/react"_q, QByteArray(R"({"chat_id": 11, "post_no": 6, "user_id": 3, "reaction": ")")
			+ joy.toUtf8() + "\"}");
		if (const auto result = Call<MTPUpdates>(MTPmessages_GetMessagesReactions(
				Channel(11),
				MTP_vector<MTPint>(1, MTP_int(reactedId))), "messages.getMessagesReactions")) {
			const auto counts = reactionsIn(*result);
			CHECK(counts.contains(joy) && counts.at(joy).first == 1);
		}
		if (const auto list = Call<MTPmessages_MessageReactionsList>(
				MTPmessages_GetMessageReactionsList(
					MTP_flags(0),
					Channel(11),
					MTP_int(reactedId),
					MTPReaction(),
					MTPstring(),
					MTP_int(50)), "messages.getMessageReactionsList")) {
			// Bob's thumbs up and fire, Carol's new reaction.
			CHECK(list->c_messages_messageReactionsList().vcount().v == 3);
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
		MTP_int(4),
		MTP_dialogFilter(
			MTP_flags(0),
			MTP_int(4),
			MTP_textWithEntities(MTP_string("Work"), MTPVector<MTPMessageEntity>()),
			MTPstring(),
			MTPint(),
			MTPVector<MTPInputPeer>(),
			MTP_vector<MTPInputPeer>(1, Channel(10)),
			MTPVector<MTPInputPeer>())), "messages.updateDialogFilter").has_value());

	// Organization folders: edits are reverted, removal hides the folder.
	const auto folderUpdate = [] {
		for (const auto &buffer : Updates) {
			auto updates = MTPUpdates();
			auto from = buffer.constData();
			if (!updates.read(from, from + buffer.size())) {
				continue;
			}
			for (const auto &update : UpdatesOf<void>(updates)) {
				if (update.type() == mtpc_updateDialogFilter) {
					return std::make_optional(update);
				}
			}
		}
		return std::optional<MTPUpdate>();
	};
	Updates.clear();
	CHECK(Call<MTPBool>(MTPmessages_UpdateDialogFilter(
		MTP_flags(MTPmessages_UpdateDialogFilter::Flag::f_filter),
		MTP_int(2),
		MTP_dialogFilter(
			MTP_flags(0),
			MTP_int(2),
			MTP_textWithEntities(MTP_string("Renamed"), MTPVector<MTPMessageEntity>()),
			MTPstring(),
			MTPint(),
			MTPVector<MTPInputPeer>(),
			MTP_vector<MTPInputPeer>(1, Channel(10)),
			MTPVector<MTPInputPeer>())), "messages.updateDialogFilter (organization)").has_value());
	Wait([&] { return folderUpdate().has_value(); }, 5000);
	if (const auto update = folderUpdate()) {
		const auto &data = update->c_updateDialogFilter();
		const auto &filter = data.vfilter()->c_dialogFilter();
		CHECK(data.vid().v == 2);
		CHECK(qs(filter.vtitle().c_textWithEntities().vtext()) == u"Acme & Co"_q);
		// General, Carol, Bob and the bot written to above.
		CHECK(filter.vinclude_peers().v.size() == 4);
	} else {
		CHECK(false);
	}

	// A new chat of an organization joins its folder.
	Updates.clear();
	Mock(u"/_mock/new_chat"_q, R"({"chat_id": 30, "title": "Side chat", "users": [1, 3], "org": 200})");
	Wait([&] { return folderUpdate().has_value(); }, 15000);
	if (const auto update = folderUpdate()) {
		const auto &data = update->c_updateDialogFilter();
		CHECK(data.vid().v == 3);
		CHECK(data.vfilter()->c_dialogFilter().vinclude_peers().v.size() == 3);
		std::printf("ok   organization folder update\n");
	} else {
		CHECK(false);
	}

	CHECK(Call<MTPBool>(MTPmessages_UpdateDialogFilter(
		MTP_flags(0),
		MTP_int(3),
		MTPDialogFilter()), "messages.updateDialogFilter (hide organization)").has_value());
	if (const auto filters = Call<MTPmessages_DialogFilters>(
			MTPmessages_GetDialogFilters(),
			"messages.getDialogFilters (organization hidden)")) {
		const auto &list = filters->c_messages_dialogFilters().vfilters().v;
		CHECK(list.size() == 3);
		CHECK(ranges::none_of(list, [](const MTPDialogFilter &filter) {
			return (filter.type() == mtpc_dialogFilter)
				&& (filter.c_dialogFilter().vid().v == 3);
		}));
	}

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
