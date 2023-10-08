
#include <WiFi.h>
// secret, contains definition of local_ssid, local_pass, and data_url in three lines, literally: 
// char local_ssid[] = "NNN";  //  your network SSID (name)
// char local_pass[] = "PPP";  // your network password
// char data_url[] = "http://your-data-endpoint/";
#include "/home/jmoon/Arduino/libraries/local/ssid.h"
// GAAH! DO NOT USE 'HttpClient.h' - case matters, it's a totally different library!
#include <HTTPClient.h>
#include <NTPClient.h>

#include <SPI.h>
#include <TFT_eSPI.h>

//
// Another Arduino Treasure Hunt:
//
// Install the TFT_eSPI library
// Download the latest driver settings from https://www.waveshare.com/wiki/Pico-ResTouch-LCD-3.5
// Replace the Setup60_RP2040_ILI9341.h in the libraries/TFT_eSPI/User_Setups folder with the downloaded version
// Hand-edit Setup60_RP2040_ILI9341.h for:
// Uncomment  #define TFT_INVERSION_ON
// Leave the ILI9488_DRIVER uncommented - this seems to work 

TFT_eSPI tft = TFT_eSPI();

// FIXME: Put in separate, untracked file
int status = WL_IDLE_STATUS;     // the Wifi radio's status

WiFiUDP ntpUDP;

// By default 'pool.ntp.org' is used with 60 seconds update interval and
// no offset
NTPClient timeClient(ntpUDP);

//
// There are better implementations of calendar calcs I am sure, but make my own here
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

