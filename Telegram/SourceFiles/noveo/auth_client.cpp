/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "noveo/auth_client.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QJsonDocument>
#include <QtCore/QRandomGenerator>
#include <QtCore/QSysInfo>
#include <utility>

namespace Noveo {
namespace {
constexpr auto kMaxMessage = 16 * 1024 * 1024;
constexpr auto kTimeout = 30000;
constexpr auto kReconnectDelay = 3000;
const auto kGuid = QByteArray("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");

QJsonObject ClientInfo() {
	return {
		{ "clientName", "NoveoDesktop" },
		{ "clientVersion", "0.1" },
		{ "platform", QSysInfo::productType() },
		{ "deviceModel", QSysInfo::prettyProductName() },
	};
}
} // namespace

AuthClient::AuthClient(QUrl endpoint) : _endpoint(std::move(endpoint)) {
	_timeout.setSingleShot(true);
	_reconnect.setSingleShot(true);
	connect(&_timeout, &QTimer::timeout, this, [this] { failed(Error::Timeout); });
	connect(&_reconnect, &QTimer::timeout, this, [this] { open(); });
	connect(&_socket, &QSslSocket::encrypted, this, [this] {
		const auto random = QRandomGenerator::system();
		auto bytes = QByteArray(16, '\0');
		for (auto &byte : bytes) byte = char(random->generate());
		_key = bytes.toBase64();
		_socket.write("GET " + _endpoint.path(QUrl::FullyEncoded).toUtf8()
			+ " HTTP/1.1\r\nHost: " + _endpoint.authority().toUtf8()
			+ "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: "
			+ _key + "\r\nSec-WebSocket-Version: 13\r\n\r\n");
	});
	connect(&_socket, &QSslSocket::readyRead, this, [this] { readyRead(); });
	connect(&_socket, &QSslSocket::disconnected, this, [this] {
		if (_active) failed(Error::Connection);
	});
	connect(&_socket, &QSslSocket::errorOccurred, this, [this](auto) {
		if (_active) failed(Error::Connection);
	});
}

AuthClient::~AuthClient() {
	cancel();
}

void AuthClient::login(QString username, QString password) {
	clear();
	_pending = {
		{ "type", "login_with_password" },
		{ "username", std::move(username) },
		{ "password", std::move(password) },
	};
	open();
}

void AuthClient::restore(const QJsonObject &authorization, bool connectNow) {
	clear();
	_authorization = authorization;
	if (!_authorization.value("token").toString().isEmpty()
		&& !_authorization.value("user").toObject().value("userId").toString().isEmpty()) {
		if (connectNow) open();
	} else {
		_authorization = {};
	}
}

void AuthClient::open() {
	cancel();
	if (_endpoint.scheme() != "wss" || _endpoint.host().isEmpty()) {
		failed(Error::Protocol, true);
		return;
	}
	_active = true;
	_timeout.start(kTimeout);
	_socket.setPeerVerifyMode(QSslSocket::VerifyPeer);
	_socket.connectToHostEncrypted(_endpoint.host(), quint16(_endpoint.port(443)));
}

void AuthClient::cancel() {
	_active = false;
	_authenticated = false;
	_upgraded = false;
	_timeout.stop();
	_reconnect.stop();
	_socket.abort();
	_incoming.clear();
	_fragment.clear();
	_fragmented = false;
}

void AuthClient::clear() {
	cancel();
	_pending = {};
	_authorization = {};
}

QJsonObject AuthClient::authorization() const { return _authorization; }
bool AuthClient::authenticated() const { return _authenticated; }

void AuthClient::readyRead() {
	_incoming += _socket.readAll();
	if (_incoming.size() > kMaxMessage + 14) {
		failed(Error::Protocol, true);
		return;
	}
	if (!_upgraded && !upgrade()) return;
	if (_active && !frames()) failed(Error::Protocol, true);
}

bool AuthClient::upgrade() {
	const auto end = _incoming.indexOf("\r\n\r\n");
	if (end < 0) {
		if (_incoming.size() > 8192) failed(Error::Protocol, true);
		return false;
	}
	const auto lines = _incoming.left(end).split('\n');
	const auto expected = QCryptographicHash::hash(_key + kGuid, QCryptographicHash::Sha1).toBase64();
	auto accepted = false;
	auto upgradeHeader = false;
	auto connectionHeader = false;
	for (const auto &line : lines) {
		const auto colon = line.indexOf(':');
		const auto name = line.left(colon).trimmed().toLower();
		const auto value = line.mid(colon + 1).trimmed();
		if (name == "sec-websocket-accept") accepted = (value == expected);
		if (name == "upgrade") upgradeHeader = (value.toLower() == "websocket");
		if (name == "connection") {
			for (const auto &token : value.toLower().split(',')) {
				if (token.trimmed() == "upgrade") connectionHeader = true;
			}
		}
	}
	if (lines.isEmpty() || lines.front().simplified().split(' ').value(1) != "101"
		|| !accepted || !upgradeHeader || !connectionHeader) {
		failed(Error::Protocol, true);
		return false;
	}
	_incoming.remove(0, end + 4);
	_upgraded = true;
	auto payload = _pending;
	_pending = {}; // Do not retain passwords after sending the first frame.
	if (payload.isEmpty()) {
		payload = {
			{ "type", "reconnect" },
			{ "userId", _authorization.value("user").toObject().value("userId") },
			{ "token", _authorization.value("token") },
		};
	}
	payload.insert("languageCode", "en");
	payload.insert("clientInfo", ClientInfo());
	writeFrame(1, QJsonDocument(payload).toJson(QJsonDocument::Compact));
	return true;
}

bool AuthClient::frames() {
	while (_active && _incoming.size() >= 2) {
		const auto first = quint8(_incoming[0]);
		const auto second = quint8(_incoming[1]);
		const auto opcode = first & 15;
		const auto final = bool(first & 128);
		auto length = quint64(second & 127);
		auto header = 2;
		if ((first & 112) || (second & 128)) return false;
		if (length == 126 || length == 127) {
			const auto bytes = (length == 126) ? 2 : 8;
			if (_incoming.size() < 2 + bytes) return true;
			length = 0;
			for (auto i = 0; i < bytes; ++i) length = (length << 8) | quint8(_incoming[2 + i]);
			header += bytes;
		}
		if (length > kMaxMessage || (opcode >= 8 && (!final || length > 125))) return false;
		if (quint64(_incoming.size()) < quint64(header) + length) return true;
		const auto payload = _incoming.mid(header, int(length));
		_incoming.remove(0, header + int(length));
		if (opcode == 8) {
			if (payload.size() == 1) return false;
			const auto code = (payload.size() >= 2)
				? ((quint8(payload[0]) << 8) | quint8(payload[1])) : 1000;
			const auto expired = !_authorization.isEmpty() && (code == 4001 || code == 4002 || code == 4003);
			failed(expired ? Error::SessionExpired : code == 4008 ? Error::RateLimited
				: (code == 4001 || code == 4002 || code == 4003) ? Error::Credentials : Error::Connection,
				expired || (code >= 4000));
			return true;
		} else if (opcode == 9) {
			writeFrame(10, payload);
		} else if (opcode == 10) {
			continue;
		} else if (opcode == 1 || opcode == 0) {
			if (opcode == 1 && _fragmented) return false;
			if (opcode == 0 && !_fragmented) return false;
			_fragmented = !final;
			_fragment += payload;
			if (_fragment.size() > kMaxMessage) return false;
			if (final) {
				const auto text = std::exchange(_fragment, {});
				message(text);
			}
		} else {
			return false;
		}
	}
	return true;
}

void AuthClient::writeFrame(quint8 opcode, const QByteArray &payload) {
	auto frame = QByteArray(1, char(128 | opcode));
	const auto length = quint64(payload.size());
	if (length < 126) frame.append(char(128 | length));
	else {
		const auto bytes = (length <= 65535) ? 2 : 8;
		frame.append(char(128 | ((bytes == 2) ? 126 : 127)));
		for (auto i = bytes - 1; i >= 0; --i) frame.append(char(length >> (8 * i)));
	}
	const auto mask = QRandomGenerator::system()->generate();
	const auto key = reinterpret_cast<const char*>(&mask);
	frame.append(key, 4);
	for (auto i = 0; i < payload.size(); ++i) frame.append(char(payload[i] ^ key[i % 4]));
	_socket.write(frame);
}

void AuthClient::message(const QByteArray &payload) {
	const auto document = QJsonDocument::fromJson(payload);
	if (!document.isObject()) { failed(Error::Protocol, true); return; }
	const auto object = document.object();
	const auto type = object.value("type").toString();
	if (type == "login_success") {
		auto user = object.value("user").toObject();
		if (!user.contains("userId")) user.insert("userId", user.value("id"));
		const auto token = object.value("token").toString(_authorization.value("token").toString());
		if (user.value("userId").toString().isEmpty() || token.isEmpty()) {
			failed(Error::Protocol, true); return;
		}
		_authorization = { { "user", user }, { "token", token } };
		_authenticated = true;
		_timeout.stop();
		if (onConnectionChanged) onConnectionChanged(true);
		if (onAuthenticated) onAuthenticated(user);
	} else if (type == "error") {
		failed(_authorization.isEmpty() ? Error::Credentials : Error::SessionExpired, true);
	} else if (type == "login_totp_required") {
		failed(Error::TwoFactorRequired, true);
	} else if (type == "session_revoked") {
		failed(Error::SessionExpired, true);
	}
}

void AuthClient::failed(Error error, bool terminal) {
	const auto reconnect = !terminal && !_authorization.isEmpty();
	cancel();
	_pending = {};
	if (terminal) _authorization = {};
	if (onConnectionChanged) onConnectionChanged(false);
	if (reconnect) _reconnect.start(kReconnectDelay);
	else if (onError) onError(error);
}
} // namespace Noveo
