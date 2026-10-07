#include "noveo/session_client.h"
#include "noveo/request_fields.h"

#include <QtCore/QDateTime>
#include <QtCore/QJsonDocument>
#include <QtCore/QUuid>
#include <QtNetwork/QHttpMultiPart>
#include <algorithm>

namespace Noveo {
namespace {
QString String(const MTPstring &value) { return QString::fromUtf8(value.v); }
MTPTextWithEntities Rich(QString text) {
	return MTP_textWithEntities(MTP_string(text), MTPVector<MTPMessageEntity>());
}
QString RawMember(QJsonValue member) {
	if (member.isString()) return member.toString();
	const auto object = member.toObject();
	return object.value("userId").toString(object.value("id").toString());
}
} // namespace

bool SessionClient::blocksGroupInvites() const {
	return _profiles.value(NativeUserId(_self).bare).value("blockGroupInvites").toBool();
}
void SessionClient::setBlockGroupInvites(bool block, std::function<void(QString)> done) {
	const auto id = --_internalRequest;
	api(id, "/user/privacy", {{"blockGroupInvites", block}}, [=, this](QJsonObject) {
		auto profile = _profiles.value(NativeUserId(_self).bare);
		profile.insert("blockGroupInvites", block); users(QJsonArray{profile}); done({});
	}, true, done);
}
void SessionClient::changePassword(QString current, QString password, std::function<void(QString)> done) {
	if (!_auth->authenticated()) { done("NOVEO_DISCONNECTED"); return; }
	const auto id = --_internalRequest;
	_actionErrors[id] = done;
	_deadline.start();
	_pending[id] = {{mtpc_users_getUsers}, QDateTime::currentMSecsSinceEpoch() + 30000};
	wire(id, {{{"type", "change_password"}, {"oldPassword", std::move(current)}, {"newPassword", std::move(password)}}}, {"password_changed"}, [=, this](QJsonObject frame) {
		_actionErrors.remove(id);
		if (!frame.value("success").toBool() || !_auth->replaceToken(frame.value("token").toString(), frame.value("sessionId").toString())) {
			done("NOVEO_PASSWORD_CHANGE_FAILED"); return;
		}
		done({});
	});
}

bool SessionClient::canManageMemberPermissions(PeerId peer, PeerId member) const {
	const auto profile = _chatProfiles.value(peer.value);
	const auto raw = _rawUsers.value(member.value);
	return peerIsChat(peer) && !raw.isEmpty() && profile.value("canManageSettings").toBool(profile.value("ownerId").toString() == _self || profile.value("adminIds").toArray().contains(_self))
		&& raw != profile.value("ownerId").toString() && !profile.value("adminIds").toArray().contains(raw);
}
void SessionClient::chatPermissions(PeerId peer, std::function<void(QJsonObject, QString)> done) {
	const auto chat = _chatIds.value(peer.value);
	if (chat.isEmpty()) { done({}, "NOVEO_PEER_UNKNOWN"); return; }
	api(--_internalRequest, "/chat/settings", {{"action", "get_profile"}, {"chatId", chat}}, [=, this](QJsonObject response) {
		const auto profile = response.value("profile").toObject();
		_chats[peer.value] = nativeChat(profile);
		done(profile.value("permissions").toObject(), {});
	}, true, [done](QString error) { done({}, error); });
}
void SessionClient::setChatPermissions(PeerId peer, QJsonObject permissions, std::function<void(QString)> done) {
	permissions.insert("chatId", _chatIds.value(peer.value)); permissions.insert("action", "set_permissions");
	api(--_internalRequest, "/chat/settings", permissions, [=, this](QJsonObject) {
		_chatProfiles[peer.value].remove("canManageSettings"); _chatProfiles[peer.value].remove("selfPermissions");
		_auth->send({{"type", "resync_state"}}); done({});
	}, true, done);
}

void SessionClient::memberPermissions(PeerId peer, PeerId member, std::function<void(QJsonObject, QString)> done) {
	const auto raw = _rawUsers.value(member.value);
	const auto chat = _chatIds.value(peer.value);
	if (!peerIsChat(peer) || raw.isEmpty() || chat.isEmpty()) { done({}, "NOVEO_PEER_UNKNOWN"); return; }
	api(--_internalRequest, "/chat/member_permissions?chatId=" + QString::fromLatin1(QUrl::toPercentEncoding(chat)) + "&memberId=" + QString::fromLatin1(QUrl::toPercentEncoding(raw)), {},
		[done](QJsonObject response) { done(response, {}); }, false, [done](QString error) { done({}, error); });
}
void SessionClient::setMemberPermissions(PeerId peer, PeerId member, QJsonObject permissions, std::function<void(QString)> done) {
	if (!canManageMemberPermissions(peer, member)) { done("CHAT_ADMIN_REQUIRED"); return; }
	permissions.insert("chatId", _chatIds.value(peer.value)); permissions.insert("memberId", _rawUsers.value(member.value));
	api(--_internalRequest, "/chat/member_permissions", permissions, [=, this](QJsonObject) {
		_auth->send({{"type", "resync_state"}}); done({});
	}, true, done);
}

bool SessionClient::needsJoin(PeerId peer) const {
	const auto profile = _chatProfiles.value(peer.value);
	return profile.value("previewOnly").toBool() || (profile.contains("isMember") && !profile.value("isMember").toBool());
}
void SessionClient::joinChat(PeerId peer, std::function<void(QString)> done) {
	const auto raw = _chatIds.value(peer.value);
	if (raw.isEmpty() || !_auth->authenticated()) { done("NOVEO_PEER_UNKNOWN"); return; }
	const auto id = --_internalRequest; _actionErrors[id] = done;
	_pending[id] = {{mtpc_users_getUsers}, QDateTime::currentMSecsSinceEpoch() + 30000}; _deadline.start();
	wire(id, {{{"type", "join_channel"}, {"chatId", raw}}}, {"chat_joined"}, [=, this](QJsonObject frame) {
		_actionErrors.remove(id);
		auto profile = frame.value("chat").toObject();
		if (profile.isEmpty()) profile = _chatProfiles.value(peer.value);
		profile.insert("previewOnly", false); profile.insert("isMember", true);
		_chats[peer.value] = nativeChat(profile);
		if (onUpdate) onUpdate(updates({}));
		_auth->send({{"type", "resync_state"}}); done({});
	});
}

void SessionClient::wire(mtpRequestId id, QVector<QJsonObject> frames, QStringList replies,
	std::function<void(QJsonObject)> done) {
	if (frames.isEmpty()) { _pending.remove(id); fail(id, "NOVEO_BAD_REQUEST"); return; }
	_wireRequests.push_back({id, std::move(frames), std::move(replies), std::move(done)});
	wireNext();
}
void SessionClient::wireNext() {
	if (_wireRequests.isEmpty() || _wireRequests.front().sent) return;
	auto &request = _wireRequests.front();
	request.sent = true;
	if (!_auth->send(request.frames.front())) {
		const auto id = request.id;
		_wireRequests.removeFirst();
		_pending.remove(id);
		fail(id, "NOVEO_DISCONNECTED");
		wireNext();
	}
}
void SessionClient::wireEvent(const QJsonObject &frame) {
	if (_wireRequests.isEmpty() || !_wireRequests.front().sent) return;
	const auto type = frame.value("type").toString();
	const auto &front = _wireRequests.front();
	const auto &sent = front.frames.front();
	if (type == "chat_joined" && sent.contains("chatId") && frame.value("chat").isObject()
		&& frame.value("chat").toObject().value("chatId") != sent.value("chatId")) return;
	const auto requestId = frame.value("requestId").toString();
	if (!requestId.isEmpty() && requestId != QString::number(front.id)) return;
	if (!frame.value("chatId").toString().isEmpty() && sent.contains("chatId")
		&& sent.value("chatId") != frame.value("chatId")) return;
	if (!frame.value("messageId").toString().isEmpty() && sent.contains("messageId")
		&& sent.value("messageId") != frame.value("messageId")) return;
	if (type == "error") {
		// Correlated send/reaction errors belong to their own request path.
		if (frame.contains("clientTempId")) return;
		if (frame.contains("operation") && frame.value("operation") != sent.value("type")) return;
		const auto id = front.id;
		_wireRequests.removeFirst(); _pending.remove(id);
		fail(id, "NOVEO_ACTION_FAILED: " + frame.value("message").toString());
		wireNext();
		return;
	}
	if (!front.replies.contains(type)) return;
	if (type == "user_updated" && sent.value("type") == "update_profile") {
		const auto user = frame.value("user").toObject();
		const auto raw = frame.value("userId").toString(user.value("userId").toString(user.value("id").toString()));
		if (raw != _self) return;
		const auto changed = user.isEmpty() ? frame : user;
		for (const auto field : {"username", "bio", "handle"}) {
			if (sent.contains(field) && changed.value(field) != sent.value(field)) return;
		}
	}
	_wireRequests.front().frames.removeFirst();
	if (!_wireRequests.front().frames.isEmpty()) {
		_wireRequests.front().sent = false; wireNext(); return;
	}
	auto finished = _wireRequests.takeFirst();
	_pending.remove(finished.id);
	finished.done(frame);
	wireNext();
}
void SessionClient::removeMessage(PeerId peer, int id) {
	if (!peer || !id) return;
	auto &messages = _messages[peer.value];
	messages.erase(std::remove_if(messages.begin(), messages.end(), [=](const MTPMessage &m) {
		return m.type() == mtpc_message ? m.c_message().vid().v == id
			: m.type() == mtpc_messageService && m.c_messageService().vid().v == id;
	}), messages.end());
	_messageContent.remove(id); _messageObjects.remove(id);
	// Retain UUID mapping: a late reply/context request must not reuse another ID.
	refreshDialog(peer);
	const auto ids = MTP_vector<MTPint>(QVector<MTPint>{MTP_int(id)});
	if (onUpdate) onUpdate(updates({peerIsChannel(peer)
		? MTPUpdate(MTP_updateDeleteChannelMessages(MTP_long(peerToChannel(peer).bare), ids, MTP_int(++_pts), MTP_int(1)))
		: MTPUpdate(MTP_updateDeleteMessages(ids, MTP_int(++_pts), MTP_int(1)))}));
	if (onDialogs) onDialogs(dialogs());
}
void SessionClient::cacheMessages(PeerId peer, const QJsonArray &objects) {
	if (!peer) return;
	// Convert newest first to allocate unloaded older messages below the
	// history boundary, then repeat after every reply UUID has a mapping.
	for (auto i = objects.size(); i > 0; --i) {
		(void)nativeMessage(objects.at(i - 1).toObject(), peer);
	}
	for (const auto value : objects) {
		const auto native = nativeMessage(value.toObject(), peer);
		if (native.type() != mtpc_message && native.type() != mtpc_messageService) continue;
		const auto id = native.type() == mtpc_message ? native.c_message().vid().v : native.c_messageService().vid().v;
		auto &messages = _messages[peer.value];
		const auto i = std::find_if(messages.begin(), messages.end(), [=](const MTPMessage &m) {
			return m.type() == mtpc_message ? m.c_message().vid().v == id : m.type() == mtpc_messageService && m.c_messageService().vid().v == id;
		});
		if (i == messages.end()) messages.push_back(native); else *i = native;
	}
	// Repair references in cached children after a context/search page loads parents.
	for (auto &native : _messages[peer.value]) {
		const auto id = native.type() == mtpc_message ? native.c_message().vid().v : native.type() == mtpc_messageService ? native.c_messageService().vid().v : 0;
		if (_messageObjects.contains(id)) {
			native = nativeMessage(_messageObjects.value(id), peer);
		}
	}
}
MTPmessages_Messages SessionClient::messagePage(PeerId, QVector<MTPMessage> messages, int count) const {
	if (count < 0) return MTP_messages_messages(MTP_vector<MTPMessage>(messages), MTP_vector<MTPForumTopic>({}),
		MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()));
	return MTP_messages_messagesSlice(MTP_flags(MTPDmessages_messagesSlice::Flags()), MTP_int(count), MTPint(), MTPint(), MTPSearchPostsFlood(),
		MTP_vector<MTPMessage>(messages), MTP_vector<MTPForumTopic>({}), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()));
}

