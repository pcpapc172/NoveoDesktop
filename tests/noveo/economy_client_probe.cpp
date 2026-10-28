#include "noveo/session_client.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtNetwork/QSslConfiguration>
#include <iostream>

namespace base::assertion {
void log(const char *message, const char *file, int line) {
	std::cerr << message << ' ' << file << ':' << line << '\n';
}
} // namespace base::assertion

template <typename T> T Decode(const mtpBuffer &body) {
	auto result = T();
	auto from = body.constData();
	Assert(result.read(from, from + body.size()));
	Assert(from == body.constData() + body.size());
	return result;
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	QFile cert(app.arguments().at(2));
	Assert(cert.open(QIODevice::ReadOnly));
	auto ssl = QSslConfiguration::defaultConfiguration();
	ssl.addCaCertificates(QSslCertificate::fromData(cert.readAll()));
	QSslConfiguration::setDefaultConfiguration(ssl);
	const auto endpoint = QUrl(app.arguments().at(1));
	auto ws = endpoint.resolved(QUrl("/ws"));
	ws.setScheme("wss");
	Noveo::AuthClient auth(ws);
	Noveo::SessionClient client(&auth, endpoint);
	auto icons = QVector<MTPlong>();
	auto balances = 0;
	auto reentrantWallet = false;
	const auto request = [&](int id, const auto &value) {
		mtpBuffer body;
		value.write(body);
		client.request(id, body);
	};
	const auto transactions =
		[&](int id, int flags, QString offset, int limit) {
			request(id,
				MTPpayments_GetStarsTransactions(
					MTP_flags(MTPpayments_GetStarsTransactions::Flag(flags)),
					MTPstring(),
					MTP_inputPeerSelf(),
					MTP_string(offset),
					MTP_int(limit)));
		};
	client.onUpdate = [&](const MTPUpdates &updates) {
		for (const auto &u : updates.c_updates().vupdates().v) {
			if (u.type() == mtpc_updateStarsBalance) {
				if (++balances == 2) {
					request(19,
						MTPpayments_GetStarsStatus(
							MTP_flags(MTPpayments_GetStarsStatus::Flags()),
							MTP_inputPeerSelf()));
				}
			}
		}
	};
	client.onReply = [&](int id, mtpBuffer body) {
		std::cerr << "economy response " << id << '\n';
		if (id == 19) {
			Assert(Decode<MTPpayments_StarsStatus>(body)
					   .data()
					   .vbalance()
					   .c_starsAmount()
					   .vnanos()
					   .v
				   == 460000000);
			reentrantWallet = true;
		} else if (id == 1) {
			const auto result = Decode<MTPpayments_StarsStatus>(body);
			const auto &data = result.data();
			Assert(data.vbalance().c_starsAmount().vamount().v == 123);
			Assert(data.vbalance().c_starsAmount().vnanos().v == 460000000);
			Assert(data.vhistory()->v.size() == 5);
			Assert(client.selfUser().c_user().is_premium());
			client.message({{"type", "user_updated"},
				{"userId", "test-user"},
				{"bio", "Partial"}});
			Assert(client.selfUser().c_user().is_premium());
			client.message({{"type", "user_updated"},
				{"userId", "test-user"},
				{"membershipTier", ""}});
			Assert(!client.selfUser().c_user().is_premium());
            Assert(client.uploadLimitBytes() == 50LL * 1024 * 1024);
            client.setNicknameFont(u"comic"_q, [](QString error) { Assert(error.contains("Silver")); });
			client.message({{"type", "user_updated"},
				{"userId", "test-user"},
				{"membershipTier", "silver"}});
			Assert(client.selfUser().c_user().is_premium()
				   && client.membershipTier() == "silver");
            Assert(client.uploadLimitBytes() == 100LL * 1024 * 1024);
			client.message({{"type", "user_updated"},
				{"userId", "test-user"},
				{"membershipTier", "premium"}});
			Assert(client.uploadLimitBytes() == 500LL * 1024 * 1024);
			transactions(2, 1, {}, 1);
		} else if (id == 2 || id == 3 || id == 4 || id == 5) {
			const auto result = Decode<MTPpayments_StarsStatus>(body);
			const auto &data = result.data();
			const auto &history = data.vhistory()->v;
			if (id == 2) {
				Assert(
					history.size() == 1 && qs(history[0].data().vid()) == "in");
				Assert(history[0].data().vpeer().type() == mtpc_starsTransactionPeer);
				Assert(data.vusers().v.size() == 1);
				Assert(qs(*data.vnext_offset()) == "1");
				transactions(3, 1, "1", 1);
			} else if (id == 3) {
				Assert(history.size() == 1
					   && qs(history[0].data().vid()) == "tiny");
				Assert(history[0].data().vpeer().type() == mtpc_starsTransactionPeerUnsupported);
				Assert(history[0].data().vamount().c_starsAmount().vnanos().v
					   == 10000000);
				transactions(4, 2, {}, 50);
			} else if (id == 4) {
				Assert(history.size() == 1
					   && qs(history[0].data().vid()) == "out");
				Assert(history[0].data().vamount().c_starsAmount().vamount().v
					   == 0);
				Assert(history[0].data().vamount().c_starsAmount().vnanos().v
					   == -250000000);
				transactions(5, 3, {}, 50);
			} else {
				Assert(history.size() == 5 && !data.vnext_offset());
				request(6,
					MTPpayments_GetStarsTransactionsByID(
						MTP_flags(
							MTPpayments_GetStarsTransactionsByID::Flags()),
						MTP_inputPeerSelf(),
						MTP_vector<MTPInputStarsTransaction>(
							{MTP_inputStarsTransaction(
								MTP_flags(MTPDinputStarsTransaction::Flags()),
								MTP_string("old"))})));
			}
		} else if (id == 6) {
			const auto result = Decode<MTPpayments_StarsStatus>(body);
			Assert(result.data().vhistory()->v.size() == 1);
			Assert(
				result.data().vhistory()->v[0].data().vdate().v == 1700000005);
			request(7, MTPpayments_GetStarsTopupOptions());
		} else if (id == 7) {
			Assert(Decode<MTPVector<MTPStarsTopupOption>>(body).v.isEmpty());
			request(8,
				MTPpayments_GetStarsStatus(
					MTP_flags(MTPpayments_GetStarsStatus::Flags()),
					MTP_inputPeerUser(
						MTP_long(Noveo::NativeUserId("other").bare),
						MTP_long(1))));
		} else if (id == 8) {
			Assert(
				Decode<MTPRpcError>(body).c_rpc_error().verror_code().v == 400);
			request(9,
				MTPpayments_GetStarsStatus(
					MTP_flags(MTPpayments_GetStarsStatus::Flag::f_ton),
					MTP_inputPeerSelf()));
		} else if (id == 9) {
			Assert(Decode<MTPpayments_StarsStatus>(body)
					   .data()
					   .vbalance()
					   .c_starsAmount()
					   .vamount()
					   .v
				   == 0);
			request(10, MTPaccount_GetDefaultEmojiStatuses(MTP_long(0)));
		} else if (id == 10) {
			const auto result = Decode<MTPaccount_EmojiStatuses>(body);
			Assert(result.c_account_emojiStatuses().vstatuses().v.size() == 2);
			for (const auto &s :
				result.c_account_emojiStatuses().vstatuses().v) {
				icons.push_back(s.c_emojiStatus().vdocument_id());
			}
			request(11,
				MTPmessages_GetCustomEmojiDocuments(
					MTP_vector<MTPlong>(icons)));
		} else if (id == 11) {
			const auto documents = Decode<MTPVector<MTPDocument>>(body);
			Assert(documents.v.size() == 2);
			for (const auto &document : documents.v) {
				Assert(document.c_document().vfile_reference().v.startsWith(
					"noveo:https://localhost:"));
				Assert(document.c_document().vattributes().v.back().type()
					   == mtpc_documentAttributeCustomEmoji);
			}
			request(12,
				MTPaccount_UpdateEmojiStatus(
					MTP_emojiStatus(MTP_flags(MTPDemojiStatus::Flags()),
						icons.front(),
						MTPint())));
		} else if (id == 12) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			Assert(client.selfUser()
					   .c_user()
					   .vemoji_status()
					   ->c_emojiStatus()
					   .vdocument_id()
					   .v
				   == icons.front().v);
			request(13, MTPhelp_GetPeerProfileColors(MTP_int(0)));
		} else if (id == 13) {
			Assert(Decode<MTPhelp_PeerColors>(body)
					   .c_help_peerColors()
					   .vcolors()
					   .v.size()
				   == 20);
			request(14,
				MTPaccount_UpdateColor(
					MTP_flags(MTPaccount_UpdateColor::Flag::f_for_profile
							  | MTPaccount_UpdateColor::Flag::f_color),
					MTP_peerColor(
						MTP_flags(MTPDpeerColor::Flag::f_color
								  | MTPDpeerColor::Flag::f_background_emoji_id),
						MTP_int(3),
						icons.front())));
		} else if (id == 14) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			Assert(client.selfUser()
					   .c_user()
					   .vprofile_color()
					   ->c_peerColor()
					   .vcolor()
					   ->v
				   == 3);
			request(15, MTPaccount_UpdateEmojiStatus(MTP_emojiStatusEmpty()));
		} else if (id == 15) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			Assert(!client.selfUser().c_user().vemoji_status());
			request(16,
				MTPaccount_UpdateColor(
					MTP_flags(MTPaccount_UpdateColor::Flag::f_for_profile),
					MTPPeerColor()));
		} else if (id == 16) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			Assert(!client.selfUser().c_user().vprofile_color());
			request(17,
				MTPaccount_UpdateEmojiStatus(
					MTP_emojiStatus(MTP_flags(MTPDemojiStatus::Flags()),
						MTP_long(1),
						MTPint())));
		} else if (id == 17) {
			Assert(
				Decode<MTPRpcError>(body).c_rpc_error().verror_code().v == 400);
			request(18,
				MTPusers_GetUsers(MTP_vector<MTPInputUser>({MTP_inputUserSelf(),
					MTP_inputUser(MTP_long(Noveo::NativeUserId("other").bare),
						MTP_long(1))})));
		} else if (id == 18) {
			Assert(Decode<MTPVector<MTPUser>>(body).v.size() == 2);
			const auto peer = peerFromUser(Noveo::NativeUserId("other"));
			client.createStarsGiveaway(peer, "1.25", [&](QString error) {
				Assert(error.isEmpty());
				client.createStarsGiveaway(peer, "999999", [&](QString error) {
					Assert(error.contains("Not enough"));
					client.submitPremiumReceipt(
						app.arguments().at(3), [&](QString error) {
							Assert(error.isEmpty());
							QFile image(app.arguments().at(3));
							Assert(image.open(QIODevice::ReadOnly));
							client.submitPremiumReceipt(
								{},
								[&](QString error) {
									Assert(error.isEmpty());
									client.submitPremiumReceipt(
										"/missing-image", [&](QString error) {
											Assert(!error.isEmpty());
											Assert(reentrantWallet
												   && balances > 0
												   && client.walletBalance()
														  == 12220);
											client.setNicknameFont(u"comic"_q, [&](QString error) {
                                                Assert(error.isEmpty());
                                                Assert(client.nicknameFont(peerFromUser(Noveo::NativeUserId("test-user"))) == u"comic"_q);
                                                app.exit(0);
                                            });
										});
								},
								image.readAll(), u"premium_upgrade"_q);
						}, {}, u"silver"_q);
				});
			});
		} else {
			Assert(false);
		}
	};
	auth.onMessage = [&](QJsonObject frame) { client.message(frame); };
	auth.onAuthenticated = [&](QJsonObject profile) {
		client.authenticated(profile);
		request(1,
			MTPpayments_GetStarsStatus(
				MTP_flags(MTPpayments_GetStarsStatus::Flags()),
				MTP_inputPeerSelf()));
	};
	auth.onError = [&](auto) { app.exit(20); };
	QTimer::singleShot(30000, &app, [&] { app.exit(21); });
	auth.login("fixture", "fixture");
	return app.exec();
}
