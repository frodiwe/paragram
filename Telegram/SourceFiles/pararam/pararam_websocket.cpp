/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_websocket.h"

#include "base/random.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QtEndian>
#include <QtNetwork/QSslSocket>

#include <array>

namespace Pararam {
namespace {

constexpr auto kOpContinuation = 0x0;
constexpr auto kOpText = 0x1;
constexpr auto kOpBinary = 0x2;
constexpr auto kOpClose = 0x8;
constexpr auto kOpPing = 0x9;
constexpr auto kOpPong = 0xA;
constexpr auto kMaxHandshakeSize = 64 * 1024;
constexpr auto kMaxMessageSize = 16 * 1024 * 1024;

[[nodiscard]] QByteArray AcceptFor(const QByteArray &key) {
	return QCryptographicHash::hash(
		key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
		QCryptographicHash::Sha1).toBase64();
}

} // namespace

WebSocket::WebSocket() = default;

WebSocket::~WebSocket() {
	opened = nullptr;
	received = nullptr;
	closed = nullptr;
	if (_socket) {
		_socket->disconnect(this);
		_socket->abort();
		delete _socket.data();
	}
}

void WebSocket::open(const QUrl &url, std::vector<Header> headers) {
	Expects(_state == State::Idle);

	_url = url;
	_headers = std::move(headers);
	_state = State::Connecting;

	const auto secure = (url.scheme() == u"wss"_q)
		|| (url.scheme() == u"https"_q);
	const auto socket = new QSslSocket(this);
	_socket = socket;
	QObject::connect(socket, &QTcpSocket::readyRead, this, [=] {
		if (_state == State::Handshake) {
			readHandshake();
		}
		if (_state == State::Open || _state == State::Closing) {
			readFrames();
		}
	});
	QObject::connect(socket, &QTcpSocket::disconnected, this, [=] {
		finish(kCodeAbnormal, u"Disconnected."_q);
	});
	QObject::connect(socket, &QAbstractSocket::errorOccurred, this, [=](
			QAbstractSocket::SocketError) {
		if (_state != State::Closed) {
			finish(kCodeAbnormal, socket->errorString());
		}
	});
	QObject::connect(socket, &QSslSocket::sslErrors, this, [=](
			const QList<QSslError> &errors) {
		auto text = QStringList();
		for (const auto &error : errors) {
			text.push_back(error.errorString());
		}
		finish(kCodeAbnormal, text.join(u", "_q));
	});
	const auto port = url.port(secure ? 443 : 80);
	if (secure) {
		QObject::connect(socket, &QSslSocket::encrypted, this, [=] {
			sendHandshake();
		});
		socket->connectToHostEncrypted(url.host(), port);
	} else {
		QObject::connect(socket, &QTcpSocket::connected, this, [=] {
			sendHandshake();
		});
		socket->connectToHost(url.host(), port);
	}
}

void WebSocket::sendHandshake() {
	if (_state != State::Connecting || !_socket) {
		return;
	}
	_state = State::Handshake;

	auto random = QByteArray(16, Qt::Uninitialized);
	base::RandomFill(random.data(), random.size());
	_key = random.toBase64();

	auto path = _url.path(QUrl::FullyEncoded);
	if (path.isEmpty()) {
		path = u"/"_q;
	}
	if (_url.hasQuery()) {
		path += '?' + _url.query(QUrl::FullyEncoded);
	}
	const auto secure = (_url.scheme() == u"wss"_q);
	const auto defaultPort = secure ? 443 : 80;
	const auto host = (_url.port(defaultPort) == defaultPort)
		? _url.host(QUrl::FullyEncoded)
		: (_url.host(QUrl::FullyEncoded)
			+ ':'
			+ QString::number(_url.port()));

	auto request = QByteArray();
	request.append("GET " + path.toUtf8() + " HTTP/1.1\r\n");
	request.append("Host: " + host.toUtf8() + "\r\n");
	request.append("Upgrade: websocket\r\n");
	request.append("Connection: Upgrade\r\n");
	request.append("Sec-WebSocket-Key: " + _key + "\r\n");
	request.append("Sec-WebSocket-Version: 13\r\n");
	for (const auto &header : _headers) {
		request.append(header.name + ": " + header.value + "\r\n");
	}
	request.append("\r\n");
	_socket->write(request);
}

void WebSocket::readHandshake() {
	_buffer.append(_socket->readAll());
	const auto end = _buffer.indexOf("\r\n\r\n");
	if (end < 0) {
		if (_buffer.size() > kMaxHandshakeSize) {
			finish(kCodeAbnormal, u"Bad handshake response."_q);
		}
		return;
	}
	const auto head = _buffer.mid(0, end);
	_buffer = _buffer.mid(end + 4);

	const auto lines = head.split('\n');
	const auto status = lines.isEmpty()
		? QByteArray()
		: lines.front().trimmed();
	const auto parts = status.split(' ');
	if (parts.size() < 2 || parts[1] != "101") {
		const auto code = (parts.size() >= 2) ? parts[1].toInt() : 0;
		// Report HTTP refusals as close codes like the browser does,
		// 4xx statuses map to the 44xx application codes pararam uses.
		finish(
			(code >= 400 && code < 500) ? (4000 + code) : kCodeAbnormal,
			QString::fromUtf8(status));
		return;
	}
	auto accept = QByteArray();
	for (auto i = 1; i < lines.size(); ++i) {
		const auto line = lines[i].trimmed();
		const auto colon = line.indexOf(':');
		if (colon > 0
			&& line.mid(0, colon).trimmed().toLower()
				== "sec-websocket-accept") {
			accept = line.mid(colon + 1).trimmed();
		}
	}
	if (accept != AcceptFor(_key)) {
		finish(kCodeAbnormal, u"Bad Sec-WebSocket-Accept."_q);
		return;
	}
	_state = State::Open;
	if (opened) {
		opened();
	}
}

void WebSocket::readFrames() {
	if (!_socket) {
		return;
	}
	_buffer.append(_socket->readAll());
	while (_state == State::Open || _state == State::Closing) {
		if (_buffer.size() < 2) {
			return;
		}
		const auto data = reinterpret_cast<const uchar*>(_buffer.constData());
		const auto fin = (data[0] & 0x80) != 0;
		const auto opcode = int(data[0] & 0x0F);
		const auto masked = (data[1] & 0x80) != 0;
		auto length = uint64(data[1] & 0x7F);
		auto offset = 2;
		if (length == 126) {
			if (_buffer.size() < 4) {
				return;
			}
			length = qFromBigEndian<quint16>(data + 2);
			offset = 4;
		} else if (length == 127) {
			if (_buffer.size() < 10) {
				return;
			}
			length = qFromBigEndian<quint64>(data + 2);
			offset = 10;
		}
		if (length > uint64(kMaxMessageSize)) {
			finish(1009, u"Message too big."_q);
			return;
		}
		auto mask = std::array<uchar, 4>();
		if (masked) {
			if (_buffer.size() < offset + 4) {
				return;
			}
			memcpy(mask.data(), data + offset, 4);
			offset += 4;
		}
		if (uint64(_buffer.size()) < offset + length) {
			return;
		}
		auto payload = _buffer.mid(offset, int(length));
		_buffer = _buffer.mid(offset + int(length));
		if (masked) {
			for (auto i = 0; i != payload.size(); ++i) {
				payload[i] = char(uchar(payload[i]) ^ mask[i % 4]);
			}
		}
		if (opcode >= kOpClose) {
			handleFrame(opcode, std::move(payload));
			continue;
		}
		if (opcode != kOpContinuation) {
			_messageOpcode = opcode;
			_message = QByteArray();
		}
		_message.append(payload);
		if (_message.size() > kMaxMessageSize) {
			finish(1009, u"Message too big."_q);
			return;
		}
		if (fin) {
			handleFrame(_messageOpcode, base::take(_message));
		}
	}
}

void WebSocket::handleFrame(int opcode, QByteArray &&payload) {
	switch (opcode) {
	case kOpText:
	case kOpBinary:
		if (_state == State::Open && received) {
			received(payload);
		}
		break;
	case kOpPing:
		sendFrame(kOpPong, payload);
		break;
	case kOpPong:
		break;
	case kOpClose: {
		const auto code = (payload.size() >= 2)
			? int(qFromBigEndian<quint16>(payload.constData()))
			: 1005;
		const auto reason = QString::fromUtf8(payload.mid(2));
		if (_state == State::Open) {
			sendFrame(kOpClose, payload.mid(0, 2));
		}
		finish(code, reason);
	} break;
	}
}

void WebSocket::sendFrame(int opcode, const QByteArray &payload) {
	if (!_socket) {
		return;
	}
	auto frame = QByteArray();
	frame.append(char(0x80 | opcode));
	const auto size = payload.size();
	if (size < 126) {
		frame.append(char(0x80 | size));
	} else if (size <= 0xFFFF) {
		frame.append(char(0x80 | 126));
		auto length = std::array<char, 2>();
		qToBigEndian<quint16>(quint16(size), length.data());
		frame.append(length.data(), 2);
	} else {
		frame.append(char(0x80 | 127));
		auto length = std::array<char, 8>();
		qToBigEndian<quint64>(quint64(size), length.data());
		frame.append(length.data(), 8);
	}
	auto mask = std::array<char, 4>();
	base::RandomFill(mask.data(), mask.size());
	frame.append(mask.data(), 4);
	const auto offset = frame.size();
	frame.append(payload);
	for (auto i = 0; i != size; ++i) {
		frame[offset + i] = char(frame[offset + i] ^ mask[i % 4]);
	}
	_socket->write(frame);
}

void WebSocket::send(const QByteArray &text) {
	if (_state == State::Open) {
		sendFrame(kOpText, text);
	}
}

void WebSocket::close(int code, const QByteArray &reason) {
	if (_state == State::Open) {
		auto payload = QByteArray(2, Qt::Uninitialized);
		qToBigEndian<quint16>(quint16(code), payload.data());
		payload.append(reason);
		sendFrame(kOpClose, payload);
		_state = State::Closing;
		if (_socket) {
			_socket->flush();
			_socket->disconnectFromHost();
		}
	} else if (_state != State::Closed) {
		finish(code, QString::fromUtf8(reason));
	}
}

bool WebSocket::isOpen() const {
	return (_state == State::Open);
}

void WebSocket::finish(int code, const QString &reason) {
	if (_state == State::Closed) {
		return;
	}
	_state = State::Closed;
	if (const auto socket = _socket.data()) {
		socket->disconnect(this);
		socket->abort();
		socket->deleteLater();
	}
	_socket = nullptr;
	if (const auto callback = base::take(closed)) {
		callback(code, reason);
	}
}

} // namespace Pararam
