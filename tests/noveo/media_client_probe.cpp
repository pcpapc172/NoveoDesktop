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
	const auto peer = MTPInputPeer(MTP_inputPeerChat(MTP_long(Noveo::NativeUserId("group").bare)));
	std::set<int> checks;
	QVector<MTPint> forwardIds;
	MTPInputMedia albumPhoto, albumDocument;
	const auto request = [&](int id, const auto &value) {
		mtpBuffer body;
		value.write(body);
		client.request(id, body);
	};
	const auto finish = [&] {
		if (checks.size() == 18) {
			app.exit(0);
		}
	};
	const auto sendMedia = [&](int id, const MTPInputMedia &media) {
		const auto flags = id == 7
			? MTPmessages_SendMedia::Flags(MTPmessages_SendMedia::Flag::f_reply_to)
			: MTPmessages_SendMedia::Flags();
		const auto reply = id == 7
			? MTPInputReplyTo(MTP_inputReplyToMessage(MTP_flags(MTPDinputReplyToMessage::Flags()),
				  forwardIds.front(), MTPint(), MTPInputPeer(), MTPstring(),
				  MTPVector<MTPMessageEntity>(), MTPint(), MTPInputPeer(), MTPint(), MTPbytes()))
			: MTPInputReplyTo();
		request(id,
			MTPmessages_SendMedia(MTP_flags(flags), peer, reply, media, MTP_string("uploaded"),
				MTP_long(id), MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(),
				MTPInputPeer(), MTPInputQuickReplyShortcut(), MTPlong(), MTPlong(),
				MTPSuggestedPost()));
	};
	const auto uploaded = [](uint64 file, const QString &name) {
		return MTPInputMedia(MTP_inputMediaUploadedDocument(
			MTP_flags(MTPDinputMediaUploadedDocument::Flag::f_force_file),
			MTP_inputFile(MTP_long(file), MTP_int(2), MTP_string(name), MTP_string("")),
			MTPInputFile(), MTP_string("application/octet-stream"),
			MTP_vector<MTPDocumentAttribute>(
				QVector<MTPDocumentAttribute>{MTP_documentAttributeFilename(MTP_string(name))}),
			MTPVector<MTPInputDocument>(), MTPInputPhoto(), MTPint(), MTPint()));
	};
	client.onReply = [&](int id, mtpBuffer body) {
		if (id == 1) {
			const auto result = Decode<MTPmessages_Dialogs>(body);
			Assert(result.c_messages_dialogs().vdialogs().v.size() == 1);
			request(2,
				MTPmessages_GetHistory(peer, MTP_int(0), MTP_int(0), MTP_int(0), MTP_int(50),
					MTP_int(0), MTP_int(0), MTP_long(0)));
		} else if (id == 2) {
			const auto result = Decode<MTPmessages_Messages>(body);
			const auto &messages = result.c_messages_messagesSlice().vmessages().v;
			Assert(messages.size() == 11);
			std::set<QString> types;
			bool gift = false, reply = false, own = false;
			for (const auto &message : messages) {
				if (message.type() == mtpc_messageService) {
					const auto &service = message.c_messageService();
					Assert(service.vaction().type() == mtpc_messageActionStarGift);
					gift = true;
					continue;
				}
				const auto &data = message.c_message();
				const auto caption = qs(data.vmessage());
				if (caption == "text") {
					Assert(!data.vmedia());
					Assert(data.is_out());
					own = true;
					forwardIds.push_back(data.vid());
				}
				if (caption == "reply") {
					Assert(data.vreply_to());
					reply = true;
				}
				if (!data.vmedia()) {
					continue;
				}
				Assert(data.vmedia()->type() != mtpc_messageMediaUnsupported);
				if (data.vmedia()->type() == mtpc_messageMediaPhoto) {
					const auto &photo = data.vmedia()->c_messageMediaPhoto().vphoto()->c_photo();
					Assert(photo.vfile_reference().v.startsWith("noveo:https://"));
					albumPhoto = MTP_inputMediaPhoto(MTP_flags(MTPDinputMediaPhoto::Flags()),
						MTP_inputPhoto(photo.vid(), photo.vaccess_hash(), photo.vfile_reference()),
						MTPint(), MTPInputDocument());
					types.insert("photo");
				} else {
					const auto &doc
						= data.vmedia()->c_messageMediaDocument().vdocument()->c_document();
					const auto mime = qs(doc.vmime_type());
					types.insert(mime);
					Assert(doc.vfile_reference().v.startsWith("noveo:https://"));
					if (caption == "gif") {
						forwardIds.push_back(data.vid());
					}
					if (caption == "file") {
						albumDocument = MTP_inputMediaDocument(
							MTP_flags(MTPDinputMediaDocument::Flags()),
							MTP_inputDocument(doc.vid(), doc.vaccess_hash(), doc.vfile_reference()),
							MTPInputPhoto(), MTPint(), MTPint(), MTPstring());
					}
					if (caption == "tgs") {
						Assert(mime == "application/x-tgsticker");
						for (const auto &attr : doc.vattributes().v) {
							Assert(attr.type() != mtpc_documentAttributeAnimated);
						}
					}
					if (caption == "sticker" || caption == "tgs" || caption == "webm") {
						bool sticker = false, dimensions = false;
						for (const auto &attr : doc.vattributes().v) {
							sticker |= attr.type() == mtpc_documentAttributeSticker;
							dimensions |= attr.type() == mtpc_documentAttributeImageSize;
						}
						Assert(sticker && dimensions);
					}
				}
			}
			Assert(gift && reply && own && types.size() == 8 && forwardIds.size() == 2);
			request(3,
				MTPmessages_GetMessages(MTP_vector<MTPInputMessage>(
					QVector<MTPInputMessage>{MTP_inputMessageID(forwardIds.front())})));
			request(4,
				MTPmessages_ForwardMessages(MTP_flags(MTPmessages_ForwardMessages::Flags()), peer,
					MTP_vector<MTPint>(forwardIds),
					MTP_vector<MTPlong>(QVector<MTPlong>{MTP_long(400), MTP_long(401)}), peer,
					MTPint(), MTPInputReplyTo(), MTPint(), MTPint(), MTPInputPeer(),
					MTPInputQuickReplyShortcut(), MTPlong(), MTPint(), MTPlong(),
					MTPSuggestedPost()));
			request(5, MTPupload_SaveFilePart(MTP_long(500), MTP_int(1), MTP_bytes("tail")));
		} else if (id == 3) {
			const auto result = Decode<MTPmessages_Messages>(body);
			Assert(result.c_messages_messages().vmessages().v.size() == 1);
			Assert(qs(result.c_messages_messages().vmessages().v.front().c_message().vmessage())
				== "text");
		} else if (id == 4 || id == 9) {
			const auto result = Decode<MTPUpdates>(body);
			Assert(result.c_updates().vupdates().v.size() == 4);
		} else if (id == 5) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(6,
				MTPupload_SaveBigFilePart(
					MTP_long(500), MTP_int(0), MTP_int(2), MTP_bytes("head")));
		} else if (id == 6) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			sendMedia(7, uploaded(500, "fixture.bin"));
		} else if (id == 7) {
			const auto result = Decode<MTPUpdates>(body);
			Assert(result.type() == mtpc_updateShortSentMessage);
			Assert(result.c_updateShortSentMessage().is_out());
			Assert(result.c_updateShortSentMessage().vmedia()->type() == mtpc_messageMediaDocument);
			request(8,
				MTPmessages_UploadMedia(
					MTP_flags(MTPmessages_UploadMedia::Flags()), MTPstring(), peer, albumPhoto));
		} else if (id == 8) {
			Assert(Decode<MTPMessageMedia>(body).type() == mtpc_messageMediaPhoto);
			request(9,
				MTPmessages_SendMultiMedia(MTP_flags(MTPmessages_SendMultiMedia::Flags()), peer,
					MTPInputReplyTo(),
					MTP_vector<MTPInputSingleMedia>(QVector<MTPInputSingleMedia>{
						MTP_inputSingleMedia(MTP_flags(MTPDinputSingleMedia::Flags()), albumPhoto,
							MTP_long(900), MTP_string("album photo"),
							MTPVector<MTPMessageEntity>()),
						MTP_inputSingleMedia(MTP_flags(MTPDinputSingleMedia::Flags()),
							albumDocument, MTP_long(901), MTP_string("album file"),
							MTPVector<MTPMessageEntity>())}),
					MTPint(), MTPInputPeer(), MTPInputQuickReplyShortcut(), MTPlong(), MTPlong()));
			request(10, MTPupload_SaveFilePart(MTP_long(1000), MTP_int(0), MTP_bytes("head")));
		} else if (id == 10) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(11, MTPupload_SaveFilePart(MTP_long(1000), MTP_int(1), MTP_bytes("tail")));
		} else if (id == 11) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			sendMedia(12, uploaded(1000, "fail.bin"));
		} else if (id == 12) {
			Assert(Decode<MTPRpcError>(body).c_rpc_error().verror_code().v == 400);
			request(13, MTPupload_SaveFilePart(MTP_long(1300), MTP_int(0), MTP_bytes("head")));
			request(16,
				MTPupload_SaveFilePart(MTP_long(1600), MTP_int(0),
					MTP_bytes(
						QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0l"
											   "EQVR42mP8/x8AAwMCAO+/l9sAAAAASUVORK5CYII="))));
		} else if (id == 13) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(14, MTPupload_SaveFilePart(MTP_long(1300), MTP_int(1), MTP_bytes("tail")));
		} else if (id == 14) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			sendMedia(15, uploaded(1300, "cancel.bin"));
			QTimer::singleShot(100, &app, [&] {
				client.cancel(15);
				QTimer::singleShot(1000, &app, [&] {
					checks.insert(200);
					finish();
				});
			});
		} else if (id == 16) {
			Assert(Decode<MTPBool>(body).type() == mtpc_boolTrue);
			request(17,
				MTPmessages_UploadMedia(MTP_flags(MTPmessages_UploadMedia::Flags()), MTPstring(),
					peer,
					MTP_inputMediaUploadedPhoto(MTP_flags(MTPDinputMediaUploadedPhoto::Flags()),
						MTP_inputFile(
							MTP_long(1600), MTP_int(1), MTP_string("tiny.png"), MTP_string("")),
						MTPVector<MTPInputDocument>(), MTPint(), MTPInputDocument())));
		} else if (id == 17) {
			const auto media = Decode<MTPMessageMedia>(body);
			const auto &size
				= media.c_messageMediaPhoto().vphoto()->c_photo().vsizes().v.front().c_photoSize();
			Assert(size.vw().v == 1 && size.vh().v == 1);
		} else {
			Assert(false);
		}
		checks.insert(id);
		finish();
	};
	client.onUpdate = [&](const MTPUpdates &updates) {
		for (const auto &update : updates.c_updates().vupdates().v) {
			if (update.type() == mtpc_updateReadHistoryOutbox) {
				Assert(update.c_updateReadHistoryOutbox().vmax_id().v > 0);
				checks.insert(100);
				finish();
			}
		}
	};
	auth.onMessage = [&](const QJsonObject &frame) { client.message(frame); };
	auth.onAuthenticated = [&](const QJsonObject &profile) {
		client.authenticated(profile);
		request(1,
			MTPmessages_GetDialogs(MTP_flags(MTPmessages_GetDialogs::Flags()), MTPint(), MTPint(),
				MTPint(), MTP_inputPeerEmpty(), MTP_int(50), MTP_long(0)));
	};
	auth.onError = [&](auto) { app.exit(20); };
	QTimer::singleShot(25000, &app, [&] {
		for (const auto id : checks) {
			std::cerr << id << ' ';
		}
		app.exit(21);
	});
	auth.login("test-user", "test-password");
	return app.exec();
}
