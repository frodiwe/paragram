/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "base/const_string.h"

#define TDESKTOP_REQUESTED_ALPHA_VERSION (0ULL)

#ifdef TDESKTOP_ALLOW_CLOSED_ALPHA
#define TDESKTOP_ALPHA_VERSION TDESKTOP_REQUESTED_ALPHA_VERSION
#else // TDESKTOP_ALLOW_CLOSED_ALPHA
#define TDESKTOP_ALPHA_VERSION (0ULL)
#endif // TDESKTOP_ALLOW_CLOSED_ALPHA

// used in Updater.cpp and Setup.iss for Windows
// Pararam Desktop uses its own id, name and data folder so that it never
// touches the data of an installed Telegram Desktop.
constexpr auto AppId = "{8C1A1B6E-3F2D-4C3B-9A51-2E7D4B5F6A10}"_cs;
constexpr auto AppNameOld = "Pararam Win (Unofficial)"_cs;
constexpr auto AppName = "Pararam Desktop"_cs;
constexpr auto AppFile = "Pararam"_cs;
constexpr auto AppVersion = 7002010;
constexpr auto AppVersionStr = "7.2.10";
constexpr auto AppBetaVersion = true;
constexpr auto AppAlphaVersion = TDESKTOP_ALPHA_VERSION;
