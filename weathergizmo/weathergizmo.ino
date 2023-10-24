
#include <WiFi.h>
// secret, contains definition of local_ssid, local_pass, and data_url in three lines, literally: 
// char local_ssid[] = "NNN";  //  your network SSID (name)
// char local_pass[] = "PPP";  // your network password
// char data_url[] = "http://your-data-endpoint/";
#include "/home/jmoon/Arduino/libraries/local/ssid.h"
// GAAH! DO NOT USE 'HttpClient.h' - case matters, it's a totally different library!
#include <HTTPClient.h>
#include <NTPClient.h>

// JFC - yet another Arduino treasure hunt
// The fonts in Adafruit_GFX library are named exactly the same as in the TFT_eSPI lib, but they are NOT the same or compatible
// (despite compiling without error, you will get garbage using either the tft.printx functions or e.g. the GFXcanvas.printx functions)
// The flicker-free-ish built in fonts of the TFT_eSPI lib are ugly and buggy
// The FreeFonts (in the TFT lib) are prettier, but need to be "erased" for in-place updates which causes flicker
//
// So, basically everything needs to be rendered in a bitmap first then updated to screen (no tft.printx for you!)
//
// Also. The performance of the Pico W and Waveshare ResTouch 3.5" LCD is not spectacular - about 500 ms to clear the screen
// See this discussion about the hobbled design of the LCD board https://github.com/Bodmer/TFT_eSPI/discussions/1554 
// for this reason I implemented deferred drawing of the bitmaps so the touch button would respond reasonably well

#include <SPI.h>
#include <Adafruit_GFX.h>
#include "Fonts/FreeSerif9pt7b.h"
#include "Fonts/FreeMonoBold9pt7b.h"
#include "Fonts/FreeMonoBold24pt7b.h"
#include "Fonts/FreeMonoBold18pt7b.h"
#include "Fonts/FreeMonoBold12pt7b.h"
#include <TFT_eSPI.h>
#include <hardware/pwm.h>
#include <elapsedMillis.h>
#include <ArduinoJson.h>

// Backlight update = 133 MHz/(255*2360) = 221 Hz
#define BACKLIGHT_DIV 255
#define BACKLIGHT_TOP 2360
#define MIN_SCREEN_LEVEL 100

//
// Another Arduino Treasure Hunt:
//
// Install the TFT_eSPI library
// Download the latest driver settings from https://www.waveshare.com/wiki/Pico-ResTouch-LCD-3.5
// Replace the Setup60_RP2040_ILI9341.h in the libraries/TFT_eSPI/User_Setups folder with the downloaded version
// Hand-edit Setup60_RP2040_ILI9341.h for:
// Uncomment  #define TFT_INVERSION_ON
// #define TFT_SPI_PORT 1 (for the Waveshare 3.5 ResTouch/Pico W combo) 
// Leave the ILI9488_DRIVER uncommented - this seems to work 
// change SPI_FREQUENCY to 70000000 - seemed to speed up display

TFT_eSPI tft = TFT_eSPI();
uint8_t backlight_pwm_slice;
TFT_eSPI_Button details_button = TFT_eSPI_Button();
TFT_eSPI_Button history_button = TFT_eSPI_Button();

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
const char WeekDays[][4] = {"Thu","Fri","Sat","Sun","Mon","Tue","Wed"};
const char Months[][4] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

// offset without DST
static int16_t timezone_minutes = -8*60; // Pacific standard time; will adjust for DST below
static int16_t dst_clockchange_minutes = 60; // "Spring ahead, fall back"

const char timezone_strings[][4] = {"PST","PDT"};
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

char time_cstring[] = "00 : 00 : 00 AM PST";
char date_cstring[] = "MON JAN XX, 2023";

