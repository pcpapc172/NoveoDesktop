#include "noveo/call_client.h"

#include <QtCore/QJsonArray>
#include <QtCore/QPointer>
#include <QtCore/QUrl>

namespace Noveo {

CallClient::CallClient(Config config)
: _config(std::move(config))
, _state(_config.incoming ? State::Incoming : State::Connecting)
, _seen(_config.incoming) {
	_timeout.setSingleShot(true);
	connect(&_timeout, &QTimer::timeout, this, [this] {
		stop(true, QString("The call was not answered."));
	});
	if (_config.incoming) _timeout.start(60000);
}

CallClient::~CallClient() {
	// Leave once, also when the account logs out or the application quits.
	if (_state != State::Ended && _started) {
		if (_screenSharing) send("voice_screen_share_stopped");
		send("voice_leave");
	}
}

bool CallClient::send(const QString &type) {
	auto action = QJsonObject{ { "type", type }, { "chatId", _config.chatId } };
	if (!_config.callId.isEmpty()) action.insert("callId", _config.callId);
	return _config.send && !_config.chatId.isEmpty() && _config.send(std::move(action));
}

void CallClient::setState(State state) {
	if (_state == state || _state == State::Ended) return;
	_state = state;
	if (state == State::Active) {
		_wasActive = true;
		_timeout.stop();
	}
	if (onState) onState(state);
}

void CallClient::start() {
	if (_state == State::Ended || _started || _config.incoming) return;
	_started = true;
	_timeout.start(45000);
	if (!send("voice_start")) {
		stop(false, QString("Could not connect to Noveo. Please try again."));
		return;
	}
	connectRoom();
}

void CallClient::answer() {
	if (_state != State::Incoming || _started) return;
	_started = true;
	_timeout.start(30000);
	connectRoom();
}

void CallClient::connectRoom() {
	setState(State::Connecting);
	const auto weak = QPointer<CallClient>(this);
	if (!_config.token) { stop(true, QString("The call service is unavailable.")); return; }
	_config.token(_config.chatId, _config.callId, [weak](QJsonObject token, QString error) {
		if (!weak || weak->_state == State::Ended) return;
		const auto url = QUrl(token.value("serverUrl").toString());
		if (!error.isEmpty() || (url.scheme() != "wss" && url.scheme() != "https") || url.host().isEmpty()
			|| !url.userInfo().isEmpty() || token.value("participantToken").toString().isEmpty()
			|| token.value("callId").toString().isEmpty()
			|| (!weak->_config.callId.isEmpty() && token.value("callId").toString() != weak->_config.callId)) {
			weak->stop(true, error.isEmpty() ? QString("Could not connect to the call.") : error);
			return;
		}
		weak->_config.callId = token.value("callId").toString();
		if (weak->onConnect) weak->onConnect(std::move(token));
	});
}

void CallClient::stop(bool sendLeave, QString error) {
	if (_state == State::Ended) return;
	_timeout.stop();
	if (sendLeave && _started) {
		if (_screenSharing) send("voice_screen_share_stopped");
		send("voice_leave");
	}
	_state = State::Ended;
	if (onDisconnect) onDisconnect();
	if (!error.isEmpty() && onError) onError(std::move(error));
	if (onState) onState(State::Ended);
}

void CallClient::serverEvent(const QJsonObject &event) {
	if (_state == State::Ended) return;
	const auto type = event.value("type").toString();
	if (type == "voice_chat_update") {
		const auto active = event.value("activeVoiceChats");
		if (!active.isObject()) return;
		const auto calls = active.toObject();
		auto mine = calls.value(_config.chatId).toObject();
		if (!_config.callId.isEmpty() && mine.value("callId").toString() != _config.callId) mine = {};
		if (mine.isEmpty() && !_config.callId.isEmpty()) {
			for (auto i = calls.begin(); i != calls.end(); ++i) {
				if (i.value().toObject().value("callId").toString() == _config.callId) {
					_config.chatId = i.key(); // temp_<user> became a persistent DM.
					mine = i.value().toObject();
					break;
				}
			}
		}
		if (mine.isEmpty()) {
			if (_seen) stop(false);
			return;
		}
		_seen = true;
		if (_config.callId.isEmpty()) _config.callId = mine.value("callId").toString();
		_remoteCount = 0;
		for (const auto participant : mine.value("participants").toArray()) {
			if (participant.toString() != _config.selfId) ++_remoteCount;
		}
		if (onParticipants) onParticipants(_remoteCount + (_joined ? 1 : 0));
		if (_joined && (_config.group || _remoteCount > 0)) setState(State::Active);
	} else if (type == "voice_call_error" || type == "voice_call_ended") {
		const auto chat = event.value("chatId").toString();
		const auto call = event.value("callId").toString();
		if ((!chat.isEmpty() && chat != _config.chatId)
			|| (!call.isEmpty() && !_config.callId.isEmpty() && call != _config.callId)) return;
		stop(false, type == "voice_call_error"
			? event.value("message").toString(QString("Could not connect to the call.")) : QString());
	}
}

void CallClient::participants(const QJsonObject &event) {
	_remoteCount = event.value("ids").toArray().size();
	if (!_joined) return;
	if (onParticipants) onParticipants(_remoteCount + 1);
	if (_config.group || _remoteCount > 0) setState(State::Active);
	else if (_wasActive && _state != State::Reconnecting) stop(true);
}

void CallClient::mediaEvent(const QJsonObject &event) {
	if (_state == State::Ended) return;
	const auto type = event.value("e").toString();
	if (type == "connected") {
		if (!_joined) {
			_joined = true;
			if (!send("voice_join")) {
				stop(true, QString("Could not connect to Noveo. Please try again."));
				return;
			}
		}
		if (onMuted) onMuted(event.value("muted").toBool(), event.value("micError").toString());
		setState(State::Ringing);
		participants(event);
	} else if (type == "participants") {
		participants(event);
	} else if (type == "muted") {
		if (onMuted) onMuted(event.value("muted").toBool(), event.value("micError").toString());
	} else if (type == "reconnecting") {
		setState(State::Reconnecting);
		_timeout.start(30000);
	} else if (type == "reconnected") {
		setState((_config.group || _remoteCount > 0) ? State::Active : State::Ringing);
		if (_state == State::Ringing) _timeout.start(45000);
	} else if (type == "screenPublished" && event.value("screen").toBool()) {
		if (!_screenSharing) { _screenSharing = true; send("voice_screen_share_started"); }
	} else if (type == "screenStopped") {
		if (_screenSharing) { _screenSharing = false; send("voice_screen_share_stopped"); }
	} else if (type == "disconnected" || type == "error") {
		stop(true, event.value("message").toString());
	}
}

} // namespace Noveo
