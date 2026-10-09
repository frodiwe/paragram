/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_api.h"

#include <QtCore/QDataStream>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTimer>
#include <QtCore/QUrlQuery>
#include <QtNetwork/QNetworkCookie>
#include <QtNetwork/QNetworkReply>

namespace Pararam {
namespace {

constexpr auto kSerializeTag = quint32(0x50415231); // 'PAR1'
constexpr auto kXsrfHeader = "X-Xsrftoken";
constexpr auto kRequestTimeoutMs = 30 * 1000;
constexpr auto kTransferTimeoutMs = 10 * 60 * 1000;
constexpr auto kMaxRetries = 2;

[[nodiscard]] QString EnvOr(const char *name, const char *fallback) {
	const auto value = qEnvironmentVariable(name);
	return value.isEmpty() ? QString::fromLatin1(fallback) : value;
}

[[nodiscard]] QString TrimSlash(QString url) {
	while (url.endsWith('/')) {
		url.chop(1);
	}
	return url;
}

[[nodiscard]] bool LooksLikeTwoFactorRequest(const QJsonObject &object) {
	const auto check = [](const QString &value) {
		const auto lower = value.toLower();
		return lower.contains(u"two_step"_q)
			|| lower.contains(u"totp"_q)
			|| lower.contains(u"2fa"_q)
			|| lower.contains(u"second"_q);
	};
	for (auto i = object.begin(); i != object.end(); ++i) {
		if (check(i.key()) && i.value().toBool(false)) {
			return true;
		}
		if (i.value().isString() && check(i.value().toString())) {
			return true;
		}
	}
	return false;
}

} // namespace

class Api::CookieJar final : public QNetworkCookieJar {
public:
	using QNetworkCookieJar::QNetworkCookieJar;

	[[nodiscard]] QList<QNetworkCookie> cookies() const {
		return allCookies();
	}
	void setCookies(const QList<QNetworkCookie> &list) {
		setAllCookies(list);
	}

	bool setCookiesFromUrl(
			const QList<QNetworkCookie> &list,
			const QUrl &url) override {
		const auto result = QNetworkCookieJar::setCookiesFromUrl(list, url);
		if (result && changed) {
			changed();
		}
		return result;
	}

	std::function<void()> changed;

};

Api::Api()
: _jar(new CookieJar(this)) {
	_manager.setCookieJar(_jar);
	_jar->setParent(this);
	_jar->changed = [=] { changed(); };
	setBaseUrls(
		EnvOr("PARARAM_API_URL", kDefaultApiUrl),
		EnvOr("PARARAM_FILE_URL", kDefaultFilesUrl));
}

Api::~Api() {
	cancelAll();
}

void Api::setBaseUrls(const QString &api, const QString &files) {
	_apiUrl = TrimSlash(api);
	_filesUrl = TrimSlash(files);
}

QString Api::apiUrl() const {
	return _apiUrl;
}

QString Api::filesUrl() const {
	return _filesUrl;
}

QNetworkRequest Api::prepare(const QUrl &url) const {
	auto result = QNetworkRequest(url);
	result.setRawHeader("Accept", "application/json");
	result.setRawHeader("User-Agent", "pararam-desktop/1.0");
	if (!_xsrf.isEmpty()) {
		result.setRawHeader(kXsrfHeader, _xsrf.toUtf8());
	}
	result.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::NoLessSafeRedirectPolicy);
	result.setTransferTimeout(kRequestTimeoutMs);
	return result;
}

HttpError Api::ParseError(QNetworkReply *reply, const QByteArray &body) {
	auto result = HttpError();
	result.status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	result.body = body;
	const auto document = QJsonDocument::fromJson(body);
	if (document.isObject()) {
		const auto object = document.object();
		const auto error = object.value(u"error"_q);
		const auto message = object.value(u"message"_q);
		if (error.isString()) {
			result.type = error.toString();
		} else if (error.isObject()) {
			result.type = error.toObject().value(u"code"_q).toString();
		}
		if (message.isString()) {
			result.message = message.toString();
		}
		const auto codes = object.value(u"codes"_q).toObject();
		if (result.type.isEmpty() && !codes.isEmpty()) {
			result.type = codes.begin().value().toString();
		}
	}
	if (result.message.isEmpty()) {
		result.message = result.type.isEmpty()
			? reply->errorString()
			: result.type;
	}
	if (result.type.isEmpty()) {
		result.type = result.status
			? (u"HTTP_"_q + QString::number(result.status))
			: u"NETWORK_ERROR"_q;
	}
	return result;
}