void get_date(uint32_t inputSecs = 0) {

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
  const char* dst_name = timezone_strings[0];
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

  sprintf(time_cstring,"%2d:%02d:%02d %s %s",hours,minutes,seconds,ampm,dst_name);
  sprintf(date_cstring,"%s %s %2d, %4d", WeekDays[day_of_week_idx], Months[month_idx], day_of_month, current_year);

//  String f_hours = (hours >= 10)?String(hours):" "+String(hours);
//  String f_minutes = (minutes >= 10)?String(minutes):"0"+String(minutes);
//  String f_seconds = (seconds >= 10)?String(seconds):"0"+String(seconds);
//  String time_string = f_hours + " : " + f_minutes + " : " + f_seconds + " " + String(ampm) + " " + dst_name;
//  String date_string = WeekDays[day_of_week_idx] + " " + Months[month_idx] + " " + String(day_of_month) + ", " + String(current_year);

//  return(date_string + " | " + time_string);

}

void initial_screen() {

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_GREEN);
  tft.setTextWrap(true);
  tft.setTextSize(1);
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


void draw_message(const char* e) {
//  tft.setCursor(5, 430, 2);
//  tft.setTextFont(2);
//  tft.setTextColor(TFT_RED, TFT_LIGHTGREY,true);  
//  tft.fillRect(0, 425, 175, 40, TFT_DARKGREY);
//  tft.println(e);
}

elapsedMillis performance_millis;

enum GIZMO_STATES {
  STATE_SLEEP,
  STATE_UPDATE,
  STATE_UPDATE_WEB,
  STATE_DIMMING,
  STATE_SHOW_FORECAST,
  STATE_SHOW_HISTORY,
  STATE_WAIT
};

uint8_t gizmo_state;

elapsedMillis awake_millis;
elapsedMillis update_millis;
elapsedMillis web_millis;

uint32_t awake_delay = 60*1000; // ms
uint32_t update_delay = 2*1000; // ms, for raw sensor data
uint32_t web_delay = 10*1000; // ms, update forecast, etc; make longer

uint8_t dimming_interval = 10; // ms
uint8_t dimming_levels = 100; // linear walk-down of screen brightness 

enum BITMAP_NAMES {
  DATE_CANVAS,
  T_CANVAS,
  H_CANVAS,
  WINDV_CANVAS,
  WINDA_CANVAS,
  PRECIP_CANVAS,
  NUM_BITMAPS
};

// render bitmaps one at a time during sleep state - it is slow!
bool dirty[NUM_BITMAPS]; 

int bpos[][4] = {
  {0,0,320,50}, // x, y, w, h; date
  {0,70,320,50}, // temp
  {0,120,320,50}, // hum
  {0,200,320,55}, // wind v
  {0,255,320,50}, // wind angle
  {0,340,320,55} // precip
};

int canvas_colors[][2] = {
  {TFT_SKYBLUE,TFT_BLACK},
  {TFT_GREEN,TFT_BLACK},
  {TFT_GREEN,TFT_BLACK},
  {TFT_GREEN,0x01},
  {TFT_GREEN,0x01},
  {TFT_GREEN,0x01}
};

float wind_angle_breaks[][2] = {
  {0, 22.5}, // N
  {22.5, 67.5}, // NE
  {67.5, 112.5}, // E
  {112.5, 157.5}, // SE
  {157.5, 202.5}, // S
  {202.5, 247.5}, // SW
  {247.5, 292.5}, // W
  {292.5, 337.5}, // NW
  {337.5, 360} // N
};

char wind_angle_labels[][3] = {
  " N",
  "NE",
  " E",
  "SE",
  " S",
  "SW",
  " W",
  "NW",
  " N"
};

