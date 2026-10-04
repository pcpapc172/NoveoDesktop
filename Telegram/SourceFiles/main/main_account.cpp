/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "main/main_account.h"

#include "noveo/auth_client.h"
#include "noveo/session_client.h"
#include "ui/image/image_location.h"
#include "lang/lang_keys.h"
#include <QtCore/QJsonDocument>

#include "base/platform/base_platform_info.h"
#include "core/application.h"
#include "storage/storage_account.h"
#include "storage/storage_domain.h" // Storage::StartResult.
#include "storage/serialize_common.h"
#include "storage/serialize_peer.h"
#include "storage/localstorage.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "data/data_chat.h"
#include "data/data_channel.h"
#include "data/data_changes.h"
#include "window/window_controller.h"
#include "media/audio/media_audio.h"
#include "mtproto/mtproto_config.h"
#include "mainwidget.h"
#include "api/api_updates.h"
#include "apiwrap.h"
#include "ui/ui_utility.h"
#include "main/main_app_config.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "main/main_session_settings.h"

namespace Main {
namespace {

constexpr auto kWideIdsTag = ~uint64(0);
constexpr auto kNoveoAuthorizationTag = quint32(0x4E4F5645);

[[nodiscard]] QString ComposeDataString(const QString &dataName, int index) {
	auto result = dataName;
	result.replace('#', QString());
	if (index > 0) {
		result += '#' + QString::number(index + 1);
	}
	return result;
}

} // namespace

Account::Account(not_null<Domain*> domain, const QString &dataName, int index)
: _domain(domain)
, _local(std::make_unique<Storage::Account>(
	this,
	ComposeDataString(dataName, index))) {
}

Account::~Account() {
	if (_noveo) _noveo->cancel();
	if (const auto session = maybeSession()) {
		session->saveSettingsNowIfNeeded();
		_local->writeSearchSuggestionsIfNeeded();
	}
	destroySession(DestroyReason::Quitting);
}

Storage::Domain &Account::domainLocal() const {
	return _domain->local();
}

[[nodiscard]] Storage::StartResult Account::legacyStart(
		const QByteArray &passcode) {
	Expects(!_appConfig);

	return _local->legacyStart(passcode);
}

std::unique_ptr<MTP::Config> Account::prepareToStart(
		std::shared_ptr<MTP::AuthKey> localKey) {
	return _local->start(std::move(localKey));
}

void Account::start(std::unique_ptr<MTP::Config> config) {
	_appConfig = std::make_unique<AppConfig>(this);
	startNoveoRuntime(config
		? std::move(config)
		: std::make_unique<MTP::Config>(
			Core::App().fallbackProductionConfig()));
	prepareNoveoClient();
	const auto authorization = _noveo->authorization();
	if (!authorization.isEmpty()) _noveo->restore(authorization);
	watchSessionChanges();
}

void Account::prepareToStartAdded(
		std::shared_ptr<MTP::AuthKey> localKey) {
	_local->startAdded(std::move(localKey));
}

void Account::watchSessionChanges() {
	sessionChanges(
	) | rpl::on_next([=](Session *session) {
		if (!session && _mtp) {
			_mtp->setUserPhone(QString());
		}
	}, _lifetime);
}

uint64 Account::willHaveSessionUniqueId(MTP::Config *config) const {
	// See also Session::uniqueId.
	if (!_sessionUserId) {
		return 0;
	}
	return _sessionUserId.bare
		| (config && config->isTestMode() ? 0x0100'0000'0000'0000ULL : 0ULL);
}

void Account::createSession(
		const MTPUser &user,
		std::unique_ptr<SessionSettings> settings) {
	createSession(
		user,
		QByteArray(),
		0,
		settings ? std::move(settings) : std::make_unique<SessionSettings>());
}

void Account::createSession(
		UserId id,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	DEBUG_LOG(("sessionUserSerialized.size: %1").arg(serialized.size()));
	QDataStream peekStream(serialized);
	const auto phone = Serialize::peekUserPhone(streamVersion, peekStream);
	const auto flags = MTPDuser::Flag::f_self | (phone.isEmpty()
		? MTPDuser::Flag()
		: MTPDuser::Flag::f_phone);

	createSession(
		MTP_user(
			MTP_flags(flags),
			MTP_long(base::take(_sessionUserId).bare),
			MTPlong(), // access_hash
			MTPstring(), // first_name
			MTPstring(), // last_name
			MTPstring(), // username
			MTP_string(phone),
			MTPUserProfilePhoto(),
			MTPUserStatus(),
			MTPint(), // bot_info_version
			MTPVector<MTPRestrictionReason>(),
			MTPstring(), // bot_inline_placeholder
			MTPstring(), // lang_code
			MTPEmojiStatus(),
			MTPVector<MTPUsername>(),
			MTPRecentStory(),
			MTPPeerColor(), // color
			MTPPeerColor(), // profile_color
			MTPint(), // bot_active_users
			MTPlong(), // bot_verification_icon
			MTPlong(), // send_paid_messages_stars
			MTPlong()), // linked_community_id
		serialized,
		streamVersion,
		std::move(settings));
}

void Account::createSession(
		const MTPUser &user,
		QByteArray serialized,
		int streamVersion,
		std::unique_ptr<SessionSettings> settings) {
	Expects(_mtp != nullptr);
	Expects(_session == nullptr);
	Expects(_sessionValue.current() == nullptr);

	_session = std::make_unique<Session>(this, user, std::move(settings));
	if (!serialized.isEmpty()) {
		local().readSelf(_session.get(), serialized, streamVersion);
	}
	_sessionValue = _session.get();

	Ensures(_session != nullptr);
}

void Account::destroySession(DestroyReason reason) {
	_storedSessionSettings.reset();
	_sessionUserId = 0;
	_sessionUserSerialized = {};
	if (!sessionExists()) {
		return;
	}

	// Assigning _sessionValue fires sessionChanges() synchronously, and a
	// listener may enter a nested event dispatch that drains crl::on_main.
	// Nothing may delete this Account while we're still on the stack.
	_destroyingSession = true;
	_sessionValue = nullptr;

	if (reason == DestroyReason::LoggedOut) {
		_session->finishLogout();
	}
	_session = nullptr;
	_destroyingSession = false;
}

bool Account::destroyingSession() const {
	return _destroyingSession;
}

bool Account::sessionExists() const {
	return (_sessionValue.current() != nullptr);
}

Session &Account::session() const {
	Expects(sessionExists());

	return *_sessionValue.current();
}

Session *Account::maybeSession() const {
	return _sessionValue.current();
}

rpl::producer<Session*> Account::sessionValue() const {
	return _sessionValue.value();
}

rpl::producer<Session*> Account::sessionChanges() const {
	return _sessionValue.changes();
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpValue() const {
	return _mtpValue.value() | rpl::map([](MTP::Instance *instance) {
		return not_null{ instance };
	});
}

rpl::producer<not_null<MTP::Instance*>> Account::mtpMainSessionValue() const {
	return mtpValue() | rpl::map([=](not_null<MTP::Instance*> instance) {
		return instance->mainDcIdValue() | rpl::map_to(instance);
	}) | rpl::flatten_latest();
}

rpl::producer<MTPUpdates> Account::mtpUpdates() const {
	return _mtpUpdates.events();
}

rpl::producer<> Account::mtpNewSessionCreated() const {
	return _mtpNewSessionCreated.events();
}

void Account::setMtpMainDcId(MTP::DcId mainDcId) {
	Expects(!_mtp);

	_mtpFields.mainDcId = mainDcId;
}

void Account::setLegacyMtpKey(std::shared_ptr<MTP::AuthKey> key) {
	Expects(!_mtp);
	Expects(key != nullptr);

	_mtpFields.keys.push_back(std::move(key));
}

QByteArray Account::serializeMtpAuthorization() const {
	const auto serialize = [&](
			MTP::DcId mainDcId,
			const MTP::AuthKeysList &keys,
			const MTP::AuthKeysList &keysToDestroy) {
		const auto keysSize = [](auto &list) {
			const auto keyDataSize = MTP::AuthKey::Data().size();
			return sizeof(qint32)
				+ list.size() * (sizeof(qint32) + keyDataSize);
		};
		const auto writeKeys = [](
				QDataStream &stream,
				const MTP::AuthKeysList &keys) {
			stream << qint32(keys.size());
			for (const auto &key : keys) {
				stream << qint32(key->dcId());
				key->write(stream);
			}
		};

		auto result = QByteArray();
		// wide tag + userId + mainDcId
		auto size = 2 * sizeof(quint64) + sizeof(qint32);
		size += keysSize(keys) + keysSize(keysToDestroy);
		result.reserve(size);
		{
			QDataStream stream(&result, QIODevice::WriteOnly);
			stream.setVersion(QDataStream::Qt_5_1);

			const auto currentUserId = sessionExists()
				? session().userId()
				: UserId();
			stream
				<< quint64(kWideIdsTag)
				<< quint64(currentUserId.bare)
				<< qint32(mainDcId);
			writeKeys(stream, keys);
			writeKeys(stream, keysToDestroy);
			if (_noveo && !_noveo->authorization().isEmpty()) {
				stream << kNoveoAuthorizationTag
					<< QJsonDocument(_noveo->authorization()).toJson(QJsonDocument::Compact);
			}

			DEBUG_LOG(("MTP Info: Keys written, userId: %1, dcId: %2"
				).arg(currentUserId.bare
				).arg(mainDcId));
		}
		return result;
	};
	return serialize(_mtp ? _mtp->mainDcId() : _mtpFields.mainDcId, {}, {});
}

void Account::setSessionUserId(UserId userId) {
	Expects(!sessionExists());

	_sessionUserId = userId;
}

void Account::setSessionFromStorage(
		std::unique_ptr<SessionSettings> data,
		QByteArray &&selfSerialized,
		int32 selfStreamVersion) {
	Expects(!sessionExists());

	DEBUG_LOG(("sessionUserSerialized set: %1"
		).arg(selfSerialized.size()));

	_storedSessionSettings = std::move(data);
	_sessionUserSerialized = std::move(selfSerialized);
	_sessionUserStreamVersion = selfStreamVersion;
}

SessionSettings *Account::getSessionSettings() {
	if (_sessionUserId) {
		return _storedSessionSettings
			? _storedSessionSettings.get()
			: nullptr;
	} else if (const auto session = maybeSession()) {
		return &session->settings();
	}
	return nullptr;
}

void Account::setMtpAuthorization(const QByteArray &serialized) {
	Expects(!_mtp);

	QDataStream stream(serialized);
	stream.setVersion(QDataStream::Qt_5_1);

	auto legacyUserId = Serialize::read<qint32>(stream);
	auto legacyMainDcId = Serialize::read<qint32>(stream);
	auto userId = quint64();
	auto mainDcId = qint32();
	if (((uint64(legacyUserId) << 32) | uint64(legacyMainDcId))
		== kWideIdsTag) {
		userId = Serialize::read<quint64>(stream);
		mainDcId = Serialize::read<qint32>(stream);
	} else {
		userId = legacyUserId;
		mainDcId = legacyMainDcId;
	}
	if (stream.status() != QDataStream::Ok) {
		LOG(("MTP Error: "
			"Could not read main fields from mtp authorization."));
		return;
	}

	setSessionUserId(userId);
	_mtpFields.mainDcId = mainDcId;

	const auto readKeys = [&](auto &keys) {
		const auto count = Serialize::read<qint32>(stream);
		if (stream.status() != QDataStream::Ok) {
			LOG(("MTP Error: "
				"Could not read keys count from mtp authorization."));
			return;
		}
		keys.reserve(count);
		for (auto i = 0; i != count; ++i) {
			const auto dcId = Serialize::read<qint32>(stream);
			const auto keyData = Serialize::read<MTP::AuthKey::Data>(stream);
			if (stream.status() != QDataStream::Ok) {
				LOG(("MTP Error: "
					"Could not read key from mtp authorization."));
				return;
			}
			keys.push_back(std::make_shared<MTP::AuthKey>(MTP::AuthKey::Type::ReadFromFile, dcId, keyData));
		}
	};
	readKeys(_mtpFields.keys);
	auto legacyKeysToDestroy = MTP::AuthKeysList();
	readKeys(legacyKeysToDestroy);
	_mtpFields.keys.clear();
	if (!stream.atEnd() && stream.status() == QDataStream::Ok) {
		auto tag = quint32();
		auto authorization = QByteArray();
		stream >> tag >> authorization;
		if (stream.status() == QDataStream::Ok && tag == kNoveoAuthorizationTag) {
			prepareNoveoClient();
			_noveo->restore(QJsonDocument::fromJson(authorization).object(), false);
		}
	}
	LOG(("MTP Info: "
		"read keys, current: %1, to destroy: %2"
		).arg(_mtpFields.keys.size()
		).arg(legacyKeysToDestroy.size()));
}

void Account::startNoveoRuntime(std::unique_ptr<MTP::Config> config) {
	Expects(!_mtp);

	auto fields = base::take(_mtpFields);
	fields.config = std::move(config);
	fields.deviceModel = Platform::DeviceModelPretty();
	fields.systemVersion = Platform::SystemVersionPretty();
	_mtp = std::make_unique<MTP::Instance>(
		MTP::Instance::Mode::Noveo,
		std::move(fields));
	_mtp->setNoveoRequestHandler([this](mtpRequestId id, const mtpBuffer &body) {
		prepareNoveoClient();
		_noveoApi->request(id, body);
	}, [this](mtpRequestId id) {
		if (_noveoApi) _noveoApi->cancel(id);
	});

	const auto writingConfig = _lifetime.make_state<bool>(false);
	rpl::merge(
		_mtp->config().updates(),
		_mtp->dcOptions().changed() | rpl::to_empty
	) | rpl::filter([=] {
		return !*writingConfig;
	}) | rpl::on_next([=] {
		*writingConfig = true;
		Ui::PostponeCall(_mtp.get(), [=] {
			local().writeMtpConfig();
			*writingConfig = false;
		});
	}, _lifetime);

	_mtpFields.mainDcId = _mtp->mainDcId();

	_mtp->setStateChangedHandler([this](MTP::ShiftedDcId, int32) {
		Core::App().settings().proxy().connectionTypeChangesNotify();
	});

	if (_sessionUserId && _noveo && !_noveo->authorization().isEmpty()) {
		// Older Noveo builds used 56 bits, overlapping the native peer type.
		const auto correctedId = std::max(
			_sessionUserId.bare & PeerId::kChatTypeMask,
			uint64(1));
		if (_sessionUserId.bare != correctedId) {
			_sessionUserId = correctedId;
			_sessionUserSerialized.clear();
		}
		createSession(
			_sessionUserId,
			base::take(_sessionUserSerialized),
			base::take(_sessionUserStreamVersion),
			(_storedSessionSettings
				? std::move(_storedSessionSettings)
				: std::make_unique<SessionSettings>()));
	}
	_storedSessionSettings = nullptr;

	if (const auto session = maybeSession()) {
		// Skip all pending self updates so that we won't local().writeSelf.
		session->changes().sendNotifications();
	}

	_mtpValue = _mtp.get();
}

void Account::prepareNoveoClient() {
	if (_noveo) return;
	_noveo = std::make_unique<Noveo::AuthClient>();
	_noveoApi = std::make_unique<Noveo::SessionClient>(_noveo.get());
	_noveoApi->onReply = [this](mtpRequestId id, mtpBuffer buffer) {
		if (!_mtp) return;
		auto response = MTP::Response();
		response.requestId = id;
		response.reply = std::move(buffer);
		_mtp->processCallback(response);
	};
	_noveoApi->onUpdate = [this](const MTPUpdates &updates) {
		_mtpUpdates.fire_copy(updates);
	};
	_noveoApi->onDialogs = [this](const MTPmessages_Dialogs &dialogs) {
		const auto session = maybeSession();
		if (!session) return;
		const auto &data = dialogs.c_messages_dialogs();
		session->data().processUsers(data.vusers());
		session->data().processChats(data.vchats());
		for (const auto &chat : data.vchats().v) {
			const auto peerId = chat.match([](const MTPDchannel &c) { return peerFromChannel(c.vid().v); },
				[](const MTPDchat &c) { return peerFromChat(c.vid().v); },
				[](const auto &) { return PeerId(); });
			if (!peerId) continue;
			const auto peer = session->data().peer(peerId);
			const auto allowed = Data::AllowedReactions{ .type = Data::AllowedReactionsType::All };
			if (const auto channel = peer->asChannel()) channel->setAllowedReactions(allowed);
			if (const auto group = peer->asChat()) group->setAllowedReactions(allowed);
		}
		session->data().applyDialogs(nullptr, data.vmessages().v,
			data.vdialogs().v, int(data.vdialogs().v.size()));
		session->data().chatsListChanged(nullptr);
		session->api().requestMoreDialogsIfNeeded();
	};
	_noveoApi->onAvatar = [this](PeerId id, const QUrl &url) {
		const auto session = maybeSession();
		if (!session) return;
		const auto peer = session->data().peer(id);
		const auto location = url.isEmpty() ? ImageLocation()
			: ImageLocation(DownloadLocation{ PlainUrlLocation{ url.toString() } }, 640, 640);
		if (peer->userpicLocation() == location) return;
		if (!url.isEmpty()) {
			session->data().processPhoto(Noveo::NativeAvatar(url));
		}
		peer->setUserpic(url.isEmpty() ? 0 : Noveo::NativeUserId(url.toString()).bare, location, false);
		session->changes().peerUpdated(peer, Data::PeerUpdate::Flag::Photo);
	};
	_noveo->onMessage = [this](const QJsonObject &message) {
		_noveoApi->message(message);
	};
	_noveo->onDiagnostic = [](QString message) {
		LOG(("Noveo: %1").arg(message));
	};
	_noveo->onConnectionChanged = [this](bool connected) {
		if (_mtp) _mtp->setNoveoConnected(connected);
		if (!connected) _noveoApi->disconnected();
	};
	_noveo->onError = [this](Noveo::AuthClient::Error error) {
		using Error = Noveo::AuthClient::Error;
		const auto text = (error == Error::Credentials)
			? tr::lng_noveo_login_invalid(tr::now)
			: (error == Error::RateLimited) ? tr::lng_noveo_login_rate_limited(tr::now)
			: (error == Error::TwoFactorRequired) ? tr::lng_noveo_login_two_factor(tr::now)
			: (error == Error::Timeout) ? tr::lng_noveo_login_timeout(tr::now)
			: tr::lng_noveo_login_connection_error(tr::now);
		if (const auto fail = base::take(_noveoLoginFail)) fail(text);
		if (error == Error::SessionExpired) {
			crl::on_main(this, [this] { logOut(); });
		}
	};
	_noveo->onAuthenticated = [this](const QJsonObject &profile) {
		_noveoLoginFail = nullptr;
		_noveoApi->authenticated(profile);
		const auto user = _noveoApi->selfUser();
		if (const auto session = maybeSession()) {
			session->data().processUser(user);
		} else {
			createSession(user);
		}
		local().writeMtpData(); // Existing encrypted account storage.
		Local::sync();
	};
}

void Account::loginNoveo(QString username, QString password, Fn<void(QString)> fail) {
	prepareNoveoClient();
	_noveoLoginFail = std::move(fail);
	_noveo->login(std::move(username), std::move(password));
}

void Account::cancelNoveoLogin() {
	_noveoLoginFail = nullptr;
	if (_noveo && !sessionExists()) _noveo->clear();
}

void Account::logOut() {
	if (_loggingOut) return;
	_loggingOut = true;
	_noveoLoginFail = nullptr;
	if (_noveo) _noveo->clear();
	if (_noveoApi) _noveoApi->reset();
	if (_mtp) _mtp->setNoveoConnected(false);
	loggedOut();
}

bool Account::loggingOut() const {
	return _loggingOut;
}

void Account::forcedLogOut() {
	if (sessionExists()) {
		loggedOut();
	}
}

void Account::loggedOut() {
	_loggingOut = false;
	Media::Player::mixer()->stopAndClear();
	destroySession(DestroyReason::LoggedOut);
	local().reset();
	cSetOtherOnline(0);
}

void Account::suggestMainDcId(MTP::DcId mainDcId) {
	Expects(_mtp != nullptr);

	_mtp->suggestMainDcId(mainDcId);
	if (_mtpFields.mainDcId != MTP::Instance::Fields::kNotSetMainDc) {
		_mtpFields.mainDcId = mainDcId;
	}
}

void Account::destroyStaleAuthorizationKeys() {
	// Legacy intro callers have no Telegram authorization keys to destroy.
}

void Account::setHandleLoginCode(Fn<void(QString)> callback) {
	_handleLoginCode = std::move(callback);
}

void Account::handleLoginCode(const QString &code) const {
	if (_handleLoginCode) {
		_handleLoginCode(code);
	}
}


} // namespace Main
