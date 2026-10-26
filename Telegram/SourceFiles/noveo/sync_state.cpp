#include "noveo/sync_state.h"

namespace Noveo {

void SyncState::restore(const QJsonObject &saved) {
	items = saved.value("items").toObject();
	pending = saved.value("pending").toObject();
	_conflicts = saved.value("conflicts").toObject();
	revision = saved.value("revision").toInteger(-1);
	_sequence = saved.value("sequence").toInteger();
	imported = saved.value("imported").toBool();
	localImport = saved.value("localImport").toArray();
}

QJsonObject SyncState::serialize() const {
	return {
		{"items", items},
		{"pending", pending},
		{"conflicts", _conflicts},
		{"revision", revision},
		{"sequence", _sequence},
		{"imported", imported},
		{"localImport", localImport},
	};
}

QString SyncState::Id(const QString &kind, const QString &key) {
	return kind + '|' + key;
}

qint64 SyncState::version(const QString &id) const {
	return items.value(id).toObject().value("version").toInteger();
}

bool SyncState::contains(const QString &kind, const QString &key) const {
	const auto id = Id(kind, key);
	return items.contains(id) || pending.contains(id);
}

QJsonValue SyncState::serverValue(
	const QString &kind, const QString &key) const {
	return items.value(Id(kind, key)).toObject().value("value");
}

QJsonValue SyncState::value(const QString &kind, const QString &key) const {
	const auto id = Id(kind, key);
	const auto local = pending.value(id).toObject();
	const auto value = serverValue(kind, key);
	if (local.isEmpty()) {
		return value;
	}
	const auto change = local.value("value");
	if (kind == "dialog" && change.isObject()) {
		auto merged = value.toObject();
		const auto patch = change.toObject();
		for (auto i = patch.begin(); i != patch.end(); ++i) {
			merged.insert(i.key(), i.value());
		}
		return merged;
	}
	return change;
}

QSet<QString> SyncState::ids() const {
	auto result = QSet<QString>();
	for (auto i = items.begin(); i != items.end(); ++i) {
		result.insert(i.key());
	}
	for (auto i = pending.begin(); i != pending.end(); ++i) {
		result.insert(i.key());
	}
	return result;
}

qint64 SyncState::queue(
	const QString &kind, const QString &key, QJsonValue value) {
	const auto id = Id(kind, key);
	const auto prior = pending.value(id).toObject();
	if (kind == "dialog" && value.isObject()) {
		auto merged = prior.value("value").toObject();
		const auto patch = value.toObject();
		for (auto i = patch.begin(); i != patch.end(); ++i) {
			merged.insert(i.key(), i.value());
		}
		value = merged;
	}
	pending.insert(
		id,
		QJsonObject{
			{"kind", kind},
			{"key", key},
			{"value", value},
			{"base",
			 prior.isEmpty() ? (revision < 0 ? -1 : version(id))
							 : prior.value("base").toInteger(-1)},
			{"sequence", ++_sequence},
		});
	return _sequence;
}

QSet<QString> SyncState::accept(const QJsonObject &frame) {
	auto changed = QSet<QString>();
	const auto next = frame.value("revision").toInteger(-1);
	if (next < revision) {
		return changed;
	}
	if (frame.value("items").isArray()) {
		auto replacement = QJsonObject();
		for (const auto value : frame.value("items").toArray()) {
			const auto item = value.toObject();
			const auto id = Id(
				item.value("kind").toString(), item.value("key").toString());
			if (items.value(id) != item) {
				changed.insert(id);
			}
			replacement.insert(id, item);
		}
		items = replacement;
	} else if (frame.value("item").isObject()) {
		const auto item = frame.value("item").toObject();
		const auto id = Id(
			item.value("kind").toString(), item.value("key").toString());
		if (item.value("version").toInteger() >= version(id)) {
			if (items.value(id) != item) {
				changed.insert(id);
			}
			items.insert(id, item);
		}
	} else {
		return changed;
	}
	revision = next;
	return changed;
}

QJsonObject SyncState::next() {
	if (pending.isEmpty()) {
		return {};
	}
	const auto id = pending.begin().key();
	auto entry = pending.value(id).toObject();
	if (entry.value("base").toInteger(-1) < 0) {
		entry.insert("base", version(id));
		pending.insert(id, entry);
	}
	return entry;
}

QJsonObject SyncState::Request(const QJsonObject &entry) {
	return {
		{"kind", entry.value("kind")},
		{"key", entry.value("key")},
		{"value", entry.value("value")},
		{"expectedVersion", entry.value("base")},
	};
}

void SyncState::acknowledge(const QJsonObject &sent, qint64 version) {
	const auto id
		= Id(sent.value("kind").toString(), sent.value("key").toString());
	auto latest = pending.value(id).toObject();
	if (latest.isEmpty()) {
		return;
	}
	if (latest.value("sequence") == sent.value("sequence")) {
		pending.remove(id);
	} else {
		latest.insert("base", version);
		pending.insert(id, latest);
	}
}

void SyncState::conflict(const QJsonObject &sent) {
	const auto id
		= Id(sent.value("kind").toString(), sent.value("key").toString());
	if (pending.contains(id)) {
		_conflicts.insert(id, pending.take(id));
		while (_conflicts.size() > 100) {
			_conflicts.erase(_conflicts.begin());
		}
	}
}

}  // namespace Noveo