uint64 Api::get(const QString &path, Done done, Fail fail) {
	return jsonRequest(Method::Get, path, QByteArray(), done, fail);
}

uint64 Api::post(
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail) {
	return jsonRequest(
		Method::Post,
		path,
		QJsonDocument(body).toJson(QJsonDocument::Compact),
		done,
		fail);
}

uint64 Api::put(
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail) {
	return jsonRequest(
		Method::Put,
		path,
		QJsonDocument(body).toJson(QJsonDocument::Compact),
		done,
		fail);
}

uint64 Api::del(const QString &path, Done done, Fail fail) {
	return jsonRequest(Method::Delete, path, QByteArray(), done, fail);
}

uint64 Api::jsonRequest(
		Method method,
		const QString &path,
		const QByteArray &body,
		Done done,
		Fail fail,
		int attempt) {
	const auto requestId = ++_requestIdCounter;
	auto request = prepare(QUrl(_apiUrl + path));
	if (method != Method::Get && method != Method::Delete) {
		request.setRawHeader("Content-Type", "application/json");
	}
	const auto reply = [&] {
		switch (method) {
		case Method::Get: return _manager.get(request);
		case Method::Post: return _manager.post(request, body);
		case Method::Put: return _manager.put(request, body);
		case Method::Delete: return _manager.deleteResource(request);
		}
		return _manager.get(request);
	}();
	_replies.emplace(requestId, reply);
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (!_replies.contains(requestId)) {
			return; // Cancelled.
		}
		finished(requestId);
		const auto data = reply->readAll();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto retry = [&](int delay) {
			QTimer::singleShot(delay, this, [=] {
				jsonRequest(method, path, body, done, fail, attempt + 1);
			});
		};
		if (reply->error() == QNetworkReply::NoError
			&& status >= 200
			&& status < 300) {
			if (status == 204 || data.trimmed().isEmpty()) {
				done(QJsonValue(QJsonObject()));
				return;
			}
			auto parseError = QJsonParseError();
			const auto document = QJsonDocument::fromJson(data, &parseError);
			if (parseError.error != QJsonParseError::NoError) {
				auto error = HttpError();
				error.status = status;
				error.type = u"BAD_JSON"_q;
				error.message = parseError.errorString();
				error.body = data;
				fail(error);
				return;
			}
			done(document.isArray()
				? QJsonValue(document.array())
				: QJsonValue(document.object()));
			return;
		}
		const auto error = ParseError(reply, data);
		if (error.type == u"xsrf"_q && attempt < kMaxRetries) {
			_xsrf = QString();
			requestXsrf([=] {
				jsonRequest(method, path, body, done, fail, attempt + 1);
			}, fail);
			return;
		}
		if ((status == 429 || status >= 500 || status == 0)
			&& attempt < kMaxRetries
			&& method == Method::Get) {
			retry(500 * (1 << attempt));
			return;
		}
		fail(error);
	});
	return requestId;
}

void Api::finished(uint64 requestId) {
	_replies.erase(requestId);
}

void Api::cancel(uint64 requestId) {
	const auto i = _replies.find(requestId);
	if (i == end(_replies)) {
		return;
	}
	const auto reply = i->second;
	_replies.erase(i);
	if (reply) {
		reply->abort();
	}
}

void Api::cancelAll() {
	auto replies = std::move(_replies);
	for (const auto &[id, reply] : replies) {
		if (reply) {
			reply->abort();
		}
	}
}

