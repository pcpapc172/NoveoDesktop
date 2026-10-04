/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "noveo/session_client.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtCore/QFile>
#include <QtCore/QTemporaryFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QMimeDatabase>
#include <QtGui/QImageReader>
#include <QtNetwork/QHttpMultiPart>
#include <QtNetwork/QNetworkReply>
#include <algorithm>

namespace Noveo {
namespace {

QString Text(const QJsonValue &value) {
	const auto result = value.toString().trimmed();
	const auto lower = result.toLower();
	return (lower == "null" || lower == "undefined" || lower == "none") ? QString() : result;
}

QString RawId(const QJsonObject &object) {
	return Text(object.value("userId")).isEmpty() ? Text(object.value("id"))
												  : Text(object.value("userId"));
}

int Date(const QJsonObject &object) {
	for (const auto key : {"timestamp", "createdAt", "created_at", "date", "time"}) {
		const auto value = object.value(key);
		auto seconds = value.isDouble() ? qint64(value.toDouble()) : value.toString().toLongLong();
		if (seconds > 100000000000LL) {
			seconds /= 1000;
		}
		if (seconds > 0) {
			return int(std::min(seconds, qint64(INT_MAX)));
		}
		const auto parsed = QDateTime::fromString(value.toString(), Qt::ISODate);
		if (parsed.isValid()) {
			return int(parsed.toSecsSinceEpoch());
		}
	}
	return 0;
}

QUrl Avatar(const QJsonObject &object) {
	for (const auto key : {"avatarUrl", "avatar", "photo", "image"}) {
		auto text = Text(object.value(key));
		if (text.isEmpty() && object.value(key).isObject()) {
			const auto nested = object.value(key).toObject();
			for (const auto field :
				{"url", "src", "path", "downloadUrl", "fileUrl", "thumbUrl", "thumbnailUrl"}) {
				text = Text(nested.value(field));
				if (!text.isEmpty()) {
					break;
				}
			}
		}
		text.replace('\\', '/');
		if (text.isEmpty() || text.endsWith("default.png")) {
			continue;
		}
		const auto url = QUrl("https://noveo.ir:8443/").resolved(QUrl(text));
		if (url.scheme() == "https" && !url.host().isEmpty()) {
			return url;
		}
	}
	return {};
}

QUrl FileUrl(const QJsonObject &file) {
	for (const auto key : {"url", "fileUrl", "path"}) {
		auto text = Text(file.value(key));
		text.replace('\\', '/');
		if (text.isEmpty()) {
			continue;
		}
		const auto url = QUrl("https://noveo.ir:8443/").resolved(QUrl(text));
		if (url.scheme() == "https" && !url.host().isEmpty()) {
			return url;
		}
	}
	return {};
}

MTPMessageMedia FileMedia(const QJsonObject &file, int date) {
	date = std::max(1, date);
	const auto url = FileUrl(file);
	if (url.isEmpty()) {
		return MTP_messageMediaEmpty();
	}
	const auto remote = url.path().toLower();
	auto name = Text(file.value("name"));
	if (name.isEmpty()) {
		name = Text(file.value("fileName"));
	}
	if (name.isEmpty()) {
		name = remote.section('/', -1);
	}
	auto mime = Text(file.value("type")).toLower();
	if (mime.isEmpty()) {
		mime = Text(file.value("mimeType")).toLower();
	}
	if (mime.isEmpty() || mime == "application/octet-stream" || mime == "binary/octet-stream") {
		mime = QMimeDatabase().mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
	}
	const auto lower = name.toLower();
	const auto tgs = file.value("stickerType").toString().compare("tgs", Qt::CaseInsensitive) == 0
		|| mime.contains("tgsticker") || mime.contains("lottie") || mime == "application/x-tgs"
		|| lower.endsWith(".tgs") || remote.endsWith(".tgs");
	const auto gif = mime == "image/gif" || lower.endsWith(".gif") || remote.endsWith(".gif");
	const auto sticker = tgs || file.value("sticker").toBool()
		|| ((lower.startsWith("sticker") || lower.contains("_sticker") || lower.contains("-sticker")
				|| remote.contains("/sticker"))
			&& mime.startsWith("image/"));
	if (tgs) {
		mime = "application/x-tgsticker";
	} else if (gif) {
		mime = "image/gif";
	} else if (sticker && mime != "video/webm") {
		mime = "image/webp";
	}
	const auto id = NativeUserId("noveo-file:" + url.toString()).bare;
	const auto reference = MTP_bytes("noveo:" + url.toString().toUtf8());
	const auto size = std::max(qint64(0), file.value("size").toVariant().toLongLong());
	const auto width = std::clamp(file.value("width").toInt(sticker ? 512 : 1280), 1, 10000);
	const auto height = std::clamp(file.value("height").toInt(sticker ? 512 : 960), 1, 10000);
	const auto image
		= !sticker && !gif && mime.startsWith("image/") && !file.value("asDocument").toBool();
	if (image) {
		const auto photo = MTP_photo(MTP_flags(MTPDphoto::Flags()), MTP_long(id), MTP_long(0),
			reference, MTP_int(date),
			MTP_vector<MTPPhotoSize>(
				QVector<MTPPhotoSize>{MTP_photoSize(MTP_string("x"), MTP_int(width),
					MTP_int(height), MTP_int(int(std::clamp(size, qint64(1), qint64(INT_MAX)))))}),
			MTPVector<MTPVideoSize>(), MTP_int(0));
		return MTP_messageMediaPhoto(
			MTP_flags(MTPDmessageMediaPhoto::Flag::f_photo), photo, MTPint(), MTPDocument());
	}
	auto attributes
		= QVector<MTPDocumentAttribute>{MTP_documentAttributeFilename(MTP_string(name))};
	if (sticker && !gif) {
		attributes.push_back(MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)));
		attributes.push_back(MTP_documentAttributeSticker(
			MTP_flags(MTPDdocumentAttributeSticker::Flags()),
			MTP_string(file.value("alt").toString()), MTP_inputStickerSetEmpty(), MTPMaskCoords()));
	} else if (mime.startsWith("video/")) {
		const auto flags = file.value("roundVideo").toBool()
			? MTPDdocumentAttributeVideo::Flags(MTPDdocumentAttributeVideo::Flag::f_round_message)
			: MTPDdocumentAttributeVideo::Flags();
		attributes.push_back(MTP_documentAttributeVideo(MTP_flags(flags),
			MTP_double(std::max(0., file.value("duration").toDouble())), MTP_int(width),
			MTP_int(height), MTPint(), MTPdouble(), MTPstring()));
	} else if (mime.startsWith("audio/") || file.value("voice").toBool()) {
		auto flags = MTPDdocumentAttributeAudio::Flags();
		if (file.value("voice").toBool() || mime == "audio/ogg" || mime == "audio/opus") {
			flags |= MTPDdocumentAttributeAudio::Flag::f_voice;
		}
		if (!Text(file.value("title")).isEmpty()) {
			flags |= MTPDdocumentAttributeAudio::Flag::f_title;
		}
		if (!Text(file.value("performer")).isEmpty()) {
			flags |= MTPDdocumentAttributeAudio::Flag::f_performer;
		}
		attributes.push_back(MTP_documentAttributeAudio(MTP_flags(flags),
			MTP_int(std::max(0, file.value("duration").toInt())),
			MTP_string(Text(file.value("title"))), MTP_string(Text(file.value("performer"))),
			MTPbytes()));
	}
	if (gif || (!sticker && file.value("animated").toBool())) {
		attributes.push_back(MTP_documentAttributeAnimated());
		if (gif) {
			attributes.push_back(MTP_documentAttributeImageSize(MTP_int(width), MTP_int(height)));
		}
	}
	auto thumbs = QVector<MTPPhotoSize>();
	const auto thumbnail = QByteArray::fromBase64(file.value("thumb").toString().toUtf8());
	if (!thumbnail.isEmpty() && thumbnail.size() <= 256 * 1024) {
		thumbs.push_back(MTP_photoCachedSize(
			MTP_string("s"), MTP_int(width), MTP_int(height), MTP_bytes(thumbnail)));
	}
	const auto document = MTP_document(
		MTP_flags(thumbs.isEmpty() ? MTPDdocument::Flags()
								   : MTPDdocument::Flags(MTPDdocument::Flag::f_thumbs)),
		MTP_long(id), MTP_long(0), reference, MTP_int(date), MTP_string(mime), MTP_long(size),
		MTP_vector<MTPPhotoSize>(thumbs), MTPVector<MTPVideoSize>(), MTP_int(0),
		MTP_vector<MTPDocumentAttribute>(attributes));
	return MTP_messageMediaDocument(MTP_flags(MTPDmessageMediaDocument::Flag::f_document), document,
		MTPVector<MTPDocument>(), MTPPhoto(), MTPint(), MTPint());
}

int MessageId(const MTPMessage &message) {
	return message.match([](const auto &data) { return data.vid().v; });
}

int MessageDate(const MTPMessage &message) {
	return message.match([](const MTPDmessage &data) { return data.vdate().v; },
		[](const MTPDmessageService &data) { return data.vdate().v; },
		[](const auto &) { return 0; });
}

MTPPeerNotifySettings Notify() {
	return MTP_peerNotifySettings(MTP_flags(MTPDpeerNotifySettings::Flags()), MTPBool(), MTPBool(),
		MTPint(), MTPNotificationSound(), MTPNotificationSound(), MTPNotificationSound(), MTPBool(),
		MTPBool(), MTPNotificationSound(), MTPNotificationSound(), MTPNotificationSound());
}

struct HistoryRequest {
	MTPInputPeer peer;
	MTPint offset, date, addOffset, limit, maxId, minId;
	MTPlong hash;
};

