/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "intro/intro_step.h"

namespace Ui {
class InputField;
class PasswordInput;
} // namespace Ui

namespace Intro {
namespace details {

// Signs in to pararam.io with an email, a password and,
// when the account has it enabled, a two-factor code.
class PararamLoginWidget final : public Step {
public:
	PararamLoginWidget(
		QWidget *parent,
		not_null<Main::Account*> account,
		not_null<Data*> data);

	void setInnerFocus() override;
	void activate() override;
	void cancelled() override;
	void submit() override;
	rpl::producer<QString> nextButtonText() const override;

	bool hasBack() const override {
		return true;
	}

protected:
	void resizeEvent(QResizeEvent *e) override;

private:
	int errorTop() const override;
	void updateControlsGeometry();
	void showCodeField();

	object_ptr<Ui::InputField> _email;
	object_ptr<Ui::PasswordInput> _password;
	object_ptr<Ui::InputField> _code;
	bool _codeShown = false;
	bool _sent = false;
	int _attempt = 0;

};

} // namespace details
} // namespace Intro
