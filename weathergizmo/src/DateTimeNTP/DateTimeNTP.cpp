#include "DateTimeNTP.h"

// constructor
DateTimeNTP::DateTimeNTP(NTPClient *client) {
	const char init_time[] = "00 : 00 : 00 AM PST";
	strncpy(this->time_cstring,init_time,strlen(init_time));
	const char init_date[] = "MON JAN XX, 2023";
	strncpy(this->date_cstring,init_date,strlen(init_date));
	this->_timeClient=client;
}

bool DateTimeNTP::start() {

	this->_timeClient->begin();
	// once an hour should be good enough
	this->_timeClient->setUpdateInterval(1000*3600); 
	// this should be true on the first call to the client
	bool success = this->_timeClient->update();
	// record time of start for uptime
	this->init_secs = this->_timeClient->getEpochTime();
	this->last_secs = this->_timeClient->getEpochTime();
	return(success);
}

// sets month offsets for given (possibly Leap) year
uint8_t DateTimeNTP::check_leap_year(uint16_t year) {

  uint8_t LY = ((year%4)==0)?1:0;
  // if divisible by 4, then it is a leap year
  if (LY==1) {
    this->month_boundaries = this->MB_LY;
  }
  else {
    this->month_boundaries = this->MB;
  }
  return(LY);
}

void DateTimeNTP::update_dst(uint16_t Y) {

  // number of Leap Days since 1970
  int LD = int((Y - 1968)/4);
  uint8_t LY = this->check_leap_year(Y); // 1 is Leap Year, 0 otherwise

  // Day of Jan 1 this year from 1970
  uint32_t DJ1 = (Y-1970)*365+LD-LY; // don't include last leap day yet in day calc 
  // Week Day of Jan 1 this year
  int8_t WDJ1 = DJ1%7; // Jan 1 this year index, Thur = 0

  int8_t first_sunday = 3 - WDJ1; // can be negative

  // First Sunday in November
  for (int i=first_sunday; i < 365+LY; i+=7) {
    if( ((i + 1) > this->month_boundaries[10]) && ((i + 1) <= this->month_boundaries[11]) ) {
      this->dst_end_day = i+1;
      break;
    }
  }
  // Second Sunday in March
  uint8_t once=0;
  for (int i=first_sunday; i < 365+LY; i+=7) {
    if( ((i + 1) > this->month_boundaries[2]) && ((i + 1) <= this->month_boundaries[3]) ) {
      if (once==0) { // first Sunday
        once++;
      }
      else {
        this->dst_start_day = i+1;
        break;
      }
    }
  }

  // Jan 1 12:00:00 AM UTC for year Y
  uint32_t current_year_timestamp = 3600*24*((Y-1970)*365+LD-LY); 
  // 2 AM in current timezone - don't forget 1-index of dst_x_day
  this->dst_start_utctimestamp = current_year_timestamp + (this->dst_start_day-1)*24*3600 + 2*3600 - this->timezone_minutes*60;
  this->dst_end_utctimestamp = current_year_timestamp + (this->dst_end_day-1)*24*3600 + 2*3600 - this->timezone_minutes*60;

}

uint32_t DateTimeNTP::get_year_from_days(uint32_t D) {
  
  uint32_t Y = uint32_t(1970+(4*(D+1)-1)/(4*365+1));
  // Careful! Day 365 of a Leap Year is -1 in this calc
  int32_t DayInYear = int32_t(D) - ((Y-1970)*365+int32_t((Y-1969)/4)); 
  
  // Remove extra leap day from calc, then all should work
  // alternatively, just set Y = Y-1
  if (DayInYear < 0) {
    // for Leap Years
    Y = uint32_t(1970+(4*(D+1)-2)/(4*365+1));
  }

  return(Y);
}

bool DateTimeNTP::get_date(uint32_t inputSecs) {

  this->_timeClient->update();
  bool success = true;

  uint32_t epochSecs = inputSecs; 
  // might have provided a timestamp for testing...
  if (epochSecs == 0) {
    epochSecs = this->_timeClient->getEpochTime();
	// sanity check: later than around Jan 2024
	uint32_t check_secs = (uint32_t)54*(uint32_t)3600*(uint32_t)(24*365);
    if (epochSecs < check_secs) {
		success=false;
    }
  }
  this->last_secs = epochSecs;

  // check for DST below  
  int16_t current_offset_minutes = this->timezone_minutes;

  // Completed days since Jan 1, 1970 with timezone offset
  uint32_t D = uint32_t((epochSecs+60*current_offset_minutes)/(24*3600));
  
  // Current year, taking Leap Days and time zone into account
  uint32_t Y = get_year_from_days(D);

  if (current_year !=Y) {
    this->update_dst(Y);
    this->current_year = Y; // keep track of last date request year
  }
  // check for DST - this never affects Y
  const char* dst_name = this->timezone_strings[0];
  if ((epochSecs > this->dst_start_utctimestamp)&&(epochSecs < this->dst_end_utctimestamp)) 
  {
    current_offset_minutes = timezone_minutes + dst_clockchange_minutes;
    dst_name = timezone_strings[1];
    D = uint32_t((epochSecs+60*current_offset_minutes)/(24*3600));
  }

  // number of Leap Days since 1970
  int LD = int((Y- 1968)/4);
  uint8_t LY = this->check_leap_year(Y); // updates for Leap Years

  // number of completed days this year (0-indexed)
  uint16_t YD = D - ((Y-1970)*365+LD-LY);

  // get month index for day of year
  int month_idx = 0;
  int day_of_month = 0;

  for (; month_idx < 12; ++month_idx) {
    if( ((YD + 1) > this->month_boundaries[month_idx]) && ((YD + 1) <= this->month_boundaries[month_idx+1]) ) {
      day_of_month = YD+1-this->month_boundaries[month_idx];
      break;
    }
  }

  // get day of week; Jan 1, 1970 being a Thursday
  uint8_t day_of_week_idx = uint8_t( D % 7);

  // here is the formatted datetime string!
  uint32_t current_timestamp = epochSecs + current_offset_minutes*60;
  uint16_t hours = ((current_timestamp % 86400L)/3600); // 0-23
  uint16_t minutes = (current_timestamp % 3600)/60;
  uint16_t seconds = (current_timestamp % 60);

  char ampm[] = "AM";
  if (hours > 11) {
    strncpy(ampm,"PM",2);
    hours = hours - 12;
  }
  if (hours==0) {
    hours = 12; // convention
  }

  if (success) {
	sprintf(this->time_cstring,"%2d:%02d:%02d %s %s",hours,minutes,seconds,ampm,dst_name);
  	sprintf(this->date_cstring,"%s %s %2d, %4d", this->WeekDays[day_of_week_idx], this->Months[month_idx], day_of_month, this->current_year);
  }
  else { // use for error messages
	sprintf(this->time_cstring,"es %x",epochSecs);
  	sprintf(this->date_cstring,"%x %x",this->init_secs,this->last_secs); 
  }
//  String f_hours = (hours >= 10)?String(hours):" "+String(hours);
//  String f_minutes = (minutes >= 10)?String(minutes):"0"+String(minutes);
//  String f_seconds = (seconds >= 10)?String(seconds):"0"+String(seconds);
//  String time_string = f_hours + " : " + f_minutes + " : " + f_seconds + " " + String(ampm) + " " + dst_name;
//  String date_string = WeekDays[day_of_week_idx] + " " + Months[month_idx] + " " + String(day_of_month) + ", " + String(current_year);

//  return(date_string + " | " + time_string);
	return success;
}