std::optional<HistoryRequest> ReadHistory(const mtpBuffer &body) {
	auto from = body.constData() + 1;
	const auto end = body.constData() + body.size();
	auto result = HistoryRequest();
	return result.peer.read(from, end) && result.offset.read(from, end)
			&& result.date.read(from, end) && result.addOffset.read(from, end)
			&& result.limit.read(from, end) && result.maxId.read(from, end)
			&& result.minId.read(from, end) && result.hash.read(from, end)
		? std::optional<HistoryRequest>(std::move(result))
		: std::nullopt;
}

struct SendRequest {
	MTPflags<MTPmessages_SendMessage::Flags> flags;
	MTPInputPeer peer;
	MTPInputReplyTo reply;
	MTPstring text;
	MTPlong random;
};

std::optional<SendRequest> ReadSend(const mtpBuffer &body) {
	auto from = body.constData() + 1;
	const auto end = body.constData() + body.size();
	auto result = SendRequest();
	return result.flags.read(from, end) && result.peer.read(from, end)
			&& (!(result.flags.v & MTPmessages_SendMessage::Flag::f_reply_to)
				|| result.reply.read(from, end))
			&& result.text.read(from, end) && result.random.read(from, end)
		? std::optional<SendRequest>(std::move(result))
		: std::nullopt;
}

} // namespace

UserId NativeUserId(const QString &rawId) {
	const auto digest = QCryptographicHash::hash(rawId.toUtf8(), QCryptographicHash::Sha256);
	auto id = uint64();
	for (auto i = 1; i != 7; ++i) {
		id = (id << 8) | uchar(digest[i]);
	}
	return std::max(id, uint64(1));
}

SessionClient::SessionClient(AuthClient *auth, QUrl apiEndpoint)
	: _auth(auth), _apiEndpoint(std::move(apiEndpoint)) {
	_deadline.setInterval(1000);
	connect(&_deadline, &QTimer::timeout, this, [this] {
		const auto now = QDateTime::currentMSecsSinceEpoch();
		auto expired = QVector<mtpRequestId>();
		for (auto i = _pending.cbegin(); i != _pending.cend(); ++i) {
			if (i->deadline <= now) {
				expired.push_back(i.key());
			}
		}
		for (const auto id : expired) {
			cancel(id);
			fail(id, "NOVEO_REQUEST_TIMEOUT");
		}
		for (auto i = _uploads.begin(); i != _uploads.end();) {
			if (now - i.value()->touched > 10 * 60 * 1000) {
				i = _uploads.erase(i);
			} else {
				++i;
			}
		}
		if (_pending.isEmpty() && _uploads.isEmpty()) {
			_deadline.stop();
		}
		for (auto i = _sends.begin(); i != _sends.end();) {
			if (!_pending.contains(i->requestId)) {
				i = _sends.erase(i);
			} else {
				++i;
			}
		}
	});
}

void SessionClient::authenticated(const QJsonObject &profile) {
	_self = RawId(profile);
	users(QJsonArray{profile});
	_historyReady = false;
	_contactsReady = false;
	_auth->send({{"type", "resync_state"}});
	contacts();
	gifts();
	drain();
	avatars();
}

MTPUser SessionClient::selfUser() const {
	return _users.value(NativeUserId(_self).bare);
}

void SessionClient::disconnected() {
	_historyReady = false;
	_historyRequests.clear();
}

void SessionClient::reset() {
	_deadline.stop();
	_pending.clear();
	for (const auto id : _transfers.keys()) {
		cancel(id);
	}
	_sends.clear();
	_sendGroups.clear();
	_uploads.clear();
	_files.clear();
	_messageContent.clear();
	_messageObjects.clear();
	_historyRequests.clear();
	_historyAnswered.clear();
	_historyExhausted.clear();
	_users.clear();
	_profiles.clear();
	_chats.clear();
	_dialogs.clear();
	_messages.clear();
	_rawUsers.clear();
	_chatIds.clear();
	_chatPeers.clear();
	_messageIds.clear();
	_rawMessages.clear();
	_avatars.clear();
	_readOutbox.clear();
	_readInbox.clear();
	_contacts.clear();
	_gifts.clear();
	_deferredChats = {};
	_giftsLoading = false;
	_self.clear();
	_historyReady = _contactsReady = false;
}

void SessionClient::fail(mtpRequestId id, const QString &reason) {
	reply(id, MTPRpcError(MTP_rpc_error(MTP_int(400), MTP_string(reason))));
}

void SessionClient::cancel(mtpRequestId id) {
	const auto transfer = _transfers.take(id);
	if (transfer) {
		transfer->abort();
	}
	_sendGroups.remove(id);
	_pending.remove(id);
	_historyRequests.remove(id);
	_historyAnswered.remove(id);
	for (auto i = _sends.begin(); i != _sends.end();) {
		if (i->requestId == id) {
			i = _sends.erase(i);
		} else {
			++i;
		}
	}
}

void SessionClient::request(mtpRequestId id, const mtpBuffer &body) {
	if (body.isEmpty()) {
		fail(id, "NOVEO_BAD_REQUEST");
		return;
	}
	_pending.insert(id, {body, QDateTime::currentMSecsSinceEpoch() + 30000});
	if (!_deadline.isActive()) {
		_deadline.start();
	}
	drain();
}

MTPUser SessionClient::user(const QJsonObject &profile, bool contact) {
	const auto raw = RawId(profile);
	const auto id = NativeUserId(raw);
	_rawUsers[id.bare] = raw;
	auto flags = MTPDuser::Flag::f_access_hash | MTPDuser::Flag::f_first_name
		| MTPDuser::Flag::f_username | MTPDuser::Flag::f_status;
	if (raw == _self) {
		flags |= MTPDuser::Flag::f_self;
	}
	if (contact || profile.value("isContact").toBool()
		|| (_users.contains(id.bare) && _users.value(id.bare).c_user().is_contact())) {
		flags |= MTPDuser::Flag::f_contact;
	}
	if (profile.value("isDisabled").toBool()) {
		flags |= MTPDuser::Flag::f_deleted;
	}
	if (profile.value("isVerified").toBool()) {
		flags |= MTPDuser::Flag::f_verified;
	}
	if (profile.value("isBot").toBool()) {
		flags |= MTPDuser::Flag::f_bot;
	}
	auto name = Text(profile.value("contactName"));
	if (name.isEmpty()) {
		name = Text(profile.value("displayName"));
	}
	if (name.isEmpty()) {
		name = Text(profile.value("username"));
	}
	if (name.isEmpty()) {
		name = raw;
	}
	auto handle = Text(profile.value("handle"));
	if (handle.startsWith('@')) {
		handle.remove(0, 1);
	}
	auto lastSeen = profile.value("lastSeen").toVariant().toLongLong();
	if (lastSeen > 100000000000LL) {
		lastSeen /= 1000;
	}
	const MTPUserStatus status = profile.value("online").toBool()
		? MTPUserStatus(
			  MTP_userStatusOnline(MTP_int(int(QDateTime::currentSecsSinceEpoch()) + 120)))
		: lastSeen ? MTPUserStatus(MTP_userStatusOffline(MTP_int(int(lastSeen))))
				   : MTPUserStatus(MTP_userStatusEmpty());
	const auto avatar = Avatar(profile);
	if (!avatar.isEmpty()) {
		_avatars[peerFromUser(id).value] = avatar;
	} else if (profile.contains("avatarUrl") || profile.contains("avatar")
		|| profile.contains("photo") || profile.contains("image")) {
		_avatars[peerFromUser(id).value] = QUrl();
	}
	return MTP_user(MTP_flags(flags), MTP_long(id.bare), MTP_long(1), MTP_string(name), MTPstring(),
		MTP_string(handle), MTPstring(), MTPUserProfilePhoto(), status, MTPint(),
		MTPVector<MTPRestrictionReason>(), MTPstring(), MTPstring(), MTPEmojiStatus(),
		MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(), MTPPeerColor(), MTPint(),
		MTPlong(), MTPlong(), MTPlong());
}

void SessionClient::users(const QJsonArray &profiles, bool contact) {
	for (const auto value : profiles) {
		const auto profile = value.toObject();
		if (RawId(profile).isEmpty()) {
			continue;
		}
		auto merged = _profiles.value(NativeUserId(RawId(profile)).bare);
		for (auto i = profile.begin(); i != profile.end(); ++i) {
			merged.insert(i.key(), i.value());
		}
		_profiles[NativeUserId(RawId(profile)).bare] = merged;
		const auto native = user(merged, contact);
		_users[native.c_user().vid().v] = native;
	}
}

void SessionClient::contacts() {
	if (_contactsLoading || !_auth->authenticated()) {
		return;
	}
	_contactsLoading = true;
	const auto authorization = _auth->authorization();
	QNetworkRequest request(_apiEndpoint.resolved(QUrl("/user/contacts")));
	request.setRawHeader("X-User-ID", RawId(authorization.value("user").toObject()).toUtf8());
	request.setRawHeader("X-Auth-Token", authorization.value("token").toString().toUtf8());
	request.setRawHeader("Origin", "https://noveo.ir");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(20000);
	const auto reply = _http.get(request);
	connect(reply, &QNetworkReply::readyRead, this, [reply] {
		if (reply->bytesAvailable() > 16 * 1024 * 1024) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, authorization] {
		_contactsLoading = false;
		const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto document = QJsonDocument::fromJson(reply->readAll());
		const auto ok = reply->error() == QNetworkReply::NoError && status == 200
			&& document.object().value("contacts").isArray();
		reply->deleteLater();
		if (authorization != _auth->authorization()) {
			contacts();
			return;
		}
		if (_auth->onDiagnostic) {
			_auth->onDiagnostic(
				QStringLiteral("Contacts response: HTTP %1, valid=%2").arg(status).arg(ok));
		}
		if (ok) {
			const auto profiles = document.object().value("contacts").toArray();
			users(profiles, true);
			_contacts.clear();
			for (const auto value : profiles) {
				const auto raw = RawId(value.toObject());
				if (!raw.isEmpty()) {
					_contacts.push_back(
						MTP_contact(MTP_long(NativeUserId(raw).bare), MTPBool(MTP_boolFalse())));
				}
			}
			_contactsReady = true;
			drain();
		} else {
			auto failed = QVector<mtpRequestId>();
			for (auto i = _pending.cbegin(); i != _pending.cend(); ++i) {
				if (uint32(i->body.front()) == mtpc_contacts_getContacts) {
					failed.push_back(i.key());
				}
			}
			for (const auto id : failed) {
				_pending.remove(id);
				fail(id, "NOVEO_CONTACTS_FAILED");
			}
		}
	});
}

