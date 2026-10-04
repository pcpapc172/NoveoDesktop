/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "noveo/auth_client.h"
#include "scheme.h"
#include "data/data_peer_id.h"

#include <QtCore/QJsonArray>
#include <QtCore/QMap>
#include <QtCore/QSet>
#include <QtNetwork/QNetworkAccessManager>

namespace Noveo {

[[nodiscard]] UserId NativeUserId(const QString &rawId);

// Translates native API data types locally; all network traffic uses Noveo.
class SessionClient final : public QObject {
public:
	explicit SessionClient(AuthClient *auth, QUrl apiEndpoint = QUrl("https://noveo.ir:8443"));
	void authenticated(const QJsonObject &profile);
	[[nodiscard]] MTPUser selfUser() const;
	void disconnected();
	void message(const QJsonObject &message);
	void request(mtpRequestId id, const mtpBuffer &body);
	void cancel(mtpRequestId id);
	void reset();

	std::function<void(mtpRequestId, mtpBuffer)> onReply;
	std::function<void(const MTPUpdates &)> onUpdate;
	std::function<void(const MTPmessages_Dialogs &)> onDialogs;
	std::function<void(PeerId, const QUrl &)> onAvatar;

private:
	struct Pending {
		mtpBuffer body;
		qint64 deadline = 0;
	};
	struct Send {
		mtpRequestId requestId = 0;
		uint64 randomId = 0;
		PeerId peer;
	};
	void drain();
	void fail(mtpRequestId id, const QString &reason);
	void contacts();
	void users(const QJsonArray &users, bool contacts = false);
	void history(const QJsonArray &chats);
	[[nodiscard]] MTPUser user(const QJsonObject &profile, bool contact = false);
	[[nodiscard]] MTPMessage nativeMessage(const QJsonObject &object, PeerId peer);
	[[nodiscard]] MTPmessages_Dialogs dialogs() const;
	void avatars();
	[[nodiscard]] PeerId inputPeer(const MTPInputPeer &peer) const;
	template <typename Response> void reply(mtpRequestId id, const Response &response) {
		mtpBuffer buffer;
		response.write(buffer);
		if (onReply) {
			onReply(id, std::move(buffer));
		}
		avatars();
	}

	AuthClient *_auth;
	QUrl _apiEndpoint;
	QNetworkAccessManager _http;
	QTimer _deadline;
	QMap<mtpRequestId, Pending> _pending;
	QMap<QString, Send> _sends;
	QMap<mtpRequestId, PeerId> _historyRequests;
	QSet<mtpRequestId> _historyAnswered;
	QSet<uint64> _historyExhausted;
	QMap<uint64, MTPUser> _users;
	QMap<uint64, QJsonObject> _profiles;
	QMap<uint64, MTPChat> _chats;
	QMap<uint64, MTPDialog> _dialogs;
	QMap<uint64, QVector<MTPMessage>> _messages;
	QMap<uint64, QString> _rawUsers;
	QMap<uint64, QString> _chatIds;
	QMap<QString, PeerId> _chatPeers;
	QMap<QString, int> _messageIds;
	QMap<int, QString> _rawMessages;
	QMap<uint64, QUrl> _avatars;
	QVector<MTPContact> _contacts;
	QString _self;
	int _nextMessage = 1000000000;
	int _olderMessage = 1000000000;
	int _pts = 0;
	bool _historyReady = false;
	bool _contactsReady = false;
	bool _contactsLoading = false;
	bool _avatarScheduled = false;

};

} // namespace Noveo
