/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <QtCore/QJsonObject>
#include <QtCore/QElapsedTimer>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QNetworkProxy>
#include <functional>

namespace Noveo {

// Account-owned connection: passwords are used for the first frame only.
class AuthClient final : public QObject {
public:
	enum class Error {
		Connection,
		Timeout,
		Credentials,
		RateLimited,
		Protocol,
		TwoFactorRequired,
		SessionExpired,
	};

	explicit AuthClient(QUrl endpoint = QUrl(QStringLiteral("wss://noveo.ir:8443/ws")));
	~AuthClient();
	void login(QString username, QString password);
	bool submitTotp(QString code);
	bool replaceToken(QString token, QString sessionId);
	void restore(const QJsonObject &authorization, bool connectNow = true);
	void setProxy(const QNetworkProxy &proxy);
	void networkAvailable(bool available);
	void checkProxy(const QNetworkProxy &proxy);
	void cancel();
	void clear();
	bool send(const QJsonObject &message);
	[[nodiscard]] QJsonObject authorization() const;
	[[nodiscard]] bool authenticated() const;
	[[nodiscard]] bool awaitingTotp() const { return _awaitingTotp; }

	std::function<void(const QJsonObject&)> onAuthenticated;
	std::function<void()> onAuthorizationChanged;
	std::function<void(Error)> onError;
	std::function<void()> onTotpRequired;
	std::function<void(int)> onProxyChecked;
	std::function<void(bool)> onConnectionChanged;
	std::function<void(QString)> onDiagnostic;
	std::function<void(const QJsonObject&)> onMessage;

private:
	void open();
	void readyRead();
	bool upgrade();
	bool frames();
	void message(const QByteArray &payload);
	void writeFrame(quint8 opcode, const QByteArray &payload);
	void failed(Error error, bool terminal = false);

	QUrl _endpoint;
	QElapsedTimer _probeStarted;
	QSslSocket _socket;
	QTimer _timeout;
	QTimer _reconnect;
	QTimer _heartbeat;
	QByteArray _key;
	QByteArray _incoming;
	QByteArray _fragment;
	QJsonObject _pending;
	QJsonObject _authorization;
	bool _awaitingTotp = false;
	bool _probing = false;
	bool _networkAvailable = true;
	bool _upgraded = false;
	bool _authenticated = false;
	bool _active = false;
	bool _fragmented = false;
};

} // namespace Noveo
