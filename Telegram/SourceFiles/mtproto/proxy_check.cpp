/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/proxy_check.h"

#include "mtproto/facade.h"
#include "noveo/auth_client.h"
#include "mtproto/mtproto_dc_options.h"

namespace MTP {

using Connection = details::AbstractConnection;

namespace {

class NoveoProxyConnection final : public Connection {
public:
	explicit NoveoProxyConnection(const ProxyData &proxy)
	: Connection(QThread::currentThread(), proxy)
	, _client(QUrl("wss://noveo.ir:8443/ws")) {
		_client.onProxyChecked = [this](int ping) {
			_pingTime = ping;
			if (ping > 0) {
				emit connected();
			} else {
				emit error(kErrorCodeOther);
			}
		};
	}

	details::ConnectionPointer clone(const ProxyData &proxy) override {
		return details::ConnectionPointer::New<NoveoProxyConnection>(proxy);
	}
	crl::time pingTime() const override { return _pingTime; }
	crl::time fullConnectTimeout() const override { return 10000; }
	void sendData(mtpBuffer &&) override { }
	void disconnectFromServer() override { _client.cancel(); }
	void connectToServer(const QString &, int, const bytes::vector &, int16, bool) override {
		_client.checkProxy(ToNetworkProxy(_proxy));
	}
	bool isConnected() const override { return _pingTime > 0; }
	int32 debugState() const override { return isConnected() ? 1 : 0; }
	QString transport() const override { return u"WebSocket"_q; }
	QString tag() const override { return u"NoveoProxyCheck"_q; }

private:
	Noveo::AuthClient _client;
};

} // namespace


void ResetProxyCheckers(
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6) {
	v4 = nullptr;
	v6 = nullptr;
}

void DropProxyChecker(
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6,
		not_null<Connection*> raw) {
	if (v4.get() == raw) {
		v4 = nullptr;
	} else if (v6.get() == raw) {
		v6 = nullptr;
	}
}

bool HasProxyCheckers(
		const ProxyCheckConnection &v4,
		const ProxyCheckConnection &v6) {
	return v4 || v6;
}

void StartProxyCheck(
		not_null<Instance*> mtproto,
		const ProxyData &proxy,
		bool tryIPv6,
		ProxyCheckConnection &v4,
		ProxyCheckConnection &v6,
		Fn<void(Connection *raw, int ping)> done,
		Fn<void(Connection *raw)> fail) {
	using Variants = DcOptions::Variants;

	ResetProxyCheckers(v4, v6);
	if (mtproto->isNoveo()) {
		if (proxy.type != ProxyData::Type::Socks5
			&& proxy.type != ProxyData::Type::Http) {
			return;
		}
		v4 = details::ConnectionPointer::New<NoveoProxyConnection>(proxy);
		const auto raw = v4.get();
		raw->connect(raw, &Connection::connected, [=] {
			if (done) done(raw, int(raw->pingTime()));
		});
		raw->connect(raw, &Connection::error, [=] {
			if (fail) fail(raw);
		});
		v4->connectToServer({}, 0, {}, 0, false);
		return;
	}
	if (proxy.type == ProxyData::Type::Web) {
		return;
	}
	const auto connType = (proxy.type == ProxyData::Type::Http)
		? Variants::Http
		: Variants::Tcp;
	const auto dcId = mtproto->mainDcId();
	const auto setup = [&](ProxyCheckConnection &checker, const bytes::vector &secret) {
		checker = Connection::Create(
			mtproto,
			connType,
			QThread::currentThread(),
			secret,
			proxy);
		const auto raw = checker.get();
		raw->connect(raw, &Connection::connected, [=] {
			if (done) {
				done(raw, raw->pingTime());
			}
		});
		const auto failed = [=] {
			if (fail) {
				fail(raw);
			}
		};
		raw->connect(raw, &Connection::disconnected, failed);
		raw->connect(raw, &Connection::error, failed);
	};
	if (proxy.type == ProxyData::Type::Mtproto) {
		const auto secret = proxy.secretFromMtprotoPassword();
		setup(v4, secret);
		v4->connectToServer(
			proxy.host,
			proxy.port,
			secret,
			dcId,
			false);
		return;
	}
	const auto options = mtproto->dcOptions().lookup(
		dcId,
		DcType::Regular,
		true);
	const auto tryConnect = [&](ProxyCheckConnection &checker, Variants::Address address) {
		const auto &list = options.data[address][connType];
		if (list.empty() || ((address == Variants::IPv6) && !tryIPv6)) {
			checker = nullptr;
			return;
		}
		const auto &endpoint = list.front();
		setup(checker, endpoint.secret);
		checker->connectToServer(
			QString::fromStdString(endpoint.ip),
			endpoint.port,
			endpoint.secret,
			dcId,
			false);
	};
	tryConnect(v4, Variants::IPv4);
	tryConnect(v6, Variants::IPv6);
}

} // namespace MTP