GFXcanvas1 *canvases[NUM_BITMAPS]; 
#define MAX_FORECAST_LEN 256
char forecasts[8][MAX_FORECAST_LEN];
#define MAX_HISTORY_LEN 128
char history_data[8][MAX_HISTORY_LEN];

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


  tft.init();

  // can use PWM on this pin to dim screen - TODO
  pinMode(TFT_BL, OUTPUT); // GPIO13 = PWM 6B 
  gpio_set_function(TFT_BL, GPIO_FUNC_PWM);
  backlight_pwm_slice = pwm_gpio_to_slice_num(TFT_BL);
  pwm_config backlightConfig = pwm_get_default_config();
  pwm_config_set_wrap(&backlightConfig, BACKLIGHT_TOP); // with 255 prescaling, gets to 220 Hz  
  pwm_init(backlight_pwm_slice, &backlightConfig, true);
  pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP/2); // initial value
  pwm_set_clkdiv_int_frac(backlight_pwm_slice, BACKLIGHT_DIV, 0); // 133 MHz/255 = 521.6 kHz clock freq

  tft.setRotation(2);
  tft.fillScreen((TFT_BLACK));


// DO NOT use the Adafruit_GFX fonts! 
//  tft.setFreeFont(&FreeSerif9pt7b); 
  tft.setTextFont(2);
  tft.setTextSize(2);

//  tft.setCursor(20, 0, 2);
  tft.setTextColor(TFT_SKYBLUE);
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

  update_millis = 0;
  web_millis = 0;
  awake_millis = 0;
  pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP);

  char blabel[] = "Forecast"; 
  details_button.initButton(&tft,250,450,120,50,TFT_SKYBLUE,TFT_BLACK,TFT_SKYBLUE,blabel,2);
  details_button.drawButton();

  char hlabel[] = "History"; 
  history_button.initButton(&tft,70,450,120,50,TFT_SKYBLUE,TFT_BLACK,TFT_SKYBLUE,hlabel,2);
  history_button.drawButton();

  // allocate canvases for rendering
  for (int i=0; i < NUM_BITMAPS; ++i) {
    canvases[i] = new GFXcanvas1(bpos[i][2],bpos[i][3]);
  }


}

