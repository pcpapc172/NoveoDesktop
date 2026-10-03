/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "intro/intro_step.h"

namespace Ui {
class InputField;
class PasswordInput;
} // namespace Ui

namespace Intro::details {

class PhoneWidget final : public Step {
public:
	PhoneWidget(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);

	QString accessibilityName() override;

	void setInnerFocus() override;
	void activate() override;
	void finished() override;
	void cancelled() override;
	void submit() override;
	rpl::producer<QString> nextButtonText() const override;
	~PhoneWidget();

	bool hasBack() const override {
		return true;
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	rpl::variable<bool> _submitting = false;
	object_ptr<Ui::InputField> _username;
	object_ptr<Ui::PasswordInput> _password;

};

} // namespace Intro::details