MTPMessageMedia SessionClient::pollMedia(const QJsonObject &source, const QString &uuid) const {
	const auto options = source.value("options").toArray();
	if (options.size() < 2) return MTP_messageMediaEmpty();
	auto flags = MTPDpoll::Flags();
	if (source.value("isExpired").toBool()) flags |= MTPDpoll::Flag::f_closed;
	if (!source.value("anonymous").toBool(true)) flags |= MTPDpoll::Flag::f_public_voters;
	if (source.value("examMode").toBool()) flags |= MTPDpoll::Flag::f_quiz;
	if (source.value("multipleChoice").toBool()) flags |= MTPDpoll::Flag::f_multiple_choice;
	if (source.value("revotingDisabled").toBool()) flags |= MTPDpoll::Flag::f_revoting_disabled;
	if (source.value("shuffleAnswers").toBool()) flags |= MTPDpoll::Flag::f_shuffle_answers;
	if (source.value("resultsAfterVote").toBool()) flags |= MTPDpoll::Flag::f_hide_results_until_close;
	if (source.value("isAuthor").toBool()) flags |= MTPDpoll::Flag::f_creator;
	if (source.value("shuffleAnswers").toBool()) flags |= MTPDpoll::Flag::f_shuffle_answers;
	if (source.value("resultsAfterVote").toBool()) flags |= MTPDpoll::Flag::f_hide_results_until_close;
	const auto expires = source.value("expiresAt").toInt();
	if (expires) flags |= MTPDpoll::Flag::f_close_date;
	const auto votes = source.value("votes").toObject();
	auto chosen = votes.value(_self).toArray();
	if (votes.value(_self).isString()) chosen = QJsonArray{votes.value(_self)};
	if (source.value("viewerChoiceIds").isArray()) chosen = source.value("viewerChoiceIds").toArray();
	else if (source.value("viewerChoiceId").isString()) chosen = QJsonArray{source.value("viewerChoiceId")};
	auto answers = QVector<MTPPollAnswer>();
	auto results = QVector<MTPPollAnswerVoters>();
	const auto correct = source.value("correctOptionIds").toArray();
	for (const auto value : options) {
		const auto option = value.toObject(); const auto key = option.value("id").toString();
		const auto bytes = key.toUtf8();
		answers.push_back(MTP_pollAnswer(MTP_flags(MTPDpollAnswer::Flags()), Rich(option.value("text").toString()), MTP_bytes(bytes), MTPMessageMedia(), MTPPeer(), MTPint()));
		auto resultFlags = MTPDpollAnswerVoters::Flags(MTPDpollAnswerVoters::Flag::f_voters);
		if (chosen.contains(key)) resultFlags |= MTPDpollAnswerVoters::Flag::f_chosen;
		if (option.value("isCorrect").toBool() || correct.contains(key) || source.value("correctOptionId").toString() == key) resultFlags |= MTPDpollAnswerVoters::Flag::f_correct;
		auto count = 0;
		for (const auto v : votes) if ((v.isArray() && v.toArray().contains(key)) || v.toString() == key) ++count;
		if (option.contains("voteCount")) count = std::max(0, option.value("voteCount").toInt());
		if (!source.value("canSeeResults").toBool(true)) count = 0;
		results.push_back(MTP_pollAnswerVoters(MTP_flags(resultFlags), MTP_bytes(bytes), MTP_int(count), MTPVector<MTPPeer>()));
	}
	const auto pollId = NativeUserId("noveo-poll:" + uuid).bare;
	auto resultFlags = MTPDpollResults::Flag::f_results | MTPDpollResults::Flag::f_total_voters;
	if (source.value("canViewVotes").toBool()) resultFlags |= MTPDpollResults::Flag::f_can_view_stats;
	const auto solution = source.value("solution").toString();
	if (!solution.isEmpty()) resultFlags |= MTPDpollResults::Flag::f_solution;
	const auto total = source.value("canSeeResults").toBool(true) ? source.value("totalVotes").toInt(votes.size()) : 0;
	return MTP_messageMediaPoll(MTP_flags(MTPDmessageMediaPoll::Flags()),
		MTP_poll(MTP_long(pollId), MTP_flags(flags), Rich(source.value("question").toString()), MTP_vector<MTPPollAnswer>(answers), MTPint(), MTP_int(expires), MTPVector<MTPstring>(), MTP_long(pollId)),
		MTP_pollResults(MTP_flags(resultFlags), MTP_vector<MTPPollAnswerVoters>(results), MTP_int(total), MTPVector<MTPPeer>(), MTP_string(solution), MTPVector<MTPMessageEntity>(), MTPMessageMedia()), MTPMessageMedia());
}

