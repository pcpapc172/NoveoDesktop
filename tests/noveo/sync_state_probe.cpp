#include "noveo/sync_state.h"
#include <QtCore/QJsonDocument>
#include <cstdlib>
#include <iostream>

QJsonObject Json(const char *text) {
	return QJsonDocument::fromJson(text).object();
}
void Check(bool condition) {
	if (!condition) std::abort();
}
int main() {
	Noveo::SyncState state;
	state.accept(Json(R"({"revision":0,"items":[]})"));
	state.queue("draft", "user:bob", Json(R"({"text":"first"})"));
	const auto first = state.next();
	state.queue("draft", "user:bob", Json(R"({"text":"newer"})"));
	state.accept(Json(R"({"revision":2,"items":[{"kind":"draft","key":"user:bob","version":2,"value":{"text":"other device"}}]})"));
	state.acknowledge(first, 1);
	Check(state.next().value("base").toInteger() == 1);
	Noveo::SyncState restored;
	restored.restore(state.serialize());
	Check(restored.next() == state.next());
	state.accept(Json(R"({"revision":0,"items":[]})"));
	Check(state.revision == 2 && state.items.size() == 1);
	state.conflict(state.next());
	Check(state.pending.isEmpty());
	Check(state.serialize().value("conflicts").toObject().value("draft|user:bob").toObject().value("value").toObject().value("text") == "newer");
	state.queue("draft", "user:bob", QJsonValue(QJsonValue::Null));
	Check(Noveo::SyncState::Request(state.next()).value("value").isNull());
	Noveo::SyncState other;
	Check(other.items.isEmpty() && other.pending.isEmpty());
	state.queue("dialog", "chat:g", Json(R"({"archived":true})"));
	state.queue("dialog", "chat:g", Json(R"({"unread":true})"));
	const auto dialog = state.value("dialog", "chat:g").toObject();
	Check(dialog.value("archived").toBool() && dialog.value("unread").toBool());
	std::cout << "PASS durable sync queue: restart, isolation, in-flight edits, stale snapshots, late acknowledgments, conflict preservation, explicit clears and merged patches\n";
}
