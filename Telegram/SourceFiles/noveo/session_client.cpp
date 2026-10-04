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
			_pending.remove(id);
			_historyRequests.remove(id);
			_historyAnswered.remove(id);
			fail(id, "NOVEO_REQUEST_TIMEOUT");
		}
		if (_pending.isEmpty()) {
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
	_sends.clear();
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
	_contacts.clear();
	_self.clear();
	_historyReady = _contactsReady = false;
}

void SessionClient::fail(mtpRequestId id, const QString &reason) {
	reply(id, MTPRpcError(MTP_rpc_error(MTP_int(400), MTP_string(reason))));
}

void SessionClient::cancel(mtpRequestId id) {
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
		_avatars.remove(peerFromUser(id).value);
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
		const auto older = !known.isEmpty() && Date(object) < known.front().c_message().vdate().v;
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
	const auto replyRaw = Text(object.value("replyToId"));
	const auto replyId = _messageIds.value(replyRaw);
	if (replyId) {
		flags |= MTPDmessage::Flag::f_reply_to;
	}
	const auto mediaUnsupported = content.isObject()
		&& (content.toObject().contains("file") || content.toObject().contains("poll"));
	if (mediaUnsupported) {
		flags |= MTPDmessage::Flag::f_media;
	}
	return MTP_message(MTP_flags(flags), MTP_int(id),
		MTPPeer(MTP_peerUser(MTP_long(senderId.bare))), MTPint(), MTPstring(),
		MTPPeer(peerToMTP(peer)), MTPPeer(), MTPMessageFwdHeader(), MTPlong(), MTPlong(), MTPPeer(),
		MTP_messageReplyHeader(MTP_flags(MTPDmessageReplyHeader::Flag::f_reply_to_msg_id),
			MTP_int(replyId), MTPPeer(), MTPMessageFwdHeader(), MTPMessageMedia(), MTPint(),
			MTPstring(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPbytes()),
		MTP_int(Date(object)), MTP_string(text),
		mediaUnsupported ? MTPMessageMedia(MTP_messageMediaUnsupported()) : MTPMessageMedia(),
		MTPReplyMarkup(), MTPVector<MTPMessageEntity>(), MTPint(), MTPint(), MTPMessageReplies(),
		MTPint(), MTPstring(), MTPlong(), MTPMessageReactions(), MTPVector<MTPRestrictionReason>(),
		MTPint(), MTPint(), MTPlong(), MTPFactCheck(), MTPint(), MTPlong(), MTPSuggestedPost(),
		MTPint(), MTPstring(), MTPRichMessage());
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
		for (const auto &row : rows) {
			const auto native = nativeMessage(row, peer);
			if (native.type() != mtpc_message) {
				continue;
			}
			const auto id = native.c_message().vid().v;
			const auto existing = std::find_if(messages.begin(), messages.end(),
				[=](const auto &m) { return m.c_message().vid().v == id; });
			if (existing == messages.end()) {
				messages.push_back(native);
			} else {
				*existing = native;
			}
		}
		std::sort(messages.begin(), messages.end(), [](const auto &a, const auto &b) {
			return a.c_message().vid().v < b.c_message().vid().v;
		});
		_messages[peer.value] = messages;
		const auto top = messages.isEmpty() ? 0 : messages.back().c_message().vid().v;
		const auto unread = std::max(0, object.value("unreadCount").toInt());
		_dialogs[peer.value] = MTP_dialog(MTP_flags(MTPDdialog::Flags()), MTPPeer(peerToMTP(peer)),
			MTP_int(top), MTP_int(unread ? 0 : top), MTP_int(0), MTP_int(unread), MTPint(),
			MTPint(), MTPint(), Notify(), MTPint(), MTPDraftMessage(), MTPint(), MTPint());
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
	if (type == "user_list_update") {
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
			if (native.type() != mtpc_message) {
				continue;
			}
			const auto exists = std::any_of(messages.begin(), messages.end(),
				[&](const auto &m) { return m.c_message().vid().v == native.c_message().vid().v; });
			if (!exists) {
				messages.push_back(native);
			}
		}
		std::sort(messages.begin(), messages.end(), [](const auto &a, const auto &b) {
			return a.c_message().vid().v < b.c_message().vid().v;
		});
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
		if (!peer) {
			_auth->send({{"type", "resync_state"}});
			return;
		}
		const auto native = nativeMessage(object, peer);
		if (native.type() != mtpc_message) {
			return;
		}
		const auto &data = native.c_message();
		auto &messages = _messages[peer.value];
		const auto exists = std::any_of(messages.begin(), messages.end(),
			[&](const auto &m) { return m.c_message().vid().v == data.vid().v; });
		if (!exists) {
			messages.push_back(native);
		}
		if (_sends.contains(tempId)) {
			const auto send = _sends.take(tempId);
			_pending.remove(send.requestId);
			reply(send.requestId,
				MTPUpdates(
					MTP_updateShortSentMessage(MTP_flags(MTPDupdateShortSentMessage::Flag::f_out),
						data.vid(), MTP_int(++_pts), MTP_int(1), data.vdate(), MTPMessageMedia(),
						MTPVector<MTPMessageEntity>(), MTPint())));
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
				MTP_vector<MTPUser>(users), MTP_vector<MTPChat>(QVector<MTPChat>()), data.vdate(),
				MTP_int(0)));
		}
		_auth->send({{"type", "resync_state"}});
	} else if (type == "error") {
		const auto tempId = Text(frame.value("clientTempId"));
		if (_sends.contains(tempId)) {
			const auto send = _sends.take(tempId);
			_pending.remove(send.requestId);
			fail(send.requestId, "NOVEO_SEND_FAILED");
		}
	}
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
				while (start < result.size() && result[start].c_message().vid().v >= offset) {
					++start;
				}
				start = std::clamp(start + request->addOffset.v, 0, int(result.size()));
			}
			if (offset > 0 && start + limit > result.size()
				&& !_historyExhausted.contains(peer.value) && !_historyAnswered.contains(id)
				&& !_chatIds.value(peer.value).isEmpty()) {
				if (!_historyRequests.contains(id) && _auth->authenticated()) {
					const auto before
						= result.isEmpty() ? now + 3600 : result.back().c_message().vdate().v;
					auto frame = QJsonObject{{"type", "load_older_messages"},
						{"chatId", _chatIds.value(peer.value)}, {"requestId", QString::number(id)},
						{"beforeTimestamp", before}};
					if (!result.isEmpty()) {
						frame.insert("beforeMessageId",
							_rawMessages.value(result.back().c_message().vid().v));
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
