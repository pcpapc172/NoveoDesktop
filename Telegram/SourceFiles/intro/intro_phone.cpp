/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_phone.h"

#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "crl/crl_on_main.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"

#include "styles/style_intro.h"

namespace Intro::details {

PhoneWidget::PhoneWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _totp(this, st::introName, tr::lng_noveo_totp_code())
, _username(this, st::introName, tr::lng_noveo_login_username())
, _password(this, st::introPassword, tr::lng_noveo_login_password()) {
	_password->setClearButtonEnabled(false);
	_totp->hide();
	_totp->submits() | rpl::on_next([this] { submit(); }, lifetime());
	setTitleText(tr::lng_noveo_login_title());
	setDescriptionText(tr::lng_noveo_login_description());
	setErrorCentered(true);
	setTabOrder(_username, _password);

	_username->changes(
	) | rpl::on_next([this] {
		hideError();
	}, lifetime());
	_username->submits(
	) | rpl::on_next([this] {
		_password->setFocusFast();
	}, lifetime());
	connect(_password, &Ui::PasswordInput::changed, this, [this] {
		hideError();
	});
	connect(_password, &Ui::PasswordInput::submitted, this, [this] {
		submit();
	});
}

PhoneWidget::~PhoneWidget() {
	account().cancelNoveoLogin();
}

rpl::producer<QString> PhoneWidget::nextButtonText() const {
	return _submitting.value() | rpl::map([](bool submitting) {
		return submitting ? tr::lng_noveo_login_progress() : tr::lng_intro_next();
	}) | rpl::flatten_latest();
}

QString PhoneWidget::accessibilityName() {
	return tr::lng_noveo_login_title(tr::now);
}

void PhoneWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	_username->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	_totp->moveToLeft(contentLeft(), contentTop() + st::introStepFieldTop);
	_password->moveToLeft(contentLeft(),
		_username->y() + _username->height() + st::introPhoneTop);
}

void PhoneWidget::submit() {
	if (isHidden() || _submitting.current()) {
		return;
	} else if (_twoFactor) {
		_submitting = true;
		_totp->setEnabled(false);
		account().submitNoveoTotp(_totp->getLastText(), crl::guard(this, [this](QString error) {
			loginFailed(std::move(error));
		}));
		return;
	} else if (_username->getLastText().trimmed().isEmpty()) {
		_username->showError();
		_username->setFocusFast();
		showError(tr::lng_noveo_login_username_required());
		return;
	} else if (_password->getLastText().isEmpty()) {
		_password->showError();
		_password->setFocusFast();
		showError(tr::lng_noveo_login_password_required());
		return;
	}
	hideError();
	_submitting = true;
	_username->setEnabled(false);
	_password->setEnabled(false);
	account().loginNoveo(_username->getLastText().trimmed(), _password->getLastText(),
		crl::guard(this, [this](QString error) { loginFailed(std::move(error)); }),
		crl::guard(this, [this] { totpRequired(); }));
}

void PhoneWidget::totpRequired() {
	_twoFactor = true;
	_submitting = false;
	_password->clear();
	_username->hide();
	_password->hide();
	_totp->show();
	_totp->setEnabled(true);
	setDescriptionText(tr::lng_noveo_totp_description());
	_totp->setFocusFast();
}

void PhoneWidget::loginFailed(QString error) {
	if (_twoFactor && !account().noveoAwaitingTotp()) cancelled();
	_submitting = false;
	if (_twoFactor) {
		_totp->setEnabled(true);
		_totp->clear();
		_totp->setFocusFast();
	} else {
		_username->setEnabled(true);
		_password->setEnabled(true);
		_password->clear();
		_password->setFocusFast();
	}
	showError(rpl::single(std::move(error)));
}

void PhoneWidget::setInnerFocus() {
	if (_twoFactor) _totp->setFocusFast();
	else _username->setFocusFast();
}

void PhoneWidget::activate() {
	Step::activate();
	showChildren();
	if (_twoFactor) { _username->hide(); _password->hide(); }
	else _totp->hide();
	setInnerFocus();
}

void PhoneWidget::finished() {
	Step::finished();
	_totp->clear();
	_password->clear();
}

void PhoneWidget::cancelled() {
	account().cancelNoveoLogin();
	_twoFactor = false;
	_totp->clear();
	_totp->hide();
	_totp->setEnabled(true);
	_username->show();
	_password->show();
	setDescriptionText(tr::lng_noveo_login_description());
	_submitting = false;
	_username->setEnabled(true);
	_password->setEnabled(true);
	_password->clear();
}

} // namespace Intro::details
