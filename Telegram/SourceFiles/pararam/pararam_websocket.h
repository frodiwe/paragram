/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

#include <functional>
#include <vector>

class QTcpSocket;

namespace Pararam {

// Minimal RFC 6455 client (text messages only), used for the pararam.io
// sockets. Qt WebSockets is not part of the Qt build Telegram Desktop uses.
// Lives on the main thread, all callbacks are invoked on the main thread.
class WebSocket final : public QObject {
public:
	struct Header {
		QByteArray name;
		QByteArray value;
	};

	// Close codes that do not come from the server.
	static constexpr auto kCodeAbnormal = 1006;

	WebSocket();
	~WebSocket();

	std::function<void()> opened;
	std::function<void(const QByteArray &text)> received;
	// Called once, after the socket was opened or failed to open.
	std::function<void(int code, const QString &reason)> closed;

	void open(const QUrl &url, std::vector<Header> headers = {});
	void send(const QByteArray &text);
	void close(int code = 1000, const QByteArray &reason = QByteArray());

	[[nodiscard]] bool isOpen() const;

private:
	enum class State {
		Idle,
		Connecting,
		Handshake,
		Open,
		Closing,
		Closed,
	};

	void sendHandshake();
	void readHandshake();
	void readFrames();
	void handleFrame(int opcode, QByteArray &&payload);
	void sendFrame(int opcode, const QByteArray &payload);
	void finish(int code, const QString &reason);

	QPointer<QTcpSocket> _socket;
	QUrl _url;
	std::vector<Header> _headers;
	QByteArray _key;
	QByteArray _buffer;
	QByteArray _message; // Fragmented message being assembled.
	int _messageOpcode = 0;
	State _state = State::Idle;

};

} // namespace Pararam
