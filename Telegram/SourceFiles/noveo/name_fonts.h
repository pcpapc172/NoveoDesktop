#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

class PeerData;
namespace style { struct TextStyle; }
namespace Noveo {
struct NameFont { QString id; QString label; QString file; };
const QVector<NameFont> &NameFonts();
QString NameFontFamily(const QString &id);
const style::TextStyle &NameStyle(const style::TextStyle &base, PeerData *peer);
} // namespace Noveo