MTPChat SessionClient::nativeChat(const QJsonObject &incoming) {
	auto profile = incoming;
	const auto raw = profile.value("chatId").toString(profile.value("id").toString());
	const auto id = NativeUserId(raw).bare;
	const auto channel = profile.value("chatType").toString() == "channel";
	const auto peer = channel ? peerFromChannel(ChannelId(id)) : peerFromChat(ChatId(id));
	auto merged = _chatProfiles.value(peer.value);
	for (auto i = profile.begin(); i != profile.end(); ++i) merged.insert(i.key(), i.value());
	profile = merged;
	_chatIds[peer.value] = raw; _chatPeers[raw] = peer; _chatProfiles[peer.value] = profile;
	const auto owner = profile.value("ownerId").toString() == _self;
	if (!profile.contains("canManageSettings") && !_chatProfilesLoading.contains(raw)) {
		_chatProfilesLoading.insert(raw);
		api(--_internalRequest, "/chat/settings", {{"action", "get_profile"}, {"chatId", raw}}, [=, this](QJsonObject response) {
			_chatProfilesLoading.remove(raw);
			auto details = response.value("profile").toObject();
			if (details.value("chatId").toString() != raw) return;
			if (!details.contains("canManageSettings")) details.insert("canManageSettings", details.value("ownerId").toString() == _self || (!channel && details.value("adminIds").toArray().contains(_self)));
			_chats[peer.value] = nativeChat(details);
			if (onUpdate) onUpdate(updates({}));
		}, true, [=, this](QString) { _chatProfilesLoading.remove(raw); });
	}
	const auto admin = owner || profile.value("adminIds").toArray().contains(_self);
	if (!channel && !admin && !profile.contains("selfPermissions") && !_memberPermissionsLoading.contains(raw)) {
		_memberPermissionsLoading.insert(raw);
		memberPermissions(peer, peerFromUser(NativeUserId(_self)), [=, this](QJsonObject response, QString error) {
			_memberPermissionsLoading.remove(raw);
			if (!error.isEmpty() || !_chatProfiles.contains(peer.value)) return;
			auto latest = _chatProfiles.value(peer.value);
			latest.insert("selfPermissions", response.value("effectivePermissions"));
			const auto role = response.value("role").toString();
			if (role == "owner") latest.insert("ownerId", _self);
			if (role == "admin") { auto admins = latest.value("adminIds").toArray(); if (!admins.contains(_self)) admins.push_back(_self); latest.insert("adminIds", admins); }
			if (role == "owner" || role == "admin") latest.insert("canManageSettings", true);
			_chats[peer.value] = nativeChat(latest);
			if (onUpdate) onUpdate(updates({}));
		});
	}
	const auto manage = owner || (admin && profile.value("canManageSettings").toBool(true));
	auto adminFlags = MTPDchatAdminRights::Flags();
	if (admin) adminFlags |= MTPDchatAdminRights::Flag::f_post_messages | MTPDchatAdminRights::Flag::f_edit_messages | MTPDchatAdminRights::Flag::f_delete_messages;
	if (manage) adminFlags |= MTPDchatAdminRights::Flag::f_change_info | MTPDchatAdminRights::Flag::f_invite_users | MTPDchatAdminRights::Flag::f_pin_messages | MTPDchatAdminRights::Flag::f_ban_users | MTPDchatAdminRights::Flag::f_add_admins | MTPDchatAdminRights::Flag::f_manage_call;
	const auto rights = MTP_chatAdminRights(MTP_flags(adminFlags));
	auto banned = MTPDchatBannedRights::Flags();
	const auto permissions = !admin && profile.value("selfPermissions").isObject() ? profile.value("selfPermissions").toObject() : profile.value("permissions").toObject();
	if (!permissions.value("canSendMessages").toBool(true)) banned |= MTPDchatBannedRights::Flag::f_send_plain | MTPDchatBannedRights::Flag::f_send_messages;
	if (!permissions.value("canSendFiles").toBool(true)) banned |= MTPDchatBannedRights::Flag::f_send_media;
	if (!permissions.value("canAddMembers").toBool(true)) banned |= MTPDchatBannedRights::Flag::f_invite_users;
	const auto defaultRights = MTP_chatBannedRights(MTP_flags(banned), MTP_int(0));
	const auto count = profile.value("memberCount").toInt(profile.value("members").toArray().size());
	const auto title = MTP_string(profile.value("chatName").toString());
	if (channel) {
		auto flags = MTPDchannel::Flags(MTPDchannel::Flag::f_broadcast | MTPDchannel::Flag::f_access_hash | MTPDchannel::Flag::f_default_banned_rights | MTPDchannel::Flag::f_participants_count);
		if (owner) flags |= MTPDchannel::Flag::f_creator;
		if (admin) flags |= MTPDchannel::Flag::f_admin_rights;
		if (profile.value("previewOnly").toBool() || (profile.contains("isMember") && !profile.value("isMember").toBool())) flags |= MTPDchannel::Flag::f_left;
		if (profile.value("isVerified").toBool()) flags |= MTPDchannel::Flag::f_verified;
		const auto handle = profile.value("handle").toString().remove('@');
		if (!handle.isEmpty()) flags |= MTPDchannel::Flag::f_username;
		return MTP_channel(
		MTP_flags(flags),
		MTP_long(id),
		MTP_long(1),
		title,
		MTP_string(handle),
		MTP_chatPhotoEmpty(),
		MTPint(),
		MTPVector<MTPRestrictionReason>(),
		rights,
		MTPChatBannedRights(),
		defaultRights,
		MTP_int(count),
		MTPVector<MTPUsername>(),
		MTPRecentStory(),
		MTPPeerColor(),
		MTPPeerColor(),
		MTPEmojiStatus(),
		MTPint(),
		MTPint(),
		MTPlong(),
		MTPlong(),
		MTPlong(),
		MTPlong());
	}
	auto flags = MTPDchat::Flags(MTPDchat::Flag::f_default_banned_rights);
	if (profile.value("previewOnly").toBool() || (profile.contains("isMember") && !profile.value("isMember").toBool())) flags |= MTPDchat::Flag::f_left;
	if (owner) flags |= MTPDchat::Flag::f_creator;
	if (admin) flags |= MTPDchat::Flag::f_admin_rights;
	return MTP_chat(
		MTP_flags(flags),
		MTP_long(id),
		title,
		MTP_chatPhotoEmpty(),
		MTP_int(count),
		MTPint(),
		MTP_int(1),
		MTPInputChannel(),
		rights,
		defaultRights);
}

MTPChatFull SessionClient::fullChat(PeerId peer, const QJsonObject &profile) {
	const auto raw = _chatIds.value(peer.value);
	const auto pin = _pins.value(raw);
	if (!peerIsChannel(peer)) {
		auto participants = QVector<MTPChatParticipant>();
		for (const auto member : profile.value("members").toArray()) {
			const auto id = RawMember(member); if (id.isEmpty()) continue;
			const auto uid = MTP_long(NativeUserId(id).bare);
			participants.push_back(id == profile.value("ownerId").toString()
				? MTPChatParticipant(MTP_chatParticipantCreator(MTP_flags(MTPDchatParticipantCreator::Flags()), uid, MTPstring()))
				: profile.value("adminIds").toArray().contains(id)
				? MTPChatParticipant(MTP_chatParticipantAdmin(MTP_flags(MTPDchatParticipantAdmin::Flags()), uid, MTP_long(NativeUserId(profile.value("ownerId").toString()).bare), MTP_int(0), MTPstring()))
				: MTPChatParticipant(MTP_chatParticipant(MTP_flags(MTPDchatParticipant::Flags()), uid, MTP_long(0), MTP_int(0), MTPstring())));
		}
		auto flags = MTPDchatFull::Flags(MTPDchatFull::Flag::f_available_reactions);
		if (pin) flags |= MTPDchatFull::Flag::f_pinned_msg_id;
		return MTP_chatFull(
		MTP_flags(flags),
		MTP_long(peerToChat(peer).bare),
		MTP_string(profile.value("bio").toString()),
		MTP_chatParticipants(MTP_long(peerToChat(peer).bare), MTP_vector<MTPChatParticipant>(participants), MTP_int(1)),
		MTPPhoto(),
		notify(peer),
		MTPExportedChatInvite(),
		MTPVector<MTPBotInfo>(),
		MTP_int(pin),
		MTPint(),
		MTPInputGroupCall(),
		MTPint(),
		MTPPeer(),
		MTPstring(),
		MTPint(),
		MTPVector<MTPlong>(),
		MTP_chatReactionsAll(MTP_flags(MTPDchatReactionsAll::Flags())),
		MTPint());
	}
	auto flags = MTPDchannelFull::Flags(MTPDchannelFull::Flag::f_participants_count | MTPDchannelFull::Flag::f_available_reactions);
	if (profile.value("canViewMembers").toBool(true)) flags |= MTPDchannelFull::Flag::f_can_view_participants;
	if (pin) flags |= MTPDchannelFull::Flag::f_pinned_msg_id;
	return MTP_channelFull(
		MTP_flags(flags),
		MTP_long(peerToChannel(peer).bare),
		MTP_string(profile.value("bio").toString()),
		MTP_int(profile.value("memberCount").toInt(profile.value("members").toArray().size())),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTP_photoEmpty(MTP_long(0)),
		notify(peer),
		MTPExportedChatInvite(),
		MTPVector<MTPBotInfo>(),
		MTPlong(),
		MTPint(),
		MTP_int(pin),
		MTPStickerSet(),
		MTPint(),
		MTPint(),
		MTPlong(),
		MTPChannelLocation(),
		MTPint(),
		MTPint(),
		MTPint(),
		MTP_int(_pts),
		MTPInputGroupCall(),
		MTPint(),
		MTPVector<MTPstring>(),
		MTPPeer(),
		MTPstring(),
		MTPint(),
		MTPVector<MTPlong>(),
		MTPPeer(),
		MTP_chatReactionsAll(MTP_flags(MTPDchatReactionsAll::Flags())),
		MTPint(),
		MTPPeerStories(),
		MTPWallPaper(),
		MTPint(),
		MTPint(),
		MTPStickerSet(),
		MTPBotVerification(),
		MTPint(),
		MTPlong(),
		MTPProfileTab(),
		MTPlong());
}

void SessionClient::chatAction(mtpRequestId id, PeerId peer, QString action, QJsonObject extra, bool booleanReply) {
	const auto raw = _chatIds.value(peer.value);
	if (raw.isEmpty()) { _pending.remove(id); fail(id, "NOVEO_PEER_UNKNOWN"); return; }
	const auto submit = [=, this](QJsonObject fields) {
		fields.insert("chatId", raw); fields.insert("action", action);
		api(id, "/chat/settings", fields, [=, this](QJsonObject response) {
			_auth->send({{"type", "resync_state"}});
			if (booleanReply) reply(id, MTPBool(MTP_boolTrue()));
			else reply(id, updates({}));
		}, true);
	};
	if (action == "update_profile") {
		// The server requires both fields. Read the current profile to preserve
		// the description on rename, and the name on description changes.
		api(id, "/chat/settings", {{"action", "get_profile"}, {"chatId", raw}}, [=, this](QJsonObject response) {
			const auto profile = response.value("profile").toObject();
			auto fields = extra;
			if (!fields.contains("chatName")) fields.insert("chatName", profile.value("chatName"));
			if (!fields.contains("bio")) fields.insert("bio", profile.value("bio").toString());
			submit(fields);
		}, true, {}, true);
	} else {
		submit(extra);
	}
}

