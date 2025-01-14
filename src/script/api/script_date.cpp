/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file script_date.cpp Implementation of ScriptDate. */

#include "../../stdafx.h"
#include "script_date.hpp"
#include "../../date_func.h"
#include "../../settings_type.h"

#include <time.h>

#include <time.h>

#include "../../safeguards.h"

/* static */ bool ScriptDate::IsValidDate(Date date)
{
	return date >= 0;
}

/* static */ ScriptDate::Date ScriptDate::GetCurrentDate()
{
	return (ScriptDate::Date)_date;
}

/* static */ int32 ScriptDate::GetDayLengthFactor()
{
	return _settings_game.economy.day_length_factor;
}

/* static */ int32 ScriptDate::GetYear(ScriptDate::Date date)
{
	if (date < 0) return DATE_INVALID;

	::YearMonthDay ymd;
	::ConvertDateToYMD(date, &ymd);
	return ymd.year;
}

/* static */ int32 ScriptDate::GetMonth(ScriptDate::Date date)
{
	if (date < 0) return DATE_INVALID;

	::YearMonthDay ymd;
	::ConvertDateToYMD(date, &ymd);
	return ymd.month + 1;
}

/* static */ int32 ScriptDate::GetDayOfMonth(ScriptDate::Date date)
{
	if (date < 0) return DATE_INVALID;

	::YearMonthDay ymd;
	::ConvertDateToYMD(date, &ymd);
	return ymd.day;
}

/* static */ ScriptDate::Date ScriptDate::GetDate(int32 year, int32 month, int32 day_of_month)
{
	if (month < 1 || month > 12) return DATE_INVALID;
	if (day_of_month < 1 || day_of_month > 31) return DATE_INVALID;
	if (year < 0 || year > MAX_YEAR) return DATE_INVALID;

	return (ScriptDate::Date)::ConvertYMDToDate(year, month - 1, day_of_month);
}

/* static */ int32 ScriptDate::GetSystemTime()
{
	time_t t;
	time(&t);
	return t;
}

/* static */ bool ScriptDate::IsTimeShownInMinutes()
{
	return _settings_game.game_time.time_in_minutes;
}

/* static */ int32 ScriptDate::GetTicksPerMinute()
{
	return _settings_game.game_time.ticks_per_minute;
}

/* static */ DateTicksScaled ScriptDate::GetCurrentScaledDateTicks()
{
	return _scaled_date_ticks;
}

/* static */ int32 ScriptDate::GetHour(DateTicksScaled ticks)
{
	Minutes minutes = (ticks / _settings_game.game_time.ticks_per_minute) + _settings_game.game_time.clock_offset;
	return MINUTES_HOUR(minutes);
}

/* static */ int32 ScriptDate::GetMinute(DateTicksScaled ticks)
{
	Minutes minutes = (ticks / _settings_game.game_time.ticks_per_minute) + _settings_game.game_time.clock_offset;
	return MINUTES_MINUTE(minutes);
}
