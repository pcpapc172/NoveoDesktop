#include "noveo/session_client.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtNetwork/QSslConfiguration>
#include <iostream>
#include <set>

namespace base::assertion {
void log(const char *message, const char *file, int line) {
	std::cerr << message << ' ' << file << ':' << line << '\n';
}
} // namespace base::assertion

template <typename T> T Decode(const mtpBuffer &buffer) {
	auto result = T();
	auto from = buffer.constData();
	Assert(result.read(from, from + buffer.size()));
	Assert(from == buffer.constData() + buffer.size());
	return result;
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	QFile file(app.arguments().at(2));
	Assert(file.open(QIODevice::ReadOnly));
	auto config = QSslConfiguration::defaultConfiguration();
	config.addCaCertificates(QSslCertificate::fromData(file.readAll()));
	QSslConfiguration::setDefaultConfiguration(config);
	const auto base = QUrl(app.arguments().at(1));
	auto endpoint = base.resolved(QUrl("/ws"));
	endpoint.setScheme("wss");
	Noveo::AuthClient auth(endpoint);
	Noveo::SessionClient client(&auth, base);
	std::set<int> checks;
	int connects = 0;
	bool disconnected = false;
	int oldest = 0;

	const auto request = [&](int id, const auto &value) {
		mtpBuffer buffer;
		value.write(buffer);
		client.request(id, buffer);
	};
	const auto finish = [&] {
		if (checks.size() == 12 && connects == 2 && disconnected) {
			app.exit(0);
		}
	};
	client.onAvatar = [&](PeerId peer, const QUrl &url) {
		Assert(peerIsUser(peer));
		Assert(url.scheme() == "https");
		checks.insert(100);
		finish();
	};
	client.onReply = [&](int id, mtpBuffer buffer) {
		if (id == 1) {
			const auto result = Decode<MTPmessages_Dialogs>(buffer);
			const auto &data = result.c_messages_dialogs();
			Assert(data.vdialogs().v.size() == 3);
			Assert(data.vchats().v.size() == 2);
			Assert(data.vmessages().v.size() == 3);
			request(10,
				MTPusers_GetFullUser(
					MTP_inputUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1))));
			const auto peer = MTPInputPeer(
				MTP_inputPeerUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1)));
			request(4,
				MTPmessages_GetHistory(peer, MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50),
					MTP_int(0), MTP_int(0), MTP_long(0)));
			request(7,
				MTPmessages_SendMessage(MTP_flags(MTPmessages_SendMessage::Flags()), peer,
					MTPInputReplyTo(), MTP_string("desktop test"), MTP_long(42), MTPReplyMarkup(),
					MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPInputPeer(),
					MTPInputQuickReplyShortcut(), MTPlong(), MTPlong(), MTPSuggestedPost(),
					MTPInputRichMessage()));
		} else if (id == 2) {
			const auto result = Decode<MTPcontacts_Contacts>(buffer);
			const auto &data = result.c_contacts_contacts();
			Assert(data.vcontacts().v.size() == 1);
			Assert(data.vcontacts().v.front().c_contact().vuser_id().v
				== qint64(Noveo::NativeUserId("other-user").bare));
			bool contact = false;
			for (const auto &user : data.vusers().v) {
				if (user.c_user().vid().v == qint64(Noveo::NativeUserId("other-user").bare)) {
					contact = user.c_user().is_contact();
				}
			}
			Assert(contact);
		} else if (id == 3) {
			const auto result = Decode<MTPmessages_PeerDialogs>(buffer);
			Assert(result.c_messages_peerDialogs().vdialogs().v.empty());
		} else if (id == 4 || id == 5) {
			const auto result = Decode<MTPmessages_Messages>(buffer);
			const auto &data = result.c_messages_messagesSlice();
			Assert(data.vmessages().v.size() == 2);
			Assert(data.vmessages().v.front().c_message().vid().v
				> data.vmessages().v.back().c_message().vid().v);
			if (id == 4) {
				oldest = data.vmessages().v.back().c_message().vid().v;
				const auto peer = MTPInputPeer(MTP_inputPeerUser(
					MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1)));
				request(5,
					MTPmessages_GetHistory(peer, MTP_int(oldest), MTP_int(0), MTP_int(0),
						MTP_int(50), MTP_int(0), MTP_int(0), MTP_long(0)));
			} else {
				Assert(data.vmessages().v.front().c_message().vid().v < oldest);
				request(8,
					MTPmessages_ReadHistory(
						MTP_inputPeerUser(
							MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1)),
						MTP_int(oldest)));
			}
		} else if (id == 6 || id == 9) {
			const auto result = Decode<MTPRpcError>(buffer);
			Assert(result.c_rpc_error().verror_code().v == 400);
		} else if (id == 7) {
			const auto result = Decode<MTPUpdates>(buffer);
			Assert(result.type() == mtpc_updateShortSentMessage);
			Assert(result.c_updateShortSentMessage().vid().v > oldest);
		} else if (id == 8) {
			Decode<MTPmessages_AffectedMessages>(buffer);
		} else if (id == 10) {
			const auto result = Decode<MTPusers_UserFull>(buffer);
			Assert(result.c_users_userFull().vfull_user().c_userFull().vid().v
				== qint64(Noveo::NativeUserId("other-user").bare));
		} else {
			Assert(false);
		}
		checks.insert(id);
		finish();
	};
	client.onDialogs = [&](const MTPmessages_Dialogs&) {
        if (connects == 2) checks.insert(101);
        finish();
    };
	auth.onMessage = [&](const QJsonObject &message) { client.message(message); };
	auth.onConnectionChanged = [&](bool connected) {
		if (!connected) {
			disconnected = true;
			client.disconnected();
		}
	};
	auth.onError = [&](auto) { app.exit(20); };
	auth.onAuthenticated = [&](const QJsonObject &profile) {
		++connects;
		client.authenticated(profile);
		if (connects == 1) {
			request(1,
				MTPmessages_GetDialogs(MTP_flags(MTPmessages_GetDialogs::Flags()), MTPint(),
					MTPint(), MTPint(), MTP_inputPeerEmpty(), MTP_int(50), MTP_long(0)));
			request(2, MTPcontacts_GetContacts(MTP_long(0)));
			request(3, MTPmessages_GetPinnedDialogs(MTP_int(0)));
			request(6, MTPhelp_GetNearestDc());
			client.request(9, {});
		}
		finish();
	};
	QTimer::singleShot(18000, &app, [&] {
		std::cerr << "Incomplete checks: " << checks.size() << ", connections: " << connects
				  << '\n';
		app.exit(21);
	});
	request(11, MTPcontacts_GetContacts(MTP_long(0)));
	client.cancel(11);
	auth.login("test-user", "test-password");
	const auto result = app.exec();
	auth.clear();
	client.reset();
	return result;
}
