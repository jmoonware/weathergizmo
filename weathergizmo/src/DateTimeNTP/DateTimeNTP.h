#ifndef DateTimeNTP_h
#define DateTimeNTP_h

#include "Arduino.h"
#include <WiFi.h>
#include <NTPClient.h>

//
// There are probably better implementations of calendar calcs 
//
// USA DST Info (unless the US actually abandons this...)
//    begins at 2:00 a.m. on the second Sunday of March (at 2 a.m. the local time time skips ahead to 3 a.m. so there is one less hour in that day)
//    ends at 2:00 a.m. on the first Sunday of November (at 2 a.m. the local time becomes 1 a.m. and that hour is repeated, so there is an extra hour in that day)
//
// To get UTC Year from elapsed Days D = epochTime/(24*3600) since Jan 1, 1970:
// 
// D = (Y-1970)*365 + floor((Y-1969)/4)
//
// This is trickier to invert than you might think - see code below for get_year_from_days
//

// Leap Year Info - Just use "Divisible by 4" rule - the 100/400 rule isn't going to apply for a long time
// Leap days LD since 1970 LD = int((Y- 1968)/4)
// Is Leap year if remainder of Y%4 == 0; 
//
// Jan 1, 1970 was a Thursday, so Day of Week number is D%7 (Thursday is Day 0, 0-indexed) of [Thu,Fri,Sat,Sun,Mon,Tue,Wed])
//
// Days this year is YD = D - ((Y-1970)*365+LD)
//
// Month boundaries are MB = cumsum([31,28,31,30,31,30,31,31,30,31,30,31]) (where 28 -> 29 on leap years, 1-indexed)
// Month index i ==> (YD + 1) <= MB[i+1] and (YD + 1) > MB[i]
//
// Second Sunday of March:
// Find day of Jan 1 this year: DJ1 = (Y-1970)+365+LD; so Weekday WDJ1 = DJ1-7*int(DJ1/7) (Thursday = 0)
// So Sundays this year are every STY = (3 - WDJ1) + n*7, n: STY >= 0
// Now take the second time STY+1 > MB[i_Feb] and STY+1 <= MB[i_March] 
// For November, change i_October, i_November above take the first time STY+1 > MB[i_Nov] and STY+1 <= MB[i_Dec] 

// NTPClient returns the seconds timestamp, UTC in the Jan 1, 1970 epoch, which will roll-over (signed 32 bit) in 2038
// NTP uses 32 unsigned, and will roll-over on February 7, 2036 (and 136 years from Jan 1, 1900)

class DateTimeNTP 
{
	public:
		DateTimeNTP(NTPClient *client);
		bool start(); // true if successful
		bool get_date(uint32_t inputSecs = 0);
		char time_cstring[32]; // [] = "00 : 00 : 00 AM EST";
		char date_cstring[32]; // [] = "MON JAN XX, 2023";
		uint32_t init_secs;
		uint32_t last_secs;

	private:
		NTPClient *_timeClient;

		// Month day boundaries, corrected below for leap year
		const int16_t MB[13] = {0,31,59,90,120,151,181,212,243,273,304,334,365};
		const int16_t MB_LY[13] = {0,31,60,91,121,152,182,213,244,274,305,335,366};
		const int16_t *month_boundaries;
		constexpr static char WeekDays[7][4] = {"Thu","Fri","Sat","Sun","Mon","Tue","Wed"};
		constexpr static char Months[12][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

		constexpr static char timezone_strings[2][4] = {"EST","EDT"};

		// offset without DST
		int16_t timezone_minutes = -5*60; // 5 = Eastern Standard Time
		int16_t dst_clockchange_minutes = 60; // "Spring ahead, fall back"
		int16_t current_year = 0; // gets set on first call
		uint32_t dst_start_utctimestamp=0; // for this year
		uint32_t dst_end_utctimestamp=0;
		// Second Sunday in March this year, and first Sunday 
		// in November, 1-indexed
		int16_t dst_start_day = 0;
		int16_t dst_end_day = 0;

		// sets month offsets for given (possibly Leap) year
		uint8_t check_leap_year(uint16_t year); 
		void update_dst(uint16_t Y); 
		uint32_t get_year_from_days(uint32_t D); 
}; // class

#endif // DateTimeNTP_h