// Month day boundaries, corrected below for leap year
const int16_t MB[13] = {0,31,59,90,120,151,181,212,243,273,304,334,365};
const int16_t MB_LY[13] = {0,31,60,91,121,152,182,213,244,274,305,335,366};
const int16_t *month_boundaries;
const String WeekDays[7] = {"Thu","Fri","Sat","Sun","Mon","Tue","Wed"};
const String Months[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

// offset without DST
static int16_t timezone_minutes = -8*60; // Pacific standard time; will adjust for DST below
static int16_t dst_clockchange_minutes = 60; // "Spring ahead, fall back"

const String timezone_strings[2] = {"PST","PDT"};
static int16_t current_year = 0; // gets set on first call

static uint32_t dst_start_utctimestamp=0; // for this year
static uint32_t dst_end_utctimestamp=0;
// Second Sunday in March this year, and first Sunday in November, 1-indexed
static int16_t dst_start_day = 0;
static int16_t dst_end_day = 0;

// sets month offsets for given (possibly Leap) year
uint8_t check_leap_year(uint16_t year) {

  uint8_t LY = ((year%4)==0)?1:0;
  // if divisible by 4, then it is a leap year
  if (LY==1) {
    month_boundaries = MB_LY;
  }
  else {
    month_boundaries = MB;
  }
  return(LY);
}

void update_dst(uint16_t Y) {

  // number of Leap Days since 1970
  int LD = int((Y - 1968)/4);
  uint8_t LY = check_leap_year(Y); // 1 is Leap Year, 0 otherwise

  // Day of Jan 1 this year from 1970
  uint32_t DJ1 = (Y-1970)*365+LD-LY; // don't include last leap day yet in day calc 
  // Week Day of Jan 1 this year
  int8_t WDJ1 = DJ1%7; // Jan 1 this year index, Thur = 0

  int8_t first_sunday = 3 - WDJ1; // can be negative

  // First Sunday in November
  for (int i=first_sunday; i < 365+LY; i+=7) {
    if( ((i + 1) > month_boundaries[10]) && ((i + 1) <= month_boundaries[11]) ) {
      dst_end_day = i+1;
      break;
    }
  }
  // Second Sunday in March
  uint8_t once=0;
  for (int i=first_sunday; i < 365+LY; i+=7) {
    if( ((i + 1) > month_boundaries[2]) && ((i + 1) <= month_boundaries[3]) ) {
      if (once==0) { // first Sunday
        once++;
      }
      else {
        dst_start_day = i+1;
        break;
      }
    }
  }

//  tft.println(String(Y) + " S/E: " + String(dst_start_day) + "/" + String(dst_end_day));

  // Jan 1 12:00:00 AM UTC for year Y
  uint32_t current_year_timestamp = 3600*24*((Y-1970)*365+LD-LY); 
  // 2 AM in current timezone - don't forget 1-index of dst_x_day
  dst_start_utctimestamp = current_year_timestamp + (dst_start_day-1)*24*3600 + 2*3600 - timezone_minutes*60;
  dst_end_utctimestamp = current_year_timestamp + (dst_end_day-1)*24*3600 + 2*3600 - timezone_minutes*60;

}

uint32_t get_year_from_days(uint32_t D) {
  
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

String get_date(uint32_t inputSecs = 0) {

  timeClient.update();

  uint32_t epochSecs = inputSecs; 
  // might have provided a timestamp for testing...
  if (epochSecs == 0) {
    epochSecs = timeClient.getEpochTime();
  }

  // check for DST below  
  int16_t current_offset_minutes = timezone_minutes;

  // Completed days since Jan 1, 1970 with timezone offset
  uint32_t D = uint32_t((epochSecs+60*current_offset_minutes)/(24*3600));
  
  // Current year, taking Leap Days and time zone into account
  uint32_t Y = get_year_from_days(D);

  if (current_year !=Y) {
    update_dst(Y);
    current_year = Y; // keep track of last date request year
  }
  // check for DST - this never affects Y
  String dst_name = timezone_strings[0];
  if ((epochSecs > dst_start_utctimestamp)&&(epochSecs < dst_end_utctimestamp)) 
  {
    current_offset_minutes = timezone_minutes + dst_clockchange_minutes;
    dst_name = timezone_strings[1];
    D = uint32_t((epochSecs+60*current_offset_minutes)/(24*3600));
  }

  // number of Leap Days since 1970
  int LD = int((Y- 1968)/4);
  uint8_t LY = check_leap_year(Y); // updates for Leap Years

  // number of completed days this year (0-indexed)
  uint16_t YD = D - ((Y-1970)*365+LD-LY);

  // get month index for day of year
  int month_idx = 0;
  int day_of_month = 0;

  for (; month_idx < 12; ++month_idx) {
    if( ((YD + 1) > month_boundaries[month_idx]) && ((YD + 1) <= month_boundaries[month_idx+1]) ) {
      day_of_month = YD+1-month_boundaries[month_idx];
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

  String f_hours = (hours >= 10)?String(hours):" "+String(hours);
  String f_minutes = (minutes >= 10)?String(minutes):"0"+String(minutes);
  String f_seconds = (seconds >= 10)?String(seconds):"0"+String(seconds);
  String time_string = f_hours + " : " + f_minutes + " : " + f_seconds + " " + String(ampm) + " " + dst_name;
  String date_string = WeekDays[day_of_week_idx] + " " + Months[month_idx] + " " + String(day_of_month) + ", " + String(current_year);

  return(date_string + " " + time_string);

}

void setup() {
 
  // blink once when setup begins
  digitalWrite(PIN_LED, HIGH);
  delay(100);
  digitalWrite(PIN_LED, LOW);
  delay(100);

// nothing was coming out of pins from scope so had to do this manually
  gpio_set_function(TFT_CS, GPIO_FUNC_SPI);
  gpio_set_function(TFT_SCLK, GPIO_FUNC_SPI);
  gpio_set_function(TFT_MOSI, GPIO_FUNC_SPI);
  gpio_set_function(TFT_MISO, GPIO_FUNC_SPI);
  // can use PWM on this pin to dim screen - TODO
  pinMode(TFT_BL, OUTPUT);

  tft.init();

  tft.setRotation(2);
  tft.fillScreen((TFT_BLACK));


//  tft.setCursor(20, 0, 2);
  tft.setTextColor(TFT_BLUE);  
  tft.setFreeFont(&FreeSerif9pt7b);
//  tft.setTextSize(1);
  tft.println(" ");
  tft.println("Hello!");

  tft.setTextColor(TFT_GREEN); 
  tft.println("Hello!");

  tft.setTextColor(TFT_RED); 
  tft.println("Hello!");

  tft.setTextColor(TFT_WHITE);
  tft.println("Connecting"); 
  while (status != WL_CONNECTED) {
    // Connect to WPA/WPA2 network:
    status = WiFi.begin(local_ssid,local_pass);
    tft.print('.');
    // wait 3 seconds for connection:
    delay(3000);
  }

  // now attempt to use NTP client to set time!
  timeClient.begin();
  timeClient.update();
  timeClient.setUpdateInterval(1000*3600); // once an hour should be good enough

  // x0,x1,y0,y1,ctl = [bits from LSB: 1=rotate,2=invertx,3=inverty]
  // NOTE: Calibration values are RAW extent values - which are between ~300-3600 in both X and Y
  uint16_t calibrationData[5] = {300,3600,300,3600,0};
  tft.setTouch(calibrationData);

  initial_screen();
  delay(5000);
  tft.fillScreen(TFT_BLACK);
  tft.setCursor(0,0);
  tft.println("");

}

void initial_screen() {

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setTextWrap(true);
  tft.setCursor(0,0);
  tft.println(""); // cursor is at bottom of font
  tft.println("Connected!");
  tft.println("");

  IPAddress ip = WiFi.localIP();
  tft.setTextColor(TFT_WHITE);
  tft.print("IP Address: ");
  tft.setTextColor(TFT_YELLOW);
  tft.println(ip);

  // print MAC address:
  byte mac[6];
  WiFi.macAddress(mac);
  tft.setTextColor(TFT_WHITE);
  tft.print("MAC: ");
  char mac_str[18];
  sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", mac[5],mac[4],mac[3],mac[2],mac[1],mac[0]);
  tft.setTextColor(TFT_YELLOW);
  tft.println(mac_str);

  tft.setTextColor(TFT_WHITE);
  HTTPClient http;
  if (http.begin(data_url)) {
    if (http.GET() > 0) {
        String data = http.getString();
        tft.println(data);
    }
    http.end();
  }

  String date_string = get_date();
  Serial.println(date_string);
  tft.println(date_string);

}


static uint32_t color = 0xFFFF;
#define MAXLINES 16
static uint8_t maxlines = MAXLINES;

static uint32_t test_times[MAXLINES] = {
  ((2024-1970)*365+13)*3600*24 + 8*3600, // 12 AM Jan 1 2024, PST
  ((2024-1970)*365+13 + 60)*3600*24, // Feb 29, 2024, PST
  ((2024-1970)*365+13 + 69)*3600*24, // DST 2024
  ((2024-1970)*365+13 + 70)*3600*24, // DST 2024
  ((2024-1970)*365+13 + 307)*3600*24, // DST 2024
  ((2024-1970)*365+13 + 308)*3600*24, // DST 2024
  ((2024-1970)*365+14 + 365)*3600*24, // Leap 2024
  ((2024-1970)*365+14 + 365)*3600*24 + 8*3600-1, // Dec 31 2025, PST
  ((2024-1970)*365+14 + 365)*3600*24 + 8*3600, // Midnight Jan 1 2025
  ((2025-1970)*365+14 + 67)*3600*24, // DST 2025, 12 AM Day 68 (Sun) UTC
  ((2025-1970)*365+14 + 68)*3600*24, // DST 2025
  ((2025-1970)*365+14 + 305)*3600*24, // DST 2025
  ((2025-1970)*365+14 + 306)*3600*24, // DST 2025
  ((2025-1970)*365+14 + 365)*3600*24, // Dec 31 4 pm PST 2025
  ((2025-1970)*365+14 + 365)*3600*24+9*3600, // 1 am Jan 1 2026
  ((2025-1970)*365+14 + 365)*3600*24 + 8*3600-1 // Dec 31 2025, PST
};

void loop() {

  uint16_t x, y;
  static uint16_t color = TFT_WHITE;

  if (maxlines > 0) {
    digitalWrite(PIN_LED, HIGH);
    tft.setTextColor(color, TFT_WHITE);  
    color = color - 128;
    tft.println(get_date(test_times[MAXLINES-maxlines]));
    maxlines--;
    delay(50);
    digitalWrite(PIN_LED, LOW);
    delay(50);
  }
  else if (tft.getTouch(&x, &y)) {
    tft.setTextColor(TFT_GREEN, TFT_BLACK,true);  

    tft.fillRect(5, 385,150,17,TFT_BLACK);
    tft.setCursor(5, 400, 2);
    tft.printf("x: %i     ", x);

    tft.fillRect(5, 405,150,17,TFT_BLACK);
    tft.setCursor(5, 420, 2);
    tft.printf("y: %i    ", y);

    tft.drawCircle(x, y, 5, TFT_GREEN);
  }



}
