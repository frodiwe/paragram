/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class SideBarButton;
} // namespace Ui

namespace style {
struct SideBarButton;
} // namespace style

namespace Pararam {

// Chat folders of pararam.io organizations show the organization logo
// instead of the folder icon.
void SetupFolderLogo(
	not_null<Ui::SideBarButton*> button,
	not_null<Main::Session*> session,
	int filterId,
	const style::SideBarButton &st);

// The logo leaves room for the unread badge the button paints below it.
void SetFolderLogoBadge(
	not_null<Ui::SideBarButton*> button,
	const QString &badge);

} // namespace Pararam
