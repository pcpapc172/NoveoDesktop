#include "noveo/session_client.h"

#include "noveo/request_fields.h"

#include <QtCore/QDateTime>
#include <algorithm>
#include <array>

namespace Noveo {
namespace {

QString Key(const QJsonObject &entry) {
	return SyncState::Id(
		entry.value("kind").toString(), entry.value("key").toString());
}

bool Matches(const QJsonObject &sent, const QJsonValue &actual) {
	const auto wanted = sent.value("value");
	if (sent.value("kind") != "dialog" || !wanted.isObject()) {
		return wanted == actual;
	}
	const auto patch = wanted.toObject();
	const auto server = actual.toObject();
	for (auto i = patch.begin(); i != patch.end(); ++i) {
		if (server.value(i.key()) != i.value()) {
			return false;
		}
	}
	return true;
}

constexpr auto EntityTypes = std::array{
	std::pair{mtpc_messageEntityBold, "bold"},
	std::pair{mtpc_messageEntityItalic, "italic"},
	std::pair{mtpc_messageEntityUnderline, "underline"},
	std::pair{mtpc_messageEntityStrike, "strike"},
	std::pair{mtpc_messageEntitySpoiler, "spoiler"},
	std::pair{mtpc_messageEntityCode, "code"},
	std::pair{mtpc_messageEntityPre, "pre"},
	std::pair{mtpc_messageEntityTextUrl, "textUrl"},
	std::pair{mtpc_messageEntityUrl, "url"},
	std::pair{mtpc_messageEntityEmail, "email"},
	std::pair{mtpc_messageEntityMention, "mention"},
	std::pair{mtpc_messageEntityHashtag, "hashtag"},
	std::pair{mtpc_messageEntityCashtag, "cashtag"},
	std::pair{mtpc_messageEntityBotCommand, "botCommand"},
	std::pair{mtpc_messageEntityPhone, "phone"},
	std::pair{mtpc_messageEntityBlockquote, "blockquote"},
	std::pair{mtpc_messageEntityCustomEmoji, "customEmoji"},
	std::pair{mtpc_messageEntityMentionName, "mentionName"},
	std::pair{mtpc_inputMessageEntityMentionName, "mentionName"},
};

QVector<MTPMessageEntity> NativeEntities(const QJsonArray &entities) {
	auto result = QVector<MTPMessageEntity>();
	for (const auto value : entities) {
		const auto e = value.toObject();
		const auto type = e.value("type").toString();
		const auto offset = MTP_int(e.value("offset").toInt());
		const auto length = MTP_int(e.value("length").toInt());
		if (type == "bold") {
			result.push_back(MTP_messageEntityBold(offset, length));
		} else if (type == "italic") {
			result.push_back(MTP_messageEntityItalic(offset, length));
		} else if (type == "underline") {
			result.push_back(MTP_messageEntityUnderline(offset, length));
		} else if (type == "strike") {
			result.push_back(MTP_messageEntityStrike(offset, length));
		} else if (type == "spoiler") {
			result.push_back(MTP_messageEntitySpoiler(offset, length));
		} else if (type == "code") {
			result.push_back(MTP_messageEntityCode(offset, length));
		} else if (type == "pre") {
			result.push_back(MTP_messageEntityPre(
				offset, length, MTP_string(e.value("language").toString())));
		} else if (type == "textUrl") {
			result.push_back(MTP_messageEntityTextUrl(
				offset, length, MTP_string(e.value("url").toString())));
		} else if (type == "url") {
			result.push_back(MTP_messageEntityUrl(offset, length));
		} else if (type == "email") {
			result.push_back(MTP_messageEntityEmail(offset, length));
		} else if (type == "mention") {
			result.push_back(MTP_messageEntityMention(offset, length));
		} else if (type == "hashtag") {
			result.push_back(MTP_messageEntityHashtag(offset, length));
		} else if (type == "cashtag") {
			result.push_back(MTP_messageEntityCashtag(offset, length));
		} else if (type == "botCommand") {
			result.push_back(MTP_messageEntityBotCommand(offset, length));
		} else if (type == "phone") {
			result.push_back(MTP_messageEntityPhone(offset, length));
		} else if (type == "blockquote") {
			result.push_back(MTP_messageEntityBlockquote(
				MTP_flags(MTPDmessageEntityBlockquote::Flags()),
				offset,
				length));
		} else if (type == "customEmoji") {
			result.push_back(MTP_messageEntityCustomEmoji(
				offset,
				length,
				MTP_long(e.value("documentId").toString().toULongLong())));
		} else if (type == "mentionName") {
			result.push_back(MTP_messageEntityMentionName(
				offset,
				length,
				MTP_long(NativeUserId(e.value("userId").toString()).bare)));
		}
	}
	return result;
}

}  // namespace

QString SessionClient::syncKey(PeerId peer) const {
	const auto raw = peerIsUser(peer) ? _rawUsers.value(peer.value)
									  : _chatIds.value(peer.value);
	return raw.isEmpty() || raw.startsWith("noveo-synthetic-peer:")
			   ? QString()
			   : (peerIsUser(peer) ? u"user:"_q : u"chat:"_q) + raw;
}

PeerId SessionClient::syncPeer(const QString &key) const {
	return key.startsWith("user:")	 ? peerFromUser(NativeUserId(key.mid(5)))
		   : key.startsWith("chat:") ? _chatPeers.value(key.mid(5))
									 : PeerId();
}

QJsonValue SessionClient::syncValue(QString kind, QString key) const {
	if (_sync.contains(kind, key)) {
		return _sync.value(kind, key);
	}
	if (!_sync.imported) {
		for (const auto value : _sync.localImport) {
			const auto entry = value.toObject();
			if (entry.value("kind") == kind && entry.value("key") == key) {
				return entry.value("value");
			}
		}
	}
	return QJsonValue(QJsonValue::Undefined);
}

void SessionClient::syncPersist() {
	_auth->saveSyncState(_sync.serialize());
}

void SessionClient::syncRetry() {
	_syncRetry.start(_syncRetryDelay);
	_syncRetryDelay = std::min(60000, _syncRetryDelay * 2);
}

void SessionClient::syncRefresh() {
	if (_syncLoading || !_auth->authenticated()) {
		return;
	}
	_syncLoading = true;
	const auto generation = _syncGeneration;
	api(
		--_internalRequest,
		u"/user/sync"_q,
		{},
		[=, this](QJsonObject frame) {
			if (generation != _syncGeneration) {
				return;
			}
			_syncLoading = false;
			if (!frame.value("items").isArray()) {
				syncRetry();
				return;
			}
			syncAccept(frame);
		},
		false,
		[=, this](QString) {
			if (generation != _syncGeneration) {
				return;
			}
			_syncLoading = false;
			_syncFetched = true;
			syncApply(_sync.ids());
			drain();
			syncRetry();
		});
}

void SessionClient::syncAccept(const QJsonObject &frame) {
	if (!frame.value("items").isArray()
		&& (!_syncReady
			|| frame.value("revision").toInteger() > _sync.revision + 1)) {
		syncRefresh();
		return;
	}
	if (frame.value("revision").toInteger(-1) < _sync.revision) {
		return;
	}
	const auto changed = _sync.accept(frame);
	if (frame.value("items").isArray()) {
		_syncReady = _syncFetched = true;
	}
	syncImportLocal();
	syncPersist();
	syncApply(changed);
	drain();
	syncFlush();
}

void SessionClient::syncQueue(
	QString kind, QString key, QJsonValue value, mtpRequestId id) {
	const auto sequence = _sync.queue(kind, key, value);
	if (id) {
		_syncWriters[SyncState::Id(kind, key)].push_back({id, sequence});
	}
}

void SessionClient::syncComplete(const QJsonObject &entry, QString error) {
	const auto key = Key(entry);
	auto &writers = _syncWriters[key];
	auto done = QVector<mtpRequestId>();
	for (auto i = writers.begin(); i != writers.end();) {
		if (!error.isEmpty()
			|| i->sequence <= entry.value("sequence").toInteger()) {
			done.push_back(i->requestId);
			i = writers.erase(i);
		} else {
			++i;
		}
	}
	for (const auto id : done) {
		if (!_pending.contains(id)) {
			continue;
		}
		if (error.isEmpty() && --_syncRemaining[id] > 0) {
			continue;
		}
		const auto folder = uint32(_pending.value(id).body.front())
							== mtpc_folders_editPeerFolders;
		_pending.remove(id);
		_syncRemaining.remove(id);
		_syncRequests.remove(id);
		if (!error.isEmpty()) {
			fail(id, error);
		} else if (folder) {
			reply(id, updates({}));
		} else {
			reply(id, MTPBool(MTP_boolTrue()));
		}
	}
}

void SessionClient::syncFlush() {
	if (_syncSaving || _sync.pending.isEmpty() || !_auth->authenticated()) {
		return;
	}
	if (!_syncReady) {
		syncRefresh();
		return;
	}
	const auto entry = _sync.next();
	const auto generation = _syncGeneration;
	_syncSaving = true;
	syncPersist();
	api(
		--_internalRequest,
		u"/user/sync"_q,
		SyncState::Request(entry),
		[=, this](QJsonObject frame) {
			if (generation != _syncGeneration) {
				return;
			}
			_syncSaving = false;
			if (!frame.value("items").isArray()) {
				syncRetry();
				return;
			}
			auto changed = _sync.accept(frame);
			const auto kind = entry.value("kind").toString();
			const auto key = entry.value("key").toString();
			auto error = QString();
			if (frame.value("error") == "STATE_VERSION_CONFLICT"
				&& !Matches(entry, _sync.serverValue(kind, key))) {
				_sync.conflict(entry);
				error = u"STATE_VERSION_CONFLICT"_q;
			} else {
				auto version = entry.value("base").toInteger();
				for (const auto value : frame.value("items").toArray()) {
					const auto item = value.toObject();
					if (Key(item) == Key(entry)) {
						version = item.value("version").toInteger();
					}
				}
				_sync.acknowledge(entry, version);
				_syncRetryDelay = 2000;
			}
			changed.insert(Key(entry));
			syncPersist();
			syncApply(changed);
			syncComplete(entry, error);
			syncFlush();
		},
		true,
		[=, this](QString error) {
			if (generation != _syncGeneration) {
				return;
			}
			_syncSaving = false;
			if (error == "Invalid account state") {
				_sync.conflict(entry);
				syncPersist();
				syncComplete(entry, u"NOVEO_SYNC_INVALID_STATE"_q);
				syncFlush();
			} else {
				syncRetry();
			}
		},
		false,
		nullptr,
		true);
}

QJsonArray SessionClient::syncEntities(
	const QVector<MTPMessageEntity> &entities) const {
	auto result = QJsonArray();
	for (const auto &entity : entities) {
		auto object = entity.match([](const auto &data) {
			return QJsonObject{
				{"offset", data.voffset().v}, {"length", data.vlength().v}};
		});
		const auto type = std::find_if(
			EntityTypes.begin(), EntityTypes.end(), [&](const auto &item) {
				return item.first == entity.type();
			});
		if (type == EntityTypes.end()) {
			return QJsonArray{QJsonObject()};
		}
		object.insert("type", QString::fromLatin1(type->second));
		if (entity.type() == mtpc_messageEntityPre) {
			object.insert(
				"language", qs(entity.c_messageEntityPre().vlanguage()));
		}
		if (entity.type() == mtpc_messageEntityTextUrl) {
			object.insert("url", qs(entity.c_messageEntityTextUrl().vurl()));
		}
		if (entity.type() == mtpc_messageEntityCustomEmoji) {
			object.insert(
				"documentId",
				QString::number(
					entity.c_messageEntityCustomEmoji().vdocument_id().v));
		}
		if (entity.type() == mtpc_messageEntityBlockquote
			&& entity.c_messageEntityBlockquote().is_collapsed()) {
			return QJsonArray{QJsonObject()};
		}
		if (entity.type() == mtpc_messageEntityMentionName
			|| entity.type() == mtpc_inputMessageEntityMentionName) {
			const auto id
				= entity.type() == mtpc_messageEntityMentionName
					  ? uint64(
							entity.c_messageEntityMentionName().vuser_id().v)
					  : entity.c_inputMessageEntityMentionName()
							.vuser_id()
							.match(
								[](const MTPDinputUser &d) {
									return uint64(d.vuser_id().v);
								},
								[this](const MTPDinputUserSelf &) {
									return NativeUserId(_self).bare;
								},
								[](const auto &) { return uint64(0); });
			const auto raw = _rawUsers.value(id);
			if (raw.isEmpty()) {
				return QJsonArray{QJsonObject()};
			}
			object.insert("userId", raw);
		}
		result.push_back(object);
	}
	return result;
}

QJsonValue SessionClient::syncDraft(
	QString text,
	bool noWebpage,
	const std::optional<MTPInputReplyTo> &reply,
	const QVector<MTPMessageEntity> &entities) const {
	if (text.isEmpty() && !reply) {
		return QJsonValue(QJsonValue::Null);
	}
	auto result = QJsonObject{{"text", text}, {"noWebpage", noWebpage}};
	if (reply) {
		if (reply->type() != mtpc_inputReplyToMessage) {
			return QJsonValue(QJsonValue::Undefined);
		}
		const auto &data = reply->c_inputReplyToMessage();
		if (data.vflags().v != 0) {
			return QJsonValue(QJsonValue::Undefined);
		}
		const auto raw = _rawMessages.value(data.vreply_to_msg_id().v);
		if (raw.isEmpty()) {
			return QJsonValue(QJsonValue::Undefined);
		}
		result.insert("replyTo", raw);
	}
	const auto serialized = syncEntities(entities);
	for (const auto value : serialized) {
		if (value.toObject().isEmpty()) {
			return QJsonValue(QJsonValue::Undefined);
		}
	}
	result.insert("entities", serialized);
	return result;
}

std::optional<MTPDraftMessage> SessionClient::syncNativeDraft(
	const QString &key) {
	const auto value = syncValue("draft", key);
	if (!value.isObject()) {
		return MTP_draftMessageEmpty(
			MTP_flags(MTPDdraftMessageEmpty::Flags()), MTPint());
	}
	const auto object = value.toObject();
	auto reply = MTPInputReplyTo();
	auto flags = MTPDdraftMessage::Flags();
	if (object.value("noWebpage").toBool()) {
		flags |= MTPDdraftMessage::Flag::f_no_webpage;
	}
	const auto uuid = object.value("replyTo").toString();
	if (!uuid.isEmpty()) {
		if (!_messageIds.contains(uuid)) {
			const auto peer = syncPeer(key);
			if (!_syncReplyContexts.contains(uuid)
				&& _chatIds.contains(peer.value)) {
				_syncReplyContexts.insert(uuid);
				_auth->send(
					{{"type", "load_message_context"},
					 {"chatId", _chatIds.value(peer.value)},
					 {"messageId", uuid},
					 {"requestId", "sync-draft:" + uuid}});
			}
			return std::nullopt;
		}
		flags |= MTPDdraftMessage::Flag::f_reply_to;
		reply = MTP_inputReplyToMessage(
			MTP_flags(MTPDinputReplyToMessage::Flags()),
			MTP_int(_messageIds.value(uuid)),
			MTPint(),
			MTPInputPeer(),
			MTPstring(),
			MTPVector<MTPMessageEntity>(),
			MTPint(),
			MTPInputPeer(),
			MTPint(),
			MTPbytes());
	}
	const auto entities = NativeEntities(object.value("entities").toArray());
	if (!entities.isEmpty()) {
		flags |= MTPDdraftMessage::Flag::f_entities;
	}
	const auto date = _sync.items.value(SyncState::Id("draft", key))
						  .toObject()
						  .value("updatedAt")
						  .toInt(int(QDateTime::currentSecsSinceEpoch()));
	return MTP_draftMessage(
		MTP_flags(flags),
		reply,
		MTP_string(object.value("text").toString()),
		MTP_vector<MTPMessageEntity>(entities),
		MTPInputMedia(),
		MTP_int(date),
		MTPlong(),
		MTPSuggestedPost(),
		MTPRichMessage());
}

QJsonObject SessionClient::syncFolder(const MTPDialogFilter &filter) const {
	if (filter.type() != mtpc_dialogFilter) {
		return {};
	}
	const auto &f = filter.c_dialogFilter();
	auto flags = QJsonObject{
		{"contacts", f.is_contacts()},
		{"nonContacts", f.is_non_contacts()},
		{"groups", f.is_groups()},
		{"broadcasts", f.is_broadcasts()},
		{"bots", f.is_bots()},
		{"excludeMuted", f.is_exclude_muted()},
		{"excludeRead", f.is_exclude_read()},
		{"excludeArchived", f.is_exclude_archived()},
	};
	auto result = QJsonObject{
		{"title", qs(f.vtitle().c_textWithEntities().vtext())},
		{"flags", flags}};
	if (f.vemoticon()) {
		result.insert("emoticon", qs(*f.vemoticon()));
	}
	if (f.vcolor()) {
		result.insert("color", f.vcolor()->v);
	}
	const auto peers = [&](const auto &list) {
		auto out = QJsonArray();
		for (const auto &input : list.v) {
			const auto key = syncKey(inputPeer(input));
			if (key.isEmpty()) {
				return QJsonArray{QJsonValue(QJsonValue::Null)};
			}
			if (!out.contains(key)) {
				out.push_back(key);
			}
		}
		return out;
	};
	for (const auto &pair :
		 {std::pair{"pinnedPeers", peers(f.vpinned_peers())},
		  std::pair{"includePeers", peers(f.vinclude_peers())},
		  std::pair{"excludePeers", peers(f.vexclude_peers())}}) {
		if (pair.second.contains(QJsonValue(QJsonValue::Null))) {
			return {};
		}
		result.insert(pair.first, pair.second);
	}
	return result;
}

MTPmessages_DialogFilters SessionClient::syncFilters() const {
	auto all = QMap<int, MTPDialogFilter>();
	all.insert(0, MTP_dialogFilterDefault());
	auto ids = _sync.ids();
	for (const auto value : _sync.localImport) {
		ids.insert(Key(value.toObject()));
	}
	const auto peers = [&](const QJsonArray &array) {
		auto out = QVector<MTPInputPeer>();
		for (const auto value : array) {
			const auto peer = syncPeer(value.toString());
			if (peerIsUser(peer)) {
				out.push_back(MTP_inputPeerUser(
					MTP_long(peerToUser(peer).bare), MTP_long(1)));
			} else if (peerIsChat(peer)) {
				out.push_back(
					MTP_inputPeerChat(MTP_long(peerToChat(peer).bare)));
			} else if (peerIsChannel(peer)) {
				out.push_back(MTP_inputPeerChannel(
					MTP_long(peerToChannel(peer).bare), MTP_long(1)));
			}
		}
		return MTP_vector<MTPInputPeer>(out);
	};
	for (const auto &id : ids) {
		if (!id.startsWith("folder|")) {
			continue;
		}
		const auto key = id.mid(7);
		const auto value = syncValue("folder", key);
		if (!value.isObject()) {
			continue;
		}
		const auto o = value.toObject();
		const auto b = o.value("flags").toObject();
		auto flags = MTPDdialogFilter::Flags();
		if (b.value("contacts").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_contacts;
		}
		if (b.value("nonContacts").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_non_contacts;
		}
		if (b.value("groups").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_groups;
		}
		if (b.value("broadcasts").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_broadcasts;
		}
		if (b.value("bots").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_bots;
		}
		if (b.value("excludeMuted").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_exclude_muted;
		}
		if (b.value("excludeRead").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_exclude_read;
		}
		if (b.value("excludeArchived").toBool()) {
			flags |= MTPDdialogFilter::Flag::f_exclude_archived;
		}
		if (o.contains("emoticon")) {
			flags |= MTPDdialogFilter::Flag::f_emoticon;
		}
		if (o.contains("color")) {
			flags |= MTPDdialogFilter::Flag::f_color;
		}
		all.insert(
			key.toInt(),
			MTP_dialogFilter(
				MTP_flags(flags),
				MTP_int(key.toInt()),
				MTP_textWithEntities(
					MTP_string(o.value("title").toString()),
					MTPVector<MTPMessageEntity>()),
				MTP_string(o.value("emoticon").toString()),
				MTP_int(o.value("color").toInt()),
				peers(o.value("pinnedPeers").toArray()),
				peers(o.value("includePeers").toArray()),
				peers(o.value("excludePeers").toArray())));
	}
	auto result = QVector<MTPDialogFilter>();
	for (const auto value : syncValue("folderOrder", "all").toArray()) {
		if (all.contains(value.toInt())) {
			result.push_back(all.take(value.toInt()));
		}
	}
	for (const auto &filter : all) {
		result.push_back(filter);
	}
	return MTP_messages_dialogFilters(
		MTP_flags(MTPDmessages_dialogFilters::Flags()),
		MTP_vector<MTPDialogFilter>(result));
}

void SessionClient::syncCaptureLocal() {
	if (_syncCaptured || _sync.imported || !_historyReady) {
		return;
	}
	_syncCaptured = true;
	if (!onLocalSyncState) {
		return;
	}
	auto peers = QVector<PeerId>();
	for (const auto id : _dialogs.keys()) {
		peers.push_back(PeerId(id));
	}
	const auto local = onLocalSyncState(peers);
	const auto append = [&](QString kind, QString key, QJsonValue value) {
		if (!key.isEmpty()) {
			_sync.localImport.push_back(
				QJsonObject{{"kind", kind}, {"key", key}, {"value", value}});
		}
	};
	for (auto i = local.dialogs.begin(); i != local.dialogs.end(); ++i) {
		append("dialog", syncKey(PeerId(i.key())), i.value());
	}
	for (auto i = local.drafts.begin(); i != local.drafts.end(); ++i) {
		if (i.value().type() != mtpc_draftMessage) {
			continue;
		}
		const auto &d = i.value().c_draftMessage();
		const auto draft = syncDraft(
			qs(d.vmessage()),
			d.is_no_webpage(),
			(d.vreply_to() ? std::optional(*d.vreply_to()) : std::nullopt),
			d.ventities().value_or_empty());
		if (!draft.isUndefined() && !draft.isNull()) {
			append("draft", syncKey(PeerId(i.key())), draft);
		}
	}
	auto order = QJsonArray();
	for (const auto &f : local.filters) {
		if (f.type() == mtpc_dialogFilterDefault) {
			order.push_back(0);
		} else if (f.type() == mtpc_dialogFilter) {
			const auto value = syncFolder(f);
			if (!value.isEmpty()) {
				append(
					"folder",
					QString::number(f.c_dialogFilter().vid().v),
					value);
			}
			order.push_back(f.c_dialogFilter().vid().v);
		}
	}
	if (order.size() > 1) {
		append("folderOrder", "all", order);
	}
	syncPersist();
}

void SessionClient::syncImportLocal() {
	if (!_syncReady || !_historyReady || !_syncCaptured || _sync.imported) {
		return;
	}
	for (const auto value : _sync.localImport) {
		const auto entry = value.toObject();
		const auto kind = entry.value("kind").toString();
		const auto key = entry.value("key").toString();
		if (!_sync.contains(kind, key)) {
			syncQueue(kind, key, entry.value("value"));
		}
	}
	_sync.imported = true;
	_sync.localImport = {};
	syncPersist();
	syncFlush();
}

MTPDialog SessionClient::syncDialog(PeerId peer, const MTPDialog &dialog) {
	const auto &d = dialog.c_dialog();
	const auto key = syncKey(peer);
	const auto value = syncValue("dialog", key);
	const auto state = value.toObject();
	auto flags = d.vflags().v | MTPDdialog::Flag::f_folder_id;
	if (value.isObject()) {
		flags &= ~(
			MTPDdialog::Flag::f_pinned | MTPDdialog::Flag::f_unread_mark);
		if (state.value("pinnedRank").toInt() > 0) {
			flags |= MTPDdialog::Flag::f_pinned;
		}
		if (state.value("unread").toBool()) {
			flags |= MTPDdialog::Flag::f_unread_mark;
		}
		flags |= MTPDdialog::Flag::f_folder_id;
	}
	auto draft = (d.vdraft() ? *d.vdraft() : MTPDraftMessage());
	if (!syncValue("draft", key).isUndefined()) {
		if (const auto loaded = syncNativeDraft(key)) {
			draft = *loaded;
			flags |= MTPDdialog::Flag::f_draft;
		}
	}
	return MTP_dialog(
		MTP_flags(flags),
		d.vpeer(),
		d.vtop_message(),
		d.vread_inbox_max_id(),
		d.vread_outbox_max_id(),
		d.vunread_count(),
		d.vunread_mentions_count(),
		d.vunread_reactions_count(),
		d.vunread_poll_votes_count(),
		notify(peer),
		MTP_int(d.vpts().value_or_empty()),
		draft,
		value.isObject() ? MTP_int(state.value("archived").toBool() ? 1 : 0)
						 : MTP_int(d.vfolder_id().value_or_empty()),
		MTP_int(d.vttl_period().value_or_empty()));
}

MTPmessages_PeerDialogs SessionClient::syncPinned(int folder) const {
	auto sorted = QVector<std::pair<int, MTPDialog>>();
	for (auto i = _dialogs.begin(); i != _dialogs.end(); ++i) {
		const auto &d = i.value().c_dialog();
		if (!d.is_pinned() || d.vfolder_id().value_or_empty() != folder) {
			continue;
		}
		const auto rank = syncValue("dialog", syncKey(PeerId(i.key())))
							  .toObject()
							  .value("pinnedRank")
							  .toInt(200);
		sorted.push_back({rank, i.value()});
	}
	std::stable_sort(
		sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
			return a.first < b.first;
		});
	auto dialogs = QVector<MTPDialog>();
	for (const auto &pair : sorted) {
		dialogs.push_back(pair.second);
	}
	const auto snapshotOwner = this->dialogs();
	const auto &snapshot = snapshotOwner.c_messages_dialogs();
	return MTP_messages_peerDialogs(
		MTP_vector<MTPDialog>(dialogs),
		snapshot.vmessages(),
		snapshot.vchats(),
		snapshot.vusers(),
		MTP_updates_state(
			MTP_int(_pts),
			MTPint(),
			MTP_int(int(QDateTime::currentSecsSinceEpoch())),
			MTPint(),
			MTPint()));
}

