/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"
#include "base/bytes.h"
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "base/timer.h"
#include "mtproto/core_types.h"

#include <QtCore/QObject>

#include <memory>

namespace Pararam {

class Backend;
class Reader;
class CallSession;
class SessionSignaling;
class WebSocket;
struct ActiveCall;

// pararam.io calls for the Telegram Desktop call UI.
//
// 1:1 calls: the backend plays the Telegram call server, including the
// Diffie-Hellman exchange the UI insists on (the key is not used), while
// joining the pararam.io room of the private chat. Once the media server
// accepted us, phoneCall.protocol carries the "pararam" library version,
// so the UI creates Media::CallInstance, which takes the signaling.
class CallManager final : public QObject {
public:
	explicit CallManager(not_null<Backend*> backend);
	~CallManager();

	// Returns false if the request is not about calls.
	bool handle(mtpRequestId requestId, mtpTypeId type, Reader &reader);

	void start(); // Logged in and loaded, start watching for calls.
	void stop(); // Logged out.

private:
	struct OneToOne;

	void requestCall(mtpRequestId requestId, Reader &reader);
	void acceptCall(mtpRequestId requestId, Reader &reader);
	void confirmCall(mtpRequestId requestId, Reader &reader);
	void receivedCall(mtpRequestId requestId, Reader &reader);
	void discardCall(mtpRequestId requestId, Reader &reader);
	void replyDhConfig(mtpRequestId requestId, Reader &reader);

	[[nodiscard]] bool checkCall(const MTPInputPhoneCall &peer) const;
	void startSession(bool outgoing);
	void sessionApproved();
	void sessionJoined(int64 userId);
	void sessionLeft(int64 userId);
	void sessionEnded();
	void startMedia();
	void finishCall(const MTPPhoneCallDiscardReason &reason);
	void pushCall(const MTPPhoneCall &call);
	[[nodiscard]] MTPPhoneCall waitingToMTP(bool received) const;
	[[nodiscard]] MTPPhoneCall activeToMTP() const;
	[[nodiscard]] MTPPhoneCall discardedToMTP(
		const MTPPhoneCallDiscardReason &reason) const;
	[[nodiscard]] MTPPhoneCallProtocol protocol() const;
	[[nodiscard]] int32 duration() const;

	void connectSocket();
	void socketClosed();
	void refreshCalls();
	void scheduleRefresh(crl::time delay);
	void applyCalls(const std::vector<ActiveCall> &calls);
	void ringIncoming(const ActiveCall &call);

	const not_null<Backend*> _backend;
	std::unique_ptr<OneToOne> _call;

	std::unique_ptr<WebSocket> _socket;
	bool _socketConnected = false;
	int _socketFailures = 0;
	base::Timer _socketReconnectTimer;
	base::Timer _socketHeartbeatTimer;
	base::Timer _refreshTimer;
	bool _refreshing = false;
	bool _refreshAgain = false;
	bool _started = false;
	base::flat_set<QString> _seenSessions;
	base::flat_map<int64, crl::time> _finishedAt;

};

} // namespace Pararam