void SessionClient::gifts() {
	_giftsLoading = true;
	const auto authorization = _auth->authorization();
	QNetworkRequest request(_apiEndpoint.resolved(QUrl("/gifts/catalog")));
	request.setRawHeader("X-User-ID", _self.toUtf8());
	request.setRawHeader("X-Auth-Token", authorization.value("token").toString().toUtf8());
	request.setRawHeader("Origin", "https://noveo.ir");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(10000);
	const auto reply = _http.get(request);
	const auto response = std::make_shared<QByteArray>();
	connect(reply, &QNetworkReply::readyRead, this, [reply, response] {
		response->append(reply->readAll());
		if (response->size() > 4 * 1024 * 1024) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, response, authorization] {
		response->append(reply->readAll());
		reply->deleteLater();
		if (authorization != _auth->authorization()) {
			return;
		}
		_giftsLoading = false;
		if (reply->error() == QNetworkReply::NoError && response->size() <= 4 * 1024 * 1024) {
			for (const auto value :
				QJsonDocument::fromJson(*response).object().value("gifts").toArray()) {
				const auto info = value.toObject();
				const auto id = Text(info.value("giftId"));
				if (!id.isEmpty()) {
					_gifts[id] = info;
				}
				if (info.value("giftNumber").toInt() > 0) {
					_gifts[QString::number(info.value("giftNumber").toInt())] = info;
				}
			}
		}
		if (!_deferredChats.isEmpty()) {
			const auto chats = std::exchange(_deferredChats, QJsonArray());
			history(chats);
		}
	});
}

void SessionClient::refreshDialog(PeerId peer) {
	if (!_dialogs.contains(peer.value)) {
		return;
	}
	const auto original = _dialogs.value(peer.value);
	const auto &old = original.c_dialog();
	const auto messages = _messages.value(peer.value);
	_dialogs[peer.value] = MTP_dialog(MTP_flags(MTPDdialog::Flags()), old.vpeer(),
		MTP_int(messages.isEmpty() ? 0 : MessageId(messages.back())),
		MTP_int(std::max(old.vread_inbox_max_id().v, _readInbox.value(peer.value))),
		MTP_int(_readOutbox.value(peer.value)), old.vunread_count(), old.vunread_mentions_count(),
		old.vunread_reactions_count(), old.vunread_poll_votes_count(), old.vnotify_settings(),
		MTPint(), MTPDraftMessage(), MTPint(), MTPint());
}

void SessionClient::avatars() {
	if (_avatarScheduled) {
		return;
	}
	_avatarScheduled = true;
	QTimer::singleShot(0, this, [this] {
		_avatarScheduled = false;
		if (onAvatar) {
			for (auto i = _avatars.cbegin(); i != _avatars.cend(); ++i) {
				onAvatar(PeerId(i.key()), i.value());
			}
		}
	});
}

PeerId SessionClient::inputPeer(const MTPInputPeer &peer) const {
	return peer.match([](const MTPDinputPeerUser &data) { return peerFromUser(data.vuser_id()); },
		[](const MTPDinputPeerChat &data) { return peerFromChat(data.vchat_id().v); },
		[](const MTPDinputPeerChannel &data) { return peerFromChannel(data.vchannel_id().v); },
		[this](const MTPDinputPeerSelf &) { return peerFromUser(NativeUserId(_self)); },
		[](const auto &) { return PeerId(); });
}

MTPMessage SessionClient::nativeMessage(const QJsonObject &object, PeerId peer) {
	auto raw = Text(object.value("messageId"));
	if (raw.isEmpty()) {
		raw = Text(object.value("id"));
	}
	if (raw.isEmpty()) {
		return MTP_messageEmpty(
			MTP_flags(MTPDmessageEmpty::Flags()), MTP_int(0), MTPPeer(peerToMTP(peer)));
	}
	auto id = _messageIds.value(raw);
	if (!id) {
		const auto known = _messages.value(peer.value);
		const auto older = !known.isEmpty() && Date(object) < MessageDate(known.front());
		id = older ? --_olderMessage : ++_nextMessage;
		_messageIds[raw] = id;
		_rawMessages[id] = raw;
	}
	auto sender = Text(object.value("senderId"));
	if (sender.isEmpty()) {
		sender = Text(object.value("sender"));
	}
	const auto profile = object.value("senderProfile").toObject();
	if (!RawId(profile).isEmpty()) {
		users(QJsonArray{profile});
		sender = RawId(profile);
	}
	const auto senderId = NativeUserId(sender);
	if (!_users.contains(senderId.bare)) {
		users(QJsonArray{QJsonObject{{"userId", sender}, {"username", sender}}});
	}
	auto flags = MTPDmessage::Flags(MTPDmessage::Flag::f_from_id);
	if (sender == _self) {
		flags |= MTPDmessage::Flag::f_out;
	}
	if (peerIsChannel(peer)) {
		flags |= MTPDmessage::Flag::f_post;
	}
	auto content = object.value("content");
	if (content.isString()) {
		const auto parsed = QJsonDocument::fromJson(content.toString().toUtf8());
		if (parsed.isObject()) {
			content = parsed.object();
		}
	}
	auto text = content.isObject() ? Text(content.toObject().value("text")) : Text(content);
	if (text.isEmpty()) {
		text = Text(object.value("text"));
	}
	auto replyRaw = QString();
	for (const auto key :
		{"replyToId", "reply_to_id", "replyToMessageId", "reply_to_message_id", "reply_to"}) {
		replyRaw = Text(object.value(key));
		if (!replyRaw.isEmpty()) {
			break;
		}
	}
	for (const auto key : {"replyTo", "reply_to"}) {
		if (!replyRaw.isEmpty()) {
			break;
		}
		const auto nested = object.value(key).toObject();
		replyRaw = Text(nested.value("messageId"));
		if (replyRaw.isEmpty()) {
			replyRaw = Text(nested.value("id"));
		}
	}
	const auto replyId = _messageIds.value(replyRaw);
	if (replyId) {
		flags |= MTPDmessage::Flag::f_reply_to;
	}
	_messageContent[id] = content.isObject() ? content.toObject() : QJsonObject{{"text", text}};
	_messageObjects[id] = object;
	for (const auto reader : object.value("seenBy").toArray()) {
		const auto uid = reader.isString() ? reader.toString() : RawId(reader.toObject());
		if (sender == _self && !uid.isEmpty() && uid != _self) {
			_readOutbox[peer.value] = std::max(_readOutbox.value(peer.value), id);
		}
		if (sender != _self && uid == _self) {
			_readInbox[peer.value] = std::max(_readInbox.value(peer.value), id);
		}
	}
	const auto file = content.toObject().value("file").toObject();
	const auto url = FileUrl(file);
	if (!url.isEmpty()) {
		_files[NativeUserId("noveo-file:" + url.toString()).bare] = file;
	}
	const auto media
		= file.isEmpty() ? MTPMessageMedia(MTP_messageMediaEmpty()) : FileMedia(file, Date(object));
	if (media.type() != mtpc_messageMediaEmpty) {
		flags |= MTPDmessage::Flag::f_media;
	}
	const auto forward = content.toObject().value("forwardedInfo").toObject();
	if (!forward.isEmpty()) {
		flags |= MTPDmessage::Flag::f_fwd_from;
	}
	static const auto giftLink = QRegularExpression(
		R"(\[gift\]\(https?://web\.noveo\.ir/gift(?:/([0-9]+)|\?id=([^)&#]+))\))");
	const auto match = giftLink.match(text);
	if (match.hasMatch() && media.type() == mtpc_messageMediaEmpty) {
		const auto key = match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
		const auto info = _gifts.value(key);
		if (!info.isEmpty()) {
			const auto sticker = FileMedia(QJsonObject{{"url", info.value("imageUrl")},
											   {"name", "sticker"}, {"sticker", true}},
				std::max(1, Date(object)));
			if (sticker.type() == mtpc_messageMediaDocument) {
				auto giftFlags = MTPDstarGift::Flags(MTPDstarGift::Flag::f_title);
				const auto total = info.value("initialStock").toInt();
				if (total > 0) {
					giftFlags |= MTPDstarGift::Flag::f_limited;
				}
				const auto gift = MTP_starGift(MTP_flags(giftFlags),
					MTP_long(NativeUserId("noveo-gift:" + Text(info.value("giftId"))).bare),
					*sticker.c_messageMediaDocument().vdocument(),
					MTP_long(qRound64(info.value("priceTenths").toDouble() / 100.)),
					MTP_int(info.value("stockRemaining").toInt()), MTP_int(total), MTPlong(),
					MTP_long(0), MTPint(), MTPint(), MTPlong(), MTPlong(),
					MTP_string(Text(info.value("name"))), MTPPeer(), MTPint(), MTPint(), MTPint(),
					MTPstring(), MTPint(), MTPint(), MTPint(), MTPStarGiftBackground());
				auto rest = text;
				rest.remove(match.capturedStart(), match.capturedLength());
				auto actionFlags
					= MTPDmessageActionStarGift::Flags(MTPDmessageActionStarGift::Flag::f_from_id);
				if (!rest.trimmed().isEmpty()) {
					actionFlags |= MTPDmessageActionStarGift::Flag::f_message;
				}
				const auto action = MTP_messageActionStarGift(MTP_flags(actionFlags), gift,
					MTP_textWithEntities(
						MTP_string(rest.trimmed()), MTP_vector<MTPMessageEntity>()),
					MTPlong(), MTPint(), MTPlong(), MTP_peerUser(MTP_long(senderId.bare)),
					MTPPeer(), MTPlong(), MTPstring(), MTPint(), MTPPeer(), MTPint());
				auto serviceFlags = MTPDmessageService::Flags(MTPDmessageService::Flag::f_from_id);
				if (sender == _self) {
					serviceFlags |= MTPDmessageService::Flag::f_out;
				}
				return MTP_messageService(MTP_flags(serviceFlags), MTP_int(id),
					MTP_peerUser(MTP_long(senderId.bare)), peerToMTP(peer), MTPPeer(),
					MTPMessageReplyHeader(), MTP_int(Date(object)), action, MTPMessageReactions(),
					MTPint());
			}
		}
	}
	return MTP_message(MTP_flags(flags), MTP_int(id),
		MTPPeer(MTP_peerUser(MTP_long(senderId.bare))), MTPint(), MTPstring(),
		MTPPeer(peerToMTP(peer)), MTPPeer(),
		MTP_messageFwdHeader(MTP_flags(MTPDmessageFwdHeader::Flag::f_from_name), MTPPeer(),
			MTP_string(Text(forward.value("from"))),
			MTP_int(Date(QJsonObject{{"timestamp", forward.value("originalTs")}})), MTPint(),
			MTPstring(), MTPPeer(), MTPint(), MTPPeer(), MTPstring(), MTPint(), MTPstring()),
		MTPlong(), MTPlong(), MTPPeer(),
		MTP_messageReplyHeader(MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id),
			MTP_int(replyId), MTPPeer(), MTPMessageFwdHeader(), MTPMessageMedia(), MTPint(),
			MTPstring(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPbytes()),
		MTP_int(Date(object)), MTP_string(text), media, MTPReplyMarkup(),
		MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPMessageReplies(), MTPint(),
		MTPstring(), MTPlong(), MTPMessageReactions(), MTPVector<MTPRestrictionReason>(), MTPint(),
		MTPint(), MTPlong(), MTPFactCheck(), MTPint(), MTPlong(), MTPSuggestedPost(), MTPint(),
		MTPstring(), MTPRichMessage());
}

