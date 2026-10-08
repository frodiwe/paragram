/*
This file is part of Pararam Desktop,
an alternative pararam.io client based on Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "mtproto/core_types.h"

namespace Pararam {

// Sequential reader of serialized TL request parameters.
// Generated TL function classes keep their fields private,
// so requests are parsed parameter by parameter following
// the order from the api.tl scheme.
class Reader final {
public:
	Reader(const mtpPrime *from, const mtpPrime *end)
	: _from(from)
	, _end(end) {
	}

	template <typename Type>
	[[nodiscard]] Type read() {
		auto result = Type();
		if (_ok && !result.read(_from, _end)) {
			_ok = false;
		}
		return result;
	}

	[[nodiscard]] int32 int32Value() {
		return read<MTPint>().v;
	}
	[[nodiscard]] uint64 int64Value() {
		return uint64(read<MTPlong>().v);
	}
	[[nodiscard]] uint32 flags() {
		return uint32(read<MTPint>().v);
	}
	[[nodiscard]] QString string() {
		return qs(read<MTPstring>());
	}
	[[nodiscard]] QByteArray bytes() {
		return read<MTPbytes>().v;
	}
	[[nodiscard]] mtpTypeId typeId() {
		if (!_ok || _from >= _end) {
			_ok = false;
			return 0;
		}
		return mtpTypeId(*_from++);
	}

	// Skips an optional value, if the flag is set.
	template <typename Type>
	void skipIf(bool condition) {
		if (condition) {
			[[maybe_unused]] const auto skipped = read<Type>();
		}
	}

	[[nodiscard]] bool ok() const {
		return _ok;
	}
	[[nodiscard]] const mtpPrime *position() const {
		return _from;
	}
	[[nodiscard]] const mtpPrime *end() const {
		return _end;
	}

private:
	const mtpPrime *_from = nullptr;
	const mtpPrime *_end = nullptr;
	bool _ok = true;

};

} // namespace Pararam
