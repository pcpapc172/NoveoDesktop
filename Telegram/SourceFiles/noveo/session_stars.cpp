#include "noveo/session_client.h"
#include "noveo/request_fields.h"

#include <QtCore/QDateTime>
#include <QtCore/QBuffer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QMimeDatabase>
#include <QtCore/QRegularExpression>
#include <QtGui/QImageReader>
#include <QtNetwork/QHttpMultiPart>
#include <algorithm>
#include <utility>

namespace Noveo {
namespace {

MTPStarsAmount StarsAmount(qint64 hundredths) {
	return MTP_starsAmount(MTP_long(hundredths / 100),
		MTP_int(int((hundredths % 100) * 10000000)));
}

qint64 StarsHundredths(const QString &amount) {
	static const auto valid =
		QRegularExpression(u"^([0-9]{1,12})(?:\\.([0-9]{1,2}))?$"_q);
	const auto match = valid.match(amount);
	return match.hasMatch()
			   ? match.captured(1).toLongLong() * 100
					 + match.captured(2).leftJustified(2, '0').toInt()
			   : 0;
}

} // namespace

void SessionClient::applyWallet(qint64 balance) {
	_walletBalance = std::max(qint64(0), balance);
	_wallet.insert("balanceTenths", _walletBalance);
	if (onUpdate) {
		onUpdate(
			updates({MTP_updateStarsBalance(StarsAmount(_walletBalance))}));
	}
}

void SessionClient::loadWallet(std::function<void(QJsonObject, QString)> done) {
	if (!_auth->authenticated()) {
		if (done) {
			done({}, "NOVEO_DISCONNECTED");
		}
		return;
	}
	if (done) {
		_walletWaiters.push_back(std::move(done));
	}
	if (_walletLoading) {
		return;
	}
	_walletLoading = true;
	const auto revision = _walletRevision;
	const auto complete = [this, revision](QJsonObject wallet, QString error) {
		if (error.isEmpty()) {
			if (revision != _walletRevision) {
				wallet.insert("balanceTenths", _walletBalance);
			}
			_wallet = wallet;
			_walletLoaded = true;
			applyWallet(wallet.value("balanceTenths").toInteger());
		}
		const auto waiters = std::exchange(_walletWaiters, {});
		_walletLoading = false;
		for (const auto &done : waiters) {
			done(wallet, error);
		}
	};
	api(
		--_internalRequest,
		"/stars/overview",
		{},
		[complete](QJsonObject response) {
			const auto wallet = response.value("wallet");
			complete(wallet.toObject(),
				wallet.isObject() ? QString() : "NOVEO_WALLET_INVALID");
		},
		false,
		[complete](QString error) { complete({}, error); });
}

QString SessionClient::membershipTier() const {
	const auto profile = _profiles.value(NativeUserId(_self).bare);
	const auto tier =
		profile.value("membershipTier").toString().trimmed().toLower();
	return (tier == "premium" || tier == "silver") ? tier : QString();
}

MTPUser SessionClient::premiumBotUser() {
	const auto raw = QString("00000000-0000-0000-0000-000000000007");
	users(QJsonArray{QJsonObject{{"userId", raw},
		{"username", "Premium Bot"},
		{"isBot", true},
		{"isVerified", true}}});
	return _users.value(NativeUserId(raw).bare);
}

MTPpayments_StarsStatus SessionClient::starsStatus(const QJsonObject &wallet,
	int flags,
	int offset,
	int limit,
	const QSet<QString> &ids) const {
	auto transactions = QVector<QJsonObject>();
	auto seen = QSet<QString>();
	for (const auto value : wallet.value("transactions").toArray()) {
		const auto tx = value.toObject();
		const auto id = tx.value("transactionId").toString();
		const auto amount = tx.value("amountTenths").toInteger();
		const auto incoming = bool(flags & 1);
		const auto outgoing = bool(flags & 2);
		if (id.isEmpty() || seen.contains(id)
			|| (!ids.isEmpty() && !ids.contains(id))
			|| tx.value("type") == "message_reward"
			|| tx.value("description").toString().trimmed() == "Sent a message."
			|| (incoming != outgoing
				&& (incoming ? amount <= 0 : amount >= 0))) {
			continue;
		}
		seen.insert(id);
		transactions.push_back(tx);
	}
	std::stable_sort(transactions.begin(),
		transactions.end(),
		[flags](const auto &a, const auto &b) {
			const auto date = [](const QJsonObject &tx) {
				const auto value = tx.value("createdAt").toInteger();
				return value > 100000000000LL ? value / 1000 : value;
			};
			return (flags & 4) ? date(a) < date(b) : date(a) > date(b);
		});
	auto history = QVector<MTPStarsTransaction>();
	auto historyUsers = QVector<MTPUser>();
	auto includedUsers = QSet<uint64>();
	const auto end = std::min(qsizetype(offset) + limit, transactions.size());
	for (auto index = qsizetype(offset); index < end; ++index) {
		const auto &tx = transactions[index];
		auto date = tx.value("createdAt").toInteger();
		if (date > 100000000000LL) {
			date /= 1000;
		}
		auto title = tx.value("description").toString().trimmed();
		if (title.isEmpty() || title == "null") {
			title = tx.value("type").toString();
		}
		const auto rawUserId = tx.value("relatedUserId").toString();
		const auto userId = rawUserId.isEmpty() ? 0 : NativeUserId(rawUserId).bare;
		auto transactionPeer = MTP_starsTransactionPeerUnsupported();
		if (userId && _users.contains(userId)) {
			transactionPeer = MTP_starsTransactionPeer(MTP_peerUser(MTP_long(userId)));
			if (!includedUsers.contains(userId)) {
				includedUsers.insert(userId);
				historyUsers.push_back(_users.value(userId));
			}
		}
		history.push_back(MTP_starsTransaction(
			MTP_flags(title.isEmpty()
						  ? MTPDstarsTransaction::Flags()
						  : MTPDstarsTransaction::Flags(
								MTPDstarsTransaction::Flag::f_title)),
			MTP_string(tx.value("transactionId").toString()),
			StarsAmount(tx.value("amountTenths").toInteger()),
			MTP_int(int(std::clamp(date, qint64(0), qint64(INT_MAX)))),
			transactionPeer,
			MTP_string(title),
			MTPstring(),
			MTPWebDocument(),
			MTPint(),
			MTPstring(),
			MTPbytes(),
			MTPint(),
			MTPVector<MTPMessageMedia>(),
			MTPint(),
			MTPint(),
			MTPStarGift(),
			MTPint(),
			MTPint(),
			MTPPeer(),
			MTPStarsAmount(),
			MTPint(),
			MTPint(),
			MTPint(),
			MTPint()));
	}
	auto statusFlags = MTPDpayments_starsStatus::Flag::f_history
					   | MTPDpayments_starsStatus::Flag::f_subscriptions;
	if (end < transactions.size()) {
		statusFlags |= MTPDpayments_starsStatus::Flag::f_next_offset;
	}
	return MTP_payments_starsStatus(MTP_flags(statusFlags),
		StarsAmount(wallet.value("balanceTenths").toInteger()),
		MTPVector<MTPStarsSubscription>(),
		MTPstring(),
		MTPlong(),
		MTP_vector<MTPStarsTransaction>(history),
		MTP_string(QString::number(end)),
		MTPVector<MTPChat>(),
		MTP_vector<MTPUser>(historyUsers));
}

bool SessionClient::economyRequest(mtpRequestId id, const mtpBuffer &body) {
	const auto type = uint32(body.front());
	const auto supported = {mtpc_payments_getStarsStatus,
		mtpc_payments_getStarsTransactions,
		mtpc_payments_getStarsTransactionsByID,
		mtpc_payments_getStarsSubscriptions,
		mtpc_payments_getStarsTopupOptions,
		mtpc_payments_getStarsGiftOptions};
	if (std::find(supported.begin(), supported.end(), type)
		== supported.end()) {
		return false;
	}
	if (!_auth->authenticated() || _economyRequests.contains(id)) {
		return true;
	}
	const auto reject = [&](QString error = "NOVEO_BAD_REQUEST") {
		_pending.remove(id);
		fail(id, error);
		return true;
	};
	if (type == mtpc_payments_getStarsTopupOptions
		|| type == mtpc_payments_getStarsGiftOptions) {
		_pending.remove(id);
		if (type == mtpc_payments_getStarsTopupOptions) {
			reply(id, MTPVector<MTPStarsTopupOption>());
		} else {
			reply(id, MTPVector<MTPStarsGiftOption>());
		}
		return true;
	}
	auto peer = MTPInputPeer();
	auto flags = 0;
	auto offset = 0;
	auto limit = 50;
	auto ton = false;
	auto subscriptions = false;
	auto ids = QSet<QString>();
	if (type == mtpc_payments_getStarsStatus) {
		const auto r = ReadRequest<mtpc_payments_getStarsStatus>(body);
		if (!r) {
			return reject();
		}
		peer = r->peer;
		ton = r->flags.v & 1;
	} else if (type == mtpc_payments_getStarsTransactions) {
		const auto r = ReadRequest<mtpc_payments_getStarsTransactions>(body);
		if (!r || (r->flags.v & 8)) {
			return reject();
		}
		peer = r->peer;
		flags = r->flags.v;
		ton = flags & 16;
		const auto text = QString::fromUtf8(r->offset.v);
		auto ok = text.isEmpty();
		if (!text.isEmpty()) {
			offset = text.toInt(&ok);
		}
		if (!ok || offset < 0) {
			return reject();
		}
		limit = std::clamp(r->limit.v, 1, 100);
	} else if (type == mtpc_payments_getStarsTransactionsByID) {
		const auto r =
			ReadRequest<mtpc_payments_getStarsTransactionsByID>(body);
		if (!r) {
			return reject();
		}
		peer = r->peer;
		ton = r->flags.v & 1;
		for (const auto &item : r->id.v) {
			ids.insert(
				QString::fromUtf8(item.c_inputStarsTransaction().vid().v));
		}
		if (ids.isEmpty()) {
			return reject();
		}
	} else {
		const auto r = ReadRequest<mtpc_payments_getStarsSubscriptions>(body);
		if (!r) {
			return reject();
		}
		peer = r->peer;
		subscriptions = true;
	}
	if (peer.type() != mtpc_inputPeerSelf
		&& inputPeer(peer) != peerFromUser(NativeUserId(_self))) {
		return reject("NOVEO_WALLET_SELF_ONLY");
	}
	if (ton || subscriptions) {
		_pending.remove(id);
		reply(id, starsStatus({{"balanceTenths", ton ? 0 : _walletBalance}}));
		return true;
	}
	_economyRequests.insert(id);
	const auto complete = [=, this](QJsonObject wallet, QString error) {
		_economyRequests.remove(id);
		if (!_pending.remove(id)) {
			return;
		}
		if (!error.isEmpty()) {
			fail(id, error);
		} else {
			reply(id, starsStatus(wallet, flags, offset, limit, ids));
		}
	};
	if (offset && _walletLoaded) {
		complete(_wallet, {});
	} else {
		loadWallet(complete);
	}
	return true;
}

void SessionClient::createStarsGiveaway(
	PeerId peer, QString amount, std::function<void(QString)> done) {
	const auto chat = _chatIds.value(peer.value);
	const auto hundredths = StarsHundredths(amount);
	if (chat.isEmpty() || !hundredths || !_auth->authenticated()) {
		done("Enter a valid amount and choose an existing chat.");
		return;
	}
	if (_starsActions.contains(peer.value)) {
		done("A Stars transfer to this chat is already in progress.");
		return;
	}
	_starsActions.insert(peer.value);
	api(
		--_internalRequest,
		"/stars/giveaway",
		{{"chatId", chat}, {"amount", amount}},
		[=, this](QJsonObject) {
			_starsActions.remove(peer.value);
			done({});
			loadWallet();
			_auth->send({{"type", "resync_state"}});
		},
		true,
		[=, this](QString error) {
			_starsActions.remove(peer.value);
			done(error);
		});
}

void SessionClient::submitPremiumReceipt(
	QString path, std::function<void(QString)> done, QByteArray content) {
	const auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
	auto file = static_cast<QIODevice *>(nullptr);
	if (content.isEmpty()) {
		file = new QFile(path, multipart);
	} else {
		const auto buffer = new QBuffer(multipart);
		buffer->setData(content);
		file = buffer;
	}
	if (!_auth->authenticated() || !file->open(QIODevice::ReadOnly)
		|| file->size() <= 0 || file->size() > 15 * 1024 * 1024
		|| !QImageReader(file).canRead()) {
		delete multipart;
		done("Choose a receipt image smaller than 15 MB.");
		return;
	}
	file->seek(0);
	auto name = QFileInfo(path).fileName();
	if (name.isEmpty()) {
		name = "receipt." + QString::fromLatin1(QImageReader(file).format());
	}
	file->seek(0);
	name.replace('"', '_').replace('\\', '_').replace('\r', '_').replace(
		'\n', '_');
	auto tier = QHttpPart();
	tier.setHeader(
		QNetworkRequest::ContentDispositionHeader, "form-data; name=\"tier\"");
	tier.setBody("premium");
	multipart->append(tier);
	auto receipt = QHttpPart();
	receipt.setHeader(QNetworkRequest::ContentDispositionHeader,
		"form-data; name=\"receipt\"; filename=\"" + name + "\"");
	receipt.setHeader(QNetworkRequest::ContentTypeHeader,
		content.isEmpty() ? QMimeDatabase().mimeTypeForFile(path).name()
						  : QMimeDatabase().mimeTypeForData(content).name());
	receipt.setBodyDevice(file);
	multipart->append(receipt);
	api(
		--_internalRequest,
		"/payment/request",
		{},
		[done](QJsonObject) { done({}); },
		true,
		done,
		false,
		multipart);
}

} // namespace Noveo
