/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/basic_types.h"

#include <QtCore/QObject>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkCookieJar>

#include <map>
#include <memory>
#include <vector>

class QNetworkReply;

namespace Pararam {

struct HttpError {
	int status = 0; // 0 means a network error.
	QString type; // Short machine readable error, like "xsrf".
	QString message; // Human readable message.
	QByteArray body;

	[[nodiscard]] bool unauthorized() const {
		return (status == 401);
	}
};

struct LoginResult {
	enum class Status {
		Success,
		NeedTwoFactorCode,
		Failed,
	};
	Status status = Status::Failed;
	QString error;
	QJsonObject profile;
};

struct UploadField {
	QString name;
	QString value;
};

// Thin asynchronous wrapper around the pararam.io REST API.
// Lives on the main thread, all callbacks are invoked on the main thread.
class Api final : public QObject {
public:
	using Done = std::function<void(const QJsonValue &result)>;
	using Fail = std::function<void(const HttpError &error)>;
	using BytesDone = std::function<void(const QByteArray &bytes)>;
	using Progress = std::function<void(qint64 received, qint64 total)>;

	Api();
	~Api();

	static constexpr auto kDefaultApiUrl = "https://api.pararam.io";
	static constexpr auto kDefaultFilesUrl = "https://file.pararam.io";

	// Reads PARARAM_API_URL and PARARAM_FILE_URL from the environment
	// so that a self-hosted server or a local mock can be used.
	void setBaseUrls(const QString &api, const QString &files);
	[[nodiscard]] QString apiUrl() const;
	[[nodiscard]] QString filesUrl() const;

	uint64 get(const QString &path, Done done, Fail fail);
	uint64 post(
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail);
	uint64 put(
		const QString &path,
		const QJsonObject &body,
		Done done,
		Fail fail);
	uint64 del(const QString &path, Done done, Fail fail);

	uint64 download(
		const QString &guid,
		const QString &name,
		BytesDone done,
		Fail fail,
		Progress progress = nullptr);
	uint64 downloadUrl(const QString &url, BytesDone done, Fail fail);

	// Requests only the beginning of a file, if the server supports ranges.
	uint64 downloadHead(
		const QString &guid,
		const QString &name,
		int bytes,
		BytesDone done,
		Fail fail);
	uint64 upload(
		const std::vector<UploadField> &fields,
		const QString &fileName,
		const QString &mimeType,
		const QByteArray &data,
		Done done,
		Fail fail);

	void cancel(uint64 requestId);
	void cancelAll();

	void login(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult)> done);
	void logout(std::function<void()> done);

	[[nodiscard]] bool authorized() const;
	[[nodiscard]] QByteArray serialize() const;
	bool restore(const QByteArray &serialized);
	void clear();

	// Called whenever cookies or the xsrf token changed and
	// the session should be persisted.
	void setChangedCallback(std::function<void()> callback);

	[[nodiscard]] static QString EncodeDigit(int64 value);

private:
	class CookieJar;
	struct Pending;

	enum class Method {
		Get,
		Post,
		Put,
		Delete,
	};

	uint64 jsonRequest(
		Method method,
		const QString &path,
		const QByteArray &body,
		Done done,
		Fail fail,
		int attempt = 0);
	void ensureXsrf(std::function<void()> ready, Fail fail);
	void requestXsrf(std::function<void()> ready, Fail fail);
	void finished(uint64 requestId);

	[[nodiscard]] QNetworkRequest prepare(const QUrl &url) const;
	[[nodiscard]] static HttpError ParseError(
		QNetworkReply *reply,
		const QByteArray &body);
	void changed();

	void loginStep(
		const QString &email,
		const QString &password,
		const QString &code,
		std::function<void(LoginResult)> done,
		int attempt);
	void loginFinish(
		bool hadCode,
		std::function<void(LoginResult)> done);

	QNetworkAccessManager _manager;
	CookieJar *_jar = nullptr;
	QString _apiUrl;
	QString _filesUrl;
	QString _xsrf;
	bool _authorized = false;
	uint64 _requestIdCounter = 0;
	std::map<uint64, QPointer<QNetworkReply>> _replies;
	std::vector<std::function<void()>> _xsrfWaiters;
	std::vector<Fail> _xsrfFailWaiters;
	bool _xsrfRequesting = false;
	std::function<void()> _changedCallback;

};

} // namespace Pararam