void Api::requestXsrf(std::function<void()> ready, Fail fail) {
	_xsrfWaiters.push_back(std::move(ready));
	_xsrfFailWaiters.push_back(std::move(fail));
	if (_xsrfRequesting) {
		return;
	}
	_xsrfRequesting = true;
	auto request = prepare(QUrl(_apiUrl + u"/auth/init"_q));
	const auto reply = _manager.get(request);
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		_xsrfRequesting = false;
		const auto token = QString::fromUtf8(reply->rawHeader(kXsrfHeader));
		auto waiters = std::move(_xsrfWaiters);
		auto failWaiters = std::move(_xsrfFailWaiters);
		if (!token.isEmpty()) {
			_xsrf = token;
			changed();
			for (const auto &waiter : waiters) {
				waiter();
			}
		} else {
			auto error = ParseError(reply, reply->readAll());
			if (error.type.startsWith(u"HTTP_2"_q)) {
				error.type = u"XSRF_MISSING"_q;
				error.message = u"Could not obtain the XSRF token."_q;
			}
			for (const auto &waiter : failWaiters) {
				if (waiter) {
					waiter(error);
				}
			}
		}
	});
}

void Api::ensureXsrf(std::function<void()> ready, Fail fail) {
	if (!_xsrf.isEmpty()) {
		ready();
	} else {
		requestXsrf(std::move(ready), std::move(fail));
	}
}

uint64 Api::download(
		const QString &guid,
		const QString &name,
		BytesDone done,
		Fail fail,
		Progress progress) {
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
	const auto url = _filesUrl
		+ u"/download/"_q
		+ guid
		+ '/'
		+ (encoded.isEmpty() ? u"file"_q : encoded);
	const auto requestId = ++_requestIdCounter;
	auto request = prepare(QUrl(url));
	request.setTransferTimeout(kTransferTimeoutMs);
	const auto reply = _manager.get(request);
	_replies.emplace(requestId, reply);
	if (progress) {
		QObject::connect(
			reply,
			&QNetworkReply::downloadProgress,
			this,
			[=](qint64 received, qint64 total) { progress(received, total); });
	}
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (!_replies.contains(requestId)) {
			return;
		}
		finished(requestId);
		const auto data = reply->readAll();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() == QNetworkReply::NoError
			&& status >= 200
			&& status < 300) {
			done(data);
		} else {
			fail(ParseError(reply, data));
		}
	});
	return requestId;
}

uint64 Api::downloadHead(
		const QString &guid,
		const QString &name,
		int bytes,
		BytesDone done,
		Fail fail) {
	const auto encoded = QString::fromLatin1(QUrl::toPercentEncoding(name));
	const auto url = _filesUrl
		+ u"/download/"_q
		+ guid
		+ '/'
		+ (encoded.isEmpty() ? u"file"_q : encoded);
	const auto requestId = ++_requestIdCounter;
	auto request = prepare(QUrl(url));
	request.setRawHeader(
		"Range",
		"bytes=0-" + QByteArray::number(std::max(bytes, 1) - 1));
	const auto reply = _manager.get(request);
	_replies.emplace(requestId, reply);
	QObject::connect(reply, &QNetworkReply::readyRead, this, [=] {
		// If the server ignores the range, stop after enough bytes.
		if (reply->bytesAvailable() >= bytes) {
			const auto data = reply->read(bytes);
			reply->abort();
			if (_replies.contains(requestId)) {
				finished(requestId);
				done(data);
			}
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (!_replies.contains(requestId)) {
			return;
		}
		finished(requestId);
		const auto data = reply->readAll();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (reply->error() == QNetworkReply::NoError
			&& status >= 200
			&& status < 300) {
			done(data);
		} else {
			fail(ParseError(reply, data));
		}
	});
	return requestId;
}

uint64 Api::downloadUrl(const QString &url, BytesDone done, Fail fail) {
	const auto requestId = ++_requestIdCounter;
	const auto full = url.startsWith('/') ? (_apiUrl + url) : url;
	auto request = prepare(QUrl(full));
	request.setTransferTimeout(kTransferTimeoutMs);
	const auto reply = _manager.get(request);
	_replies.emplace(requestId, reply);
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		if (!_replies.contains(requestId)) {
			return;
		}
		finished(requestId);
		const auto data = reply->readAll();
		if (reply->error() == QNetworkReply::NoError) {
			done(data);
		} else {
			fail(ParseError(reply, data));
		}
	});
	return requestId;
}