MTPmessages_Dialogs SessionClient::dialogs() const {
	auto dialogs = QVector<MTPDialog>();
	auto top = QVector<MTPMessage>();
	auto chats = QVector<MTPChat>();
	auto users = QVector<MTPUser>();
	for (const auto &dialog : _dialogs) {
		dialogs.push_back(dialog);
	}
	for (const auto &messages : _messages) {
		if (!messages.isEmpty()) {
			top.push_back(messages.back());
		}
	}
	for (const auto &chat : _chats) {
		chats.push_back(chat);
	}
	for (const auto &user : _users) {
		users.push_back(user);
	}
	return MTP_messages_dialogs(MTP_vector<MTPDialog>(dialogs), MTP_vector<MTPMessage>(top),
		MTP_vector<MTPChat>(chats), MTP_vector<MTPUser>(users));
}

void SessionClient::history(const QJsonArray &chats) {
	if (_giftsLoading) {
		for (const auto chat : chats) {
			for (const auto message : chat.toObject().value("messages").toArray()) {
				const auto content = message.toObject().value("content");
				const auto text = content.isObject() ? content.toObject().value("text").toString()
													 : content.toString();
				if (text.contains("[gift](")) {
					_deferredChats = chats;
					return;
				}
			}
		}
	}
	_dialogs.clear();
	for (const auto value : chats) {
		const auto object = value.toObject();
		auto raw = Text(object.value("chatId"));
		if (raw.isEmpty()) {
			raw = Text(object.value("id"));
		}
		if (raw.isEmpty() || object.value("hideInNoveoClient").toBool()) {
			continue;
		}
		auto type = Text(object.value("chatType")).toLower();
		if (type.isEmpty()) {
			type = Text(object.value("type")).toLower();
		}
		const auto name = Text(object.value("chatName"));
		auto peer = PeerId();
		if (type == "group" || type == "channel") {
			const auto id = NativeUserId(raw).bare;
			const auto channel = type == "channel";
			peer = channel ? peerFromChannel(ChannelId(id)) : peerFromChat(ChatId(id));
			_chats[peer.value] = channel
				? MTPChat(MTP_channel(
					  MTP_flags(MTPDchannel::Flag::f_broadcast | MTPDchannel::Flag::f_access_hash),
					  MTP_long(id), MTP_long(1), MTP_string(name), MTPstring(),
					  MTPChatPhoto(MTP_chatPhotoEmpty()), MTPint(),
					  MTPVector<MTPRestrictionReason>(), MTPChatAdminRights(),
					  MTPChatBannedRights(), MTPChatBannedRights(), MTPint(),
					  MTPVector<MTPUsername>(), MTPRecentStory(), MTPPeerColor(), MTPPeerColor(),
					  MTPEmojiStatus(), MTPint(), MTPint(), MTPlong(), MTPlong(), MTPlong(),
					  MTPlong()))
				: MTPChat(MTP_chat(MTP_flags(MTPDchat::Flags()), MTP_long(id), MTP_string(name),
					  MTPChatPhoto(MTP_chatPhotoEmpty()),
					  MTP_int(object.value("members").toArray().size()), MTPint(), MTP_int(1),
					  MTPInputChannel(), MTPChatAdminRights(), MTPChatBannedRights()));
			const auto url = Avatar(object);
			if (!url.isEmpty()) {
				_avatars[peer.value] = url;
			}
		} else {
			auto other = QString();
			const auto members = object.value("members").toArray();
			for (const auto member : members) {
				const auto id = member.isString() ? member.toString() : RawId(member.toObject());
				if (!id.isEmpty() && id != _self) {
					other = id;
					break;
				}
			}
			if (type == "saved" || (members.size() == 1 && other.isEmpty())) {
				other = _self;
			}
			if (other.isEmpty()) {
				other = "noveo-synthetic-peer:" + raw;
			}
			const auto id = NativeUserId(other);
			peer = peerFromUser(id);
			if (!_users.contains(id.bare)) {
				users(QJsonArray{QJsonObject{{"userId", other}, {"username", name}}});
			}
		}
		_chatIds[peer.value] = raw;
		_chatPeers[raw] = peer;
		auto rows = QVector<QJsonObject>();
		for (const auto row : object.value("messages").toArray()) {
			rows.push_back(row.toObject());
		}
		std::stable_sort(rows.begin(), rows.end(),
			[](const auto &a, const auto &b) { return Date(a) < Date(b); });
		auto messages = _messages.value(peer.value);
		if (messages.isEmpty()) {
			for (const auto &row : rows) {
				auto raw = Text(row.value("messageId"));
				if (raw.isEmpty()) {
					raw = Text(row.value("id"));
				}
				if (!raw.isEmpty() && !_messageIds.contains(raw)) {
					const auto id = ++_nextMessage;
					_messageIds[raw] = id;
					_rawMessages[id] = raw;
				}
			}
		}
		for (const auto &row : rows) {
			const auto native = nativeMessage(row, peer);
			if (native.type() != mtpc_message && native.type() != mtpc_messageService) {
				continue;
			}
			const auto id = MessageId(native);
			const auto existing = std::find_if(messages.begin(), messages.end(),
				[=](const auto &m) { return MessageId(m) == id; });
			if (existing == messages.end()) {
				messages.push_back(native);
			} else {
				*existing = native;
			}
		}
		std::sort(messages.begin(), messages.end(),
			[](const auto &a, const auto &b) { return MessageId(a) < MessageId(b); });
		_messages[peer.value] = messages;
		const auto top = messages.isEmpty() ? 0 : MessageId(messages.back());
		const auto unread = std::max(0, object.value("unreadCount").toInt());
		_dialogs[peer.value] = MTP_dialog(MTP_flags(MTPDdialog::Flags()), MTPPeer(peerToMTP(peer)),
			MTP_int(top), MTP_int(std::max(_readInbox.value(peer.value), unread ? 0 : top)),
			MTP_int(_readOutbox.value(peer.value)), MTP_int(unread), MTPint(), MTPint(), MTPint(),
			Notify(), MTPint(), MTPDraftMessage(), MTPint(), MTPint());
	}
	_historyReady = true;
	if (_auth->onDiagnostic) {
		_auth->onDiagnostic(QStringLiteral("Chat sync complete: %1 dialogs, %2 users")
				.arg(_dialogs.size())
				.arg(_users.size()));
	}
	drain();
	if (onDialogs) {
		onDialogs(dialogs());
	}
	avatars();
}

