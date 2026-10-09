/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/timer.h"

#include <QtCore/QJsonObject>
#include <QtCore/QObject>
#include <QtCore/QPointer>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace Pararam {

class Api;
class WebSocket;
struct HttpError;

// The media server appears in a call as this pseudo user. It receives
// our audio and video and sends down the mixed audio of everyone.
inline constexpr auto kChoirmasterId = int64(14548);

// POST /core/call result: credentials for one room on a media server.
struct CallAccess {
	QString roomId;
	QString mediaServer;
	QString accessKey;
	QString turnHost;
	QString turnUser;
	QString turnPassword;

	[[nodiscard]] bool valid() const;
	[[nodiscard]] static CallAccess Parse(const QJsonObject &data);
};

// One entry of GET /core/call.
struct ActiveCall {
	int64 chatId = 0;
	std::vector<int64> users;
	int64 starterId = 0;
	bool silent = false;
	QString sessionId;

	[[nodiscard]] static std::vector<ActiveCall> ParseList(
		const QJsonObject &data);
};

struct CallUser {
	int64 id = 0;
	QString name; // Only for guests joined by an invitation link.
	bool external = false;
	bool audio = false;
	bool video = false;
	QString videoSource; // "empty", "video" or "screen".

	[[nodiscard]] static CallUser Parse(const QJsonObject &data);
};

// Signaling of one call in a chat: the media server socket and
// its HTTP API. Lives on the main thread.
class CallSession final : public QObject {
public:
	struct Options {
		int64 chatId = 0;
		int64 selfId = 0;
		QString selfName;
		bool silent = false; // Group calls do not ring the others.
		bool stayInCall = false; // Do not end the call when left alone.
	};

	enum class EndReason {
		Left, // We left (or the server confirmed our "leave").
		Dropped, // An admin dropped us.
		Banned,
		Failed,
	};

	struct Listener {
		std::function<void()> approved;
		std::function<void(const CallUser &user)> joined;
		std::function<void(int64 userId)> left;
		std::function<void(const CallUser &user)> changed;
		std::function<void(int64 userId, bool speaking)> speaking;
		std::function<void(int64 userId)> currentVideoUser;
		std::function<void(EndReason reason, const QString &error)> ended;
	};

	CallSession(not_null<Api*> api, Options options);
	~CallSession();

	void start();
	void leave();

	[[nodiscard]] int64 chatId() const;
	[[nodiscard]] int64 selfId() const;
	[[nodiscard]] bool silent() const;
	[[nodiscard]] bool approved() const;
	[[nodiscard]] bool finished() const;
	[[nodiscard]] const CallAccess &access() const;

	// Participants except ourselves and the media server.
	[[nodiscard]] const std::map<int64, CallUser> &users() const;
	[[nodiscard]] bool mediaServerJoined() const;

	int addListener(Listener listener);
	void removeListener(int id);

	// The media server answers our offers, it never sends offers itself.
	void sendOffer(
		int64 dstUserId,
		const QByteArray &sdp,
		bool iceRestart,
		std::function<void(QByteArray answer)> done,
		std::function<void(QString error)> fail);
	void setAudioEnabled(bool enabled);
	void setVideoEnabled(bool enabled, const QString &source);
	void requestVideoQualities(const std::map<int64, QString> &qualities);

	// Declines a call without joining it.
	static void Reject(not_null<Api*> api, int64 chatId, int64 selfId);

private:
	struct Outgoing {
		QByteArray requestId;
		QByteArray json;
		int attempts = 0;
	};
	struct HttpRequest {
		QString path;
		QJsonObject body;
		std::function<void(const QJsonValue &data)> done;
		std::function<void(const QString &error)> fail;
	};

	void requestAccess();
	void connectSocket();
	void socketOpened();
	void socketClosed(int code, const QString &reason);
	void socketReceived(const QByteArray &text);
	void handleMessage(const QJsonObject &message);
	void handleSessionEvent(int64 sourceId, const QJsonObject &data);
	void scheduleReconnect();
	void sendHeartbeat();
	void sendReliable(QJsonObject message);
	void retransmit();
	void confirm(const QByteArray &requestId);
	void sendRaw(const QByteArray &json);
	void queueHttp(HttpRequest request);
	void sendNextHttp();
	void sendDeviceInfo();
	void finish(EndReason reason, const QString &error = QString());

	template <typename Method, typename ...Args>
	void notify(Method method, const Args &...args);

	const not_null<Api*> _api;
	const Options _options;
	CallAccess _access;
	std::unique_ptr<WebSocket> _socket;
	QString _callUid;
	QByteArray _httpAuthKey;
	bool _approved = false;
	bool _finished = false;
	bool _leaving = false;
	bool _mediaServerJoined = false;
	int _reconnects = 0;
	std::map<int64, CallUser> _users;
	std::map<int, Listener> _listeners;
	int _listenerId = 0;

	std::deque<Outgoing> _unconfirmed;
	std::vector<QByteArray> _bufferedBeforeApprove;
	std::set<QByteArray> _seenRequestIds;
	std::deque<QByteArray> _seenOrder;
	base::Timer _retransmitTimer;

	int _heartbeat = 0;
	bool _heartbeatWaiting = false;
	base::Timer _heartbeatTimer;
	base::Timer _heartbeatTimeout;
	base::Timer _approveTimeout;
	base::Timer _reconnectTimer;

	std::deque<HttpRequest> _http;
	bool _httpSending = false;
	base::Timer _httpTimer;
	std::map<int64, QString> _videoQualities;

	std::shared_ptr<bool> _alive;

};

} // namespace Pararam