uint64 Api::upload(
		const std::vector<UploadField> &fields,
		const QString &fileName,
		const QString &mimeType,
		const QByteArray &data,
		Done done,
		Fail fail) {
	const auto requestId = ++_requestIdCounter;
	const auto boundary = QByteArray("PARARAM-DESKTOP-FORM-BOUNDARY");
	auto body = QByteArray();
	for (const auto &field : fields) {
		if (field.value.isNull()) {
			continue;
		}
		body.append("--" + boundary + "\r\n");
		body.append("Content-Disposition: form-data; name=\""
			+ field.name.toUtf8()
			+ "\"\r\n\r\n");
		body.append(field.value.toUtf8());
		body.append("\r\n");
	}
	auto escapedName = fileName;
	escapedName.replace('"', '\'');
	body.append("--" + boundary + "\r\n");
	body.append("Content-Disposition: form-data; name=\"file\"; filename=\""
		+ escapedName.toUtf8()
		+ "\"\r\n");
	body.append("Content-Type: "
		+ (mimeType.isEmpty()
			? QByteArray("application/octet-stream")
			: mimeType.toUtf8())
		+ "\r\n\r\n");
	body.append(data);
	body.append("\r\n--" + boundary + "--\r\n\r\n");

	const auto send = [=] {
		if (!_replies.contains(requestId)) {
			return;
		}
		auto request = prepare(QUrl(_filesUrl + u"/upload"_q));
		request.setRawHeader(
			"Content-Type",
			"multipart/form-data; boundary=" + boundary);
		request.setTransferTimeout(kTransferTimeoutMs);
		const auto reply = _manager.post(request, body);
		_replies[requestId] = reply;
		QObject::connect(reply, &QNetworkReply::finished, this, [=] {
			reply->deleteLater();
			if (!_replies.contains(requestId)) {
				return;
			}
			finished(requestId);
			const auto result = reply->readAll();
			const auto status = reply->attribute(
				QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (reply->error() == QNetworkReply::NoError
				&& status >= 200
				&& status < 300) {
				const auto document = QJsonDocument::fromJson(result);
				done(QJsonValue(document.object()));
			} else {
				fail(ParseError(reply, result));
			}
		});
	};
	_replies.emplace(requestId, nullptr);
	ensureXsrf(send, fail);
	return requestId;
}

QString Api::EncodeDigit(int64 value) {
	static const auto alphabet = QByteArray(
		"0123456789abcdefghijklmnopqrstuvwxyz"
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ_.");
	const auto base = int64(alphabet.size());
	if (value <= 0) {
		return u"0"_q;
	}
	auto result = QString();
	while (value > 0) {
		result.prepend(QChar(alphabet[int(value % base)]));
		value /= base;
	}
	return result;
}

void Api::login(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult)> done) {
	_authorized = false;
	_xsrf = QString();
	_jar->setCookies({});
	requestXsrf([=] {
		loginStep(email, password, code, done, 0);
	}, [=](const HttpError &error) {
		done({ LoginResult::Status::Failed, error.message });
	});
}

void Api::loginStep(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult)> done,
		int attempt) {
	auto body = QJsonObject();
	body.insert(u"email"_q, email);
	body.insert(u"password"_q, password);
	const auto failed = [=](const QString &text) {
		done({ LoginResult::Status::Failed, text });
	};
	auto request = prepare(QUrl(_apiUrl + u"/auth/login/password"_q));
	request.setRawHeader("Content-Type", "application/json");
	const auto reply = _manager.post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	QObject::connect(reply, &QNetworkReply::finished, this, [=] {
		reply->deleteLater();
		const auto data = reply->readAll();
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto object = QJsonDocument::fromJson(data).object();
		if (reply->error() != QNetworkReply::NoError
			|| status < 200
			|| status >= 300) {
			const auto error = ParseError(reply, data);
			if (error.type == u"xsrf"_q && attempt < kMaxRetries) {
				_xsrf = QString();
				requestXsrf([=] {
					loginStep(email, password, code, done, attempt + 1);
				}, [=](const HttpError &error) { failed(error.message); });
				return;
			} else if (error.type == u"captcha_required"_q) {
				failed(u"pararam.io asks for a captcha. Please log in once "
					"in the web version and try again later."_q);
				return;
			}
			failed(error.message.isEmpty()
				? u"Wrong email or password."_q
				: error.message);
			return;
		}
		const auto needsCode = LooksLikeTwoFactorRequest(object);
		if (!code.isEmpty()) {
			auto codeBody = QJsonObject();
			codeBody.insert(u"code"_q, code);
			post(u"/auth/totp"_q, codeBody, [=](const QJsonValue &) {
				loginFinish(true, done);
			}, [=](const HttpError &error) {
				failed((error.status >= 400 && error.status < 500)
					? u"Wrong two-factor authentication code."_q
					: error.message);
			});
		} else if (needsCode) {
			done({ LoginResult::Status::NeedTwoFactorCode });
		} else {
			loginFinish(false, done);
		}
	});
}

