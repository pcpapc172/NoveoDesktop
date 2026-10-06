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
}

template <typename T> T Decode(const mtpBuffer &buffer) {
	auto result = T();
	auto from = buffer.constData();
	Assert(result.read(from, from + buffer.size()));
	Assert(from == buffer.constData() + buffer.size());
	return result;
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	QFile certificate(app.arguments().at(2));
	Assert(certificate.open(QIODevice::ReadOnly));
	auto ssl = QSslConfiguration::defaultConfiguration();
	ssl.addCaCertificates(QSslCertificate::fromData(certificate.readAll()));
	QSslConfiguration::setDefaultConfiguration(ssl);
	const auto endpoint = QUrl(app.arguments().at(1));
	auto ws = endpoint.resolved(QUrl("/ws"));
	ws.setScheme("wss");
	Noveo::AuthClient auth(ws);
	Noveo::SessionClient client(&auth, endpoint);
	if (app.arguments().size() > 3) {
		const auto proxy = QNetworkProxy(QNetworkProxy::Socks5Proxy, "127.0.0.1", app.arguments().at(3).toUShort(), "proxy-user", "proxy-password");
		auth.setProxy(proxy);
		client.setProxy(proxy);
	}
	const auto peer = MTPInputPeer(MTP_inputPeerChat(MTP_long(Noveo::NativeUserId("group").bare)));
	const auto notifyPeer = MTPInputNotifyPeer(MTP_inputNotifyPeer(peer));
	std::set<int> checks;
	bool started = false;
	int emojiId = 0, giftId = 0, botId = 0;
	const auto request = [&](int id, const auto &value) {
		mtpBuffer body;
		value.write(body);
		client.request(id, body);
	};
	const auto finish = [&] {
		if (checks.size() == 35) app.exit(0);
	};
	const auto reaction = [&](int id, bool big, bool remove) {
		request(id, MTPmessages_SendReaction(MTP_flags(MTPmessages_SendReaction::Flag::f_reaction
			| (big ? MTPmessages_SendReaction::Flag::f_big : MTPmessages_SendReaction::Flag())),
			peer, MTP_int(emojiId), MTP_vector<MTPReaction>(remove ? QVector<MTPReaction>()
				: QVector<MTPReaction>{MTP_reactionEmoji(MTP_string("❤️"))})));
	};
	const auto mute = [&](int id, int until) {
		request(id, MTPaccount_UpdateNotifySettings(notifyPeer, MTP_inputPeerNotifySettings(
			MTP_flags(MTPDinputPeerNotifySettings::Flag::f_mute_until), MTPBool(), MTPBool(),
			MTP_int(until), MTPNotificationSound(), MTPBool(), MTPBool(), MTPNotificationSound())));
	};
	const auto saved = [&](int id, const QString &offset) {
		request(id, MTPpayments_GetSavedStarGifts(MTP_flags(MTPpayments_GetSavedStarGifts::Flags()),
			MTP_inputPeerSelf(), MTPint(), MTP_string(offset), MTP_int(1)));
	};
	client.onReply = [&](int id, mtpBuffer body) {
		if (id == 1) {
			const auto result = Decode<MTPmessages_Messages>(body);
			Assert(result.c_messages_messagesSlice().vmessages().v.size() == 5);
			for (const auto &message : result.c_messages_messagesSlice().vmessages().v) {
				if (message.type() == mtpc_messageService && message.c_messageService().vaction().type() == mtpc_messageActionGiftStars) {
					const auto &stars = message.c_messageService().vaction().c_messageActionGiftStars();
					Assert(stars.vamount().v == 1000 && stars.vstars().v == 1000);
					Assert(client.giftDetails(message.c_messageService().vid().v).value("mine").toBool());
				} else if (message.type() == mtpc_messageService) {
					giftId = message.c_messageService().vid().v;
					Assert(message.c_messageService().vaction().type() == mtpc_messageActionStarGift);
					Assert(client.giftDetails(giftId).value("giveawayId") == "giveaway");
					Assert(!client.giftDetails(giftId).value("mine").toBool());
				} else if (qs(message.c_message().vmessage()).startsWith("😀")) {
					const auto &bot = message.c_message();
					botId = bot.vid().v;
					Assert(qs(bot.vmessage()) == "😀 A new login inline\n  code\n**unclosed");
					Assert(bot.ventities() && bot.ventities()->v.size() == 3);
					const auto &bold = bot.ventities()->v[0].c_messageEntityBold();
					Assert(bold.voffset().v == 3 && bold.vlength().v == 11);
					Assert(bot.ventities()->v[1].type() == mtpc_messageEntityCode);
					Assert(qs(bot.ventities()->v[2].c_messageEntityPre().vlanguage()) == "cpp");
					Assert(bot.vreply_markup());
					const auto &buttons = bot.vreply_markup()->c_replyInlineMarkup().vrows().v[0].c_keyboardInlineButtonRow().vbuttons().v;
					Assert(buttons.size() == 2);
					Assert(buttons[0].c_keyboardInlineButton().vtype().c_inlineButtonTypeCallback().vdata().v == "ack:session");
					Assert(buttons[1].c_keyboardInlineButton().vtype().type() == mtpc_inlineButtonTypeUrl);
				} else if (qs(message.c_message().vmessage()) == "❤️") {
					emojiId = message.c_message().vid().v;
				} else {
					const auto &photo = message.c_message().vmedia()->c_messageMediaPhoto().vphoto()->c_photo();
					Assert(photo.vsizes().v.size() == 2);
					Assert(photo.vsizes().v.front().type() == mtpc_photoCachedSize);
					Assert(!photo.vsizes().v.front().c_photoCachedSize().vbytes().v.isEmpty());
				}
			}
			Assert(emojiId && giftId && botId);
			const auto otherPeer = peerFromUser(Noveo::NativeUserId("other-user"));
			Assert(client.voiceChatId(otherPeer) == "dm");
			Assert(client.voicePeer("dm", "other-user") == otherPeer);
			client.voiceToken("group", "fixture-call", [&](QJsonObject token, QString error) {
				Assert(error.isEmpty() && token.value("participantToken") == "room-only-token");
				for (const auto type : { "voice_start", "voice_join", "voice_leave" }) {
					Assert(client.voiceAction({ { "type", type }, { "chatId", "group" }, { "callId", "fixture-call" } }));
				}
				checks.insert(140); finish();
			});
			request(35, MTPmessages_GetHistory(MTP_inputPeerUser(MTP_long(Noveo::NativeUserId("other-user").bare), MTP_long(0)),
				MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50), MTP_int(0), MTP_int(0), MTP_long(0)));
			request(31, MTPmessages_GetBotCallbackAnswer(MTP_flags(MTPmessages_GetBotCallbackAnswer::Flag::f_data),
				peer, MTP_int(botId), MTP_bytes("ack:session"), MTPInputCheckPasswordSRP()));
			client.giftAction("sell", "gift-one", PeerId(), [&](QString error) {
				Assert(error.isEmpty()); checks.insert(108); finish();
			});
			request(2, MTPmessages_GetAvailableReactions(MTP_int(0)));
			request(3, MTPmessages_GetStickerSet(MTP_inputStickerSetAnimatedEmoji(), MTP_int(0)));
			request(4, MTPmessages_GetStickerSet(MTP_inputStickerSetAnimatedEmojiAnimations(), MTP_int(0)));
			request(5, MTPmessages_GetFavedStickers(MTP_long(0)));
			request(6, MTPusers_GetFullUser(MTP_inputUserSelf()));
			saved(7, "");
			request(8, MTPaccount_GetNotifySettings(notifyPeer));
			request(9, MTPphotos_GetUserPhotos(MTP_inputUserSelf(), MTP_int(0), MTP_long(0), MTP_int(10)));
			request(10, MTPmessages_GetTopReactions(MTP_int(8), MTP_long(0)));
			request(20, MTPmessages_SetTyping(MTP_flags(MTPmessages_SetTyping::Flags()), peer,
				MTPint(), MTP_sendMessageTypingAction()));
			request(21, MTPmessages_SetTyping(MTP_flags(MTPmessages_SetTyping::Flags()), peer,
				MTPint(), MTP_sendMessageEmojiInteraction(MTP_string("❤️"), MTP_int(emojiId),
					MTP_dataJSON(MTP_string(R"({"v":1,"a":[{"i":1,"t":0}]})")))));
			request(22, MTPmessages_SetTyping(MTP_flags(MTPmessages_SetTyping::Flags()), peer,
				MTPint(), MTP_sendMessageEmojiInteractionSeen(MTP_string("❤️"))));
			reaction(23, false, false);
			client.giftAction("buy", "fail", PeerId(), [&](QString error) {
				Assert(error == "Not enough Stars."); checks.insert(105); finish();
			});
			client.giftAction("buy", "fail", PeerId(), [&](QString) { Assert(false); });
			client.giftAction("claim", "giveaway", peerFromChat(Noveo::NativeUserId("group").bare), [&](QString error) {
				Assert(error.isEmpty()); checks.insert(106); finish();
			});
			client.giftAction("giveaway", "gift-one", peerFromChat(Noveo::NativeUserId("group").bare), [&](QString error) {
				Assert(error.isEmpty()); checks.insert(107); finish();
			});
		} else if (id == 2) {
			const auto result = Decode<MTPmessages_AvailableReactions>(body);
			Assert(result.c_messages_availableReactions().vreactions().v.size() > 60);
			for (const auto &reaction : result.c_messages_availableReactions().vreactions().v) {
				Assert(reaction.c_availableReaction().veffect_animation().c_document().vfile_reference().v.contains("/static/reactions/"));
			}
		} else if (id == 3 || id == 4) {
			const auto result = Decode<MTPmessages_StickerSet>(body);
			const auto &data = result.c_messages_stickerSet();
			Assert(data.vdocuments().v.size() == 1 && data.vpacks().v.size() == 1);
			Assert(data.vpacks().v.front().c_stickerPack().vdocuments().v.front().v == data.vdocuments().v.front().c_document().vid().v);
		} else if (id == 5) {
			const auto result = Decode<MTPmessages_FavedStickers>(body);
			const auto &documents = result.c_messages_favedStickers().vstickers().v;
			Assert(documents.size() == 2);
			const auto &doc = documents.front().c_document();
			request(13, MTPmessages_FaveSticker(MTP_inputDocument(doc.vid(), doc.vaccess_hash(), doc.vfile_reference()), MTP_boolTrue()));
		} else if (id == 35) {
			const auto result = Decode<MTPmessages_Messages>(body);
			const auto &messages = result.c_messages_messagesSlice().vmessages().v;
			Assert(messages.size() == 2);
			for (const auto &message : messages) {
				const auto &service = message.c_messageService();
				const auto &action = service.vaction().c_messageActionPhoneCall();
				if (service.is_out()) {
					Assert(action.vduration()->v == 42 && action.vreason()->type() == mtpc_phoneCallDiscardReasonHangup);
				} else {
					Assert(!action.vduration() && action.vreason()->type() == mtpc_phoneCallDiscardReasonMissed);
				}
			}
		} else if (id == 6) {
			const auto result = Decode<MTPusers_UserFull>(body);
			const auto &full = result.c_users_userFull().vfull_user().c_userFull();
			Assert(full.is_phone_calls_available());
			Assert(full.vstargifts_count()->v == 2 && full.vprofile_photo()->c_photo().vdate().v > 0);
		} else if (id == 7 || id == 14) {
			const auto result = Decode<MTPpayments_SavedStarGifts>(body);
			const auto &data = result.c_payments_savedStarGifts();
			Assert(data.vcount().v == 2 && data.vgifts().v.size() == 1);
			if (id == 7) { Assert(qs(*data.vnext_offset()) == "1"); saved(14, "1"); }
			else Assert(!data.vnext_offset());
		} else if (id == 8 || id == 12 || id == 16) {
			const auto result = Decode<MTPPeerNotifySettings>(body);
			Assert(result.c_peerNotifySettings().vmute_until()->v == (id == 12 ? 3600 : 0));
			if (id == 8) mute(11, 3600);
			if (id == 12) mute(15, 0);
		} else if (id == 9) {
			const auto result = Decode<MTPphotos_Photos>(body);
			Assert(result.c_photos_photos().vphotos().v.size() == 1);
		} else if (id == 10) {
			const auto result = Decode<MTPmessages_Reactions>(body);
			Assert(result.c_messages_reactions().vreactions().v.size() == 8);
		} else if (id == 11 || id == 13 || id == 15 || (id >= 20 && id <= 22)) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			if (id == 11) request(12, MTPaccount_GetNotifySettings(notifyPeer));
			if (id == 15) request(16, MTPaccount_GetNotifySettings(notifyPeer));
		} else if (id >= 23 && id <= 25) {
			const auto result = Decode<MTPUpdates>(body);
			const auto &reactions = result.c_updates().vupdates().v.front().c_updateMessageReactions().vreactions().c_messageReactions();
			if (id == 25) Assert(reactions.vresults().v.empty());
			else {
				Assert(reactions.vresults().v.front().c_reactionCount().vcount().v == 1);
				Assert(reactions.vresults().v.front().c_reactionCount().vchosen_order());
				reaction(id + 1, id == 23, id == 24);
			}
		} else if (id == 31) {
			const auto result = Decode<MTPmessages_BotCallbackAnswer>(body);
			Assert(qs(*result.c_messages_botCallbackAnswer().vmessage()) == "Confirmed");
		} else Assert(false);
		checks.insert(id); finish();
	};
	client.onUpdate = [&](const MTPUpdates &result) {
		for (const auto &update : result.c_updates().vupdates().v) {
			if (update.type() == mtpc_updateChatUserTyping) {
				const auto type = update.c_updateChatUserTyping().vaction().type();
				checks.insert(type == mtpc_sendMessageTypingAction ? 100 : type == mtpc_sendMessageEmojiInteraction ? 101 : 102);
			} else if (update.type() == mtpc_updateMessageReactions) {
				for (const auto &recent : update.c_updateMessageReactions().vreactions().c_messageReactions().vrecent_reactions()->v) {
					if (recent.c_messagePeerReaction().is_big()) { Assert(recent.c_messagePeerReaction().is_unread()); checks.insert(103); }
				}
			} else if (update.type() == mtpc_updateEditMessage) {
				Assert(client.giftDetails(giftId).value("status") == "claimed"); checks.insert(104);
			}
		}
		finish();
	};
	client.onVoiceEvent = [&](const QJsonObject &event) {
		Assert(event.value("type") == "voice_chat_update");
		Assert(client.voiceState().value("group").toObject().value("callId") == "fixture-call");
		checks.insert(141); finish();
	};
	client.onDialogs = [&](const MTPmessages_Dialogs &result) {
		if (started) return;
		started = true;
		bool channelGift = false;
		for (const auto &message : result.c_messages_dialogs().vmessages().v) {
			if (message.type() == mtpc_messageService && message.c_messageService().is_post()) {
				Assert(message.c_messageService().vfrom_id()->type() == mtpc_peerChannel);
				channelGift = true;
			}
		}
		Assert(channelGift);
		request(1, MTPmessages_GetHistory(peer, MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50), MTP_int(0), MTP_int(0), MTP_long(0)));
	};
	auth.onMessage = [&](const QJsonObject &message) { client.message(message); };
	auth.onAuthenticated = [&](const QJsonObject &profile) { client.authenticated(profile); };
	auth.onError = [&](auto) { app.exit(20); };
	QTimer::singleShot(25000, &app, [&] {
		std::cerr << "Incomplete checks:";
		for (auto check : checks) std::cerr << ' ' << check;
		std::cerr << '\n'; app.exit(21);
	});
	auth.login("test-user", "test-password");
	const auto result = app.exec();
	auth.clear(); client.reset();
	return result;
}
