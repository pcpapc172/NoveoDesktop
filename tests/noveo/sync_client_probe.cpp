#include "noveo/session_client.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtNetwork/QSslConfiguration>
#include <iostream>

namespace base::assertion {
void log(const char* message, const char* file, int line) {
	std::cerr << message << ' ' << file << ':' << line << '\n';
}
}  // namespace base::assertion

template <typename T>
T Decode(const mtpBuffer& body) {
	auto result = T();
	auto from = body.constData();
	Assert(result.read(from, from + body.size()));
	Assert(from == body.constData() + body.size());
	return result;
}

int main(int argc, char** argv) {
	QCoreApplication app(argc, argv);
	QFile cert(app.arguments().at(2));
	Assert(cert.open(QIODevice::ReadOnly));
	auto tls = QSslConfiguration::defaultConfiguration();
	tls.addCaCertificates(QSslCertificate::fromData(cert.readAll()));
	QSslConfiguration::setDefaultConfiguration(tls);
	const auto endpoint = QUrl(app.arguments().at(1));
	auto ws = endpoint.resolved(QUrl("/ws"));
	ws.setScheme("wss");
	Noveo::AuthClient authA(ws), authB(ws);
	Noveo::SessionClient a(&authA, endpoint), b(&authB, endpoint);
	const auto other = peerFromUser(Noveo::NativeUserId("other"));
	const auto group = peerFromChat(Noveo::NativeUserId("group").bare);
	const auto input = MTPInputPeer(
		MTP_inputPeerUser(MTP_long(peerToUser(other).bare), MTP_long(1)));
	const auto inputGroup
		= MTPInputPeer(MTP_inputPeerChat(MTP_long(peerToChat(group).bare)));
	const auto dp = MTPInputDialogPeer(MTP_inputDialogPeer(input));
	const auto dg = MTPInputDialogPeer(MTP_inputDialogPeer(inputGroup));
	auto ready = 0;
	auto latestB = QString();
	auto callbacks = QSet<int>();
	auto awaitingGap = false;
	auto conflictReplies = 0;
	const auto request = [&](int id, const auto& object) {
		mtpBuffer body;
		object.write(body);
		a.request(id, body);
	};
	const auto draft = [&](int id, QString text) {
		request(id,
				MTPmessages_SaveDraft(
					MTP_flags(MTPmessages_SaveDraft::Flag::f_entities),
					MTPInputReplyTo(),
					input,
					MTP_string(text),
					MTP_vector<MTPMessageEntity>(
						{MTP_messageEntityItalic(MTP_int(0), MTP_int(2))}),
					MTPInputMedia(),
					MTPlong(),
					MTPSuggestedPost(),
					MTPInputRichMessage()));
	};
	const auto event = [&](const MTPUpdates& updates) {
		for (const auto& u : updates.c_updates().vupdates().v) {
			if (u.type() == mtpc_updateDraftMessage) {
				const auto& d = u.c_updateDraftMessage().vdraft();
				latestB = d.type() == mtpc_draftMessage
							  ? qs(d.c_draftMessage().vmessage())
							  : QString();
			}
		}
	};
	b.onUpdate = event;
	a.onUpdate = [&](const MTPUpdates& updates) {
		for (const auto& u : updates.c_updates().vupdates().v) {
			if (awaitingGap && u.type() == mtpc_updateDialogFilters) {
				awaitingGap = false;
				request(100, MTPmessages_GetDialogFilters());
			}
		}
	};
	const auto start = [&] {
		if (++ready == 2)
			request(1, MTPmessages_GetPinnedDialogs(MTP_int(0)));
	};
	authA.onAuthenticated
		= [&](const QJsonObject& user) { a.authenticated(user); };
	authB.onAuthenticated
		= [&](const QJsonObject& user) { b.authenticated(user); };
	authA.onMessage = [&](const QJsonObject& frame) {
		a.message(frame);
		if (frame.value("type") == "fixture_ready")
			start();
	};
	authB.onMessage = [&](const QJsonObject& frame) {
		b.message(frame);
		if (frame.value("type") == "fixture_ready")
			start();
	};
	a.onReply = [&](int id, mtpBuffer body) {
		if (id <= 0)
			return;
		std::cerr << "sync response " << id << '\n';
		Assert(!callbacks.contains(id));
		callbacks.insert(id);
		if (id == 1) {
			const auto pins = Decode<MTPmessages_PeerDialogs>(body);
			Assert(pins.c_messages_peerDialogs().vdialogs().v.size() == 1);
			Assert(peerFromMTP(pins.c_messages_peerDialogs()
								   .vdialogs()
								   .v.front()
								   .c_dialog()
								   .vpeer())
				   == other);
			request(2,
					MTPmessages_GetDialogs(
						MTP_flags(MTPmessages_GetDialogs::Flag::f_folder_id),
						MTP_int(1),
						MTPint(),
						MTPint(),
						MTP_inputPeerEmpty(),
						MTP_int(100),
						MTPlong()));
		} else if (id == 2) {
			const auto page = Decode<MTPmessages_Dialogs>(body);
			Assert(page.c_messages_dialogs().vdialogs().v.size() == 1);
			Assert(peerFromMTP(page.c_messages_dialogs()
								   .vdialogs()
								   .v.front()
								   .c_dialog()
								   .vpeer())
				   == group);
			request(3, MTPmessages_GetAllDrafts());
		} else if (id == 3) {
			const auto all = Decode<MTPUpdates>(body);
			const auto& d = all.c_updates()
								.vupdates()
								.v.front()
								.c_updateDraftMessage()
								.vdraft()
								.c_draftMessage();
			Assert(qs(d.vmessage()) == QString::fromUtf8("😀Hi"));
			Assert(d.ventities()->v.front().c_messageEntityBold().voffset().v
				   == 2);
			Assert(d.vreply_to()
				   && d.vreply_to()
							  ->c_inputReplyToMessage()
							  .vreply_to_msg_id()
							  .v
						  > 0);
			request(4, MTPmessages_GetDialogFilters());
		} else if (id == 4) {
			const auto all = Decode<MTPmessages_DialogFilters>(body);
			Assert(all.c_messages_dialogFilters().vfilters().v.size() == 2);
			const auto& f = all.c_messages_dialogFilters()
								.vfilters()
								.v.front()
								.c_dialogFilter();
			Assert(qs(f.vtitle().c_textWithEntities().vtext()) == "Work"
				   && f.is_contacts());
			Assert(f.vinclude_peers().v.size() == 2);
			draft(10, "first edit");
			draft(11, "second edit");
		} else if (id == 10) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
		} else if (id == 11) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(12, MTPmessages_GetAllDrafts());
		} else if (id == 12) {
			Assert(qs(Decode<MTPUpdates>(body)
						  .c_updates()
						  .vupdates()
						  .v.front()
						  .c_updateDraftMessage()
						  .vdraft()
						  .c_draftMessage()
						  .vmessage())
				   == "second edit");
			request(
				13,
				MTPfolders_EditPeerFolders(MTP_vector<MTPInputFolderPeer>(
					{MTP_inputFolderPeer(inputGroup, MTP_int(0))})));
		} else if (id == 13) {
			Decode<MTPUpdates>(body);
			request(
				14,
				MTPmessages_ToggleDialogPin(
					MTP_flags(MTPmessages_ToggleDialogPin::Flag::f_pinned),
					dg));
		} else if (id == 14) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(15,
					MTPmessages_ReorderPinnedDialogs(
						MTP_flags(
							MTPmessages_ReorderPinnedDialogs::Flag::f_force),
						MTP_int(0),
						MTP_vector<MTPInputDialogPeer>({dp, dg})));
		} else if (id == 15) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(16, MTPmessages_GetPinnedDialogs(MTP_int(0)));
		} else if (id == 16) {
			const auto pins = Decode<MTPmessages_PeerDialogs>(body);
			Assert(pins.c_messages_peerDialogs().vdialogs().v.size() == 2);
			Assert(peerFromMTP(pins.c_messages_peerDialogs()
								   .vdialogs()
								   .v.front()
								   .c_dialog()
								   .vpeer())
				   == other);
			request(
				17,
				MTPmessages_MarkDialogUnread(
					MTP_flags(MTPmessages_MarkDialogUnread::Flag::f_unread),
					MTPInputPeer(),
					dp));
		} else if (id == 17) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(18,
					MTPmessages_GetDialogUnreadMarks(
						MTP_flags(MTPmessages_GetDialogUnreadMarks::Flags()),
						MTPInputPeer()));
		} else if (id == 18) {
			Assert(Decode<MTPVector<MTPDialogPeer>>(body).v.size() == 1);
			Assert(latestB == "second edit");
			draft(40, "stale draft");
			draft(41, "newer unsent draft");
		} else if (id == 40 || id == 41) {
			Assert(
				qs(Decode<MTPRpcError>(body).c_rpc_error().verror_message())
				== "STATE_VERSION_CONFLICT");
			if (++conflictReplies == 2)
				request(42, MTPmessages_GetAllDrafts());
		} else if (id == 42) {
			Assert(qs(Decode<MTPUpdates>(body)
						  .c_updates()
						  .vupdates()
						  .v.front()
						  .c_updateDraftMessage()
						  .vdraft()
						  .c_draftMessage()
						  .vmessage())
				   == "Android conflict");
			const auto saved
				= authA.authorization().value("accountSync").toObject();
			Assert(saved.value("conflicts")
					   .toObject()
					   .value("draft|user:other")
					   .toObject()
					   .value("value")
					   .toObject()
					   .value("text")
				   == "newer unsent draft");
			awaitingGap = true;
			Assert(authA.send({{"type", "fixture_gap"}}));
		} else if (id == 100) {
			const auto all = Decode<MTPmessages_DialogFilters>(body);
			Assert(all.c_messages_dialogFilters().vfilters().v.size() == 3);
			request(101,
					MTPmessages_UpdateDialogFilter(
						MTP_flags(MTPmessages_UpdateDialogFilter::Flags()),
						MTP_int(2),
						MTPDialogFilter()));
		} else if (id == 101) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(102, MTPmessages_GetDialogFilters());
		} else if (id == 102) {
			Assert(Decode<MTPmessages_DialogFilters>(body)
					   .c_messages_dialogFilters()
					   .vfilters()
					   .v.size()
				   == 2);
			request(103,
					MTPmessages_SaveDraft(
						MTP_flags(MTPmessages_SaveDraft::Flags()),
						MTPInputReplyTo(),
						input,
						MTP_string(QString()),
						MTPVector<MTPMessageEntity>(),
						MTPInputMedia(),
						MTPlong(),
						MTPSuggestedPost(),
						MTPInputRichMessage()));
		} else if (id == 103) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(104, MTPmessages_GetAllDrafts());
		} else if (id == 104) {
			Assert(Decode<MTPUpdates>(body)
					   .c_updates()
					   .vupdates()
					   .v.front()
					   .c_updateDraftMessage()
					   .vdraft()
					   .type()
				   == mtpc_draftMessageEmpty);
			Assert(authB.authorization()
					   .value("accountSync")
					   .toObject()
					   .value("pending")
					   .toObject()
					   .isEmpty());
			app.exit(0);
		} else
			Assert(false);
	};
	authA.login("first", "fixture");
	authB.login("second", "fixture");
	QTimer::singleShot(30000, &app, [&] { app.exit(2); });
	return app.exec();
}