void SessionClient::message(const QJsonObject &frame) {
	const auto type = frame.value("type").toString();
	if (type == "message_seen" || type == "message_seen_update") {
		const auto peer = _chatPeers.value(Text(frame.value("chatId")));
		const auto id = _messageIds.value(Text(frame.value("messageId")));
		const auto reader = Text(frame.value("userId"));
		if (!peer || !id || reader.isEmpty()) {
			return;
		}
		const auto inbox = reader == _self;
		auto &read = inbox ? _readInbox[peer.value] : _readOutbox[peer.value];
		if (id <= read) {
			return;
		}
		read = id;
		refreshDialog(peer);
		const auto update = inbox && peerIsChannel(peer)
			? MTPUpdate(MTP_updateReadChannelInbox(MTP_flags(MTPDupdateReadChannelInbox::Flags()),
				  MTPint(), MTP_long(peerToChannel(peer).bare), MTP_int(id), MTP_int(0),
				  MTP_int(_pts)))
			: inbox
			? MTPUpdate(MTP_updateReadHistoryInbox(MTP_flags(MTPDupdateReadHistoryInbox::Flags()),
				  MTPint(), MTPPeer(peerToMTP(peer)), MTPint(), MTP_int(id), MTP_int(0),
				  MTP_int(++_pts), MTP_int(1)))
			: peerIsChannel(peer) ? MTPUpdate(MTP_updateReadChannelOutbox(
										MTP_long(peerToChannel(peer).bare), MTP_int(id)))
								  : MTPUpdate(MTP_updateReadHistoryOutbox(MTPPeer(peerToMTP(peer)),
										MTP_int(id), MTP_int(++_pts), MTP_int(1)));
		if (onUpdate) {
			onUpdate(MTP_updates(MTP_vector<MTPUpdate>(QVector<MTPUpdate>{update}),
				MTP_vector<MTPUser>(), MTP_vector<MTPChat>(),
				MTP_int(int(QDateTime::currentSecsSinceEpoch())), MTP_int(0)));
		}
	} else if (type == "user_list_update") {
		auto profiles = frame.value("users").toArray();
		const auto online = frame.value("online").toArray();
		for (auto i = 0; i != profiles.size(); ++i) {
			auto profile = profiles[i].toObject();
			profile.insert("online", online.contains(RawId(profile)));
			profiles[i] = profile;
		}
		users(profiles);
		if (_historyReady && onDialogs) {
			onDialogs(dialogs());
		}
		avatars();
	} else if (type == "older_messages") {
		const auto chatId = Text(frame.value("chatId"));
		const auto peer = _chatPeers.value(chatId);
		auto requestId = frame.value("requestId").toVariant().toInt();
		if (!requestId) {
			for (auto i = _historyRequests.cbegin(); i != _historyRequests.cend(); ++i) {
				if (i.value() == peer) {
					requestId = i.key();
					break;
				}
			}
		}
		if (!peer || !_historyRequests.contains(requestId) || !frame.value("messages").isArray()) {
			return;
		}
		auto rows = QVector<QJsonObject>();
		for (const auto value : frame.value("messages").toArray()) {
			rows.push_back(value.toObject());
		}
		std::stable_sort(rows.begin(), rows.end(),
			[](const auto &a, const auto &b) { return Date(a) < Date(b); });
		for (auto i = rows.size(); i > 0; --i) {
			const auto &row = rows[i - 1];
			auto raw = Text(row.value("messageId"));
			if (raw.isEmpty()) {
				raw = Text(row.value("id"));
			}
			if (!raw.isEmpty() && !_messageIds.contains(raw)) {
				const auto id = --_olderMessage;
				_messageIds[raw] = id;
				_rawMessages[id] = raw;
			}
		}
		auto &messages = _messages[peer.value];
		for (const auto &row : rows) {
			const auto native = nativeMessage(row, peer);
			if (native.type() != mtpc_message && native.type() != mtpc_messageService) {
				continue;
			}
			const auto exists = std::any_of(messages.begin(), messages.end(),
				[&](const auto &m) { return MessageId(m) == MessageId(native); });
			if (!exists) {
				messages.push_back(native);
			}
		}
		std::sort(messages.begin(), messages.end(),
			[](const auto &a, const auto &b) { return MessageId(a) < MessageId(b); });
		if (!frame.value("hasMoreHistory").toBool()) {
			_historyExhausted.insert(peer.value);
		}
		_historyRequests.remove(requestId);
		_historyAnswered.insert(requestId);
		drain();
	} else if (type == "chat_history") {
		if (frame.value("chats").isArray()) {
			history(frame.value("chats").toArray());
		}
	} else if (type == "message" || type == "new_message" || type == "message_sent") {
		auto object = frame;
		for (const auto key : {"message", "payload", "data"}) {
			if (frame.value(key).isObject()) {
				object = frame.value(key).toObject();
				break;
			}
		}
		auto chatId = Text(object.value("chatId"));
		if (chatId.isEmpty()) {
			chatId = Text(frame.value("chatId"));
		}
		const auto tempId = Text(object.value("clientTempId")).isEmpty()
			? Text(frame.value("clientTempId"))
			: Text(object.value("clientTempId"));
		auto peer = _chatPeers.value(chatId);
		if (!peer && _sends.contains(tempId)) {
			peer = _sends.value(tempId).peer;
		}
		if (peer && !chatId.isEmpty()) {
			_chatIds[peer.value] = chatId;
			_chatPeers[chatId] = peer;
		}
		if (!peer) {
			_auth->send({{"type", "resync_state"}});
			return;
		}
		const auto native = nativeMessage(object, peer);
		if (native.type() != mtpc_message && native.type() != mtpc_messageService) {
			return;
		}
		const auto nativeId = MTP_int(MessageId(native));
		const auto nativeDate = MTP_int(MessageDate(native));
		auto &messages = _messages[peer.value];
		const auto exists = std::any_of(messages.begin(), messages.end(),
			[&](const auto &m) { return MessageId(m) == nativeId.v; });
		if (!exists) {
			messages.push_back(native);
		}
		refreshDialog(peer);
		if (_sends.contains(tempId)) {
			if (native.type() == mtpc_messageService
				&& !_sendGroups.contains(_sends.value(tempId).requestId)) {
				_sendGroups[_sends.value(tempId).requestId].remaining = 1;
			}
			const auto send = _sends.take(tempId);
			if (_sendGroups.contains(send.requestId)) {
				auto &group = _sendGroups[send.requestId];
				group.updates.push_back(MTP_updateMessageID(nativeId, MTP_long(send.randomId)));
				group.updates.push_back(peerIsChannel(peer)
						? MTPUpdate(
							  MTP_updateNewChannelMessage(native, MTP_int(++_pts), MTP_int(1)))
						: MTPUpdate(MTP_updateNewMessage(native, MTP_int(++_pts), MTP_int(1))));
				if (--group.remaining == 0) {
					const auto updates = _sendGroups.take(send.requestId).updates;
					_pending.remove(send.requestId);
					reply(send.requestId,
						MTPUpdates(MTP_updates(MTP_vector<MTPUpdate>(updates),
							MTP_vector<MTPUser>(_users.values().toVector()),
							MTP_vector<MTPChat>(_chats.values().toVector()), nativeDate,
							MTP_int(0))));
				}
			} else {
				_pending.remove(send.requestId);
				reply(send.requestId,
					MTPUpdates(MTP_updateShortSentMessage(
						MTP_flags(MTPDupdateShortSentMessage::Flag::f_out
							| (native.c_message().vmedia()
									? MTPDupdateShortSentMessage::Flag::f_media
									: MTPDupdateShortSentMessage::Flag())),
						nativeId, MTP_int(++_pts), MTP_int(1), nativeDate,
						native.c_message().vmedia() ? *native.c_message().vmedia()
													: MTPMessageMedia(),
						MTPVector<MTPMessageEntity>(), MTPint())));
			}
		} else if (!exists && onUpdate) {
			auto users = QVector<MTPUser>();
			for (const auto &user : _users) {
				users.push_back(user);
			}
			onUpdate(MTP_updates(
				MTP_vector<MTPUpdate>(QVector<MTPUpdate>{peerIsChannel(peer)
						? MTPUpdate(
							  MTP_updateNewChannelMessage(native, MTP_int(++_pts), MTP_int(1)))
						: MTPUpdate(MTP_updateNewMessage(native, MTP_int(++_pts), MTP_int(1)))}),
				MTP_vector<MTPUser>(users), MTP_vector<MTPChat>(QVector<MTPChat>()), nativeDate,
				MTP_int(0)));
		}
	} else if (type == "error") {
		const auto tempId = Text(frame.value("clientTempId"));
		if (_sends.contains(tempId)) {
			const auto send = _sends.take(tempId);
			_pending.remove(send.requestId);
			fail(send.requestId, "NOVEO_SEND_FAILED");
		}
	}
}

bool SessionClient::send(mtpRequestId id, PeerId peer, QJsonObject content, uint64 randomId,
	const MTPInputReplyTo &replyTo, bool hasReply) {
	const auto raw = _chatIds.value(peer.value);
	const auto recipient = peerIsUser(peer) ? _rawUsers.value(peerToUser(peer).bare) : QString();
	if (raw.isEmpty() && recipient.isEmpty()) {
		cancel(id);
		fail(id, "NOVEO_PEER_UNKNOWN");
		return false;
	}
	const auto temp = QUuid::createUuid().toString(QUuid::WithoutBraces);
	auto frame = QJsonObject{{"type", "message"}, {"clientTempId", temp}, {"content", content}};
	frame.insert(raw.isEmpty() ? "recipientId" : "chatId", raw.isEmpty() ? recipient : raw);
	if (hasReply) {
		if (replyTo.type() != mtpc_inputReplyToMessage) {
			cancel(id);
			fail(id, "NOVEO_REPLY_UNSUPPORTED");
			return false;
		}
		const auto uuid = _rawMessages.value(replyTo.c_inputReplyToMessage().vreply_to_msg_id().v);
		if (uuid.isEmpty()) {
			cancel(id);
			fail(id, "NOVEO_REPLY_UNKNOWN");
			return false;
		}
		frame.insert("replyToId", uuid);
	}
	_sends[temp] = {id, randomId, peer};
	_auth->send(frame);
	return true;
}

QJsonObject SessionClient::mediaFile(const MTPInputMedia &media) const {
	return media.match(
		[&](const MTPDinputMediaPhoto &data) {
			return data.vid().type() == mtpc_inputPhoto
				? _files.value(data.vid().c_inputPhoto().vid().v)
				: QJsonObject();
		},
		[&](const MTPDinputMediaDocument &data) {
			return data.vid().type() == mtpc_inputDocument
				? _files.value(data.vid().c_inputDocument().vid().v)
				: QJsonObject();
		},
		[&](const MTPDinputMediaPhotoExternal &data) {
			return QJsonObject{{"url", qs(data.vurl())}, {"type", "image/jpeg"}};
		},
		[&](const MTPDinputMediaDocumentExternal &data) {
			return QJsonObject{{"url", qs(data.vurl())}, {"asDocument", true}};
		},
		[](const auto &) { return QJsonObject(); });
}

