#include "noveo/call_client.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QJsonArray>
#include <cassert>
#include <iostream>

using Client = Noveo::CallClient;
using State = Client::State;

struct Fixture {
	QVector<QJsonObject> sent;
	Client::TokenDone tokenDone;
	QString requestedChat, requestedCall, error;
	int connected = 0, disconnected = 0;
	bool muted = false;
	std::unique_ptr<Client> client;
	explicit Fixture(bool incoming = false, bool group = false) {
		client = std::make_unique<Client>(Client::Config{
			.chatId = "chat", .callId = incoming ? "call" : QString(), .selfId = "self",
			.incoming = incoming, .group = group,
			.send = [this](QJsonObject event) { sent.push_back(event); return true; },
			.token = [this](QString chat, QString call, Client::TokenDone done) {
				requestedChat = chat; requestedCall = call; tokenDone = std::move(done);
			},
		});
		client->onConnect = [this](QJsonObject) { ++connected; };
		client->onDisconnect = [this] { ++disconnected; };
		client->onError = [this](QString value) { error = value; };
		client->onMuted = [this](bool value, QString) { muted = value; };
	}
	void token(QString url = "wss://voice.noveo.ir", QString id = "call") {
		tokenDone({ { "serverUrl", url }, { "participantToken", "room-only-token" }, { "callId", id } }, {});
	}
	void connectedEvent(QJsonArray remote = {}) {
		client->mediaEvent({ { "e", "connected" }, { "ids", remote } });
	}
	void update(QString chat = "chat", QString call = "call", QJsonArray participants = { "self", "other" }) {
		client->serverEvent({ { "type", "voice_chat_update" }, { "activeVoiceChats", QJsonObject{
			{ chat, QJsonObject{ { "callId", call }, { "participants", participants } } },
		} } });
	}
	int count(QString type) const {
		int n = 0; for (const auto &event : sent) n += event.value("type").toString() == type; return n;
	}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	{
		Fixture f;
		f.client->start(); f.client->start();
		assert(f.count("voice_start") == 1 && f.requestedChat == "chat" && f.requestedCall.isEmpty());
		assert(f.count("voice_join") == 0);
		f.token(); f.connectedEvent();
		assert(f.client->state() == State::Ringing && f.count("voice_join") == 1);
		assert(f.sent.back().value("callId") == "call");
		f.update();
		assert(f.client->state() == State::Active);
		f.client->mediaEvent({ { "e", "muted" }, { "muted", true }, { "micError", "Denied" } });
		assert(f.muted);
		f.client->mediaEvent({ { "e", "participants" }, { "ids", QJsonArray{} } });
		f.client->stop(); f.connectedEvent({ "late-peer" });
		assert(f.client->state() == State::Ended && f.count("voice_leave") == 1 && f.count("voice_join") == 1);
	}
	{
		Fixture f(true);
		f.client->start(); assert(!f.tokenDone && f.sent.isEmpty());
		f.client->answer(); f.client->answer();
		assert(f.requestedCall == "call" && f.count("voice_start") == 0);
		f.token(); f.connectedEvent({ "caller" });
		assert(f.client->state() == State::Active && f.count("voice_join") == 1);
	}
	{
		Fixture f(true); f.client->stop();
		assert(f.client->state() == State::Ended && f.sent.isEmpty() && f.connected == 0);
	}
	{
		Fixture f(true);
		f.client->serverEvent({ { "type", "voice_chat_update" }, { "activeVoiceChats", QJsonObject{} } });
		assert(f.client->state() == State::Ended);
	}
	{
		Fixture f; f.client->start(); f.client->stop(); f.token();
		assert(f.connected == 0 && f.count("voice_leave") == 1);
	}
	{
		Fixture f; f.client->start(); f.client.reset(); f.token();
		assert(f.connected == 0 && f.count("voice_leave") == 1);
	}
	for (const auto url : { "ws://voice.noveo.ir", "http://voice.noveo.ir", "wss://user:password@voice.noveo.ir" }) {
		Fixture f; f.client->start(); f.token(url);
		assert(f.client->state() == State::Ended && !f.error.isEmpty() && f.connected == 0);
	}
	{
		Fixture f(true); f.client->answer(); f.token("wss://voice.noveo.ir", "replacement");
		assert(f.connected == 0 && f.client->state() == State::Ended);
	}
	{
		Fixture f; f.client->start(); f.tokenDone({}, "Call expired");
		assert(f.error == "Call expired" && f.connected == 0 && f.client->state() == State::Ended);
	}
	{
		Fixture f; f.client->start();
		f.client->serverEvent({ { "type", "voice_chat_update" }, { "activeVoiceChats", QJsonObject{} } });
		assert(f.client->state() != State::Ended);
		f.token(); f.connectedEvent(); f.update("canonical-private-chat");
		assert(f.client->chatId() == "canonical-private-chat" && f.client->state() == State::Active);
		f.client->serverEvent({ { "type", "voice_call_ended" }, { "chatId", "unrelated-chat" } });
		assert(f.client->state() == State::Active);
		f.client->serverEvent({ { "type", "voice_chat_update" }, { "activeVoiceChats", QJsonObject{} } });
		assert(f.client->state() == State::Ended && f.count("voice_leave") == 0);
	}
	{
		Fixture f(false, true); f.client->start(); f.token("https://noveo.ir:8443/livekit"); f.connectedEvent();
		assert(f.client->state() == State::Active);
		f.client->mediaEvent({ { "e", "participants" }, { "ids", QJsonArray{} } });
		assert(f.client->state() == State::Active);
		f.client->mediaEvent({ { "e", "reconnecting" } });
		assert(f.client->state() == State::Reconnecting);
		f.client->mediaEvent({ { "e", "reconnected" } });
		assert(f.client->state() == State::Active && f.count("voice_join") == 1);
		f.client->mediaEvent({ { "e", "screenPublished" }, { "screen", false } });
		assert(f.count("voice_screen_share_started") == 0);
		f.client->mediaEvent({ { "e", "screenPublished" }, { "screen", true } });
		f.client->mediaEvent({ { "e", "screenPublished" }, { "screen", true } });
		assert(f.count("voice_screen_share_started") == 1);
		f.client->stop();
		assert(f.count("voice_screen_share_stopped") == 1 && f.count("voice_leave") == 1);
	}
	std::cout << "PASS outgoing/incoming signaling, answer/decline, exactly-once join/leave, stale room rejection, late token cancellation, deleted call callbacks, TLS-only media, HTTP errors, canonical DM identity, remote hangup, group calls and reconnect\n";
}