void loop() {

  uint16_t x, y;
  static uint16_t color = TFT_WHITE;
  static int16_t screen_level = BACKLIGHT_TOP;

  if (tft.getTouch(&x, &y)) {
    awake_millis=0; // reset awake timer
    // do other stuff with x, y position
    screen_level = BACKLIGHT_TOP;

    if (details_button.contains(x,y)) {
      if (!details_button.isPressed()) {
        details_button.press(true); 
        details_button.drawButton(true);
        gizmo_state = STATE_SHOW_FORECAST;
      }
    }
    else if (history_button.contains(x,y)) {
      if (!history_button.isPressed()) {
        history_button.press(true); 
        history_button.drawButton(true);
        gizmo_state = STATE_SHOW_HISTORY;
      }
    }
    else {
      if (details_button.isPressed() || history_button.isPressed()) {
        if (gizmo_state == STATE_WAIT || gizmo_state == STATE_DIMMING || gizmo_state == STATE_SLEEP) {
          tft.fillScreen(TFT_BLACK);
        }
        details_button.press(false);
        details_button.drawButton();
        history_button.press(false);
        history_button.drawButton();
        gizmo_state = STATE_UPDATE;
      }
    }
  }
  else if (awake_millis > awake_delay) {
      gizmo_state = STATE_DIMMING;
      if (screen_level <=0) {
        if(gizmo_state != STATE_WAIT) {
          gizmo_state = STATE_SLEEP;
        }
      }
  }
  else if (update_millis > update_delay && gizmo_state != STATE_WAIT) {
      gizmo_state = STATE_UPDATE;
      update_millis = 0;
  }
  else if (web_millis > web_delay && gizmo_state != STATE_WAIT) {
      gizmo_state = STATE_UPDATE_WEB;
      web_millis = 0;
  }
  else if(gizmo_state != STATE_WAIT) {
    gizmo_state = STATE_SLEEP;
  }

  // do stuff for machine state
  switch (gizmo_state) {
    case STATE_DIMMING:
      screen_level-=(BACKLIGHT_TOP/dimming_levels);
      if (screen_level > MIN_SCREEN_LEVEL) {
        pwm_set_chan_level(backlight_pwm_slice, 1, screen_level);
        delay(dimming_interval);
      }
      break;    
    case STATE_UPDATE:
    { // note locally declared variables requiring scope
      // turn screen back on
      performance_millis = 0;

      pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP-1);

      // update date/time first
      get_date();

      canvases[DATE_CANVAS]->fillScreen(TFT_BLACK);
      canvases[DATE_CANVAS]->setFont(&FreeMonoBold12pt7b);
      canvases[DATE_CANVAS]->setCursor(30, 16);
      canvases[DATE_CANVAS]->printf("%s\n",date_cstring);
      int16_t ycur = canvases[DATE_CANVAS]->getCursorY();
      canvases[DATE_CANVAS]->setCursor(0, ycur+5);
      canvases[DATE_CANVAS]->setFont(&FreeMonoBold18pt7b);
      canvases[DATE_CANVAS]->printf("%s",time_cstring);
      dirty[DATE_CANVAS]=true;
 
      HTTPClient http;
      String sensor_string = "";
      if (http.begin(data_url)) {
        if (http.GET() > 0) {
            sensor_string = http.getString();
        }
        http.end();
      }
//      const int json_cap = 11*JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(11);
//      StaticJsonDocument<json_cap> doc;
      DynamicJsonDocument doc(1024); // just set to 1k buffer
      DeserializationError err = deserializeJson(doc,sensor_string.c_str());
      
      if(err) {
          draw_message(err.c_str());
      }

      if(!doc["wind_angle"]["reading"].isNull() && !doc["wind_vmph"]["reading"].isNull()) {
        float wind_angle = doc["wind_angle"]["reading"];
        float wind_vmph = doc["wind_vmph"]["reading"];

        int widx = -1; 
        for (int i=0; i < 9; ++i) {
          if (wind_angle >= wind_angle_breaks[i][0] && wind_angle < wind_angle_breaks[i][1]) {
            widx = i;
            break;
          }
        }
        char angle_dir[] = "XX";
        if (widx >= 0) {
          strncpy(angle_dir,wind_angle_labels[widx],2);
        }

        canvases[WINDV_CANVAS]->fillScreen(TFT_BLACK);
        canvases[WINDV_CANVAS]->setFont(&FreeMonoBold9pt7b);
        canvases[WINDV_CANVAS]->setCursor(90, 11);
        canvases[WINDV_CANVAS]->printf("WIND GAUGE");           
        canvases[WINDV_CANVAS]->setCursor(5, 50);
        canvases[WINDV_CANVAS]->setFont(&FreeMonoBold24pt7b);
        canvases[WINDV_CANVAS]->printf("%3.1f mph",wind_vmph);
        dirty[WINDV_CANVAS]=true;
  
        canvases[WINDA_CANVAS]->fillScreen(TFT_BLACK);
        canvases[WINDA_CANVAS]->setCursor(5, 40);
        canvases[WINDA_CANVAS]->setFont(&FreeMonoBold24pt7b);
        canvases[WINDA_CANVAS]->printf("%s (%3.1f)", angle_dir, wind_angle);
        dirty[WINDA_CANVAS]=true;
      }

      if(!doc["outside_T"]["reading"].isNull() && !doc["outside_H"]["reading"].isNull()) {
        float outside_T = doc["outside_T"]["reading"];
        float outside_H = doc["outside_H"]["reading"];

        canvases[T_CANVAS]->fillScreen(TFT_BLACK);
        canvases[T_CANVAS]->setFont(&FreeMonoBold24pt7b);
        canvases[T_CANVAS]->setCursor(5, 40);
        canvases[T_CANVAS]->printf("T(F) %3.1f",9*outside_T/5+32);
        dirty[T_CANVAS]=true;

        canvases[H_CANVAS]->fillScreen(TFT_BLACK);
        canvases[H_CANVAS]->setFont(&FreeMonoBold24pt7b);
        canvases[H_CANVAS]->setCursor(5, 40);
        canvases[H_CANVAS]->printf("H(%%) %3.1f",outside_H);
        dirty[H_CANVAS]=true;
      }

      char mbuf[] = "0000000000 ms ";
      uint32_t bval = performance_millis;
      sprintf(mbuf,"%d ms",bval);
      draw_message(mbuf);
      break;

    }

    case STATE_UPDATE_WEB:
    {
      HTTPClient http;
      String micro_string = "";
      if (http.begin(micro_url)) {
        if (http.GET() > 0) {
            micro_string = http.getString();
        }
        http.end();
      }
      DynamicJsonDocument doc(3072); // set to 3k buffer
      DeserializationError err = deserializeJson(doc,micro_string.c_str());
      
      if(err) {
          draw_message(err.c_str());
          break;
      }

      if( !doc["max_temp_24hr"].isNull() && !doc["min_temp_24hr"].isNull()) {
        const char* max_temp_24hr = doc["max_temp_24hr"];
        const char* min_temp_24hr = doc["min_temp_24hr"];
        sprintf(history_data[0],"Max/Min Temp (24 hr):\n  %s / %s",max_temp_24hr,min_temp_24hr);
      }
      if (!doc["max_humidity_24hr"].isNull() && !doc["min_humidity_24hr"].isNull()) {
        const char* max_humidity_24hr = doc["max_humidity_24hr"];
        const char* min_humidity_24hr = doc["min_humidity_24hr"];
        sprintf(history_data[1],"Max/Min Hum (24 hr):\n  %s / %s\n",max_humidity_24hr,min_humidity_24hr);
      }

      if (!doc["vmph_max_24hr"].isNull() && !doc["vmph_1m"].isNull()) {
        const char* vmph_max_24hr = doc["vmph_max_24hr"];
        const char* vmph_1m = doc["vmph_1m"];
        sprintf(history_data[2],"Max Wind (1 min/24 hr):\n  %s / %s",vmph_1m,vmph_max_24hr);
      }
      if (!doc["nicedt_vmph_max_24hr"].isNull()) {
        const char* vmph_max_24hr_dt = doc["nicedt_vmph_max_24hr"];
        sprintf(history_data[3],"Max Wind (24 hr):\n %s",vmph_max_24hr_dt);
      }

      if (!doc["dir_med_24hr"].isNull() && !doc["dir_med_1m"].isNull()) {
        const char* dir_med_24hr = doc["dir_med_24hr"];
        const char* dir_med_1m = doc["dir_med_1m"];
        sprintf(history_data[4],"Wind Dir (1 min/24 hr):\n %s/%s",dir_med_1m,dir_med_24hr);
      }

      if (!doc["vmph_max_record"].isNull() && !doc["nicedt_vmph_record"].isNull()) {
        const char* vmph_max_record = doc["vmph_max_record"];
        const char* vmph_max_record_dt = doc["nicedt_vmph_record"];
        sprintf(history_data[5],"Record Wind Gust:\n %s/%s\n",vmph_max_record,(vmph_max_record_dt+2));
      }

      if (!doc["precipytd_in"].isNull()) {
        const char* precipytd_in = doc["precipytd_in"];
        sprintf(history_data[6],"Precip for Year:\n  %s",precipytd_in);
      }

      char tag[] = "Forecastx";
      for (int i=0; i < 8; ++i) {
        sprintf(tag,"Forecast%d",i);
        if(!doc[tag].isNull()) {
          const char* f0 = doc[tag];
          strncpy(forecasts[i], f0, MAX_FORECAST_LEN-1);
        }
        else {
          strncpy(forecasts[i],"", MAX_FORECAST_LEN-1);
        }
 
      }

      canvases[PRECIP_CANVAS]->fillScreen(TFT_BLACK);
      canvases[PRECIP_CANVAS]->setFont(&FreeMonoBold9pt7b);
      canvases[PRECIP_CANVAS]->setCursor(90, 11);
      canvases[PRECIP_CANVAS]->printf("PRECIP (hr/day)");
      canvases[PRECIP_CANVAS]->setFont(&FreeMonoBold18pt7b);
      canvases[PRECIP_CANVAS]->setCursor(5, 48);
      if (!doc["precip_inphr"]["reading"].isNull() && !doc["dailyprecip_in"]["reading"].isNull()) {
        float precip_inphr = doc["precip_inphr"]["reading"];
        float dailyprecip_in = doc["dailyprecip_in"]["reading"];
        canvases[PRECIP_CANVAS]->printf("%.1f in/%.1f in",precip_inphr,dailyprecip_in);
      }

      dirty[PRECIP_CANVAS]=true;



      break;

    }
    case STATE_SHOW_FORECAST:
    {
      tft.fillRect(0, 0, 320, 450, TFT_BLACK);
      tft.setCursor(0,0);
      GFXcanvas1 forecast_canvas(320,450);
      forecast_canvas.setFont(&FreeSerif9pt7b);
      forecast_canvas.setCursor(0, 17);
      forecast_canvas.println(forecasts[0]);
      forecast_canvas.println("");
      forecast_canvas.println(forecasts[1]);
      forecast_canvas.println("");
      forecast_canvas.println(forecasts[2]);
      tft.drawBitmap(0,0,forecast_canvas.getBuffer(),320,450,TFT_CYAN,TFT_BLACK);
      // have to transition here
      gizmo_state = STATE_WAIT;
      break;
    }
    case STATE_SHOW_HISTORY:
    {
      tft.fillRect(0, 0, 320, 450, TFT_BLACK);
      tft.setCursor(0,0);
      GFXcanvas1 history_canvas(320,450);
      history_canvas.setFont(&FreeMonoBold12pt7b);
      history_canvas.setCursor(0, 17);
      for (int i=0; i < 7; ++i) {
        history_canvas.printf("%s\n",history_data[i]);
      }
      tft.drawBitmap(0,0,history_canvas.getBuffer(),320,450,TFT_CYAN,TFT_BLACK);
      // have to transition here
      gizmo_state = STATE_WAIT;
      break;
    }
    case STATE_WAIT:
      break;
    case STATE_SLEEP:
      for (int i=0; i < NUM_BITMAPS; ++i) {
        if (dirty[i]) {
          dirty[i]=false;
          tft.drawBitmap(bpos[i][0],bpos[i][1],canvases[i]->getBuffer(),bpos[i][2],bpos[i][3],canvas_colors[i][0],canvas_colors[i][1]);
          break; // only do first one
        }
      }
      break;
  }
//  if (maxlines > 0) {
//    digitalWrite(PIN_LED, HIGH);
//    tft.setTextColor(color, TFT_WHITE);  
//    color = color - 128;
//    tft.println(get_date(test_times[MAXLINES-maxlines]));
//    pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP/maxlines); 
//    maxlines--;
//    delay(100);
//    digitalWrite(PIN_LED, LOW);
//    delay(100);

//  }
//  else if (tft.getTouch(&x, &y)) {
//    tft.setTextColor(TFT_GREEN, TFT_BLACK,true);  

//    tft.fillRect(5, 385,150,17,TFT_BLACK);
//    tft.setCursor(5, 400, 2);
//    tft.printf("x: %i     ", x);

//    tft.fillRect(5, 405,150,17,TFT_BLACK);
//    tft.setCursor(5, 420, 2);
//    tft.printf("y: %i    ", y);

//    tft.drawCircle(x, y, 5, TFT_GREEN);

//  }

}