void SessionClient::syncApply(const QSet<QString> &changed) {
	if (!_historyReady) {
		return;
	}
	for (auto i = _dialogs.begin(); i != _dialogs.end(); ++i) {
		i.value() = syncDialog(PeerId(i.key()), i.value());
	}
	if (onDialogs) {
		onDialogs(dialogs());
	}
	auto events = QVector<MTPUpdate>();
	auto folders = false;
	for (const auto &id : changed) {
		const auto split = id.indexOf('|');
		const auto kind = id.left(split);
		const auto key = id.mid(split + 1);
		if (kind == "folder" || kind == "folderOrder") {
			folders = true;
		}
		if (kind != "draft"
			|| _syncAppliedDrafts.value(
				   key, QJsonValue(QJsonValue::Undefined))
				   == syncValue("draft", key)) {
			continue;
		}
		const auto peer = syncPeer(key);
		if (!peer) {
			continue;
		}
		if (const auto draft = syncNativeDraft(key)) {
			_syncAppliedDrafts[key] = syncValue("draft", key);
			events.push_back(MTP_updateDraftMessage(
				MTP_flags(MTPDupdateDraftMessage::Flags()),
				peerToMTP(peer),
				MTPint(),
				MTPPeer(),
				*draft));
		}
	}
	if (folders) {
		events.push_back(MTP_updateDialogFilters());
	}
	for (const auto folder : {0, 1}) {
		auto order = QVector<MTPDialogPeer>();
		const auto pinned = syncPinned(folder);
		for (const auto &d : pinned.c_messages_peerDialogs().vdialogs().v) {
			order.push_back(MTP_dialogPeer(d.c_dialog().vpeer()));
		}
		if (folder == 1 && order.isEmpty()) {
			continue;
		}
		events.push_back(MTP_updatePinnedDialogs(
			MTP_flags(
				MTPDupdatePinnedDialogs::Flag::f_order
				| MTPDupdatePinnedDialogs::Flag::f_folder_id),
			MTP_int(folder),
			MTP_vector<MTPDialogPeer>(order)));
	}
	if (onUpdate && !events.isEmpty()) {
		onUpdate(updates(events));
	}
}

