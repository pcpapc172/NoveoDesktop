#include "noveo/auth_client.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtNetwork/QSslConfiguration>
#include <iostream>

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	const auto scenario = app.arguments().at(2);
	if (scenario != "untrusted") {
		QFile file(app.arguments().at(3));
		if (!file.open(QIODevice::ReadOnly)) return 10;
		auto config = QSslConfiguration::defaultConfiguration();
		config.addCaCertificates(QSslCertificate::fromData(file.readAll()));
		QSslConfiguration::setDefaultConfiguration(config);
	}
	Noveo::AuthClient client(QUrl(app.arguments().at(1)));
	int successes = 0;
	int disconnects = 0;
	const auto proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", app.arguments().value(4).toUShort(),
		"proxy-user", "proxy-password");
	client.onConnectionChanged = [&](bool connected) {
		if (!connected) ++disconnects;
	};
	client.onProxyChecked = [&](int ping) {
		app.exit((scenario == "probe" ? ping > 0 : ping == -1) ? 0 : 17);
	};
	if (scenario.startsWith("proxy")) {
		client.setProxy(proxy);
	}
	client.onAuthenticated = [&](const QJsonObject &user) {
		if (user.value("userId") != "test-user") app.exit(11);
		const auto saved = client.authorization();
		if (saved.contains("password") || saved.value("token") != "test-token") app.exit(12);
		++successes;
		if (successes == 1 && scenario == "offline") {
			client.networkAvailable(false);
			if (client.authenticated() || disconnects != 1) app.exit(18);
			QTimer::singleShot(50, &app, [&] { client.networkAvailable(true); });
			return;
		}
		if (successes == 1 && (scenario == "proxy_switch" || scenario == "proxy_disable")) {
			client.setProxy(scenario == "proxy_disable"
				? QNetworkProxy(QNetworkProxy::NoProxy)
				: QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", app.arguments().value(4).toUShort()));
			return;
		}
		if (successes == 2 && (scenario == "offline" || scenario == "proxy_switch" || scenario == "proxy_disable") && disconnects == 0) {
			app.exit(19);
			return;
		}
		if ((scenario == "reconnect" || scenario == "heartbeat") && successes == 1) return;
		if (scenario == "restore" && successes == 1) {
			client.restore(saved);
			return;
		}
		app.exit((scenario == "totp" || scenario == "totp_retry" || scenario == "success" || scenario == "fragment" || scenario == "proxy" || successes == 2) ? 0 : 13);
	};
	if (scenario.startsWith("totp")) client.onTotpRequired = [&] {
		if (client.submitTotp("12345") || client.submitTotp("12ab56")) { app.exit(20); return; }
		if (!client.submitTotp(scenario == "totp_retry" ? "000000" : "123456")) app.exit(21);
	};
	client.onError = [&](Noveo::AuthClient::Error error) {
		using E = Noveo::AuthClient::Error;
		if (scenario == "totp_retry" && error == E::Credentials) {
			if (!client.submitTotp("123456")) app.exit(22);
			return;
		}
		const auto expected = scenario == "invalid" ? E::Credentials
			: scenario == "rate" ? E::RateLimited
			: scenario == "malformed" ? E::Protocol
			: scenario == "revoked" ? E::SessionExpired : E::Connection;
		app.exit(error == expected ? 0 : 14);
	};
	QTimer::singleShot(scenario == "heartbeat" ? 45000 : 10000, &app, [&] { app.exit(15); });
	if (scenario.startsWith("probe")) {
		client.checkProxy(proxy);
	} else if (scenario == "revoked") {
		client.restore({ { "user", QJsonObject { { "userId", "test-user" } } }, { "token", "test-token" } });
	} else client.login("test-user", "test-password");
	const auto result = app.exec();
	client.clear();
	if (!client.authorization().isEmpty() || client.authenticated()) return 16;
	return result;
}
