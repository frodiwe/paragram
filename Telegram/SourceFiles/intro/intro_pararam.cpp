/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "intro/intro_pararam.h"

#include "intro/intro_widget.h"
#include "lang/lang_keys.h"
#include "main/main_account.h"
#include "mtproto/mtp_instance.h"
#include "pararam/pararam_backend.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/password_input.h"
#include "styles/style_intro.h"

namespace Intro {
namespace details {
namespace {

constexpr auto kFieldSkip = 56;
constexpr auto kErrorSkip = 8;

[[nodiscard]] TextWithEntities CredentialsDescription() {
	return { u"Enter your pararam.io email and password."_q };
}

} // namespace

PararamLoginWidget::PararamLoginWidget(
	QWidget *parent,
	not_null<Main::Account*> account,
	not_null<Data*> data)
: Step(parent, account, data)
, _email(this, st::introPassword, rpl::single(u"Email"_q))
, _password(this, st::introPassword, rpl::single(u"Password"_q))
, _code(this, st::introPassword, rpl::single(u"Two-factor code"_q)) {
	setTitleText(rpl::single(u"Sign in to pararam.io"_q));
	setDescriptionText(rpl::single(CredentialsDescription()));

	_email->changes(
	) | rpl::on_next([=] {
		hideError();
	}, _email->lifetime());
	connect(_password, &Ui::PasswordInput::changed, [=] { hideError(); });
	_code->changes(
	) | rpl::on_next([=] {
		hideError();
	}, _code->lifetime());

	// Ui::InputField handles Enter itself instead of passing it on.
	_email->submits(
	) | rpl::on_next([=] {
		if (_password->getLastText().isEmpty()) {
			_password->setFocusFast();
		} else {
			submit();
		}
	}, _email->lifetime());
	_code->submits(
	) | rpl::on_next([=] {
		submit();
	}, _code->lifetime());

	_code->hide();
	setMouseTracking(true);
}

void PararamLoginWidget::resizeEvent(QResizeEvent *e) {
	Step::resizeEvent(e);
	updateControlsGeometry();
}

void PararamLoginWidget::updateControlsGeometry() {
	// The two-factor code replaces the email and password fields.
	const auto top = contentTop() + st::introStepFieldTop;
	_email->moveToLeft(contentLeft(), top);
	_password->moveToLeft(contentLeft(), top + kFieldSkip);
	_code->moveToLeft(contentLeft(), top);
}

int PararamLoginWidget::errorTop() const {
	return contentTop()
		+ st::introStepFieldTop
		+ (_codeShown ? 1 : 2) * kFieldSkip
		+ kErrorSkip;
}

void PararamLoginWidget::setInnerFocus() {
	if (_codeShown) {
		_code->setFocusFast();
	} else if (_email->getLastText().trimmed().isEmpty()) {
		_email->setFocusFast();
	} else {
		_password->setFocusFast();
	}
}

void PararamLoginWidget::activate() {
	Step::activate();
	_email->setVisible(!_codeShown);
	_password->setVisible(!_codeShown);
	_code->setVisible(_codeShown);
	setInnerFocus();
}

void PararamLoginWidget::cancelled() {
	++_attempt;
	_sent = false;
	if (_codeShown) {
		_codeShown = false;
		_code->hide();
		_code->clear();
		setDescriptionText(rpl::single(CredentialsDescription()));
	}
}

void PararamLoginWidget::showCodeField() {
	_codeShown = true;
	_email->hide();
	_password->hide();
	_code->show();
	updateControlsGeometry();
	_code->setFocusFast();
	setDescriptionText(rpl::single(TextWithEntities{
		u"Enter the code from your authenticator app."_q,
	}));
}

void PararamLoginWidget::submit() {
	if (_sent) {
		return;
	}
	const auto email = _email->getLastText().trimmed();
	const auto password = _password->getLastText();
	const auto code = _code->getLastText().trimmed().remove(' ');
	if (email.isEmpty()) {
		_email->showError();
		return;
	} else if (password.isEmpty()) {
		_password->showError();
		return;
	} else if (_codeShown && code.isEmpty()) {
		_code->showError();
		return;
	}
	const auto backend = Pararam::Backend::For(&account().mtp());
	if (!backend) {
		showError(rpl::single(Lang::Hard::ServerError()));
		return;
	}
	hideError();
	_sent = true;
	const auto attempt = ++_attempt;
	const auto weak = QPointer<PararamLoginWidget>(this);
	backend->login(email, password, code, [=](
			Pararam::LoginResult result,
			MTPUser user) {
		if (!weak || _attempt != attempt) {
			return;
		}
		_sent = false;
		using Status = Pararam::LoginResult::Status;
		switch (result.status) {
		case Status::Success:
			finish(user);
			return;
		case Status::NeedTwoFactorCode:
			if (_codeShown) {
				showError(rpl::single(u"Wrong two-factor code."_q));
				_code->showError();
			} else {
				showCodeField();
			}
			return;
		case Status::Failed:
			showError(rpl::single(result.error.isEmpty()
				? Lang::Hard::ServerError()
				: result.error));
			if (_codeShown && result.error.contains(u"code"_q)) {
				_code->selectAll();
				_code->showError();
			} else {
				_password->selectAll();
				_password->showError();
			}
			return;
		}
	});
}

rpl::producer<QString> PararamLoginWidget::nextButtonText() const {
	return rpl::single(u"Sign in"_q);
}

} // namespace details
} // namespace Intro
