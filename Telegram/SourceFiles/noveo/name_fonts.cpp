#include "noveo/name_fonts.h"
#include "data/data_peer.h"
#include "styles/style_widgets.h"
#include <QtGui/QFontDatabase>
#include <QtCore/QMap>
#include <map>

namespace Noveo {
const QVector<NameFont> &NameFonts() {
    static const auto fonts = QVector<NameFont>{
        {u""_q, u"Default"_q, u""_q},
        {u"8514oem"_q, u"OEM Terminal"_q, u""_q},
        {u"bradhitc"_q, u"Bradley Hand"_q, u"noveo_bradhitc.ttf"_q},
        {u"coure"_q, u"Courier"_q, u""_q},
        {u"freescpt"_q, u"Script"_q, u"noveo_freescpt.ttf"_q},
        {u"f_majik"_q, u"Majik"_q, u"noveo_f_majik.ttf"_q},
        {u"impact"_q, u"Impact"_q, u"noveo_impact.ttf"_q},
        {u"npidivani"_q, u"Divani"_q, u"noveo_npidivani.ttf"_q},
        {u"papyrus"_q, u"Papyrus"_q, u"noveo_papyrus.ttf"_q},
        {u"comicpapyrus"_q, u"Comic Papyrus"_q, u"noveo_comicpapyrus.ttf"_q},
        {u"comic"_q, u"Comic Sans MS"_q, u"noveo_comic.ttf"_q},
        {u"comicbold"_q, u"Comic Sans MS Bold"_q, u"noveo_comicbold.ttf"_q},
        {u"diploma"_q, u"Diploma"_q, u"noveo_diploma.ttf"_q},
        {u"jamburafood"_q, u"Jambura Food"_q, u"noveo_jamburafood.otf"_q},
        {u"minecraftseven"_q, u"Minecraft Seven"_q, u"noveo_minecraftseven.ttf"_q},
        {u"minecraftten"_q, u"MinecraftTen"_q, u"noveo_minecraftten.ttf"_q},
        {u"pricedownbl"_q, u"Pricedown BL"_q, u"noveo_pricedownbl.otf"_q},
        {u"reeniebeanie"_q, u"ReenieBeanie"_q, u"noveo_reeniebeanie.ttf"_q},
        {u"roguepunk"_q, u"Roguepunk"_q, u"noveo_roguepunk.ttf"_q},
        {u"tanjiro"_q, u"Tanjiro"_q, u"noveo_tanjiro.otf"_q},
        {u"welcotewelcome"_q, u"Welcote Welcome"_q, u"noveo_welcotewelcome.otf"_q},
        {u"nastaliq"_q, u"Nastaliq"_q, u"noveo_nastaliq.ttf"_q},
        {u"irani_sans"_q, u"Iranian Sans"_q, u"noveo_irani_sans.ttf"_q},
        {u"irani_sansbold"_q, u"Iranian Sans Bold"_q, u"noveo_irani_sansbold.ttf"_q},
    };
    return fonts;
}

QString NameFontFamily(const QString &id) {
    if (id.isEmpty()) return {};
    static auto families = QMap<QString, QString>();
    if (families.contains(id)) return families.value(id);
    auto family = QString();
    for (const auto &font : NameFonts()) {
        if (font.id != id) continue;
        if (font.file.isEmpty()) {
            family = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
        } else {
            const auto index = QFontDatabase::addApplicationFont(u":/gui/noveo/fonts/"_q + font.file);
            const auto loaded = QFontDatabase::applicationFontFamilies(index);
            if (!loaded.isEmpty()) family = loaded.front();
        }
        break;
    }
    families.insert(id, family);
    return family;
}

const style::TextStyle &NameStyle(const style::TextStyle &base, PeerData *peer) {
    if (!peer || !peer->isUser()) return base;
    const auto family = NameFontFamily(peer->noveoNicknameFont());
    if (family.isEmpty()) return base;
    static auto styles = std::map<std::pair<const style::TextStyle *, QString>, style::TextStyle>();
    auto &result = styles[{&base, family}];
    result = base;
    result.font = style::font(base.font->size(), base.font->flags(), family);
    return result;
}
} // namespace Noveo