bool SessionClient::syncRequest(mtpRequestId id, const mtpBuffer &body) {
	const auto type = uint32(body.front());
	constexpr auto handled = std::array{
		mtpc_messages_getDialogFilters,
		mtpc_messages_getPinnedDialogs,
		mtpc_messages_getAllDrafts,
		mtpc_messages_getDialogUnreadMarks,
		mtpc_messages_saveDraft,
		mtpc_messages_toggleDialogPin,
		mtpc_messages_reorderPinnedDialogs,
		mtpc_folders_editPeerFolders,
		mtpc_messages_markDialogUnread,
		mtpc_messages_updateDialogFilter,
		mtpc_messages_updateDialogFiltersOrder,
		mtpc_messages_getDialogs,
		mtpc_messages_getPeerDialogs,
	};
	if (std::find(handled.begin(), handled.end(), type) == handled.end()) {
		return false;
	}
	if (_syncRequests.contains(id)) {
		return true;
	}
	if (!_historyReady || (!_syncReady && !_syncFetched)) {
		return true;
	}
	const auto bad = [&] {
		_pending.remove(id);
		fail(id, u"NOVEO_SYNC_UNSUPPORTED_STATE"_q);
		return true;
	};
	const auto answer = [&](const auto &result) {
		_pending.remove(id);
		reply(id, result);
	};
	const auto dialogPeer = [&](const MTPInputDialogPeer &p) {
		return p.type() == mtpc_inputDialogPeer
				   ? inputPeer(p.c_inputDialogPeer().vpeer())
				   : PeerId();
	};
	if (type == mtpc_messages_getDialogs) {
		const auto r = ReadRequest<mtpc_messages_getDialogs>(body);
		if (!r) {
			return bad();
		}
		const auto folder = (r->flags.v & 2) ? r->folder_id.v : 0;
		const auto snapshotOwner = dialogs();
		const auto &snapshot = snapshotOwner.c_messages_dialogs();
		auto selected = QVector<MTPDialog>();
		for (const auto &d : snapshot.vdialogs().v) {
			if (d.type() == mtpc_dialogFolder) {
				if (!folder) {
					selected.push_back(d);
				}
			} else if (
				d.c_dialog().vfolder_id().value_or_empty() == folder
				&& (!(r->flags.v & 1) || !d.c_dialog().is_pinned())) {
				selected.push_back(d);
			}
		}
		answer(MTPmessages_Dialogs(MTP_messages_dialogs(
			MTP_vector<MTPDialog>(selected),
			snapshot.vmessages(),
			snapshot.vchats(),
			snapshot.vusers())));
		return true;
	}
	if (type == mtpc_messages_getPeerDialogs) {
		const auto r = ReadRequest<mtpc_messages_getPeerDialogs>(body);
		if (!r) {
			return bad();
		}
		const auto snapshotOwner = dialogs();
		const auto &snapshot = snapshotOwner.c_messages_dialogs();
		auto selected = QVector<MTPDialog>();
		for (const auto &input : r->peers.v) {
			if (input.type() == mtpc_inputDialogPeer
				&& _dialogs.contains(dialogPeer(input).value)) {
				selected.push_back(_dialogs.value(dialogPeer(input).value));
			} else if (input.type() == mtpc_inputDialogPeerFolder) {
				for (const auto &d : snapshot.vdialogs().v) {
					if (d.type() == mtpc_dialogFolder
						&& d.c_dialogFolder().vfolder().c_folder().vid().v
							   == input.c_inputDialogPeerFolder()
									  .vfolder_id()
									  .v) {
						selected.push_back(d);
					}
				}
			}
		}
		answer(MTPmessages_PeerDialogs(MTP_messages_peerDialogs(
			MTP_vector<MTPDialog>(selected),
			snapshot.vmessages(),
			snapshot.vchats(),
			snapshot.vusers(),
			MTP_updates_state(
				MTP_int(_pts),
				MTPint(),
				MTP_int(int(QDateTime::currentSecsSinceEpoch())),
				MTPint(),
				MTPint()))));
		return true;
	}
	if (type == mtpc_messages_getDialogFilters) {
		answer(syncFilters());
		return true;
	}
	if (type == mtpc_messages_getPinnedDialogs) {
		const auto r = ReadRequest<mtpc_messages_getPinnedDialogs>(body);
		if (!r) {
			return bad();
		}
		answer(syncPinned(r->folder_id.v));
		return true;
	}
	if (type == mtpc_messages_getDialogUnreadMarks) {
		const auto r = ReadRequest<mtpc_messages_getDialogUnreadMarks>(body);
		if (!r || r->flags.v != 0) {
			return bad();
		}
		auto peers = QVector<MTPDialogPeer>();
		for (const auto &dialog : _dialogs) {
			if (dialog.c_dialog().is_unread_mark()) {
				peers.push_back(MTP_dialogPeer(dialog.c_dialog().vpeer()));
			}
		}
		answer(MTPVector<MTPDialogPeer>(MTP_vector<MTPDialogPeer>(peers)));
		return true;
	}
	if (type == mtpc_messages_getAllDrafts) {
		auto events = QVector<MTPUpdate>();
		auto ids = _sync.ids();
		for (const auto value : _sync.localImport) {
			ids.insert(Key(value.toObject()));
		}
		for (const auto &key : ids) {
			if (!key.startsWith("draft|")) {
				continue;
			}
			const auto peer = syncPeer(key.mid(6));
			if (peer) {
				if (const auto draft = syncNativeDraft(key.mid(6))) {
					events.push_back(MTP_updateDraftMessage(
						MTP_flags(MTPDupdateDraftMessage::Flags()),
						peerToMTP(peer),
						MTPint(),
						MTPPeer(),
						*draft));
				}
			}
		}
		answer(updates(events));
		return true;
	}
	auto mutations = QJsonArray();
	const auto add = [&](QString kind, QString key, QJsonValue value) {
		mutations.push_back(
			QJsonObject{{"kind", kind}, {"key", key}, {"value", value}});
	};
	const auto pin = [&](PeerId peer, int rank) {
		add("dialog", syncKey(peer), QJsonObject{{"pinnedRank", rank}});
	};
	if (type == mtpc_messages_saveDraft) {
		const auto r = ReadRequest<mtpc_messages_saveDraft>(body);
		if (!r
			|| (r->flags.v
				& ((1 << 5) | (1 << 6) | (1 << 7) | (1 << 8) | (1 << 9)))) {
			return bad();
		}
		const auto value = syncDraft(
			qs(r->message),
			(r->flags.v & 2) != 0,
			(r->flags.v & 16) ? std::optional(r->reply_to) : std::nullopt,
			r->entities.v);
		if (value.isUndefined()) {
			return bad();
		}
		add("draft", syncKey(inputPeer(r->peer)), value);
	} else if (type == mtpc_messages_updateDialogFilter) {
		const auto r = ReadRequest<mtpc_messages_updateDialogFilter>(body);
		if (!r || r->id.v < 2 || r->id.v > 10000) {
			return bad();
		}
		const auto value = (r->flags.v & 1)
							   ? QJsonValue(syncFolder(r->filter))
							   : QJsonValue(QJsonValue::Null);
		if (value.isObject() && value.toObject().isEmpty()) {
			return bad();
		}
		add("folder", QString::number(r->id.v), value);
	} else if (type == mtpc_messages_updateDialogFiltersOrder) {
		const auto r
			= ReadRequest<mtpc_messages_updateDialogFiltersOrder>(body);
		if (!r) {
			return bad();
		}
		auto order = QJsonArray();
		for (const auto &id : r->order.v) {
			if (id.v == 1 || order.contains(id.v)) {
				return bad();
			}
			order.push_back(id.v);
		}
		add("folderOrder", "all", order);
	} else if (type == mtpc_messages_markDialogUnread) {
		const auto r = ReadRequest<mtpc_messages_markDialogUnread>(body);
		if (!r || (r->flags.v & 2)) {
			return bad();
		}
		add("dialog",
			syncKey(dialogPeer(r->peer)),
			QJsonObject{{"unread", (r->flags.v & 1) != 0}});
	} else if (type == mtpc_folders_editPeerFolders) {
		const auto r = ReadRequest<mtpc_folders_editPeerFolders>(body);
		if (!r) {
			return bad();
		}
		for (const auto &p : r->folder_peers.v) {
			const auto &folder = p.c_inputFolderPeer();
			if (folder.vfolder_id().v < 0 || folder.vfolder_id().v > 1) {
				return bad();
			}
			add("dialog",
				syncKey(inputPeer(folder.vpeer())),
				QJsonObject{
					{"archived", folder.vfolder_id().v == 1},
					{"pinnedRank", 0}});
		}
	} else if (type == mtpc_messages_toggleDialogPin) {
		const auto r = ReadRequest<mtpc_messages_toggleDialogPin>(body);
		if (!r) {
			return bad();
		}
		const auto peer = dialogPeer(r->peer);
		if (!_dialogs.contains(peer.value)) {
			return bad();
		}
		const auto folder = _dialogs.value(peer.value)
								.c_dialog()
								.vfolder_id()
								.value_or_empty();
		auto rank = 1;
		if (r->flags.v & 1) {
			pin(peer, rank++);
		} else {
			pin(peer, 0);
		}
		const auto old = syncPinned(folder);
		for (const auto &d : old.c_messages_peerDialogs().vdialogs().v) {
			const auto other = peerFromMTP(d.c_dialog().vpeer());
			if (other != peer) {
				pin(other, rank++);
			}
		}
	} else if (type == mtpc_messages_reorderPinnedDialogs) {
		const auto r = ReadRequest<mtpc_messages_reorderPinnedDialogs>(body);
		if (!r || r->folder_id.v < 0 || r->folder_id.v > 1) {
			return bad();
		}
		auto seen = QSet<uint64>();
		auto rank = 1;
		for (const auto &p : r->order.v) {
			const auto peer = dialogPeer(p);
			if (!peer || seen.contains(peer.value)) {
				return bad();
			}
			seen.insert(peer.value);
			pin(peer, rank++);
		}
		const auto old = syncPinned(r->folder_id.v);
		for (const auto &d : old.c_messages_peerDialogs().vdialogs().v) {
			const auto peer = peerFromMTP(d.c_dialog().vpeer());
			if (!seen.contains(peer.value)) {
				pin(peer, 0);
			}
		}
	}
	for (const auto value : mutations) {
		const auto entry = value.toObject();
		if (entry.value("key").toString().isEmpty()) {
			return bad();
		}
	}
	if (mutations.isEmpty()) {
		if (type == mtpc_folders_editPeerFolders) {
			answer(updates({}));
		} else {
			answer(MTPBool(MTP_boolTrue()));
		}
		return true;
	}
	_syncRequests.insert(id);
	_syncRemaining[id] = mutations.size();
	for (const auto value : mutations) {
		const auto entry = value.toObject();
		const auto kind = entry.value("kind").toString();
		const auto key = entry.value("key").toString();
		auto change = entry.value("value");
		if (kind == "dialog" && !_sync.contains(kind, key)) {
			const auto peer = syncPeer(key);
			const auto original = _dialogs.value(peer.value);
			const auto &old = original.c_dialog();
			auto merged = QJsonObject{
				{"pinnedRank",
				 syncValue(kind, key)
					 .toObject()
					 .value("pinnedRank")
					 .toInt()},
				{"archived", old.vfolder_id().value_or_empty() == 1},
				{"unread", old.is_unread_mark()}};
			const auto patch = change.toObject();
			for (auto i = patch.begin(); i != patch.end(); ++i) {
				merged.insert(i.key(), i.value());
			}
			change = merged;
		}
		syncQueue(kind, key, change, id);
	}
	syncPersist();
	syncApply(_sync.ids());
	syncFlush();
	return true;
}

}  // namespace Noveo
