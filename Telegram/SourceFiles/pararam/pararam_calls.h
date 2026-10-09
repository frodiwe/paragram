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

	// Group calls, for channelFull.call and the channel flags.
	[[nodiscard]] MTPInputGroupCall groupCallInput(int64 chatId) const;
	[[nodiscard]] bool groupCallActive(int64 chatId) const;
	[[nodiscard]] bool groupCallNotEmpty(int64 chatId) const;

private:
	struct OneToOne;
	struct Group;

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

	// Group calls.
	void createGroupCall(mtpRequestId requestId, Reader &reader);
	void joinGroupCall(mtpRequestId requestId, Reader &reader);
	void leaveGroupCall(mtpRequestId requestId, Reader &reader);
	void discardGroupCall(mtpRequestId requestId, Reader &reader);
	void getGroupCall(mtpRequestId requestId, Reader &reader);
	void getGroupParticipants(mtpRequestId requestId, Reader &reader);
	void editGroupCallParticipant(mtpRequestId requestId, Reader &reader);
	void checkGroupCall(mtpRequestId requestId, Reader &reader);
	void getGroupCallJoinAs(mtpRequestId requestId, Reader &reader);
	[[nodiscard]] Group *groupByInput(const MTPInputGroupCall &call) const;
	[[nodiscard]] Group &ensureGroup(int64 chatId);
	void startGroupSession(Group &group);
	void groupApproved(int64 chatId);
	void groupChanged(int64 chatId, int64 userId, bool left);
	void groupEnded(int64 chatId);
	void finishGroupSession(Group &group);
	void applyGroupCalls(const std::vector<ActiveCall> &calls);
	void removeGroup(int64 chatId);
	[[nodiscard]] std::vector<int64> groupUsers(const Group &group) const;
	[[nodiscard]] MTPGroupCall groupToMTP(const Group &group) const;
	[[nodiscard]] MTPInputGroupCall groupInputToMTP(const Group &group) const;
	[[nodiscard]] MTPGroupCallParticipant participantToMTP(
		const Group &group,
		int64 userId,
		bool left = false) const;
	[[nodiscard]] QVector<MTPGroupCallParticipant> participantsToMTP(
		const Group &group) const;
	[[nodiscard]] MTPUpdates groupUpdates(
		const Group &group,
		QVector<MTPUpdate> updates) const;
	void pushParticipants(
		Group &group,
		QVector<MTPGroupCallParticipant> participants);

	void connectSocket();
	void socketClosed();
	void refreshCalls();
	void scheduleRefresh(crl::time delay);
	void applyCalls(const std::vector<ActiveCall> &calls);
	void ringIncoming(const ActiveCall &call);

	const not_null<Backend*> _backend;
	std::unique_ptr<OneToOne> _call;
	base::flat_map<int64, std::unique_ptr<Group>> _groups;
	uint64 _groupCallIdCounter = 0;

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
