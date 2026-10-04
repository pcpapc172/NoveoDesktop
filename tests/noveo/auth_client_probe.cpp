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
	client.onAuthenticated = [&](const QJsonObject &user) {
		if (user.value("userId") != "test-user") app.exit(11);
		const auto saved = client.authorization();
		if (saved.contains("password") || saved.value("token") != "test-token") app.exit(12);
		++successes;
		if ((scenario == "reconnect" || scenario == "heartbeat") && successes == 1) return;
		if (scenario == "restore" && successes == 1) {
			client.restore(saved);
			return;
		}
		app.exit((scenario == "success" || scenario == "fragment" || successes == 2) ? 0 : 13);
	};
	client.onError = [&](Noveo::AuthClient::Error error) {
		using E = Noveo::AuthClient::Error;
		const auto expected = scenario == "invalid" ? E::Credentials
			: scenario == "rate" ? E::RateLimited
			: scenario == "malformed" ? E::Protocol
			: scenario == "revoked" ? E::SessionExpired : E::Connection;
		app.exit(error == expected ? 0 : 14);
	};
	QTimer::singleShot(scenario == "heartbeat" ? 45000 : 10000, &app, [&] { app.exit(15); });
	if (scenario == "revoked") {
		client.restore({ { "user", QJsonObject { { "userId", "test-user" } } }, { "token", "test-token" } });
	} else client.login("test-user", "test-password");
	const auto result = app.exec();
	client.clear();
	if (!client.authorization().isEmpty() || client.authenticated()) return 16;
	return result;
}