void Api::loginFinish(
		bool hadCode,
		std::function<void(LoginResult)> done) {
	get(u"/auth/next"_q, [=](const QJsonValue &) {
		get(u"/auth/init"_q, [=](const QJsonValue &) {
			get(u"/user/me"_q, [=](const QJsonValue &result) {
				const auto profile = result.toObject();
				if (profile.value(u"id"_q).toVariant().toLongLong() <= 0) {
					done({
						LoginResult::Status::Failed,
						u"Could not load the profile."_q,
					});
					return;
				}
				_authorized = true;
				changed();
				done({ LoginResult::Status::Success, QString(), profile });
			}, [=](const HttpError &error) {
				done({ LoginResult::Status::Failed, error.message });
			});
		}, [=](const HttpError &error) {
			done({ LoginResult::Status::Failed, error.message });
		});
	}, [=](const HttpError &error) {
		if (!hadCode && error.status >= 400 && error.status < 500) {
			done({ LoginResult::Status::NeedTwoFactorCode });
		} else {
			done({ LoginResult::Status::Failed, error.message });
		}
	});
}

void Api::logout(std::function<void()> done) {
	const auto finish = [=] {
		clear();
		if (done) {
			done();
		}
	};
	if (!_authorized) {
		finish();
		return;
	}
	post(u"/auth/logout"_q, QJsonObject(), [=](const QJsonValue &) {
		finish();
	}, [=](const HttpError &) {
		finish();
	});
}

bool Api::authorized() const {
	return _authorized;
}

QByteArray Api::serialize() const {
	auto result = QByteArray();
	{
		auto stream = QDataStream(&result, QIODevice::WriteOnly);
		stream.setVersion(QDataStream::Qt_5_1);
		const auto cookies = _jar->cookies();
		stream
			<< kSerializeTag
			<< _apiUrl
			<< _filesUrl
			<< _xsrf
			<< qint32(_authorized ? 1 : 0)
			<< qint32(cookies.size());
		for (const auto &cookie : cookies) {
			stream << cookie.toRawForm(QNetworkCookie::Full);
		}
	}
	return result;
}

bool Api::restore(const QByteArray &serialized) {
	if (serialized.isEmpty()) {
		return false;
	}
	auto stream = QDataStream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);
	auto tag = quint32();
	auto apiUrl = QString();
	auto filesUrl = QString();
	auto xsrf = QString();
	auto authorized = qint32();
	auto count = qint32();
	stream >> tag;
	if (tag != kSerializeTag) {
		return false;
	}
	stream >> apiUrl >> filesUrl >> xsrf >> authorized >> count;
	if (stream.status() != QDataStream::Ok || count < 0 || count > 4096) {
		return false;
	}
	auto cookies = QList<QNetworkCookie>();
	for (auto i = 0; i != count; ++i) {
		auto raw = QByteArray();
		stream >> raw;
		cookies.append(QNetworkCookie::parseCookies(raw));
	}
	if (stream.status() != QDataStream::Ok) {
		return false;
	}
	// Environment overrides win over the stored urls.
	if (qEnvironmentVariableIsEmpty("PARARAM_API_URL") && !apiUrl.isEmpty()) {
		_apiUrl = apiUrl;
	}
	if (qEnvironmentVariableIsEmpty("PARARAM_FILE_URL")
		&& !filesUrl.isEmpty()) {
		_filesUrl = filesUrl;
	}
	_xsrf = xsrf;
	_authorized = (authorized != 0);
	_jar->setCookies(cookies);
	return _authorized;
}

void Api::clear() {
	cancelAll();
	_authorized = false;
	_xsrf = QString();
	_jar->setCookies({});
	changed();
}

void Api::setChangedCallback(std::function<void()> callback) {
	_changedCallback = std::move(callback);
}

void Api::changed() {
	if (_changedCallback) {
		_changedCallback();
	}
}

} // namespace Pararam
