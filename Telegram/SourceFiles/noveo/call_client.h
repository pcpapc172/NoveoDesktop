#pragma once

#include <QtCore/QObject>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <functional>

namespace Noveo {

// Signaling belongs to the account socket; media belongs to a per-call room.
class CallClient final : public QObject {
public:
	enum class State { Incoming, Connecting, Ringing, Active, Reconnecting, Ended };
	using TokenDone = std::function<void(QJsonObject, QString)>;
	struct Config {
		QString chatId;
		QString callId;
		QString selfId;
		bool incoming = false;
		bool group = false;
		std::function<bool(QJsonObject)> send;
		std::function<void(QString, QString, TokenDone)> token;
	};
	explicit CallClient(Config config);
	~CallClient();
	void start();
	void answer();
	void stop(bool sendLeave = true, QString error = {});
	void serverEvent(const QJsonObject &event);
	void mediaEvent(const QJsonObject &event);
	[[nodiscard]] QString chatId() const { return _config.chatId; }
	[[nodiscard]] QString callId() const { return _config.callId; }
	[[nodiscard]] State state() const { return _state; }
	std::function<void(State)> onState;
	std::function<void(QJsonObject)> onConnect;
	std::function<void()> onDisconnect;
	std::function<void(bool, QString)> onMuted;
	std::function<void(QString)> onError;
	std::function<void(int)> onParticipants;
private:
	void connectRoom();
	void setState(State state);
	void participants(const QJsonObject &event);
	bool send(const QString &type);
	Config _config;
	QTimer _timeout;
	State _state;
	bool _started = false;
	bool _joined = false;
	bool _seen = false;
	bool _wasActive = false;
	bool _screenSharing = false;
	int _remoteCount = 0;
};

} // namespace Noveo
