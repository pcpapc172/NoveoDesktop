#include "noveo/session_client.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtNetwork/QSslConfiguration>
#include <iostream>

namespace base::assertion {
void log(const char *message, const char *file, int line) {
	std::cerr << message << ' ' << file << ':' << line << '\n';
}
}
template <typename T> T Decode(const mtpBuffer &buffer) {
	auto result = T(); auto from = buffer.constData();
	Assert(result.read(from, from + buffer.size()));
	Assert(from == buffer.constData() + buffer.size()); return result;
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	QFile file(app.arguments().at(2)); Assert(file.open(QIODevice::ReadOnly));
	auto ssl = QSslConfiguration::defaultConfiguration();
	ssl.addCaCertificates(QSslCertificate::fromData(file.readAll()));
	QSslConfiguration::setDefaultConfiguration(ssl);
	const auto endpoint = QUrl(app.arguments().at(1));
	auto ws = endpoint.resolved(QUrl("/ws")); ws.setScheme("wss");
	Noveo::AuthClient auth(ws); Noveo::SessionClient client(&auth, endpoint);
	const auto peer = MTPInputPeer(MTP_inputPeerChat(MTP_long(Noveo::NativeUserId("group").bare)));
	const auto channel = MTP_inputChannel(MTP_long(Noveo::NativeUserId("channel").bare), MTP_long(1));
	int editId = 0, pollId = 0, parentId = 0, deletions = 0, edits = 0;
	uint64 sessionHash = 0;
	const auto request = [&](int id, const auto &value) {
		mtpBuffer body; value.write(body); client.request(id, body);
	};
	const auto history = [&](int id) {
		request(id, MTPmessages_GetHistory(peer, MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50), MTP_int(0), MTP_int(0), MTP_long(0)));
	};
	const auto edit = [&](int id, const QString &text) {
		request(id, MTPmessages_EditMessage(MTP_flags(MTPmessages_EditMessage::Flag::f_message), peer, MTP_int(editId), MTP_string(text), MTPInputMedia(), MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPint(), MTPInputRichMessage()));
	};
	client.onUpdate = [&](const MTPUpdates &updates) {
		for (const auto &update : updates.c_updates().vupdates().v) {
			if (update.type() == mtpc_updateDeleteMessages) ++deletions;
			if (update.type() == mtpc_updateEditMessage) ++edits;
		}
	};
	client.onReply = [&](int id, mtpBuffer body) {
		std::cerr << "response " << id << '\n';
		if (id == 1) {
			const auto result = Decode<MTPmessages_Dialogs>(body);
			const auto &chats = result.c_messages_dialogs().vchats().v;
			bool owner = false, admin = false, member = false;
			for (const auto &chat : chats) {
				if (chat.type() != mtpc_chat) continue;
				const auto &data = chat.c_chat();
				if (data.vid().v == Noveo::NativeUserId("group").bare) owner = data.is_creator();
				if (data.vid().v == Noveo::NativeUserId("admin-group").bare) admin = !data.is_creator() && data.vadmin_rights();
				if (data.vid().v == Noveo::NativeUserId("member-group").bare) member = !data.is_creator() && !data.vadmin_rights() && data.vdefault_banned_rights()->c_chatBannedRights().is_send_messages();
			}
			Assert(owner && admin && member); history(2);
		} else if (id == 2) {
			const auto result = Decode<MTPmessages_Messages>(body);
			for (const auto &message : result.c_messages_messagesSlice().vmessages().v) {
				const auto &m = message.c_message();
				if (qs(m.vmessage()) == "original") editId = m.vid().v;
				if (m.vmedia() && m.vmedia()->type() == mtpc_messageMediaPoll) {
					pollId = m.vid().v;
					Assert(m.vmedia()->c_messageMediaPoll().vpoll().c_poll().vanswers().v.front().c_pollAnswer().voption().v == "o1");
				}
				if (m.vreply_to()) parentId = m.vreply_to()->c_messageReplyHeader().vreply_to_msg_id()->v;
			}
			Assert(editId && pollId && parentId);
			request(3, MTPmessages_GetMessages(MTP_vector<MTPInputMessage>({MTP_inputMessageID(MTP_int(parentId))})));
		} else if (id == 3) {
			const auto result = Decode<MTPmessages_Messages>(body);
			Assert(qs(result.c_messages_messages().vmessages().v.front().c_message().vmessage()) == "unloaded parent"); edit(4, "edited");
		} else if (id == 4) {
			Decode<MTPUpdates>(body); QTimer::singleShot(20, &app, [&] { history(5); });
		} else if (id == 5 || id == 7) {
			const auto result = Decode<MTPmessages_Messages>(body); bool found = false;
			for (const auto &message : result.c_messages_messagesSlice().vmessages().v) {
				const auto &m = message.c_message();
				if (m.vid().v == editId) { Assert(qs(m.vmessage()) == "edited" && m.vedit_date()); found = true; }
			}
			Assert(found);
			if (id == 5) edit(6, "denied");
			else request(8, MTPmessages_SendVote(peer, MTP_int(pollId), MTP_vector<MTPbytes>({MTP_bytes("o1")})));
		} else if (id == 6) {
			Assert(Decode<MTPRpcError>(body).c_rpc_error().verror_code().v == 400); history(7);
		} else if (id == 8) {
			Decode<MTPUpdates>(body); QTimer::singleShot(20, &app, [&] { history(9); });
		} else if (id == 9) {
			const auto result = Decode<MTPmessages_Messages>(body); bool chosen = false;
			for (const auto &message : result.c_messages_messagesSlice().vmessages().v) {
				const auto &m = message.c_message(); if (m.vid().v != pollId) continue;
				const auto &poll = m.vmedia()->c_messageMediaPoll();
				chosen = poll.vresults().c_pollResults().vresults()->v.front().c_pollAnswerVoters().is_chosen();
			}
			Assert(chosen);
			request(10, MTPmessages_UpdatePinnedMessage(MTP_flags(MTPmessages_UpdatePinnedMessage::Flags()), peer, MTP_int(editId)));
		} else if (id == 10) {
			Decode<MTPUpdates>(body); QTimer::singleShot(20, &app, [&] { request(11, MTPmessages_GetFullChat(MTP_long(Noveo::NativeUserId("group").bare))); });
		} else if (id == 11) {
			const auto result = Decode<MTPmessages_ChatFull>(body);
			Assert(result.c_messages_chatFull().vfull_chat().c_chatFull().vpinned_msg_id()->v == editId);
			request(12, MTPaccount_GetAuthorizations());
		} else if (id == 12) {
			const auto result = Decode<MTPaccount_Authorizations>(body);
			Assert(result.c_account_authorizations().vauthorizations().v.size() == 2);
			for (const auto &a : result.c_account_authorizations().vauthorizations().v) if (!a.c_authorization().is_current()) sessionHash = a.c_authorization().vhash().v;
			Assert(sessionHash); request(13, MTPaccount_ResetAuthorization(MTP_long(sessionHash)));
		} else if (id == 13) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(14, MTPaccount_SetPrivacy(MTP_inputPrivacyKeyChatInvite(), MTP_vector<MTPInputPrivacyRule>({MTP_inputPrivacyValueDisallowAll()})));
		} else if (id == 14) {
			Assert(Decode<MTPaccount_PrivacyRules>(body).c_account_privacyRules().vrules().v.front().type() == mtpc_privacyValueDisallowAll);
			request(15, MTPchannels_GetParticipants(channel, MTP_channelParticipantsAdmins(), MTP_int(0), MTP_int(20), MTP_long(0)));
		} else if (id == 15) {
			Assert(Decode<MTPchannels_ChannelParticipants>(body).c_channels_channelParticipants().vcount().v == 1);
			client.changePassword("current", "new-password", [&](QString error) {
				Assert(error.isEmpty()); Assert(auth.authorization().value("token") == "rotated-token");
				request(16, MTPaccount_GetAuthorizations());
			});
		} else if (id == 16) {
			Decode<MTPaccount_Authorizations>(body);
			request(17, MTPmessages_DeleteHistory(MTP_flags(MTPmessages_DeleteHistory::Flag::f_just_clear), peer, MTP_int(0), MTPint(), MTPint()));
		} else if (id == 17) {
			Assert(Decode<MTPRpcError>(body).c_rpc_error().verror_code().v == 400);
			request(18, MTPmessages_DeleteMessages(MTP_flags(MTPmessages_DeleteMessages::Flag::f_revoke), MTP_vector<MTPint>({MTP_int(editId)})));
		} else if (id == 18) {
			Decode<MTPmessages_AffectedMessages>(body); QTimer::singleShot(20, &app, [&] { history(19); });
		} else if (id == 19) {
			const auto result = Decode<MTPmessages_Messages>(body);
			for (const auto &m : result.c_messages_messagesSlice().vmessages().v) Assert(m.c_message().vid().v != editId);
			Assert(edits >= 2 && deletions == 1);
			request(20, MTPmessages_Search(MTP_flags(MTPmessages_Search::Flags()), peer, MTP_string("needle"), MTPInputPeer(), MTPInputPeer(), MTPVector<MTPReaction>(), MTPint(), MTP_inputMessagesFilterEmpty(), MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50), MTP_int(0), MTP_int(0), MTP_long(0)));
		} else if (id == 20) {
			const auto result = Decode<MTPmessages_Messages>(body);
			Assert(result.c_messages_messagesSlice().vmessages().v.size() == 1);
			Assert(qs(result.c_messages_messagesSlice().vmessages().v.front().c_message().vmessage()) == "needle older");
			request(21, MTPmessages_SearchGlobal(MTP_flags(MTPmessages_SearchGlobal::Flag::f_groups_only), MTPint(), MTPInputChannel(), MTP_string("needle"), MTP_inputMessagesFilterEmpty(), MTP_int(0), MTP_int(0), MTP_int(0), MTP_inputPeerEmpty(), MTP_int(0), MTP_int(50)));
		} else if (id == 21) {
			Assert(Decode<MTPmessages_Messages>(body).c_messages_messagesSlice().vmessages().v.size() == 1);
			const auto member = peerFromUser(Noveo::NativeUserId("other-user"));
			Assert(client.canManageMemberPermissions(peerFromChat(Noveo::NativeUserId("group").bare), member));
			client.memberPermissions(peerFromChat(Noveo::NativeUserId("group").bare), member, [&](QJsonObject result, QString error) {
				Assert(error.isEmpty() && result.value("effectivePermissions").toObject().value("canSendMessages").toBool());
				client.setMemberPermissions(peerFromChat(Noveo::NativeUserId("group").bare), peerFromUser(Noveo::NativeUserId("other-user")), {{"canSendMessages", false}, {"canSendFiles", false}, {"canAddMembers", QJsonValue(QJsonValue::Null)}}, [&](QString error) {
					Assert(error.isEmpty());
					request(22, MTPaccount_UpdateProfile(MTP_flags(MTPaccount_UpdateProfile::Flag::f_first_name | MTPaccount_UpdateProfile::Flag::f_about), MTP_string("New name"), MTPstring(), MTP_string("New bio")));
				});
			});
		} else if (id == 22) {
			Assert(qs(*Decode<MTPUser>(body).c_user().vfirst_name()) == "New name");
			request(23, MTPcontacts_AddContact(MTP_flags(MTPcontacts_AddContact::Flags()), MTP_inputUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1)), MTP_string("Friend"), MTP_string(""), MTP_string(""), MTPTextWithEntities()));
		} else if (id == 23) {
			Decode<MTPUpdates>(body);
			request(24, MTPcontacts_DeleteContacts(MTP_vector<MTPInputUser>({MTP_inputUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1)), MTP_inputUser(MTP_long(Noveo::NativeUserId("third-user").bare), MTP_long(1))})));
		} else if (id == 24) {
			Decode<MTPUpdates>(body);
			request(25, MTPmessages_CreateChat(MTP_flags(MTPmessages_CreateChat::Flags()), MTP_vector<MTPInputUser>({MTP_inputUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(1))}), MTP_string("New group"), MTPint()));
		} else if (id == 25) {
			const auto result = Decode<MTPmessages_InvitedUsers>(body);
			Assert(result.c_messages_invitedUsers().vupdates().c_updates().vchats().v.front().c_chat().vid().v == Noveo::NativeUserId("created-group").bare);
			request(26, MTPchannels_CreateChannel(MTP_flags(MTPchannels_CreateChannel::Flag::f_broadcast), MTP_string("New channel"), MTP_string("Channel description"), MTPInputGeoPoint(), MTPstring(), MTPint()));
		} else if (id == 26) {
			Assert(Decode<MTPUpdates>(body).c_updates().vchats().v.front().c_channel().vid().v == Noveo::NativeUserId("created-channel").bare);
			request(27, MTPupload_SaveFilePart(MTP_long(987), MTP_int(0), MTP_bytes(QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII="))));
		} else if (id == 27) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(28, MTPphotos_UploadProfilePhoto(MTP_flags(MTPphotos_UploadProfilePhoto::Flag::f_file), MTPInputUser(), MTP_inputFile(MTP_long(987), MTP_int(1), MTP_string("avatar.png"), MTP_string("")), MTPInputFile(), MTPdouble(), MTPVideoSize()));
		} else if (id == 28) {
			const auto result = Decode<MTPphotos_Photo>(body); Assert(result.c_photos_photo().vphoto().type() == mtpc_photo);
			request(29, MTPcontacts_Search(MTP_flags(MTPcontacts_Search::Flags()), MTP_string("publicgroup"), MTP_int(20)));
		} else if (id == 29) {
			Decode<MTPcontacts_Found>(body);
			const auto group = peerFromChat(Noveo::NativeUserId("public-group").bare);
			Assert(client.needsJoin(group));
			client.joinChat(group, [&](QString error) {
				Assert(error.isEmpty() && !client.needsJoin(peerFromChat(Noveo::NativeUserId("public-group").bare)));
				request(30, MTPmessages_DeleteChatUser(MTP_flags(MTPmessages_DeleteChatUser::Flags()), MTP_long(Noveo::NativeUserId("public-group").bare), MTP_inputUserSelf()));
			});
		} else if (id == 30) {
			Decode<MTPUpdates>(body);
			request(31, MTPmessages_EditChatTitle(MTP_long(Noveo::NativeUserId("group").bare), MTP_string("Renamed")));
		} else if (id == 31) {
			Decode<MTPUpdates>(body);
			request(32, MTPmessages_EditChatAbout(peer, MTP_string("Changed description")));
		} else if (id == 32) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue); app.exit(0);
		} else Assert(false);
	};
	auth.onMessage = [&](QJsonObject frame) { client.message(frame); };
	auth.onError = [&](auto) { app.exit(20); };
	auth.onAuthenticated = [&](QJsonObject profile) {
		client.authenticated(profile);
		request(1, MTPmessages_GetDialogs(MTP_flags(MTPmessages_GetDialogs::Flags()), MTPint(), MTPint(), MTPint(), MTP_inputPeerEmpty(), MTP_int(50), MTP_long(0)));
	};
	QTimer::singleShot(25000, &app, [&] { app.exit(21); });
	auth.login("test-user", "test-password"); const auto result = app.exec();
	auth.clear(); client.reset(); return result;
}
