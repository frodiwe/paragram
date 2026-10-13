/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "pararam/pararam_folder_logo.h"

#include "pararam/pararam_backend.h"
#include "main/main_session.h"
#include "ui/widgets/side_bar_button.h"
#include "ui/painter.h"
#include "ui/rp_widget.h"
#include "styles/style_filter_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QPointer>
#include <QtGui/QPainterPath>

namespace Pararam {
namespace {

const auto kLogoName = u"pararamFolderLogo"_q;
const auto kBadgeProperty = "pararamFolderBadge";

// Painted over the button where its icon is, the button keeps a blank one.
class FolderLogo final : public Ui::RpWidget {
public:
	FolderLogo(
		not_null<Ui::SideBarButton*> button,
		const style::SideBarButton &st,
		QImage logo);

	void setBadge(const QString &badge);

private:
	void paintEvent(QPaintEvent *e) override;
	void validateCache();

	const not_null<Ui::SideBarButton*> _button;
	const style::SideBarButton &_st;
	const QImage _logo;
	QImage _cache;
	int _badgeWidth = 0;

};

FolderLogo::FolderLogo(
	not_null<Ui::SideBarButton*> button,
	const style::SideBarButton &st,
	QImage logo)
: RpWidget(button)
, _button(button)
, _st(st)
, _logo(std::move(logo)) {
	setObjectName(kLogoName);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	setBadge(button->property(kBadgeProperty).toString());

	// Same place as the icon of the button.
	button->sizeValue() | rpl::on_next([=](QSize size) {
		const auto icon = st::foldersBlank.size();
		const auto left = (_st.iconPosition.x() < 0)
			? (size.width() - icon.width()) / 2
			: _st.iconPosition.x();
		const auto top = (_st.iconPosition.y() < 0)
			? (size.height() - icon.height()) / 2
			: _st.iconPosition.y();
		setGeometry(QRect(QPoint(left, top), icon));
		_cache = QImage();
		update();
	}, lifetime());
	show();
}

void FolderLogo::setBadge(const QString &badge) {
	const auto width = badge.isEmpty()
		? 0
		: std::max(
			_st.badgeHeight,
			_st.badgeStyle.font->width(badge) + 2 * _st.badgeSkip);
	if (_badgeWidth != width) {
		_badgeWidth = width;
		_cache = QImage();
		update();
	}
}

void FolderLogo::paintEvent(QPaintEvent *e) {
	validateCache();
	QPainter(this).drawImage(0, 0, _cache);
}

void FolderLogo::validateCache() {
	if (!_cache.isNull()) {
		return;
	}
	const auto ratio = style::DevicePixelRatio();
	_cache = QImage(size() * ratio, QImage::Format_ARGB32_Premultiplied);
	_cache.setDevicePixelRatio(ratio);
	_cache.fill(Qt::transparent);

	auto p = QPainter(&_cache);
	auto hq = PainterHighQualityEnabler(p);
	const auto skip = width() / 9;
	const auto inner = rect().marginsRemoved({ skip, skip, skip, skip });
	const auto radius = inner.width() / 4.;
	auto path = QPainterPath();
	path.addRoundedRect(inner, radius, radius);
	p.setClipPath(path);
	p.drawImage(inner, _logo.scaled(
		inner.size() * ratio,
		Qt::KeepAspectRatioByExpanding,
		Qt::SmoothTransformation));
	p.setClipping(false);

	if (_badgeWidth) {
		// Transparent where the button paints the badge, as its icons are.
		p.setCompositionMode(QPainter::CompositionMode_Source);
		p.setBrush(Qt::transparent);
		auto pen = QPen(Qt::transparent);
		pen.setWidth(2 * _st.badgeStroke);
		p.setPen(pen);
		const auto outer = _button->width();
		const auto pillLeft = std::min(
			outer / 2 + _st.badgePosition.x(),
			outer - _badgeWidth - st::defaultScrollArea.width);
		const auto r = _st.badgeHeight / 2.;
		p.drawRoundedRect(
			pillLeft - x(),
			_st.badgePosition.y() - y(),
			_badgeWidth,
			_st.badgeHeight,
			r,
			r);
	}
}

} // namespace

void SetupFolderLogo(
		not_null<Ui::SideBarButton*> button,
		not_null<Main::Session*> session,
		int filterId,
		const style::SideBarButton &st) {
	const auto backend = Backend::For(&session->mtp());
	if (!backend || filterId <= 0) {
		return;
	}
	const auto weak = QPointer<Ui::SideBarButton>(button.get());
	backend->loadFolderLogo(filterId, [=](QByteArray bytes) {
		const auto strong = weak.data();
		auto logo = QImage::fromData(bytes);
		if (!strong || logo.isNull()) {
			return; // Keeps the folder icon.
		}
		strong->setIconOverride(&st::foldersBlank, &st::foldersBlank);
		new FolderLogo(strong, st, std::move(logo));
	});
}

void SetFolderLogoBadge(
		not_null<Ui::SideBarButton*> button,
		const QString &badge) {
	button->setProperty(kBadgeProperty, badge);
	const auto logo = button->findChild<QWidget*>(
		kLogoName,
		Qt::FindDirectChildrenOnly);
	if (logo) {
		static_cast<FolderLogo*>(logo)->setBadge(badge);
	}
}

} // namespace Pararam