bool SessionClient::parityRequest(mtpRequestId id, const mtpBuffer &body) {
	for (const auto &request : _wireRequests) if (request.id == id) return true;
	if (_transfers.contains(id)) return true;
	const auto type = uint32(body.front());
	const auto bad = [&](QString error = "NOVEO_BAD_REQUEST") { _pending.remove(id); fail(id, error); return true; };
	const auto affected = [=, this](QJsonObject) { reply(id, MTPmessages_AffectedMessages(MTP_messages_affectedMessages(MTP_int(_pts), MTP_int(0)))); };
	const auto emptyUpdates = [=, this](QJsonObject) { reply(id, updates({})); };
	const auto createdUpdates = [=, this](const MTPChat &chat) {
		return MTPUpdates(MTP_updates(MTPVector<MTPUpdate>(), MTP_vector<MTPUser>(_users.values().toVector()), MTP_vector<MTPChat>({chat}), MTP_int(QDateTime::currentSecsSinceEpoch()), MTP_int(0)));
	};
	const auto channelPeer = [](const MTPInputChannel &c) { return c.type() == mtpc_inputChannel ? peerFromChannel(ChannelId(c.c_inputChannel().vchannel_id().v)) : PeerId(); };
	const auto userId = [=, this](const MTPInputUser &u) { return u.type() == mtpc_inputUserSelf ? NativeUserId(_self).bare : u.type() == mtpc_inputUser ? uint64(u.c_inputUser().vuser_id().v) : uint64(0); };
	switch (type) {
	case mtpc_messages_editMessage: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_editMessage>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		if (!(r.flags.v & (1U << 11)) || (r.flags.v & ((1U << 14) | (1U << 15) | (1U << 18) | (1U << 23)))) return bad("NOVEO_EDIT_UNSUPPORTED");
		const auto peer = inputPeer(r.peer); const auto chat = _chatIds.value(peer.value); const auto uuid = _rawMessages.value(r.id.v);
		if (chat.isEmpty() || uuid.isEmpty()) return bad("NOVEO_MESSAGE_UNKNOWN");
		wire(id, {{{"type", "edit_message"}, {"chatId", chat}, {"messageId", uuid}, {"newContent", String(r.message)}}}, {"message_updated", "message_edit"}, emptyUpdates);
		return true;
	}
	case mtpc_messages_getMessageEditData: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_getMessageEditData>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		if (!_messageObjects.contains(r.id.v)) return bad("NOVEO_MESSAGE_UNKNOWN");
		_pending.remove(id);
		reply(id, MTPmessages_MessageEditData(MTP_messages_messageEditData(MTP_flags(_messageContent.value(r.id.v).contains("file") ? MTPDmessages_messageEditData::Flag::f_caption : MTPDmessages_messageEditData::Flags()))));
		return true;
	}
	case mtpc_messages_deleteMessages: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_deleteMessages>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		auto frames = QVector<QJsonObject>();
		for (const auto mid : r.id.v) {
			const auto uuid = _rawMessages.value(mid.v); auto chat = QString();
			for (auto i = _messages.cbegin(); i != _messages.cend(); ++i) {
				for (const auto &m : i.value()) if ((m.type() == mtpc_message && m.c_message().vid().v == mid.v) || (m.type() == mtpc_messageService && m.c_messageService().vid().v == mid.v)) chat = _chatIds.value(i.key());
			}
			if (chat.isEmpty() || uuid.isEmpty()) return bad("NOVEO_MESSAGE_UNKNOWN");
			frames.push_back({{"type", "delete_message"}, {"chatId", chat}, {"messageId", uuid}, {"scope", (r.flags.v & 1) ? "everyone" : "me"}});
		}
		wire(id, std::move(frames), {"message_deleted", "message_deleted_local", "message_delete", "delete_message"}, affected);
		return true;
	}
	case mtpc_channels_deleteMessages: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_deleteMessages>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel);
		auto frames = QVector<QJsonObject>();
		for (const auto mid : r.id.v) {
			const auto uuid = _rawMessages.value(mid.v); auto chat = QString();
			chat = _chatIds.value(peer.value);
			if (chat.isEmpty() || uuid.isEmpty()) return bad("NOVEO_MESSAGE_UNKNOWN");
			frames.push_back({{"type", "delete_message"}, {"chatId", chat}, {"messageId", uuid}, {"scope", "everyone"}});
		}
		wire(id, std::move(frames), {"message_deleted", "message_deleted_local", "message_delete", "delete_message"}, affected);
		return true;
	}
	case mtpc_messages_sendVote: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_sendVote>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto chat = _chatIds.value(inputPeer(r.peer).value); const auto uuid = _rawMessages.value(r.msg_id.v);
		if (chat.isEmpty() || uuid.isEmpty()) return bad("NOVEO_MESSAGE_UNKNOWN");
		auto choices = QJsonArray();
		for (const auto &option : r.options.v) choices.push_back(QString::fromUtf8(option.v));
		wire(id, {{{"type", "vote_poll"}, {"chatId", chat}, {"messageId", uuid}, {"optionIds", choices}, {"retract", choices.isEmpty()}}}, {"message_updated"}, emptyUpdates);
		return true;
	}
	case mtpc_messages_getPollResults: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_messages_getPollResults>(body); if (!r) return bad();
		const auto peer = inputPeer(r->peer); const auto raw = _rawMessages.value(r->msg_id.v);
		if (raw.isEmpty() || !_chatIds.contains(peer.value)) return bad("NOVEO_MESSAGE_UNKNOWN");
		wire(id, {{{"type", "load_message_context"}, {"chatId", _chatIds.value(peer.value)}, {"messageId", raw}, {"requestId", QString::number(id)}}}, {"message_context"}, [=, this](QJsonObject) {
			const auto media = pollMedia(_messageContent.value(r->msg_id.v).value("poll").toObject(), raw);
			if (media.type() != mtpc_messageMediaPoll) { fail(id, "NOVEO_MESSAGE_UNKNOWN"); return; }
			const auto &poll = media.c_messageMediaPoll();
			reply(id, updates({MTP_updateMessagePoll(MTP_flags(MTPDupdateMessagePoll::Flag::f_poll | MTPDupdateMessagePoll::Flag::f_peer), peerToMTP(peer), MTP_int(r->msg_id.v), MTPint(), poll.vpoll().c_poll().vid(), poll.vpoll(), poll.vresults())}));
		});
		return true;
	}
	case mtpc_messages_getPollVotes: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_messages_getPollVotes>(body); if (!r) return bad();
		const auto source = _messageContent.value(r->id.v).value("poll").toObject();
		if (!source.value("canViewVotes").toBool()) return bad("NOVEO_POLL_VOTERS_PRIVATE");
		auto voters = QMap<QString, QJsonObject>();
		for (const auto value : source.value("options").toArray()) {
			const auto option = value.toObject(); const auto key = option.value("id").toString();
			if ((r->flags.v & 1) && r->option.v != key.toUtf8()) continue;
			for (const auto v : option.value("voters").toArray()) {
				const auto entry = v.toObject(); const auto raw = RawMember(entry); if (raw.isEmpty()) continue;
				auto item = voters.value(raw); auto choices = item.value("options").toArray(); choices.push_back(key);
				item.insert("options", choices); item.insert("votedAt", entry.value("votedAt")); voters[raw] = item;
			}
		}
		auto votes = QVector<MTPMessagePeerVote>();
		for (auto i = voters.cbegin(); i != voters.cend(); ++i) {
			auto choices = QVector<MTPbytes>(); for (const auto option : i->value("options").toArray()) choices.push_back(MTP_bytes(option.toString().toUtf8()));
			votes.push_back(MTP_messagePeerVoteMultiple(MTP_peerUser(MTP_long(NativeUserId(i.key()).bare)), MTP_vector<MTPbytes>(choices), MTP_int(i->value("votedAt").toInt())));
		}
		const auto offset = std::max(0, String(r->offset).toInt()); const auto total = votes.size();
		votes = votes.mid(offset, std::clamp(r->limit.v, 1, 100));
		const auto next = offset + votes.size(); const auto flags = next < total ? MTPDmessages_votesList::Flags(MTPDmessages_votesList::Flag::f_next_offset) : MTPDmessages_votesList::Flags();
		_pending.remove(id); reply(id, MTPmessages_VotesList(MTP_messages_votesList(MTP_flags(flags), MTP_int(total), MTP_vector<MTPMessagePeerVote>(votes), MTPVector<MTPChat>(), MTP_vector<MTPUser>(_users.values().toVector()), MTP_string(QString::number(next)))));
		return true;
	}
	case mtpc_messages_updatePinnedMessage: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_updatePinnedMessage>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto chat = _chatIds.value(inputPeer(r.peer).value); const auto uuid = _rawMessages.value(r.id.v);
		const auto unpin = bool(r.flags.v & 2);
		if (chat.isEmpty() || (!unpin && uuid.isEmpty())) return bad("NOVEO_MESSAGE_UNKNOWN");
		wire(id, {{{"type", unpin ? "unpin_message" : "pin_message"}, {"chatId", chat}, {"messageId", uuid}}}, {unpin ? "message_unpinned" : "message_pinned"}, emptyUpdates);
		return true;
	}
	case mtpc_messages_unpinAllMessages: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_unpinAllMessages>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto chat = _chatIds.value(inputPeer(r.peer).value); if (chat.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		wire(id, {{{"type", "unpin_message"}, {"chatId", chat}}}, {"message_unpinned"}, [=, this](QJsonObject) {
			reply(id, MTPmessages_AffectedHistory(MTP_messages_affectedHistory(MTP_int(_pts), MTP_int(0), MTP_int(0))));
		});
		return true;
	}
	case mtpc_messages_search: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_search>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = inputPeer(r.peer); const auto chat = _chatIds.value(peer.value);
		if (chat.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		if (r.filter.type() != mtpc_inputMessagesFilterEmpty) return bad("NOVEO_SEARCH_FILTER_UNSUPPORTED");
		wire(id, {{{"type", "search_chat_messages"}, {"chatId", chat}, {"query", String(r.q)}, {"requestId", QString::number(id)}}}, {"chat_message_search_results"}, [=, this](QJsonObject frame) {
			cacheMessages(peer, frame.value("messages").toArray());
			auto messages = QVector<MTPMessage>();
			for (const auto value : frame.value("messages").toArray()) messages.push_back(nativeMessage(value.toObject(), peer));
			const auto count = messages.size();
			if (r.offset_id.v) messages.erase(std::remove_if(messages.begin(), messages.end(), [=](const MTPMessage &m) { return (m.type() == mtpc_message ? m.c_message().vid().v : m.c_messageService().vid().v) >= r.offset_id.v; }), messages.end());
			messages.erase(std::remove_if(messages.begin(), messages.end(), [=, this](const MTPMessage &m) {
				if (m.type() != mtpc_message) return r.filter.type() != mtpc_inputMessagesFilterEmpty;
				const auto &data = m.c_message();
				return (r.min_date.v && data.vdate().v < r.min_date.v) || (r.max_date.v && data.vdate().v > r.max_date.v)
					|| (r.min_id.v && data.vid().v <= r.min_id.v) || (r.max_id.v && data.vid().v >= r.max_id.v)
					|| ((r.flags.v & 1) && (!data.vfrom_id() || peerFromMTP(*data.vfrom_id()) != inputPeer(r.from_id)));
			}), messages.end());
			messages = messages.mid(std::max(0, r.add_offset.v), std::clamp(r.limit.v, 1, 100));
			reply(id, messagePage(peer, std::move(messages), count));
		});
		return true;
	}
	case mtpc_messages_searchGlobal: {
		if (!_auth->authenticated() || !_historyReady) return true;
		const auto r = ReadRequest<mtpc_messages_searchGlobal>(body); if (!r) return bad();
		if (r->filter.type() != mtpc_inputMessagesFilterEmpty || (r->flags.v & 16)) return bad("NOVEO_SEARCH_FILTER_UNSUPPORTED");
		auto frames = QVector<QJsonObject>();
		for (const auto key : _dialogs.keys()) {
			const auto peer = PeerId(key); const auto raw = _chatIds.value(key);
			if (raw.isEmpty() || ((r->flags.v & 2) && !peerIsChannel(peer)) || ((r->flags.v & 4) && !peerIsChat(peer)) || ((r->flags.v & 8) && !peerIsUser(peer))) continue;
			frames.push_back({{"type", "search_chat_messages"}, {"chatId", raw}, {"query", String(r->q)}, {"requestId", QString::number(id)}});
		}
		if (frames.isEmpty() || String(r->q).trimmed().isEmpty()) { _pending.remove(id); reply(id, messagePage({}, {})); return true; }
		_globalSearchResults[id] = {};
		_pending[id].deadline = QDateTime::currentMSecsSinceEpoch() + std::min<qint64>(120000, 30000 + frames.size() * 1000);
		wire(id, frames, {"chat_message_search_results"}, [=, this](QJsonObject) {
			auto messages = _globalSearchResults.take(id);
			const auto date = [](const MTPMessage &m) { return m.type() == mtpc_message ? m.c_message().vdate().v : m.c_messageService().vdate().v; };
			const auto mid = [](const MTPMessage &m) { return m.type() == mtpc_message ? m.c_message().vid().v : m.c_messageService().vid().v; };
			std::stable_sort(messages.begin(), messages.end(), [&](const MTPMessage &a, const MTPMessage &b) { return date(a) != date(b) ? date(a) > date(b) : mid(a) > mid(b); });
			const auto total = messages.size();
			messages.erase(std::remove_if(messages.begin(), messages.end(), [&](const MTPMessage &m) {
				return (r->min_date.v && date(m) < r->min_date.v) || (r->max_date.v && date(m) > r->max_date.v)
					|| (r->offset_rate.v && date(m) > r->offset_rate.v)
					|| (r->offset_id.v && date(m) == r->offset_rate.v && mid(m) >= r->offset_id.v);
			}), messages.end());
			messages = messages.mid(0, std::clamp(r->limit.v, 1, 100));
			const auto nextRate = messages.isEmpty() ? 0 : date(messages.back());
			reply(id, MTPmessages_Messages(MTP_messages_messagesSlice(MTP_flags(MTPDmessages_messagesSlice::Flag::f_next_rate), MTP_int(total), MTP_int(nextRate), MTPint(), MTPSearchPostsFlood(), MTP_vector<MTPMessage>(messages), MTPVector<MTPForumTopic>(), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()))));
		});
		return true;
	}
	case mtpc_contacts_search: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_search>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto query = String(r.q).trimmed().remove('@');
		api(id, "/user/public-search?q=" + QString::fromLatin1(QUrl::toPercentEncoding(query)), {}, [=, this](QJsonObject response) {
			auto peers = QVector<MTPPeer>(); auto users = QVector<MTPUser>(); auto chats = QVector<MTPChat>();
			for (const auto value : response.value("results").toArray()) {
				auto item = value.toObject();
				if (item.value("resultType").toString() == "chat") {
					const auto known = _chatPeers.value(item.value("chatId").toString(item.value("id").toString()));
					item.insert("previewOnly", !item.value("isMember").toBool(_dialogs.contains(known.value))); const auto chat = nativeChat(item); chats.push_back(chat);
					_chats[(item.value("chatType").toString() == "channel" ? peerFromChannel(ChannelId(NativeUserId(item.value("chatId").toString()).bare)) : peerFromChat(ChatId(NativeUserId(item.value("chatId").toString()).bare))).value] = chat;
					peers.push_back(item.value("chatType").toString() == "channel" ? MTPPeer(MTP_peerChannel(MTP_long(NativeUserId(item.value("chatId").toString()).bare))) : MTPPeer(MTP_peerChat(MTP_long(NativeUserId(item.value("chatId").toString()).bare))));
				} else {
					const auto u = user(item, false); users.push_back(u); _users[u.c_user().vid().v] = u;
					peers.push_back(MTP_peerUser(u.c_user().vid()));
				}
			}
			reply(id, MTPcontacts_Found(MTP_contacts_found(MTP_vector<MTPPeer>({}), MTP_vector<MTPPeer>(peers), MTP_vector<MTPChat>(chats), MTP_vector<MTPUser>(users))));
		});
		return true;
	}
	case mtpc_contacts_resolveUsername: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_resolveUsername>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto query = String(r.username).trimmed().remove('@');
		api(id, "/user/public-search?q=" + QString::fromLatin1(QUrl::toPercentEncoding(query)), {}, [=, this](QJsonObject response) {
			auto peers = QVector<MTPPeer>(); auto users = QVector<MTPUser>(); auto chats = QVector<MTPChat>();
			for (const auto value : response.value("results").toArray()) {
				auto item = value.toObject();
				if (item.value("handle").toString().remove('@').compare(query, Qt::CaseInsensitive) != 0) continue;
				if (item.value("resultType").toString() == "chat") {
					const auto known = _chatPeers.value(item.value("chatId").toString(item.value("id").toString()));
					item.insert("previewOnly", !item.value("isMember").toBool(_dialogs.contains(known.value))); const auto chat = nativeChat(item); chats.push_back(chat);
					_chats[(item.value("chatType").toString() == "channel" ? peerFromChannel(ChannelId(NativeUserId(item.value("chatId").toString()).bare)) : peerFromChat(ChatId(NativeUserId(item.value("chatId").toString()).bare))).value] = chat;
					peers.push_back(item.value("chatType").toString() == "channel" ? MTPPeer(MTP_peerChannel(MTP_long(NativeUserId(item.value("chatId").toString()).bare))) : MTPPeer(MTP_peerChat(MTP_long(NativeUserId(item.value("chatId").toString()).bare))));
				} else {
					const auto u = user(item, false); users.push_back(u); _users[u.c_user().vid().v] = u;
					peers.push_back(MTP_peerUser(u.c_user().vid()));
				}
			}
			if (peers.isEmpty()) { fail(id, "USERNAME_NOT_OCCUPIED"); return; }
			reply(id, MTPcontacts_ResolvedPeer(MTP_contacts_resolvedPeer(peers.front(), MTP_vector<MTPChat>(chats), MTP_vector<MTPUser>(users))));
		});
		return true;
	}
	case mtpc_contacts_addContact: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_addContact>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(userId(r.id)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		api(id, "/user/contacts", {{"action", "add"}, {"userId", raw}, {"saveAs", (String(r.first_name) + " " + String(r.last_name)).trimmed()}}, [=, this](QJsonObject response) {
			_contactsReady = false; contacts(); reply(id, updates({}));
		}, true);
		return true;
	}
	case mtpc_contacts_deleteContacts: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_deleteContacts>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		if (r.id.v.size() != 1) {
			// Native callers can batch: process each acknowledged HTTP mutation in order.
			if (r.id.v.isEmpty()) return bad();
		}
		auto list = std::make_shared<QVector<MTPInputUser>>(r.id.v);
		auto next = std::make_shared<std::function<void()>>();
		const auto weak = std::weak_ptr(next);
		*next = [=, this] {
			const auto next = weak.lock(); if (!next) return;
			if (list->isEmpty()) { _pending.remove(id); _contactsReady = false; contacts(); reply(id, updates({})); *next = {}; return; }
			const auto raw = _rawUsers.value(userId(list->takeFirst()));
			if (raw.isEmpty()) { fail(id, "NOVEO_PEER_UNKNOWN"); *next = {}; return; }
			api(id, "/user/contacts", {{"action", "remove"}, {"userId", raw}}, [=, this](QJsonObject) { auto profile = _profiles.value(NativeUserId(raw).bare); profile.insert("isContact", false); profile.insert("contactName", ""); users(QJsonArray{profile}); (*next)(); }, true, [=, this](QString error) { fail(id, error); *next = {}; }, true);
		};
		(*next)();
		return true;
	}
	case mtpc_contacts_block: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_block>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(inputPeer(r.id).value); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		wire(id, {{{"type", "block_user"}, {"target_user_id", raw}}}, {"blocked_list_update"}, [=, this](QJsonObject frame) {
			_blocked.clear(); for (const auto value : frame.value("blockedUsers").toArray()) _blocked.insert(RawMember(value));
			reply(id, MTPBool(MTP_boolTrue()));
		});
		return true;
	}
	case mtpc_contacts_unblock: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_unblock>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(inputPeer(r.id).value); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		wire(id, {{{"type", "unblock_user"}, {"target_user_id", raw}}}, {"blocked_list_update"}, [=, this](QJsonObject frame) {
			_blocked.clear(); for (const auto value : frame.value("blockedUsers").toArray()) _blocked.insert(RawMember(value));
			reply(id, MTPBool(MTP_boolTrue()));
		});
		return true;
	}
	case mtpc_contacts_getBlocked: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_contacts_getBlocked>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		auto blocked = QVector<MTPPeerBlocked>();
		for (const auto &raw : _blocked) blocked.push_back(MTP_peerBlocked(MTP_peerUser(MTP_long(NativeUserId(raw).bare)), MTP_int(0)));
		const auto total = blocked.size(); blocked = blocked.mid(std::max(0, r.offset.v), std::max(0, r.limit.v));
		_pending.remove(id); reply(id, MTPcontacts_Blocked(MTP_contacts_blockedSlice(MTP_int(total), MTP_vector<MTPPeerBlocked>(blocked), MTP_vector<MTPChat>({}), MTP_vector<MTPUser>(_users.values().toVector()))));
		return true;
	}
	case mtpc_account_updateProfile: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_account_updateProfile>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		auto event = QJsonObject{{"type", "update_profile"}};
		if (r.flags.v & 1) event.insert("username", (String(r.first_name) + " " + String(r.last_name)).trimmed());
		if (r.flags.v & 4) event.insert("bio", String(r.about));
		wire(id, {event}, {"user_updated"}, [=, this](QJsonObject frame) {
			users(QJsonArray{frame.value("user").isObject() ? frame.value("user") : QJsonValue(frame)}); reply(id, selfUser());
		});
		return true;
	}
	case mtpc_account_updateUsername: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_account_updateUsername>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		wire(id, {{{"type", "update_profile"}, {"handle", String(r.username)}}}, {"user_updated"}, [=, this](QJsonObject frame) {
			users(QJsonArray{frame.value("user").isObject() ? frame.value("user") : QJsonValue(frame)}); reply(id, selfUser());
		});
		return true;
	}
	case mtpc_account_checkUsername: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_account_checkUsername>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		api(id, "/handle/check?kind=user&handle=" + QString::fromLatin1(QUrl::toPercentEncoding("@" + String(r.username))), {}, [=, this](QJsonObject response) {
			if (!response.value("available").toBool()) { fail(id, response.value("reason").toString() == "invalid" ? "USERNAME_INVALID" : "USERNAME_OCCUPIED"); return; }
			reply(id, MTPBool(MTP_boolTrue()));
		});
		return true;
	}
	case mtpc_account_getAuthorizations: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_account_getAuthorizations>(body);
		if (!parsed) return bad();
		api(id, "/user/sessions", {}, [=, this](QJsonObject response) {
			auto authorizations = QVector<MTPAuthorization>(); _sessionIds.clear();
			for (const auto value : response.value("sessions").toArray()) {
				const auto session = value.toObject(); const auto sid = session.value("sessionId").toString();
				if (sid.isEmpty()) continue;
				const auto hash = NativeUserId("session:" + sid).bare; _sessionIds[hash] = sid;
				authorizations.push_back(MTP_authorization(MTP_flags(session.value("isCurrent").toBool() ? MTPDauthorization::Flag::f_current : MTPDauthorization::Flags()), MTP_long(hash),
					MTP_string(session.value("deviceModel").toString()), MTP_string(session.value("osName").toString()), MTP_string(session.value("systemVersion").toString()), MTP_int(0),
					MTP_string(session.value("clientName").toString()), MTP_string(session.value("clientVersion").toString()), MTP_int(session.value("issuedAt").toInt()), MTP_int(session.value("lastSeenAt").toInt()),
					MTP_string(session.value("ip").toString()), MTP_string(session.value("country").toString()), MTP_string(session.value("region").toString())));
			}
			reply(id, MTPaccount_Authorizations(MTP_account_authorizations(MTP_int(365), MTP_vector<MTPAuthorization>(authorizations))));
		});
		return true;
	}
	case mtpc_account_resetAuthorization: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_account_resetAuthorization>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto sid = _sessionIds.value(r.hash.v); if (sid.isEmpty()) return bad("SESSION_ID_INVALID");
		api(id, "/user/sessions/revoke", {{"sessionId", sid}}, [=, this](QJsonObject) { reply(id, MTPBool(MTP_boolTrue())); }, true);
		return true;
	}
	case mtpc_messages_getFullChat: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_getFullChat>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = peerFromChat(ChatId(r.chat_id.v)); const auto chat = _chatIds.value(peer.value); if (chat.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		api(id, "/chat/settings", {{"action", "get_profile"}, {"chatId", chat}}, [=, this](QJsonObject response) {
			const auto profile = response.value("profile").toObject(); _chats[peer.value] = nativeChat(profile);
			for (const auto member : profile.value("members").toArray()) if (member.isObject()) users(QJsonArray{member});
			reply(id, MTPmessages_ChatFull(MTP_messages_chatFull(fullChat(peer, profile), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()))));
		}, true);
		return true;
	}
	case mtpc_channels_getFullChannel: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_getFullChannel>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel); const auto chat = _chatIds.value(peer.value); if (chat.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		api(id, "/chat/settings", {{"action", "get_profile"}, {"chatId", chat}}, [=, this](QJsonObject response) {
			const auto profile = response.value("profile").toObject(); _chats[peer.value] = nativeChat(profile);
			for (const auto member : profile.value("members").toArray()) if (member.isObject()) users(QJsonArray{member});
			reply(id, MTPmessages_ChatFull(MTP_messages_chatFull(fullChat(peer, profile), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()))));
		}, true);
		return true;
	}
	case mtpc_messages_createChat: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_createChat>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		auto members = QJsonArray(); for (const auto &u : r.users.v) { const auto raw = _rawUsers.value(userId(u)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN"); members.push_back(raw); }
		api(id, "/create_group", {{"name", String(r.title)}, {"members", members}}, [=, this](QJsonObject response) {
			const auto profile = response.value("group").toObject();
			if (profile.value("chatId").toString().isEmpty()) { fail(id, "NOVEO_CHAT_CREATE_FAILED"); return; }
			const auto chat = nativeChat(profile); _chats[peerFromChat(ChatId(NativeUserId(profile.value("chatId").toString()).bare)).value] = chat;
			_auth->send({{"type", "resync_state"}});
			reply(id, MTPmessages_InvitedUsers(MTP_messages_invitedUsers(createdUpdates(chat), MTPVector<MTPMissingInvitee>())));
		}, true);
		return true;
	}
	case mtpc_channels_joinChannel: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_joinChannel>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel); const auto chat = _chatIds.value(peer.value); if (chat.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		wire(id, {{{"type", "join_channel"}, {"chatId", chat}}}, {"chat_joined"}, emptyUpdates);
		return true;
	}
	case mtpc_channels_leaveChannel: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_leaveChannel>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		chatAction(id, channelPeer(r.channel), "leave_chat");
		return true;
	}
	case mtpc_messages_deleteChatUser: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_deleteChatUser>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(userId(r.user_id)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		chatAction(id, peerFromChat(ChatId(r.chat_id.v)), raw == _self ? "leave_chat" : "remove_member", {{"memberId", raw}});
		return true;
	}
	case mtpc_messages_addChatUser: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_addChatUser>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(userId(r.user_id)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		const auto peer = peerFromChat(ChatId(r.chat_id.v));
		api(id, "/chat/settings", {{"action", "add_members"}, {"chatId", _chatIds.value(peer.value)}, {"memberIds", QJsonArray{raw}}}, [=, this](QJsonObject) {
			_auth->send({{"type", "resync_state"}}); reply(id, MTPmessages_InvitedUsers(MTP_messages_invitedUsers(updates({}), MTPVector<MTPMissingInvitee>())));
		}, true);
		return true;
	}
	case mtpc_channels_inviteToChannel: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_inviteToChannel>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		auto members = QJsonArray(); for (const auto &u : r.users.v) { const auto raw = _rawUsers.value(userId(u)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN"); members.push_back(raw); }
		const auto peer = channelPeer(r.channel);
		api(id, "/chat/settings", {{"action", "add_members"}, {"chatId", _chatIds.value(peer.value)}, {"memberIds", members}}, [=, this](QJsonObject) {
			_auth->send({{"type", "resync_state"}}); reply(id, MTPmessages_InvitedUsers(MTP_messages_invitedUsers(updates({}), MTPVector<MTPMissingInvitee>())));
		}, true);
		return true;
	}
	case mtpc_messages_editChatTitle: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_editChatTitle>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		chatAction(id, peerFromChat(ChatId(r.chat_id.v)), "update_profile", {{"chatName", String(r.title)}});
		return true;
	}
	case mtpc_channels_editTitle: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_editTitle>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		chatAction(id, channelPeer(r.channel), "update_profile", {{"chatName", String(r.title)}});
		return true;
	}
	case mtpc_messages_editChatAbout: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_editChatAbout>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = inputPeer(r.peer);
		chatAction(id, peer, "update_profile", {{"bio", String(r.about)}}, true);
		return true;
	}
	case mtpc_messages_editChatAdmin: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_editChatAdmin>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(userId(r.user_id)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		chatAction(id, peerFromChat(ChatId(r.chat_id.v)), r.is_admin.type() == mtpc_boolTrue ? "add_admin" : "remove_admin", {{"memberId", raw}});
		return true;
	}
	case mtpc_channels_editAdmin: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_editAdmin>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto raw = _rawUsers.value(userId(r.user_id)); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		chatAction(id, channelPeer(r.channel), bool(r.admin_rights.c_chatAdminRights().vflags().v) ? "add_admin" : "remove_admin", {{"memberId", raw}});
		return true;
	}
	case mtpc_channels_editBanned: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_editBanned>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel); const auto member = _rawUsers.value(inputPeer(r.participant).value);
		if (member.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		if (!r.banned_rights.c_chatBannedRights().is_view_messages()) return bad("NOVEO_MEMBER_RESTRICTION_UNSUPPORTED");
		chatAction(id, peer, "remove_member", {{"memberId", member}});
		return true;
	}
	case mtpc_messages_editChatDefaultBannedRights: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_editChatDefaultBannedRights>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto &rights = r.banned_rights.c_chatBannedRights();
		chatAction(id, inputPeer(r.peer), "set_permissions", {{"canSendMessages", !(rights.is_send_plain() || rights.is_send_messages())},
			{"canSendFiles", !rights.is_send_media()}, {"canAddMembers", !rights.is_invite_users()},
			{"canViewMembers", _chatProfiles.value(inputPeer(r.peer).value).value("canViewMembers").toBool(true)}});
		return true;
	}
	case mtpc_channels_updateUsername: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_updateUsername>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel);
		api(id, "/chat/settings", {{"action", String(r.username).isEmpty() ? "remove_handle" : "set_handle"}, {"chatId", _chatIds.value(peer.value)}, {"handle", String(r.username)}}, [=, this](QJsonObject) {
			_auth->send({{"type", "resync_state"}}); reply(id, MTPBool(MTP_boolTrue()));
		}, true);
		return true;
	}
	case mtpc_channels_checkUsername: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_checkUsername>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		api(id, "/handle/check?kind=chat&chatId=" + QString::fromLatin1(QUrl::toPercentEncoding(_chatIds.value(channelPeer(r.channel).value))) + "&handle=" + QString::fromLatin1(QUrl::toPercentEncoding("@" + String(r.username))), {}, [=, this](QJsonObject response) {
			if (!response.value("available").toBool()) { fail(id, response.value("reason").toString() == "invalid" ? "USERNAME_INVALID" : "USERNAME_OCCUPIED"); return; }
			reply(id, MTPBool(MTP_boolTrue()));
		});
		return true;
	}
	case mtpc_messages_deleteChat: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_deleteChat>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = peerFromChat(ChatId(r.chat_id.v));
		api(id, "/chat/settings", {{"action", "delete_chat"}, {"chatId", _chatIds.value(peer.value)}}, [=, this](QJsonObject) {
			_dialogs.remove(peer.value); _messages.remove(peer.value); _auth->send({{"type", "resync_state"}});
			if (onDialogs) onDialogs(dialogs());
			reply(id, MTPBool(MTP_boolTrue()));
		}, true);
		return true;
	}
	case mtpc_channels_deleteChannel: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_channels_deleteChannel>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = channelPeer(r.channel);
		api(id, "/chat/settings", {{"action", "delete_chat"}, {"chatId", _chatIds.value(peer.value)}}, [=, this](QJsonObject) {
			_dialogs.remove(peer.value); _messages.remove(peer.value); _auth->send({{"type", "resync_state"}});
			if (onDialogs) onDialogs(dialogs());
			reply(id, updates({}));
		}, true);
		return true;
	}
	case mtpc_messages_deleteHistory: {
		if (!_auth->authenticated()) return true;
		const auto parsed = ReadRequest<mtpc_messages_deleteHistory>(body);
		if (!parsed) return bad();
		const auto &r = *parsed;
		const auto peer = inputPeer(r.peer); const auto raw = _chatIds.value(peer.value);
		if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		// Noveo has no group history-clear endpoint. Never turn clearing into
		// leaving a group or deleting it for every participant.
		if (!peerIsUser(peer) || r.min_date.v || r.max_date.v
			|| (r.max_id.v && r.max_id.v != 2147483647)) return bad("NOVEO_HISTORY_CLEAR_UNSUPPORTED");
		const auto action = "delete_private_chat";
		api(id, "/chat/settings", {{"action", action}, {"chatId", raw}, {"scope", (r.flags.v & 2) ? "everyone" : "me"}}, [=, this](QJsonObject) {
			const auto ids = _messages.value(peer.value); for (const auto &m : ids) removeMessage(peer, m.type() == mtpc_message ? m.c_message().vid().v : m.c_messageService().vid().v);
			_dialogs.remove(peer.value); _auth->send({{"type", "resync_state"}});
			reply(id, MTPmessages_AffectedHistory(MTP_messages_affectedHistory(MTP_int(_pts), MTP_int(0), MTP_int(0))));
		}, true);
		return true;
	}
	case mtpc_account_getPrivacy: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_account_getPrivacy>(body);
		if (!r) return bad();
		if (r->key.type() != mtpc_inputPrivacyKeyChatInvite) return bad("NOVEO_PRIVACY_UNSUPPORTED");
		api(id, "/user/profile?userId=" + QString::fromLatin1(QUrl::toPercentEncoding(_self)), {}, [=, this](QJsonObject response) {
			const auto profile = response.value("profile").toObject();
			users(QJsonArray{profile});
			const auto rule = profile.value("blockGroupInvites").toBool() ? MTPPrivacyRule(MTP_privacyValueDisallowAll()) : MTPPrivacyRule(MTP_privacyValueAllowAll());
			reply(id, MTPaccount_PrivacyRules(MTP_account_privacyRules(MTP_vector<MTPPrivacyRule>({rule}), MTPVector<MTPChat>(), MTP_vector<MTPUser>(_users.values().toVector()))));
		});
		return true;
	}
	case mtpc_account_setPrivacy: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_account_setPrivacy>(body);
		if (!r) return bad();
		if (r->key.type() != mtpc_inputPrivacyKeyChatInvite || r->rules.v.size() != 1) return bad("NOVEO_PRIVACY_UNSUPPORTED");
		const auto kind = r->rules.v.front().type();
		if (kind != mtpc_inputPrivacyValueAllowAll && kind != mtpc_inputPrivacyValueDisallowAll) return bad("NOVEO_PRIVACY_UNSUPPORTED");
		const auto blocked = kind == mtpc_inputPrivacyValueDisallowAll;
		api(id, "/user/privacy", {{"blockGroupInvites", blocked}}, [=, this](QJsonObject) {
			const auto rule = blocked ? MTPPrivacyRule(MTP_privacyValueDisallowAll()) : MTPPrivacyRule(MTP_privacyValueAllowAll());
			reply(id, MTPaccount_PrivacyRules(MTP_account_privacyRules(MTP_vector<MTPPrivacyRule>({rule}), MTPVector<MTPChat>(), MTP_vector<MTPUser>(_users.values().toVector()))));
		}, true);
		return true;
	}
	case mtpc_auth_resetAuthorizations: {
		if (!_auth->authenticated()) return true;
		api(id, "/user/sessions", {}, [=, this](QJsonObject response) {
			auto list = std::make_shared<QStringList>();
			for (const auto value : response.value("sessions").toArray()) {
				const auto session = value.toObject();
				if (!session.value("isCurrent").toBool() && !session.value("sessionId").toString().isEmpty()) list->push_back(session.value("sessionId").toString());
			}
			auto next = std::make_shared<std::function<void()>>();
			const auto weak = std::weak_ptr(next);
			*next = [=, this] {
				const auto next = weak.lock(); if (!next) return;
				if (list->isEmpty()) { _pending.remove(id); reply(id, MTPBool(MTP_boolTrue())); *next = {}; return; }
				api(id, "/user/sessions/revoke", {{"sessionId", list->takeFirst()}}, [=](QJsonObject) { (*next)(); }, true,
					[=, this](QString error) { fail(id, error); *next = {}; }, true);
			};
			(*next)();
		}, false, {}, true);
		return true;
	}
	case mtpc_channels_createChannel: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_channels_createChannel>(body);
		if (!r) return bad();
		if ((r->flags.v & 2) || (r->flags.v & 32)) return bad("NOVEO_CHANNEL_KIND_UNSUPPORTED");
		const auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
		const auto title = String(r->title);
		const auto handle = "@channel_" + QUuid::createUuid().toString(QUuid::Id128).left(16);
		for (const auto &pair : {qMakePair(QString("name"), title), qMakePair(QString("handle"), handle)}) {
			QHttpPart part;
			part.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"" + pair.first + "\"");
			part.setBody(pair.second.toUtf8()); multipart->append(part);
		}
		api(id, "/create_channel", {}, [=, this](QJsonObject response) {
			auto profile = response.value("channel").toObject();
			const auto raw = profile.value("chatId").toString();
			if (raw.isEmpty()) { _pending.remove(id); fail(id, "NOVEO_CHAT_CREATE_FAILED"); return; }
			const auto finish = [=, this](QJsonObject profile) {
				_pending.remove(id);
				const auto native = nativeChat(profile); _chats[peerFromChannel(ChannelId(native.c_channel().vid().v)).value] = native;
				_auth->send({{"type", "resync_state"}});
				reply(id, createdUpdates(native));
			};
			const auto about = String(r->about);
			if (about.isEmpty()) { finish(profile); return; }
			api(id, "/chat/settings", {{"action", "update_profile"}, {"chatId", raw}, {"chatName", title}, {"bio", about}}, [=](QJsonObject) mutable {
				profile.insert("bio", about); finish(profile);
			}, true);
		}, true, {}, true, multipart);
		return true;
	}
	case mtpc_photos_uploadProfilePhoto: {
		if (!_auth->authenticated()) return true;
		const auto r = ReadRequest<mtpc_photos_uploadProfilePhoto>(body);
		if (!r) return bad();
		if (!(r->flags.v & 1) || (r->flags.v & ~1)) return bad("NOVEO_AVATAR_KIND_UNSUPPORTED");
		const auto media = MTP_inputMediaUploadedPhoto(MTP_flags(MTPDinputMediaUploadedPhoto::Flags()), r->file, MTPVector<MTPInputDocument>(), MTPint(), MTPInputDocument());
		upload(id, media, [=, this](QJsonObject file) {
			auto profile = _profiles.value(NativeUserId(_self).bare); profile.insert("avatarUrl", file.value("url"));
			users(QJsonArray{profile});
			const auto url = file.value("url").toString();
			const auto photo = NativeAvatar(_apiEndpoint.resolved(QUrl(url)));
			_pending.remove(id);
			if (onUpdate) onUpdate(updates({}));
			reply(id, MTPphotos_Photo(MTP_photos_photo(photo, MTP_vector<MTPUser>({selfUser()}))));
		}, "/upload/avatar");
		return true;
	}
	case mtpc_messages_editChatPhoto:
	case mtpc_channels_editPhoto: {
		if (!_auth->authenticated()) return true;
		auto peer = PeerId(); auto photo = MTPInputChatPhoto();
		if (type == mtpc_messages_editChatPhoto) {
			const auto r = ReadRequest<mtpc_messages_editChatPhoto>(body); if (!r) return bad();
			peer = peerFromChat(ChatId(r->chat_id.v)); photo = r->photo;
		} else {
			const auto r = ReadRequest<mtpc_channels_editPhoto>(body); if (!r) return bad();
			peer = channelPeer(r->channel); photo = r->photo;
		}
		const auto raw = _chatIds.value(peer.value);
		if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		if (photo.type() == mtpc_inputChatPhotoEmpty) { chatAction(id, peer, "remove_avatar"); return true; }
		if (photo.type() != mtpc_inputChatUploadedPhoto) return bad("NOVEO_AVATAR_KIND_UNSUPPORTED");
		const auto &data = photo.c_inputChatUploadedPhoto();
		if (!data.vfile() || data.vvideo()) return bad("NOVEO_AVATAR_KIND_UNSUPPORTED");
		const auto media = MTP_inputMediaUploadedPhoto(MTP_flags(MTPDinputMediaUploadedPhoto::Flags()), *data.vfile(), MTPVector<MTPInputDocument>(), MTPint(), MTPInputDocument());
		upload(id, media, [=, this](QJsonObject) {
			_pending.remove(id); _auth->send({{"type", "resync_state"}}); reply(id, updates({}));
		}, "/chat/upload_avatar", raw);
		return true;
	}
	case mtpc_channels_getParticipants:
	case mtpc_channels_getParticipant: {
		if (!_auth->authenticated()) return true;
		auto peer = PeerId(); auto target = QString(); auto filter = MTPChannelParticipantsFilter(); auto offset = 0, limit = 100;
		if (type == mtpc_channels_getParticipants) {
			const auto r = ReadRequest<mtpc_channels_getParticipants>(body); if (!r) return bad();
			peer = channelPeer(r->channel); filter = r->filter; offset = std::max(0, r->offset.v); limit = std::clamp(r->limit.v, 1, 200);
		} else {
			const auto r = ReadRequest<mtpc_channels_getParticipant>(body); if (!r) return bad();
			peer = channelPeer(r->channel); target = _rawUsers.value(inputPeer(r->participant).value);
			if (target.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		}
		const auto raw = _chatIds.value(peer.value); if (raw.isEmpty()) return bad("NOVEO_PEER_UNKNOWN");
		api(id, "/chat/settings", {{"action", "get_profile"}, {"chatId", raw}}, [=, this](QJsonObject response) {
			const auto profile = response.value("profile").toObject();
			if (!profile.value("canViewMembers").toBool()) { fail(id, "CHAT_ADMIN_REQUIRED"); return; }
			_chats[peer.value] = nativeChat(profile);
			auto participants = QVector<MTPChannelParticipant>();
			const auto owner = profile.value("ownerId").toString(); const auto admins = profile.value("adminIds").toArray();
			auto query = QString();
			if (filter.type() == mtpc_channelParticipantsSearch) query = String(filter.c_channelParticipantsSearch().vq());
			for (const auto member : profile.value("members").toArray()) {
				const auto memberId = RawMember(member); if (memberId.isEmpty()) continue;
				if (member.isObject()) users(QJsonArray{member});
				if (!target.isEmpty() && target != memberId) continue;
				const auto admin = memberId == owner || admins.contains(memberId);
				if (filter.type() == mtpc_channelParticipantsAdmins && !admin) continue;
				if (filter.type() == mtpc_channelParticipantsBots && !_profiles.value(NativeUserId(memberId).bare).value("isBot").toBool()) continue;
				if (filter.type() == mtpc_channelParticipantsKicked || filter.type() == mtpc_channelParticipantsBanned) continue;
				if (!query.isEmpty() && !memberId.contains(query, Qt::CaseInsensitive) && !_profiles.value(NativeUserId(memberId).bare).value("username").toString().contains(query, Qt::CaseInsensitive)) continue;
				const auto uid = MTP_long(NativeUserId(memberId).bare);
				if (memberId == owner) participants.push_back(MTP_channelParticipantCreator(MTP_flags(MTPDchannelParticipantCreator::Flags()), uid, MTP_chatAdminRights(MTP_flags(MTPDchatAdminRights::Flag::f_change_info | MTPDchatAdminRights::Flag::f_delete_messages | MTPDchatAdminRights::Flag::f_invite_users | MTPDchatAdminRights::Flag::f_add_admins)), MTPstring()));
				else if (admin) participants.push_back(MTP_channelParticipantAdmin(MTP_flags(MTPDchannelParticipantAdmin::Flag::f_can_edit), uid, MTPlong(), MTP_long(NativeUserId(owner).bare), MTP_int(0), MTP_chatAdminRights(MTP_flags(MTPDchatAdminRights::Flag::f_change_info | MTPDchatAdminRights::Flag::f_delete_messages | MTPDchatAdminRights::Flag::f_invite_users)), MTPstring()));
				else participants.push_back(MTP_channelParticipant(MTP_flags(MTPDchannelParticipant::Flags()), uid, MTP_int(0), MTPint(), MTPstring()));
			}
			if (!target.isEmpty()) {
				if (participants.isEmpty()) { fail(id, "USER_NOT_PARTICIPANT"); return; }
				reply(id, MTPchannels_ChannelParticipant(MTP_channels_channelParticipant(participants.front(), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()))));
			} else {
				const auto total = participants.size();
				reply(id, MTPchannels_ChannelParticipants(MTP_channels_channelParticipants(MTP_int(total), MTP_vector<MTPChannelParticipant>(participants.mid(offset, limit)), MTP_vector<MTPChat>(_chats.values().toVector()), MTP_vector<MTPUser>(_users.values().toVector()))));
			}
		}, true);
		return true;
	}
	case mtpc_messages_getMessages:
	case mtpc_channels_getMessages: {
		if (!_auth->authenticated() || !_historyReady) return true;
		auto from = body.constData() + 1;
		const auto end = body.constData() + body.size();
		auto channel = MTPInputChannel();
		auto wanted = MTPVector<MTPInputMessage>();
		if ((body.front() == mtpc_channels_getMessages && !channel.read(from, end))
			|| !wanted.read(from, end) || from != end) return bad();
		auto frames = QVector<QJsonObject>();
		for (const auto &input : wanted.v) {
			if (input.type() != mtpc_inputMessageID) continue;
			const auto messageId = input.c_inputMessageID().vid().v;
			if (_messageObjects.contains(messageId)) continue;
			const auto peer = _contextPeers.value(messageId);
			const auto raw = _rawMessages.value(messageId);
			if (!peer || raw.isEmpty()) continue;
			frames.push_back({{"type", "load_message_context"}, {"chatId", _chatIds.value(peer.value)},
				{"messageId", raw}, {"requestId", QString::number(id)}});
		}
		if (frames.isEmpty()) return false;
		wire(id, frames, {"message_context"}, [=, this](QJsonObject frame) {
			const auto peer = _chatPeers.value(frame.value("chatId").toString());
			cacheMessages(peer, frame.value("messages").toArray());
			auto found = QVector<MTPMessage>();
			for (const auto &input : wanted.v) {
				if (input.type() != mtpc_inputMessageID) continue;
				const auto messageId = input.c_inputMessageID().vid().v;
				if (_messageObjects.contains(messageId)) {
					const auto messagePeer = _contextPeers.value(messageId, peer);
					found.push_back(nativeMessage(_messageObjects.value(messageId), messagePeer));
				}
			}
			reply(id, messagePage(peer, found));
		});
		return true;
	}
	default: return false;
	}
}
} // namespace Noveo
