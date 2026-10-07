/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "mtproto/config_loader.h"

#include "mtproto/facade.h"
#include "mtproto/mtproto_dc_options.h"
#include "mtproto/mtproto_config.h"
#include "mtproto/mtp_instance.h"

namespace MTP {
namespace details {
namespace {

constexpr auto kEnumerateDcTimeout = 8000; // 8 seconds timeout for help_getConfig to work (then move to other dc)

} // namespace

ConfigLoader::ConfigLoader(
	not_null<Instance*> instance,
	const QString &phone,
	Fn<void(const MTPConfig &result)> onDone,
	FailHandler onFail,
	bool proxyEnabled)
: _instance(instance)
, _phone(phone)
, _proxyEnabled(proxyEnabled)
, _doneHandler(onDone)
, _failHandler(onFail) {
	_enumDCTimer.setCallback([this] { enumerate(); });
}

void ConfigLoader::load() {
	if (!_instance->isKeysDestroyer()) {
		sendRequest(_instance->mainDcId());
		_enumDCTimer.callOnce(kEnumerateDcTimeout);
	} else {
		auto ids = _instance->dcOptions().configEnumDcIds();
		Assert(!ids.empty());
		_enumCurrent = ids.front();
		enumerate();
	}
}

mtpRequestId ConfigLoader::sendRequest(ShiftedDcId shiftedDcId) {
	auto done = [done = _doneHandler](const Response &response) {
		auto from = response.reply.constData();
		auto result = MTPConfig();
		if (!result.read(from, from + response.reply.size())) {
			return false;
		}
		done(result);
		return true;
	};
	return _instance->send(
		MTPhelp_GetConfig(),
		std::move(done),
		base::duplicate(_failHandler),
		shiftedDcId);
}

void ConfigLoader::terminateRequest() {
	if (_enumRequest) {
		_instance->cancel(base::take(_enumRequest));
	}
	if (_enumCurrent) {
		_instance->killSession(MTP::configDcId(_enumCurrent));
	}
}

ConfigLoader::~ConfigLoader() {
	terminateRequest();
}

void ConfigLoader::enumerate() {
	terminateRequest();
	if (!_enumCurrent) {
		_enumCurrent = _instance->mainDcId();
	}
	auto ids = _instance->dcOptions().configEnumDcIds();
	Assert(!ids.empty());

	auto i = std::find(ids.cbegin(), ids.cend(), _enumCurrent);
	if (i == ids.cend() || (++i) == ids.cend()) {
		_enumCurrent = ids.front();
	} else {
		_enumCurrent = *i;
	}
	_enumRequest = sendRequest(MTP::configDcId(_enumCurrent));

	_enumDCTimer.callOnce(kEnumerateDcTimeout);

}

void ConfigLoader::setPhone(const QString &phone) {
	if (_phone != phone) {
		_phone = phone;
	}
}

void ConfigLoader::setProxyEnabled(bool value) {
	_proxyEnabled = value;
}

} // namespace details
} // namespace MTP
