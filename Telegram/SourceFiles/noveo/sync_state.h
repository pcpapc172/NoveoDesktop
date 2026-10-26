#pragma once

#include <QtCore/QJsonArray>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>

namespace Noveo {

class SyncState final {
public:
	void restore(const QJsonObject &saved);
	[[nodiscard]] QJsonObject serialize() const;
	[[nodiscard]] static QString Id(const QString &kind, const QString &key);
	[[nodiscard]] qint64 version(const QString &id) const;
	[[nodiscard]] bool contains(
		const QString &kind, const QString &key) const;
	[[nodiscard]] QJsonValue value(
		const QString &kind, const QString &key) const;
	[[nodiscard]] QJsonValue serverValue(
		const QString &kind, const QString &key) const;
	[[nodiscard]] QSet<QString> ids() const;
	qint64 queue(const QString &kind, const QString &key, QJsonValue value);
	QSet<QString> accept(const QJsonObject &frame);
	[[nodiscard]] QJsonObject next();
	[[nodiscard]] static QJsonObject Request(const QJsonObject &entry);
	void acknowledge(const QJsonObject &sent, qint64 version);
	void conflict(const QJsonObject &sent);

	qint64 revision = -1;
	bool imported = false;
	QJsonArray localImport;
	QJsonObject pending;
	QJsonObject items;

private:
	QJsonObject _conflicts;
	qint64 _sequence = 0;
};

}  // namespace Noveo
