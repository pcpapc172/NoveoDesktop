/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/timer.h"
#include "base/weak_ptr.h"
#include "base/bytes.h"
#include "mtproto/mtproto_response.h"

namespace MTP {

class Instance;

namespace details {

class ConfigLoader : public base::has_weak_ptr {
public:
	ConfigLoader(
		not_null<Instance*> instance,
		const QString &phone,
		Fn<void(const MTPConfig &result)> onDone,
		FailHandler onFail,
		bool proxyEnabled);
	~ConfigLoader();

	void load();
	void setPhone(const QString &phone);
	void setProxyEnabled(bool value);

private:
	mtpRequestId sendRequest(ShiftedDcId shiftedDcId);
	void terminateRequest();
	void enumerate();

	not_null<Instance*> _instance;
	base::Timer _enumDCTimer;
	DcId _enumCurrent = 0;
	mtpRequestId _enumRequest = 0;

	QString _phone;
	bool _proxyEnabled = false;

	Fn<void(const MTPConfig &result)> _doneHandler;
	FailHandler _failHandler;

};

} // namespace details
} // namespace MTP
