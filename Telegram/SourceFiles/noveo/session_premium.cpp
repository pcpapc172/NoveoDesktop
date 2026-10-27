#include "noveo/session_client.h"
#include "noveo/request_fields.h"

#include <QtCore/QDateTime>
#include <QtCore/QMimeDatabase>
#include <QtGui/QColor>
#include <algorithm>
#include <array>
#include <limits>

namespace Noveo {
namespace {

constexpr auto kProfilePalette = std::array<std::array<uint32, 2>, 20>{{
	{0xE53935, 0xFF7043},
	{0xFB8C00, 0xFDD835},
	{0x43A047, 0x9CCC65},
	{0x00897B, 0x26C6DA},
	{0x1E88E5, 0x42A5F5},
	{0x3949AB, 0x7E57C2},
	{0x8E24AA, 0xEC407A},
	{0xD81B60, 0xF06292},
	{0x6D4C41, 0xA1887F},
	{0x546E7A, 0x90A4AE},
	{0x7C4DFF, 0xFF4081},
	{0x00BCD4, 0x2962FF},
	{0x8BC34A, 0x00BFA5},
	{0xFF5722, 0xFFC107},
	{0x5C6BC0, 0x26A69A},
	{0x212121, 0x616161},
	{0xAD1457, 0x6A1B9A},
	{0x0D47A1, 0x00E5FF},
	{0x1B5E20, 0xA5D6A7},
	{0xBF360C, 0xFFAB91},
}};

int ProfileColorIndex(const QJsonObject &skin) {
	auto colors = QVector<QColor>();
	if (skin.value("mode") == "solid") {
		colors = {QColor(skin.value("color").toString()),
			QColor(skin.value("color").toString())};
	} else {
		for (const auto color : skin.value("colors").toArray()) {
			if (colors.size() == 2) {
				break;
			}
			colors.push_back(QColor(color.toString()));
		}
	}
	if (colors.size() < 2 || !colors[0].isValid() || !colors[1].isValid()) {
		return -1;
	}
	auto closest = 0;
	auto distance = std::numeric_limits<int>::max();
	for (auto i = 0; i < int(kProfilePalette.size()); ++i) {
		auto delta = 0;
		for (auto c = 0; c < 2; ++c) {
			const auto target = QColor::fromRgb(kProfilePalette[i][c]);
			for (const auto d : {colors[c].red() - target.red(),
					 colors[c].green() - target.green(),
					 colors[c].blue() - target.blue()}) {
				delta += d * d;
			}
		}
		if (delta < distance) {
			distance = delta;
			closest = i;
		}
	}
	return closest;
}

MTPhelp_PeerColors ProfileColorOptions() {
	auto options = QVector<MTPhelp_PeerColorOption>();
	for (auto i = 0; i < int(kProfilePalette.size()); ++i) {
		const auto colors = MTP_vector<MTPint>(
			{MTP_int(kProfilePalette[i][0]), MTP_int(kProfilePalette[i][1])});
		const auto set = MTP_help_peerColorProfileSet(colors, colors, colors);
		options.push_back(MTP_help_peerColorOption(
			MTP_flags(MTPDhelp_peerColorOption::Flag::f_colors
					  | MTPDhelp_peerColorOption::Flag::f_dark_colors),
			MTP_int(i),
			set,
			set,
			MTPint(),
			MTPint()));
	}
	return MTP_help_peerColors(
		MTP_int(1), MTP_vector<MTPhelp_PeerColorOption>(options));
}

} // namespace

uint64 SessionClient::registerPremiumIcon(const QJsonObject &icon) {
	const auto text = icon.value("url").toString();
	if (text.isEmpty()) {
		return 0;
	}
	const auto url = _apiEndpoint.resolved(QUrl(text));
	if (url.scheme() != "https" || url.host().isEmpty()) {
		return 0;
	}
	const auto id = NativeUserId("noveo-premium-icon:" + url.toString()).bare;
	_premiumIcons[id] = icon;
	return id;
}

MTPPeerColor SessionClient::premiumProfileColor(const QJsonObject &profile) {
	const auto color =
		ProfileColorIndex(profile.value("profileSkin").toObject());
	const auto icon =
		registerPremiumIcon(profile.value("premiumStarIcons").toObject());
	auto flags = MTPDpeerColor::Flags();
	if (color >= 0) {
		flags |= MTPDpeerColor::Flag::f_color;
	}
	if (icon) {
		flags |= MTPDpeerColor::Flag::f_background_emoji_id;
	}
	return MTP_peerColor(MTP_flags(flags), MTP_int(color), MTP_long(icon));
}

MTPDocument SessionClient::premiumIconDocument(uint64 id) const {
	const auto icon = _premiumIcons.value(id);
	if (icon.isEmpty()) {
		return MTP_documentEmpty(MTP_long(id));
	}
	const auto url = _apiEndpoint.resolved(QUrl(icon.value("url").toString()));
	const auto animated =
		icon.value("type") == "tgs" || url.path().endsWith(".tgs");
	return MTP_document(MTP_flags(MTPDdocument::Flags()),
		MTP_long(id),
		MTP_long(0),
		MTP_bytes("noveo:" + url.toString().toUtf8()),
		MTP_int(1),
		MTP_string(animated
					   ? QString("application/x-tgsticker")
					   : QMimeDatabase().mimeTypeForFile(url.path()).name()),
		MTP_long(0),
		MTPVector<MTPPhotoSize>(),
		MTPVector<MTPVideoSize>(),
		MTP_int(0),
		MTP_vector<MTPDocumentAttribute>({
			MTP_documentAttributeImageSize(MTP_int(512), MTP_int(512)),
			MTP_documentAttributeCustomEmoji(
				MTP_flags(MTPDdocumentAttributeCustomEmoji::Flag::f_free),
				MTP_string(u"⭐"_q),
				MTP_inputStickerSetEmpty()),
		}));
}

bool SessionClient::premiumRequest(mtpRequestId id, const mtpBuffer &body) {
	const auto type = uint32(body.front());
	const auto handled = {mtpc_account_getDefaultEmojiStatuses,
		mtpc_account_getRecentEmojiStatuses,
		mtpc_account_getChannelDefaultEmojiStatuses,
		mtpc_account_updateEmojiStatus,
		mtpc_account_updateColor,
		mtpc_messages_getCustomEmojiDocuments,
		mtpc_help_getPeerProfileColors};
	if (std::find(handled.begin(), handled.end(), type) == handled.end()) {
		return false;
	}
	if (!_auth->authenticated() || _transfers.contains(id)) {
		return true;
	}
	for (const auto &request : _wireRequests) {
		if (request.id == id) {
			return true;
		}
	}
	const auto reject = [&](QString error = "NOVEO_PREMIUM_UNSUPPORTED") {
		_pending.remove(id);
		fail(id, error);
		return true;
	};
	if (type == mtpc_help_getPeerProfileColors) {
		if (!ReadRequest<mtpc_help_getPeerProfileColors>(body)) {
			return reject();
		}
		_pending.remove(id);
		reply(id, ProfileColorOptions());
		return true;
	}
	if (type == mtpc_account_getRecentEmojiStatuses
		|| type == mtpc_account_getChannelDefaultEmojiStatuses) {
		_pending.remove(id);
		reply(id,
			MTPaccount_EmojiStatuses(MTP_account_emojiStatuses(
				MTP_long(1), MTPVector<MTPEmojiStatus>())));
		return true;
	}
	if (type == mtpc_account_updateEmojiStatus
		|| type == mtpc_account_updateColor) {
		auto fields = QJsonObject{{"type", "update_profile"}};
		if (type == mtpc_account_updateEmojiStatus) {
			const auto r = ReadRequest<mtpc_account_updateEmojiStatus>(body);
			if (!r) {
				return reject();
			}
			auto icon = QJsonValue(QJsonValue::Null);
			if (r->emoji_status.type() == mtpc_emojiStatus) {
				const auto &status = r->emoji_status.c_emojiStatus();
				if (status.vuntil()
					|| !_premiumIcons.contains(status.vdocument_id().v)) {
					return reject();
				}
				icon = _premiumIcons.value(status.vdocument_id().v);
			} else if (r->emoji_status.type() != mtpc_emojiStatusEmpty) {
				return reject();
			}
			fields.insert("premiumStarIcon", icon);
		} else {
			const auto r = ReadRequest<mtpc_account_updateColor>(body);
			if (!r || !(r->flags.v & 2)
				|| ((r->flags.v & 4) && r->color.type() != mtpc_peerColor)) {
				return reject();
			}
			auto color = -1;
			auto icon = uint64(0);
			if (r->flags.v & 4) {
				const auto &value = r->color.c_peerColor();
				if (value.vcolor()) {
					color = value.vcolor()->v;
				}
				icon = value.vbackground_emoji_id().value_or_empty();
			}
			if (color < -1 || color >= int(kProfilePalette.size())
				|| (icon && !_premiumIcons.contains(icon))) {
				return reject();
			}
			auto skin = QJsonValue(QJsonValue::Null);
			if (color >= 0) {
				auto colors = QJsonArray();
				for (const auto rgb : kProfilePalette[color]) {
					colors.push_back(QColor::fromRgb(rgb).name());
				}
				skin = QJsonObject{
					{"mode", "gradient"}, {"colors", colors}, {"angle", 135}};
			}
			fields.insert("profileSkin", skin);
			fields.insert("premiumStarIcons",
				icon ? QJsonValue(_premiumIcons.value(icon))
					 : QJsonValue(QJsonValue::Null));
		}
		wire(id, {fields}, {"user_updated"}, [=, this](QJsonObject frame) {
			users(
				QJsonArray{frame.value("user").isObject() ? frame.value("user")
														  : QJsonValue(frame)});
			if (onUpdate) {
				onUpdate(updates({}));
			}
			reply(id, MTPBool(MTP_boolTrue()));
		});
		return true;
	}
	auto requested = QVector<MTPlong>();
	if (type == mtpc_messages_getCustomEmojiDocuments) {
		const auto r = ReadRequest<mtpc_messages_getCustomEmojiDocuments>(body);
		if (!r) {
			return reject();
		}
		requested = r->document_id.v;
	} else if (!ReadRequest<mtpc_account_getDefaultEmojiStatuses>(body)) {
		return reject();
	}
	const auto complete = [=, this] {
		_pending.remove(id);
		if (type == mtpc_messages_getCustomEmojiDocuments) {
			auto documents = QVector<MTPDocument>();
			for (const auto document : requested) {
				if (_premiumIcons.contains(document.v)) {
					documents.push_back(premiumIconDocument(document.v));
				}
			}
			reply(
				id, MTPVector<MTPDocument>(MTP_vector<MTPDocument>(documents)));
		} else {
			auto statuses = QVector<MTPEmojiStatus>();
			for (const auto document : _premiumIcons.keys()) {
				statuses.push_back(
					MTP_emojiStatus(MTP_flags(MTPDemojiStatus::Flags()),
						MTP_long(document),
						MTPint()));
			}
			reply(id,
				MTPaccount_EmojiStatuses(MTP_account_emojiStatuses(
					MTP_long(1), MTP_vector<MTPEmojiStatus>(statuses))));
		}
	};
	if (!requested.isEmpty()
		&& std::all_of(
			requested.begin(), requested.end(), [=, this](const auto id) {
				return _premiumIcons.contains(id.v);
			})) {
		complete();
		return true;
	}
	api(id,
		"/user/premium/profile-assets",
		{},
		[=, this](QJsonObject response) {
			for (const auto &key : {"stickers", "templates"}) {
				for (const auto value : response.value(key).toArray()) {
					const auto entry = value.toObject();
					auto icon = QJsonObject{{"url", entry.value("url")},
						{"type",
							entry.value("type") == "tgs" ? "tgs" : "image"},
						{"source",
							QString::fromLatin1(key) == "stickers"
								? "saved"
								: "template"}};
					if (QString::fromLatin1(key) == "templates") {
						icon.insert("templateId", entry.value("id"));
					}
					registerPremiumIcon(icon);
				}
			}
			complete();
		});
	return true;
}

} // namespace Noveo