void SessionClient::upload(
	mtpRequestId id, const MTPInputMedia &media, std::function<void(QJsonObject)> done) {
	const auto existing = mediaFile(media);
	if (!existing.isEmpty()) {
		done(existing);
		return;
	}
	auto input = MTPInputFile();
	auto metadata = QJsonObject();
	auto mime = QString("image/jpeg");
	if (media.type() == mtpc_inputMediaUploadedPhoto) {
		input = media.c_inputMediaUploadedPhoto().vfile();
	} else if (media.type() == mtpc_inputMediaUploadedDocument) {
		const auto &data = media.c_inputMediaUploadedDocument();
		input = data.vfile();
		mime = qs(data.vmime_type());
		metadata.insert("asDocument", data.is_force_file());
		for (const auto &attribute : data.vattributes().v) {
			attribute.match(
				[&](const MTPDdocumentAttributeFilename &a) {
					metadata.insert("name", qs(a.vfile_name()));
				},
				[&](const MTPDdocumentAttributeImageSize &a) {
					metadata.insert("width", a.vw().v);
					metadata.insert("height", a.vh().v);
				},
				[&](const MTPDdocumentAttributeSticker &a) {
					metadata.insert("sticker", true);
					metadata.insert("alt", qs(a.valt()));
				},
				[&](const MTPDdocumentAttributeAnimated &) { metadata.insert("animated", true); },
				[&](const MTPDdocumentAttributeVideo &a) {
					metadata.insert("width", a.vw().v);
					metadata.insert("height", a.vh().v);
					metadata.insert("duration", a.vduration().v);
					metadata.insert("roundVideo", a.is_round_message());
				},
				[&](const MTPDdocumentAttributeAudio &a) {
					metadata.insert("duration", a.vduration().v);
					metadata.insert("voice", a.is_voice());
					if (a.vtitle()) {
						metadata.insert("title", qs(*a.vtitle()));
					}
					if (a.vperformer()) {
						metadata.insert("performer", qs(*a.vperformer()));
					}
				},
				[](const auto &) {});
		}
		if (mime == "application/x-tgsticker") {
			metadata.insert("stickerType", "tgs");
		}
	} else {
		cancel(id);
		fail(id, "NOVEO_MEDIA_UNSUPPORTED");
		return;
	}
	uint64 fileId = 0;
	int parts = 0;
	auto name = QString();
	input.match(
		[&](const MTPDinputFile &f) {
			fileId = f.vid().v;
			parts = f.vparts().v;
			name = qs(f.vname());
		},
		[&](const MTPDinputFileBig &f) {
			fileId = f.vid().v;
			parts = f.vparts().v;
			name = qs(f.vname());
		},
		[](const auto &) {});
	const auto staged = _uploads.value(fileId);
	const auto reject = [&] {
		_uploads.remove(fileId);
		cancel(id);
		fail(id, "NOVEO_UPLOAD_INCOMPLETE");
	};
	if (!staged || parts <= 0 || parts > 16384 || staged->parts.size() != parts) {
		reject();
		return;
	}
	const auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
	const auto file = new QTemporaryFile(multipart);
	if (!file->open()) {
		delete multipart;
		reject();
		return;
	}
	for (auto index = 0; index != parts; ++index) {
		QFile part(staged->directory.filePath(QString::number(index)));
		if (!staged->parts.contains(index) || !part.open(QIODevice::ReadOnly)) {
			delete multipart;
			reject();
			return;
		}
		while (!part.atEnd()) {
			const auto bytes = part.read(64 * 1024);
			if (bytes.isEmpty() || file->write(bytes) != bytes.size()) {
				delete multipart;
				reject();
				return;
			}
		}
	}
	if (!file->seek(0)) {
		delete multipart;
		reject();
		return;
	}
	if (media.type() == mtpc_inputMediaUploadedPhoto) {
		QImageReader reader(file);
		const auto dimensions = reader.size();
		if (dimensions.isValid()) {
			metadata.insert("width", dimensions.width());
			metadata.insert("height", dimensions.height());
		}
		file->seek(0);
	}
	if (media.type() == mtpc_inputMediaUploadedDocument) {
		const auto &data = media.c_inputMediaUploadedDocument();
		if (data.vthumb() && data.vthumb()->type() == mtpc_inputFile) {
			const auto &thumb = data.vthumb()->c_inputFile();
			const auto staged = _uploads.take(thumb.vid().v);
			if (staged && staged->bytes <= 256 * 1024 && staged->parts.size() == thumb.vparts().v) {
				auto bytes = QByteArray();
				for (auto index = 0; index != thumb.vparts().v; ++index) {
					QFile part(staged->directory.filePath(QString::number(index)));
					if (!part.open(QIODevice::ReadOnly)) {
						bytes.clear();
						break;
					}
					bytes.append(part.readAll());
				}
				if (!bytes.isEmpty()) {
					metadata.insert("thumb", QString::fromLatin1(bytes.toBase64()));
				}
			}
		}
	}
	_uploads.remove(fileId);
	name.replace('"', '_').replace('\\', '_').replace('\r', '_').replace('\n', '_');
	QHttpPart part;
	part.setHeader(QNetworkRequest::ContentDispositionHeader,
		"form-data; name=\"file\"; filename=\"" + name + "\"");
	part.setHeader(QNetworkRequest::ContentTypeHeader, mime);
	part.setBodyDevice(file);
	multipart->append(part);
	const auto authorization = _auth->authorization();
	QNetworkRequest request(_apiEndpoint.resolved(QUrl("/upload/file")));
	request.setRawHeader("X-User-ID", _self.toUtf8());
	request.setRawHeader("X-Auth-Token", authorization.value("token").toString().toUtf8());
	request.setRawHeader(
		"X-Upload-ID", QUuid::createUuid().toString(QUuid::WithoutBraces).toUtf8());
	request.setRawHeader("Origin", "https://noveo.ir");
	request.setAttribute(
		QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(60000);
	_pending[id].deadline = QDateTime::currentMSecsSinceEpoch() + 10 * 60 * 1000;
	const auto reply = _http.post(request, multipart);
	multipart->setParent(reply);
	_transfers[id] = reply;
	const auto response = std::make_shared<QByteArray>();
	connect(reply, &QNetworkReply::readyRead, this, [reply, response] {
		response->append(reply->readAll());
		if (response->size() > 1024 * 1024) {
			reply->abort();
		}
	});
	connect(reply, &QNetworkReply::finished, this,
		[this, reply, response, id, authorization, metadata, mime, done = std::move(done)] {
			reply->deleteLater();
			if (_transfers.value(id) != reply || !_pending.contains(id)) {
				return;
			}
			if (reply->isOpen()) {
				response->append(reply->readAll());
			}
			const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			const auto ok
				= reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
			auto file = QJsonDocument::fromJson(*response).object().value("file").toObject();
			_transfers.remove(id);
			if (!ok || authorization != _auth->authorization() || FileUrl(file).isEmpty()) {
				cancel(id);
				fail(id, "NOVEO_UPLOAD_FAILED");
				return;
			}
			for (auto i = metadata.begin(); i != metadata.end(); ++i) {
				file.insert(i.key(), i.value());
			}
			file.insert("type", mime);
			_files[NativeUserId("noveo-file:" + FileUrl(file).toString()).bare] = file;
			_pending[id].deadline = QDateTime::currentMSecsSinceEpoch() + 30000;
			done(file);
		});
}

bool SessionClient::mediaRequest(mtpRequestId id, const mtpBuffer &body) {
	const auto type = uint32(body.front());
	if (type != mtpc_upload_saveFilePart && type != mtpc_upload_saveBigFilePart
		&& type != mtpc_messages_sendMedia && type != mtpc_messages_uploadMedia
		&& type != mtpc_messages_forwardMessages && type != mtpc_messages_sendMultiMedia) {
		return false;
	}
	if (!_auth->authenticated() || !_historyReady) {
		return true;
	}
	if (_transfers.contains(id) || _sendGroups.contains(id)
		|| std::any_of(_sends.cbegin(), _sends.cend(),
			[=](const auto &send) { return send.requestId == id; })) {
		return true;
	}
	auto from = body.constData() + 1;
	const auto end = body.constData() + body.size();
	const auto bad = [&] {
		cancel(id);
		fail(id, "NOVEO_BAD_REQUEST");
	};
	if (type == mtpc_upload_saveFilePart || type == mtpc_upload_saveBigFilePart) {
		auto file = MTPlong();
		auto index = MTPint();
		auto total = MTPint();
		auto bytes = MTPbytes();
		if (!file.read(from, end) || !index.read(from, end)
			|| (type == mtpc_upload_saveBigFilePart && !total.read(from, end))
			|| !bytes.read(from, end) || index.v < 0 || index.v >= 16384 || bytes.v.isEmpty()
			|| bytes.v.size() > 512 * 1024) {
			bad();
			return true;
		}
		auto staged = _uploads.value(file.v);
		if (!staged) {
			if (_uploads.size() >= 32) {
				bad();
				return true;
			}
			staged = std::make_shared<Upload>();
			_uploads[file.v] = staged;
		}
		auto totalBytes = qint64();
		for (const auto &upload : _uploads) {
			totalBytes += upload->bytes;
		}
		if (!staged->directory.isValid()
			|| totalBytes + bytes.v.size() > 8LL * 1024 * 1024 * 1024) {
			bad();
			return true;
		}
		QFile part(staged->directory.filePath(QString::number(index.v)));
		const auto previous = part.size();
		if (!part.open(QIODevice::WriteOnly) || part.write(bytes.v) != bytes.v.size()) {
			bad();
			return true;
		}
		part.close();
		staged->bytes += bytes.v.size() - previous;
		staged->parts.insert(index.v);
		staged->touched = QDateTime::currentMSecsSinceEpoch();
		_pending.remove(id);
		reply(id, MTPBool(MTP_boolTrue()));
		return true;
	}
	if (type == mtpc_messages_forwardMessages) {
		auto flags = MTPflags<MTPmessages_ForwardMessages::Flags>();
		auto source = MTPInputPeer();
		auto target = MTPInputPeer();
		auto ids = MTPVector<MTPint>();
		auto random = MTPVector<MTPlong>();
		if (!flags.read(from, end) || !source.read(from, end) || !ids.read(from, end)
			|| !random.read(from, end) || !target.read(from, end) || ids.v.isEmpty()
			|| ids.v.size() != random.v.size() || ids.v.size() > 100) {
			bad();
			return true;
		}
		if (flags.v
			& (MTPmessages_ForwardMessages::Flag::f_schedule_date
				| MTPmessages_ForwardMessages::Flag::f_send_as)) {
			bad();
			return true;
		}
		const auto peer = inputPeer(target);
		for (const auto value : ids.v) {
			if (!_messageContent.contains(value.v)) {
				cancel(id);
				fail(id, "NOVEO_MESSAGE_UNKNOWN");
				return true;
			}
		}
		_sendGroups[id].remaining = ids.v.size();
		for (auto index = 0; index != ids.v.size(); ++index) {
			const auto messageId = ids.v[index].v;
			auto content = _messageContent.value(messageId);
			if (flags.v & MTPmessages_ForwardMessages::Flag::f_drop_media_captions) {
				content.insert("text", "");
			}
			if (flags.v & MTPmessages_ForwardMessages::Flag::f_drop_author) {
				content.remove("forwardedInfo");
			} else if (!content.value("forwardedInfo").isObject()) {
				const auto original = _messageObjects.value(messageId);
				const auto profile
					= _profiles.value(NativeUserId(Text(original.value("senderId"))).bare);
				content.insert("forwardedInfo",
					QJsonObject{
						{"from", Text(profile.value("username"))}, {"originalTs", Date(original)}});
			}
			if (!send(id, peer, content, random.v[index].v)) {
				break;
			}
		}
		return true;
	}
	if (type == mtpc_messages_sendMultiMedia) {
		auto flags = MTPflags<MTPmessages_SendMultiMedia::Flags>();
		auto peer = MTPInputPeer();
		auto replyTo = MTPInputReplyTo();
		auto items = MTPVector<MTPInputSingleMedia>();
		if (!flags.read(from, end) || !peer.read(from, end)
			|| ((flags.v & MTPmessages_SendMultiMedia::Flag::f_reply_to)
				&& !replyTo.read(from, end))
			|| !items.read(from, end) || items.v.isEmpty() || items.v.size() > 10) {
			bad();
			return true;
		}
		for (const auto &item : items.v) {
			if (FileUrl(mediaFile(item.c_inputSingleMedia().vmedia())).isEmpty()) {
				bad();
				return true;
			}
		}
		_sendGroups[id].remaining = items.v.size();
		for (const auto &item : items.v) {
			const auto &data = item.c_inputSingleMedia();
			if (!send(id, inputPeer(peer),
					QJsonObject{{"text", qs(data.vmessage())}, {"file", mediaFile(data.vmedia())}},
					data.vrandom_id().v, replyTo,
					bool(flags.v & MTPmessages_SendMultiMedia::Flag::f_reply_to))) {
				break;
			}
		}
		return true;
	}
	auto peer = MTPInputPeer();
	auto media = MTPInputMedia();
	auto replyTo = MTPInputReplyTo();
	auto text = MTPstring();
	auto random = MTPlong();
	auto hasReply = false;
	if (type == mtpc_messages_sendMedia) {
		auto flags = MTPflags<MTPmessages_SendMedia::Flags>();
		if (!flags.read(from, end) || !peer.read(from, end)
			|| ((flags.v & MTPmessages_SendMedia::Flag::f_reply_to) && !replyTo.read(from, end))
			|| !media.read(from, end) || !text.read(from, end) || !random.read(from, end)) {
			bad();
			return true;
		}
		hasReply = bool(flags.v & MTPmessages_SendMedia::Flag::f_reply_to);
		if (flags.v
			& (MTPmessages_SendMedia::Flag::f_schedule_date
				| MTPmessages_SendMedia::Flag::f_send_as)) {
			bad();
			return true;
		}
	} else {
		auto flags = MTPflags<MTPmessages_UploadMedia::Flags>();
		auto business = MTPstring();
		if (!flags.read(from, end)
			|| ((flags.v & MTPmessages_UploadMedia::Flag::f_business_connection_id)
				&& !business.read(from, end))
			|| !peer.read(from, end) || !media.read(from, end)) {
			bad();
			return true;
		}
	}
	upload(id, media, [this, id, type, peer, replyTo, text, random, hasReply](QJsonObject file) {
		if (!_pending.contains(id)) {
			return;
		}
		if (type == mtpc_messages_uploadMedia) {
			_pending.remove(id);
			reply(id, FileMedia(file, int(QDateTime::currentSecsSinceEpoch())));
		} else {
			send(id, inputPeer(peer), QJsonObject{{"text", qs(text)}, {"file", file}}, random.v,
				replyTo, hasReply);
		}
	});
	return true;
}

void SessionClient::drain() {
	const auto keys = _pending.keys();
	for (const auto id : keys) {
		if (!_pending.contains(id)) {
			continue;
		}
		const auto body = _pending.value(id).body;
		const auto type = uint32(body.front());
		const auto finish = [&] {
			_pending.remove(id);
			_historyAnswered.remove(id);
		};
		const auto now = int(QDateTime::currentSecsSinceEpoch());
		const auto state = MTPupdates_State(
			MTP_updates_state(MTP_int(_pts), MTP_int(0), MTP_int(now), MTP_int(0), MTP_int(0)));
		if (mediaRequest(id, body)) {
			continue;
		}
		if (type == mtpc_updates_getState) {
			finish();
			reply(id, state);
		} else if (type == mtpc_updates_getDifference) {
			finish();
			reply(id, MTPupdates_Difference(MTP_updates_differenceEmpty(MTP_int(now), MTP_int(0))));
		} else if (type == mtpc_messages_getDialogFilters) {
			finish();
			reply(id,
				MTPmessages_DialogFilters(
					MTP_messages_dialogFilters(MTP_flags(MTPDmessages_dialogFilters::Flags()),
						MTP_vector<MTPDialogFilter>(
							QVector<MTPDialogFilter>{MTP_dialogFilterDefault()}))));
		} else if (type == mtpc_messages_getPinnedDialogs) {
			finish();
			reply(id,
				MTPmessages_PeerDialogs(
					MTP_messages_peerDialogs(MTP_vector<MTPDialog>(QVector<MTPDialog>()),
						MTP_vector<MTPMessage>(QVector<MTPMessage>()),
						MTP_vector<MTPChat>(QVector<MTPChat>()),
						MTP_vector<MTPUser>(QVector<MTPUser>()), state)));
		} else if (type == mtpc_messages_getDialogs || type == mtpc_messages_getPeerDialogs) {
			if (!_auth->authenticated() || !_historyReady) {
				continue;
			}
			finish();
			if (type == mtpc_messages_getDialogs) {
				reply(id, dialogs());
			} else {
				const auto snapshot = dialogs();
				const auto &data = snapshot.c_messages_dialogs();
				reply(id,
					MTPmessages_PeerDialogs(MTP_messages_peerDialogs(
						data.vdialogs(), data.vmessages(), data.vchats(), data.vusers(), state)));
			}
		} else if (type == mtpc_contacts_getContacts) {
			if (!_auth->authenticated()) {
				continue;
			}
			if (!_contactsReady) {
				contacts();
				continue;
			}
			auto users = QVector<MTPUser>();
			for (const auto &user : _users) {
				users.push_back(user);
			}
			finish();
			reply(id,
				MTPcontacts_Contacts(MTP_contacts_contacts(MTP_vector<MTPContact>(_contacts),
					MTP_int(_contacts.size()), MTP_vector<MTPUser>(users))));
		} else if (type == mtpc_messages_getMessages || type == mtpc_channels_getMessages) {
			if (!_historyReady) {
				continue;
			}
			auto from = body.constData() + 1;
			const auto end = body.constData() + body.size();
			auto channel = MTPInputChannel();
			auto ids = MTPVector<MTPInputMessage>();
			if ((type == mtpc_channels_getMessages && !channel.read(from, end))
				|| !ids.read(from, end)) {
				finish();
				fail(id, "NOVEO_BAD_REQUEST");
				continue;
			}
			auto found = QVector<MTPMessage>();
			for (const auto &input : ids.v) {
				if (input.type() != mtpc_inputMessageID) {
					continue;
				}
				const auto wanted = input.c_inputMessageID().vid().v;
				for (auto i = _messages.cbegin(); i != _messages.cend(); ++i) {
					if (type == mtpc_channels_getMessages
						&& (channel.type() != mtpc_inputChannel
							|| PeerId(i.key())
								!= peerFromChannel(channel.c_inputChannel().vchannel_id().v))) {
						continue;
					}
					for (const auto &message : i.value()) {
						if (MessageId(message) == wanted) {
							found.push_back(message);
						}
					}
				}
			}
			finish();
			reply(id,
				MTPmessages_Messages(MTP_messages_messages(MTP_vector<MTPMessage>(found),
					MTP_vector<MTPForumTopic>(), MTP_vector<MTPChat>(_chats.values().toVector()),
					MTP_vector<MTPUser>(_users.values().toVector()))));
		} else if (type == mtpc_messages_getHistory) {
			if (!_historyReady) {
				continue;
			}
			const auto request = ReadHistory(body);
			if (!request) {
				finish();
				fail(id, "NOVEO_BAD_REQUEST");
				continue;
			}
			const auto peer = inputPeer(request->peer);
			auto result = _messages.value(peer.value);
			const auto offset = request->offset.v;
			const auto limit = std::clamp(request->limit.v, 1, 100);
			std::reverse(result.begin(), result.end());
			auto start = 0;
			if (offset > 0) {
				while (start < result.size() && MessageId(result[start]) >= offset) {
					++start;
				}
				start = std::clamp(start + request->addOffset.v, 0, int(result.size()));
			}
			if (offset > 0 && start + limit > result.size()
				&& !_historyExhausted.contains(peer.value) && !_historyAnswered.contains(id)
				&& !_chatIds.value(peer.value).isEmpty()) {
				if (!_historyRequests.contains(id) && _auth->authenticated()) {
					const auto before = result.isEmpty() ? now + 3600 : MessageDate(result.back());
					auto frame = QJsonObject{{"type", "load_older_messages"},
						{"chatId", _chatIds.value(peer.value)}, {"requestId", QString::number(id)},
						{"beforeTimestamp", before}};
					if (!result.isEmpty()) {
						frame.insert(
							"beforeMessageId", _rawMessages.value(MessageId(result.back())));
					}
					_historyRequests[id] = peer;
					_auth->send(frame);
				}
				continue;
			}
			result = result.mid(start, limit);
			auto users = QVector<MTPUser>();
			auto chats = QVector<MTPChat>();
			for (const auto &user : _users) {
				users.push_back(user);
			}
			for (const auto &chat : _chats) {
				chats.push_back(chat);
			}
			finish();
			reply(id,
				MTPmessages_Messages(
					MTP_messages_messagesSlice(MTP_flags(MTPDmessages_messagesSlice::Flags()),
						MTP_int(_messages.value(peer.value).size()
							+ (_historyExhausted.contains(peer.value) ? 0 : 1)),
						MTPint(), MTPint(), MTPSearchPostsFlood(), MTP_vector<MTPMessage>(result),
						MTP_vector<MTPForumTopic>(QVector<MTPForumTopic>()),
						MTP_vector<MTPChat>(chats), MTP_vector<MTPUser>(users))));
		} else if (type == mtpc_messages_sendMessage) {
			if (!_auth->authenticated() || !_historyReady) {
				continue;
			}
			const auto request = ReadSend(body);
			if (!request) {
				finish();
				fail(id, "NOVEO_BAD_REQUEST");
				continue;
			}
			if (std::any_of(_sends.cbegin(), _sends.cend(),
					[=](const auto &send) { return send.requestId == id; })) {
				continue;
			}
			if (request->flags.v
				& (MTPmessages_SendMessage::Flag::f_schedule_date
					| MTPmessages_SendMessage::Flag::f_send_as
					| MTPmessages_SendMessage::Flag::f_quick_reply_shortcut
					| MTPmessages_SendMessage::Flag::f_suggested_post
					| MTPmessages_SendMessage::Flag::f_rich_message)) {
				finish();
				fail(id, "NOVEO_METHOD_UNSUPPORTED");
				continue;
			}
			const auto peer = inputPeer(request->peer);
			const auto raw = _chatIds.value(peer.value);
			const auto recipient = _rawUsers.value(peerToUser(peer).bare);
			if (raw.isEmpty() && recipient.isEmpty()) {
				finish();
				fail(id, "NOVEO_PEER_UNKNOWN");
				continue;
			}
			const auto temp = QUuid::createUuid().toString(QUuid::WithoutBraces);
			auto frame = QJsonObject{{"type", "message"}, {"clientTempId", temp},
				{"content", QJsonObject{{"text", qs(request->text)}}}};
			if (!raw.isEmpty()) {
				frame.insert("chatId", raw);
			} else {
				frame.insert("recipientId", recipient);
			}
			if (request->flags.v & MTPmessages_SendMessage::Flag::f_reply_to) {
				const auto reply = &request->reply;
				if (reply->type() != mtpc_inputReplyToMessage) {
					finish();
					fail(id, "NOVEO_REPLY_UNSUPPORTED");
					continue;
				}
				const auto uuid
					= _rawMessages.value(reply->c_inputReplyToMessage().vreply_to_msg_id().v);
				if (uuid.isEmpty()) {
					finish();
					fail(id, "NOVEO_REPLY_UNKNOWN");
					continue;
				}
				frame.insert("replyToId", uuid);
			}
			_sends[temp] = {id, uint64(request->random.v), peer};
			_auth->send(frame);
		} else if (type == mtpc_users_getFullUser) {
			if (!_auth->authenticated()) {
				continue;
			}
			auto from = body.constData() + 1;
			auto input = MTPInputUser();
			if (!input.read(from, body.constData() + body.size())) {
				finish();
				fail(id, "NOVEO_BAD_REQUEST");
				continue;
			}
			const auto uid
				= input.match([&](const MTPDinputUserSelf &) { return NativeUserId(_self).bare; },
					[](const MTPDinputUser &u) { return uint64(u.vuser_id().v); },
					[](const auto &) { return uint64(0); });
			if (!_users.contains(uid)) {
				finish();
				fail(id, "NOVEO_PEER_UNKNOWN");
				continue;
			}
			const auto full = MTPUserFull(MTP_userFull(MTP_flags(MTPDuserFull::Flag::f_about),
				MTP_long(uid), MTP_string(Text(_profiles.value(uid).value("bio"))),
				MTP_peerSettings(MTP_flags(MTPDpeerSettings::Flags()), MTPint(), MTPstring(),
					MTPint(), MTPlong(), MTPstring(), MTPlong(), MTPstring(), MTPstring(), MTPint(),
					MTPint()),
				MTPPhoto(), MTPPhoto(), MTPPhoto(), Notify(), MTPBotInfo(), MTPint(), MTPint(),
				MTPint(), MTPint(), MTPChatTheme(), MTPstring(), MTPChatAdminRights(),
				MTPChatAdminRights(), MTPWallPaper(), MTPPeerStories(), MTPBusinessWorkHours(),
				MTPBusinessLocation(), MTPBusinessGreetingMessage(), MTPBusinessAwayMessage(),
				MTPBusinessIntro(), MTPBirthday(), MTPlong(), MTPint(), MTPint(),
				MTPStarRefProgram(), MTPBotVerification(), MTPlong(), MTPDisallowedGiftsSettings(),
				MTPStarsRating(), MTPStarsRating(), MTPint(), MTPProfileTab(), MTPDocument(),
				MTPTextWithEntities(), MTPlong()));
			finish();
			reply(id,
				MTPusers_UserFull(MTP_users_userFull(full, MTP_vector<MTPChat>(QVector<MTPChat>()),
					MTP_vector<MTPUser>(QVector<MTPUser>{_users.value(uid)}))));
		} else if (type == mtpc_messages_getPeerSettings) {
			finish();
			reply(id,
				MTPmessages_PeerSettings(MTP_messages_peerSettings(
					MTP_peerSettings(MTP_flags(MTPDpeerSettings::Flags()), MTPint(), MTPstring(),
						MTPint(), MTPlong(), MTPstring(), MTPlong(), MTPstring(), MTPstring(),
						MTPint(), MTPint()),
					MTP_vector<MTPChat>(QVector<MTPChat>()),
					MTP_vector<MTPUser>(QVector<MTPUser>()))));
		} else if (type == mtpc_messages_readHistory || type == mtpc_channels_readHistory) {
			if (!_auth->authenticated()) {
				continue;
			}
			auto from = body.constData() + 1;
			const auto end = body.constData() + body.size();
			auto peer = PeerId();
			auto valid = false;
			if (type == mtpc_channels_readHistory) {
				auto input = MTPInputChannel();
				valid = input.read(from, end);
				if (valid && input.type() == mtpc_inputChannel) {
					peer = peerFromChannel(input.c_inputChannel().vchannel_id().v);
				}
			} else {
				auto input = MTPInputPeer();
				valid = input.read(from, end);
				if (valid) {
					peer = inputPeer(input);
				}
			}
			auto maxId = MTPint();
			valid = valid && maxId.read(from, end);
			if (!valid) {
				finish();
				fail(id, "NOVEO_BAD_REQUEST");
				continue;
			}
			const auto raw = _rawMessages.value(maxId.v);
			const auto chat = _chatIds.value(peer.value);
			if (!raw.isEmpty() && !chat.isEmpty()) {
				_auth->send({{"type", "message_seen"}, {"chatId", chat}, {"messageId", raw}});
			}
			finish();
			if (type == mtpc_channels_readHistory) {
				reply(id, MTPBool(MTP_boolTrue()));
			} else {
				reply(id,
					MTPmessages_AffectedMessages(
						MTP_messages_affectedMessages(MTP_int(_pts), MTP_int(0))));
			}
		} else if (type == mtpc_users_getUsers) {
			if (!_auth->authenticated()) {
				continue;
			}
			auto from = body.constData() + 1;
			auto inputs = MTPVector<MTPInputUser>();
			const auto valid = inputs.read(from, body.constData() + body.size());
			auto result = QVector<MTPUser>();
			if (valid) {
				for (const auto &input : inputs.v) {
					const auto uid = input.match(
						[&](const MTPDinputUserSelf &) { return NativeUserId(_self).bare; },
						[](const MTPDinputUser &u) { return uint64(u.vuser_id().v); },
						[](const auto &) { return uint64(0); });
					if (_users.contains(uid)) {
						result.push_back(_users.value(uid));
					}
				}
			}
			finish();
			reply(id, MTP_vector<MTPUser>(result));
		} else {
			finish();
			fail(id, "NOVEO_METHOD_UNSUPPORTED");
		}
	}
}

} // namespace Noveo
