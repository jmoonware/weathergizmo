
#include <WiFi.h>
// secret, contains definition of local_ssid, local_pass, and microd_url/microf_url , literally: 
// char local_ssid[] = "NNN";  //  your network SSID (name)
// char local_pass[] = "PPP";  // your network password
// char microd_url[] = "http://your-data-endpoint-d/";
// char microf_url[] = "http://your-data-endpoint-f/";
#include "/home/jmoon/Arduino/libraries/local/ssid_harvest.h"
// #include "/home/jmoon/Arduino/libraries/local/ssid_coleman.h"
// GAAH! DO NOT USE 'HttpClient.h' - case matters, it's a totally different library!
#include <HTTPClient.h>
#include <NTPClient.h>
#include "src/DateTimeNTP/DateTimeNTP.h"

char nws_tla[] = "ELM"; 
char second_tla[] = "SHD"; // FIXME - should have this as part of JSON payload

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

// try to import a TFT_eSPI font...
#include <TFT_eSPI.h>
#include "Fonts/GFXFF/FreeSans9pt7b.h"
#include <hardware/pwm.h>
#include <elapsedMillis.h>
#include <ArduinoJson.h>

// Backlight update = 133 MHz/(255*2360) = 221 Hz
#define BACKLIGHT_DIV 255
#define BACKLIGHT_TOP 2360
#define MIN_SCREEN_LEVEL 100

// also uncomment line 43 in HTTPClient.h
#define WEATHERGIZMO_DEBUG 0 // set to non-zero to debug on serial port
// uncommment this next line if WEATHERGIZMO_DEBUG is not zero
//#define DEBUG_WEATHERGIZMO(fmt, ...) Serial.printf(fmt, ## __VA_ARGS__ )
#ifndef DEBUG_WEATHERGIZMO
#define DEBUG_WEATHERGIZMO(...) do { (void)0; } while (0)
#endif

// needed for formatting forecast screens
#define SCREEN_WIDTH_CHARS 42
#define SCREEN_HEIGHT_LINES 20
int current_forecast_line = 0;
int forecast_lines = 0;
int forecast_screen_number = 1; // keep track of how many forecast screens we have shown

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
TFT_eSPI_Button forecast_button = TFT_eSPI_Button();
TFT_eSPI_Button history_button = TFT_eSPI_Button();
TFT_eSPI_Button local_nws_button = TFT_eSPI_Button();

int wifi_status = WL_IDLE_STATUS;     // the Wifi radio's status

WiFiUDP ntpUDP;
NTPClient theNTPUDPClient(ntpUDP);
DateTimeNTP dtntp(&theNTPUDPClient);

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
  tft.println(httpsGetInsecure(microd_url));

}

static uint32_t color = 0xFFFF;
#define MAXLINES 16
static uint8_t maxlines = MAXLINES;

elapsedMillis performance_millis;

enum GIZMO_STATES {
  STATE_SLEEP,
  STATE_UPDATE,
  STATE_DIMMING,
  STATE_SHOW_FORECAST,
  STATE_SHOW_MORE_FORECAST,
  STATE_SHOW_HISTORY,
  STATE_SHOW_LOCAL_NWS,
  STATE_WAIT,
  STATE_WAIT_NEXT,
  STATE_CHECK_WIFI,
};

uint8_t gizmo_state;

elapsedMillis awake_millis;
elapsedMillis update_millis;
elapsedMillis thp_millis;
elapsedMillis check_wifi_millis;

uint32_t awake_delay = 60*1000; // ms
uint32_t update_delay = 750; // ms, for faster things (time, wind data) 
uint32_t thp_delay = 60*1000; // ms, for slower things
uint32_t check_wifi_delay = 30*1000; // ms

uint8_t dimming_interval = 10; // ms
uint8_t dimming_levels = 100; // linear walk-down of screen brightness 

enum BITMAP_NAMES {
  DATE_CANVAS,
  BANNER_CANVAS,
  T_CANVAS,
  H_CANVAS,
  P_CANVAS,
  WINDV_CANVAS,
  WINDA_CANVAS,
  MSG_CANVAS,
  NUM_BITMAPS
};

// render bitmaps one at a time during sleep state - it is slow!
bool dirty[NUM_BITMAPS]; 

//int bpos[][4] = {
//  {0,0,320,50}, // x, y, w, h; date
//  {0,70,320,50}, // temp
//  {0,120,320,50}, // hum
//  {0,200,320,55}, // wind v
//  {0,255,320,50}, // wind angle
//  {0,340,320,55} // precip
//};

#define LABEL_WIDTH 150
#define LABEL_OFFSET 35

#define BANNER_GAP 30

int data_pos[][4] = {
  {0,0,320,50}, // x, y, w, h; date
  {0,60,320,BANNER_GAP}, // x, y, w, h; date
  {LABEL_WIDTH,70  + BANNER_GAP, 320-LABEL_WIDTH,50}, // temp
  {LABEL_WIDTH,120 + BANNER_GAP, 320-LABEL_WIDTH,50}, // hum
  {LABEL_WIDTH,170 + BANNER_GAP, 320-LABEL_WIDTH,50}, // pressure
  {LABEL_WIDTH,220 + BANNER_GAP, 320-LABEL_WIDTH,50}, // wind v
  {LABEL_WIDTH,270 + BANNER_GAP, 320-LABEL_WIDTH,50}, // wind angle
  {0,320 + BANNER_GAP,320 - LABEL_WIDTH,50}, // msg
};

int label_pos[][4] = {
  {0,0,320,50}, // x, y, w, h; date
  {0,60,320,50}, // x, y, w, h; date
  {0,70 + BANNER_GAP,LABEL_WIDTH,50}, // temp
  {0,120 + BANNER_GAP,LABEL_WIDTH,50}, // hum
  {0,170 + BANNER_GAP,LABEL_WIDTH,50}, // pressure
  {0,220 + BANNER_GAP,LABEL_WIDTH,50}, // wind v
  {0,270 + BANNER_GAP,LABEL_WIDTH,50}, // wind angle
  {0,320 + BANNER_GAP,LABEL_WIDTH,50}, // msg
};

//int canvas_colors[][2] = {
//  {TFT_SKYBLUE,TFT_BLACK},
//  {TFT_GREEN,TFT_BLACK},
//  {TFT_GREEN,TFT_BLACK},
//  {TFT_GREEN,0x01},
//  {TFT_GREEN,0x01},
//  {TFT_GREEN,0x01}
//};

// text color, background color
int label_canvas_colors[][2] = {
  {TFT_SKYBLUE,TFT_BLACK},
  {TFT_YELLOW,TFT_BLACK},
  {TFT_GREEN,TFT_DARKGREY},
  {TFT_GREEN,TFT_BLACK},
  {TFT_GREEN,TFT_DARKGREY},
  {TFT_GREEN,TFT_BLACK},
  {TFT_GREEN,TFT_DARKGREY},
  {TFT_GREEN,TFT_BLACK},
};

int data_canvas_colors[][2] = {
  {TFT_SKYBLUE,TFT_BLACK},
  {TFT_WHITE,TFT_BLACK},
  {TFT_WHITE,TFT_DARKGREY},
  {TFT_WHITE,TFT_BLACK},
  {TFT_WHITE,TFT_DARKGREY},
  {TFT_WHITE,TFT_BLACK},
  {TFT_WHITE,TFT_DARKGREY},
  {TFT_WHITE,TFT_BLACK},
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

GFXcanvas1 *label_canvases[NUM_BITMAPS]; 
GFXcanvas1 *data_canvases[NUM_BITMAPS]; 

#define MAX_HISTORY_LEN 128
char history_data[8][MAX_HISTORY_LEN];

void draw_message(const char *msg) {
//  tft.setCursor(5, 430, 2);
//  tft.setTextFont(2);
//  tft.setTextColor(TFT_RED, TFT_LIGHTGREY,true);  
//  tft.fillRect(0, 400, 175, 40, TFT_DARKGREY);
//  tft.println(msg);
  label_canvases[MSG_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[MSG_CANVAS]->setFont(&FreeMonoBold9pt7b);
  label_canvases[MSG_CANVAS]->setCursor(5, BANNER_GAP - 10);
  label_canvases[MSG_CANVAS]->printf(msg);
  tft.drawBitmap(label_pos[MSG_CANVAS][0],label_pos[MSG_CANVAS][1],label_canvases[MSG_CANVAS]->getBuffer(),label_pos[MSG_CANVAS][2],label_pos[MSG_CANVAS][3],label_canvas_colors[MSG_CANVAS][0],label_canvas_colors[MSG_CANVAS][1]);

}

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

String httpsGetInsecure(const char* url)
{
    WiFiClientSecure client;
    HTTPClient https;

    // Disable TLS certificate verification
    client.setInsecure();

    DEBUG_HTTPCLIENT("HTTPS GET: ");
    DEBUG_HTTPCLIENT(url);

    // Begin HTTPS connection
    if (!https.begin(client, url)) {
        DEBUG_HTTPCLIENT("HTTPS begin failed");
        return "";
    }

    int httpCode = https.GET();

    String payload = "";

    if (httpCode > 0) {
        DEBUG_HTTPCLIENT("HTTP response code: ");
        DEBUG_HTTPCLIENT(httpCode);

        if (httpCode == HTTP_CODE_OK) {
            payload = https.getString();
        }
    } else {
        DEBUG_HTTPCLIENT("HTTPS GET failed, error: ");
        DEBUG_HTTPCLIENT(https.errorToString(httpCode));
    }

    https.end();  // Free resources
    return payload;
}


///////////////////////////////
// Checks if wifi is connected and attempts to reconnect if not
///////////////////////////////

char message_buffer[256];

void check_wifi() {

  wifi_status = WiFi.status();

  // if we are connected don't do anything
  if (wifi_status != WL_CONNECTED) {
    while (wifi_status != WL_CONNECTED) {
      wifi_status = WiFi.begin(local_ssid,local_pass);
      digitalWrite(PIN_LED, HIGH);
      delay(100);
      digitalWrite(PIN_LED, LOW);
      // wait for connection:
      delay(1000);
      sprintf(message_buffer, "Connect wifi status = %d",wifi_status);
      draw_message(message_buffer);
    }
  }
}

void setup() {
 
  // blink once when setup begins
  digitalWrite(PIN_LED, HIGH);
  delay(100);
  digitalWrite(PIN_LED, LOW);
  delay(100);

  if (WEATHERGIZMO_DEBUG) {
    Serial.begin();
  }

// nothing was coming out of pins from scope so had to do this manually
//  gpio_set_function(TFT_CS, GPIO_FUNC_SPI);
//  gpio_set_function(TFT_SCLK, GPIO_FUNC_SPI);
//  gpio_set_function(TFT_MOSI, GPIO_FUNC_SPI);
//  gpio_set_function(TFT_MISO, GPIO_FUNC_SPI);

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
  tft.fillScreen(TFT_BLACK);


// DO NOT use the Adafruit_GFX fonts! 
//  tft.setFreeFont(&FreeSerif9pt7b); 
  tft.setTextFont(2);
  tft.setTextSize(1);

//  tft.setCursor(20, 0, 2);
  tft.setTextColor(TFT_SKYBLUE);
  tft.println(" ");
  tft.println("Hello!");

  tft.setTextColor(TFT_GREEN); 
  tft.println("Hello!");

  tft.setTextColor(TFT_RED);
  tft.println("Hello!");

  tft.setFreeFont(&FreeSans9pt7b);
  tft.println("Serif 9pt b");

//  uint8_t ft = 1; 
//  for (int i=0; i < 256; ++i) {  
//   tft.setTextFont(i); 
//   tft.printf("%d-X ",i);
//  }

  tft.setTextFont(2);
  tft.setTextSize(2);

  tft.println('\n');

  tft.setTextColor(TFT_WHITE);
  tft.println("Connecting"); 
  while (wifi_status != WL_CONNECTED) {
    // Connect to WPA/WPA2 network:
    wifi_status = WiFi.begin(local_ssid,local_pass);
    tft.print('.');
    // wait for connection:
    delay(1000);
  }


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
  awake_millis = 0;
  thp_millis = thp_delay;
  check_wifi_millis = 0;
  pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP);

  char blabel[] = "Forecast"; 
  forecast_button.initButton(&tft,250,450,120,50,TFT_SKYBLUE,TFT_BLACK,TFT_SKYBLUE,blabel,2);
  forecast_button.drawButton();

  char hlabel[] = "History"; 
  history_button.initButton(&tft,70,450,120,50,TFT_SKYBLUE,TFT_BLACK,TFT_SKYBLUE,hlabel,2);
  history_button.drawButton();

  // for the local NWS data
  char nws_label[] = "NWS     ";
  sprintf(nws_label,"NWS[%s]",nws_tla);

//  tft.setFreeFont(&FreeMonoBold12pt7b);
  local_nws_button.initButton(&tft,240,385,150,50,TFT_YELLOW,TFT_BLACK,TFT_SKYBLUE,nws_label,2);
  local_nws_button.drawButton();

  // allocate canvases for rendering
//  for (int i=0; i < NUM_BITMAPS; ++i) {
//    canvases[i] = new GFXcanvas1(bpos[i][2],bpos[i][3]);
//  }

  // allocate canvases for rendering
  for (int i=0; i < NUM_BITMAPS; ++i) {
    label_canvases[i] = new GFXcanvas1(label_pos[i][2],label_pos[i][3]);
  }
  // allocate canvases for rendering
  for (int i=0; i < NUM_BITMAPS; ++i) {
    data_canvases[i] = new GFXcanvas1(data_pos[i][2],data_pos[i][3]);
  }
  
  // print intial labels

  label_canvases[DATE_CANVAS]->fillScreen(TFT_BLACK);

  label_canvases[BANNER_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[BANNER_CANVAS]->setFont(&FreeMonoBold12pt7b);
  label_canvases[BANNER_CANVAS]->setCursor(5, BANNER_GAP - 10);
  label_canvases[BANNER_CANVAS]->printf(banner);

  label_canvases[T_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[T_CANVAS]->setFont(&FreeMonoBold18pt7b);
  label_canvases[T_CANVAS]->setCursor(5, LABEL_OFFSET);
  label_canvases[T_CANVAS]->printf("T(F)");

  label_canvases[H_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[H_CANVAS]->setFont(&FreeMonoBold18pt7b);
  label_canvases[H_CANVAS]->setCursor(5, LABEL_OFFSET);
  label_canvases[H_CANVAS]->printf("H(%%)");

  label_canvases[P_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[P_CANVAS]->setFont(&FreeMonoBold18pt7b);
  label_canvases[P_CANVAS]->setCursor(5, LABEL_OFFSET);
  label_canvases[P_CANVAS]->printf("P(inHg)");

  label_canvases[WINDA_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[WINDA_CANVAS]->setFont(&FreeMonoBold18pt7b);
  label_canvases[WINDA_CANVAS]->setCursor(5, LABEL_OFFSET);
  label_canvases[WINDA_CANVAS]->printf("D(deg)");

  label_canvases[WINDV_CANVAS]->fillScreen(TFT_BLACK);
  label_canvases[WINDV_CANVAS]->setFont(&FreeMonoBold18pt7b);
  label_canvases[WINDV_CANVAS]->setCursor(5, LABEL_OFFSET);
  label_canvases[WINDV_CANVAS]->printf("V(mph)");

  draw_labels();

  // fix up microf url
  char s_width[] = "    "; 
  sprintf(s_width,"%d",SCREEN_WIDTH_CHARS);
  size_t len_microf_url = strlen(microf_url);
  // last two chars are screen width (can't be single or triple digit) - probably should check
  // but as it is hard coded leave for now
  microf_url[len_microf_url-2]=s_width[0];
  microf_url[len_microf_url-1]=s_width[1];


}

void draw_labels() {
  for (int i=0; i < NUM_BITMAPS; ++i) {
      tft.drawBitmap(label_pos[i][0],label_pos[i][1],label_canvases[i]->getBuffer(),label_pos[i][2],label_pos[i][3],label_canvas_colors[i][0],label_canvas_colors[i][1]);
  }
}

JsonDocument forecast_doc; // buffer size
JsonDocument doc; // buffer size
JsonArray forecast_arr;


void loop() {

  uint16_t x, y;
  static uint16_t color = TFT_WHITE;
  static int16_t screen_level = BACKLIGHT_TOP;

  if (tft.getTouch(&x, &y)) {
    awake_millis=0; // reset awake timer
    // do other stuff with x, y position
    screen_level = BACKLIGHT_TOP;

    if (forecast_button.contains(x,y)) {
      if (!forecast_button.isPressed()) {
        forecast_button.press(true); 
        forecast_button.drawButton(true);
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
    else if (local_nws_button.contains(x,y)) {
      if (!local_nws_button.isPressed()) {
        local_nws_button.press(true); 
        local_nws_button.drawButton(true);
        gizmo_state = STATE_SHOW_LOCAL_NWS;
      }
    }
    else if (forecast_button.isPressed() && gizmo_state == STATE_WAIT_NEXT) {
      gizmo_state = STATE_SHOW_MORE_FORECAST;
    }
    else { // transition back to update
      if (forecast_button.isPressed() || history_button.isPressed() || local_nws_button.isPressed()) {
        if (gizmo_state == STATE_WAIT || gizmo_state == STATE_DIMMING || gizmo_state == STATE_SLEEP) {
          tft.fillScreen(TFT_BLACK);
          draw_labels();
        }
        forecast_button.press(false);
        forecast_button.drawButton();
        history_button.press(false);
        history_button.drawButton();
        local_nws_button.press(false);
        local_nws_button.drawButton();
        thp_millis = thp_delay;
        gizmo_state = STATE_UPDATE;
      }
    }
  }
  else if (awake_millis > awake_delay) {
      gizmo_state = STATE_DIMMING;
      if (screen_level <=0) {
        if(gizmo_state != STATE_WAIT && gizmo_state != STATE_WAIT_NEXT) {
          gizmo_state = STATE_SLEEP;
        }
      }
  }
  else if (check_wifi_millis > check_wifi_delay && gizmo_state != STATE_WAIT && gizmo_state != STATE_WAIT_NEXT) {
      gizmo_state = STATE_CHECK_WIFI;
      check_wifi_millis = 0;
  }
  else if (update_millis > update_delay && gizmo_state != STATE_WAIT && gizmo_state != STATE_WAIT_NEXT) {
      gizmo_state = STATE_UPDATE;
      update_millis = 0;
  }
  else if(gizmo_state != STATE_WAIT && gizmo_state != STATE_WAIT_NEXT) {
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
    case STATE_CHECK_WIFI:
    {
      check_wifi();
//      sprintf(message_buffer, "Wifi status = %d",wifi_status);
//      draw_message(message_buffer);
      break;
    } 
    case STATE_UPDATE:
    { // note locally declared variables requiring scope
      // turn screen back on
      performance_millis = 0;

      pwm_set_chan_level(backlight_pwm_slice, 1, BACKLIGHT_TOP-1);

      // update date/time first
      dtntp.get_date();

      data_canvases[DATE_CANVAS]->fillScreen(TFT_BLACK);
      data_canvases[DATE_CANVAS]->setFont(&FreeMonoBold12pt7b);
      data_canvases[DATE_CANVAS]->setCursor(30, 16);
      data_canvases[DATE_CANVAS]->printf("%s\n",dtntp.date_cstring);
      int16_t ycur = data_canvases[DATE_CANVAS]->getCursorY();
      data_canvases[DATE_CANVAS]->setCursor(0, ycur+5);
      data_canvases[DATE_CANVAS]->setFont(&FreeMonoBold18pt7b);
      data_canvases[DATE_CANVAS]->printf("%s",dtntp.time_cstring);
      dirty[DATE_CANVAS]=true;
 
      String sensor_string = httpsGetInsecure(microd_url);
//      const int json_cap = 11*JSON_OBJECT_SIZE(3) + JSON_OBJECT_SIZE(11);
//      StaticJsonDocument<json_cap> doc;

      DeserializationError err = deserializeJson(doc,sensor_string.c_str());
      
      if(err) {
//          draw_message(err.c_str());
          DEBUG_WEATHERGIZMO(err.c_str());
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

        data_canvases[WINDV_CANVAS]->fillScreen(TFT_BLACK);
        data_canvases[WINDV_CANVAS]->setCursor(5, LABEL_OFFSET);
        data_canvases[WINDV_CANVAS]->setFont(&FreeMonoBold18pt7b);
        data_canvases[WINDV_CANVAS]->printf("%3.1f",wind_vmph);
        dirty[WINDV_CANVAS]=true;
  
        data_canvases[WINDA_CANVAS]->fillScreen(TFT_BLACK);
        data_canvases[WINDA_CANVAS]->setCursor(5, LABEL_OFFSET);
        data_canvases[WINDA_CANVAS]->setFont(&FreeMonoBold18pt7b);
        data_canvases[WINDA_CANVAS]->printf("%s (%3.0f)", angle_dir, wind_angle);
        dirty[WINDA_CANVAS]=true;
      }

      if (thp_millis >= thp_delay) {
        thp_millis = 0;
        data_canvases[T_CANVAS]->fillScreen(TFT_BLACK);
        data_canvases[T_CANVAS]->setFont(&FreeMonoBold18pt7b);
        data_canvases[T_CANVAS]->setCursor(5, LABEL_OFFSET); 
        if(!doc["outside_T_F"].isNull()) {
          const char* outside_T_F = doc["outside_T_F"];
          data_canvases[T_CANVAS]->printf("%s",outside_T_F);
          dirty[T_CANVAS]=true;
        }
        if(!doc["second"])
        data_canvases[T_CANVAS]->setFont(&FreeSerif9pt7b);

        data_canvases[H_CANVAS]->fillScreen(TFT_BLACK);
        data_canvases[H_CANVAS]->setFont(&FreeMonoBold18pt7b);
        data_canvases[H_CANVAS]->setCursor(5, LABEL_OFFSET);
        if(!doc["outside_H_perc"].isNull()) {
          const char* outside_H_perc = doc["outside_H_perc"];
          data_canvases[H_CANVAS]->printf("%s",outside_H_perc);
          dirty[H_CANVAS]=true;
        }

        data_canvases[P_CANVAS]->fillScreen(TFT_BLACK);
        data_canvases[P_CANVAS]->setFont(&FreeMonoBold18pt7b);
        data_canvases[P_CANVAS]->setCursor(5, LABEL_OFFSET);
        if (!doc["outside_P_inHg"].isNull()) {
          const char* outside_P_inHg = doc["outside_P_inHg"];
          data_canvases[P_CANVAS]->printf("%s",outside_P_inHg);
          dirty[P_CANVAS]=true;
        }
      }
      for (int i=0; i < NUM_BITMAPS; ++i) {
        if (dirty[i]) {
          dirty[i]=false;
          tft.drawBitmap(data_pos[i][0],data_pos[i][1],data_canvases[i]->getBuffer(),data_pos[i][2],data_pos[i][3],data_canvas_colors[i][0],data_canvas_colors[i][1]);
        }
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
      if (!doc["max_pressure_24hr"].isNull() && !doc["min_pressure_24hr"].isNull()) {
        const char* max_pressure_24hr = doc["max_pressure_24hr"];
        const char* min_pressure_24hr = doc["min_pressure_24hr"];
        sprintf(history_data[2],"Max/Min P (24 hr):\n  %s / %s\n",max_pressure_24hr,min_pressure_24hr);
      }

      if (!doc["vmph_max_24hr"].isNull() && !doc["vmph_1m"].isNull()) {
        const char* vmph_max_24hr = doc["vmph_max_24hr"];
        const char* vmph_1m = doc["vmph_1m"];
        sprintf(history_data[3],"Max Wind (1 min/24 hr):\n  %s / %s",vmph_1m,vmph_max_24hr);
      }
      if (!doc["nicedt_vmph_max_24hr"].isNull()) {
        const char* vmph_max_24hr_dt = doc["nicedt_vmph_max_24hr"];
        sprintf(history_data[4],"Max Wind (24 hr):\n %s",vmph_max_24hr_dt);
      }

      if (!doc["dir_med_24hr"].isNull() && !doc["dir_med_1m"].isNull()) {
        const char* dir_med_24hr = doc["dir_med_24hr"];
        const char* dir_med_1m = doc["dir_med_1m"];
        sprintf(history_data[5],"Wind Dir (1 min/24 hr):\n %s/%s",dir_med_1m,dir_med_24hr);
      }

      if (!doc["vmph_max_record"].isNull() && !doc["nicedt_vmph_record"].isNull()) {
        const char* vmph_max_record = doc["vmph_max_record"];
        const char* vmph_max_record_dt = doc["nicedt_vmph_record"];
        sprintf(history_data[6],"Record Wind Gust:\n %s/%s\n",vmph_max_record,(vmph_max_record_dt+2));
      }

      draw_message("");

      break;

    }
    case STATE_SHOW_FORECAST:
    {
      tft.fillRect(0, 0, 320, 450, TFT_BLACK);
      tft.setCursor(0,0);
      GFXcanvas1 forecast_canvas(320,450);
      forecast_canvas.setFont(&FreeSerif9pt7b);

      String sensor_string = httpsGetInsecure(microf_url);
      DeserializationError err = deserializeJson(forecast_doc,sensor_string.c_str());
      if(err) {
//          draw_message(err.c_str());
          DEBUG_WEATHERGIZMO(err.c_str());
      }

      forecast_arr = forecast_doc.as<JsonArray>();
      forecast_lines = forecast_arr.size();

      forecast_canvas.setCursor(0, 17);

      forecast_screen_number = 1; // reset to first value
      for (current_forecast_line=0; current_forecast_line < SCREEN_HEIGHT_LINES && current_forecast_line < forecast_lines; ++current_forecast_line) {
        forecast_canvas.println(forecast_arr[current_forecast_line].as<const char*>());
      }

      tft.drawBitmap(0,0,forecast_canvas.getBuffer(),320,450,TFT_CYAN,TFT_BLACK);
      // have to transition here
      gizmo_state = STATE_WAIT_NEXT;
      DEBUG_WEATHERGIZMO("Show Forecast %s\n",forecasts[0]);
      break;
    }
    case STATE_SHOW_MORE_FORECAST:
    {
      tft.fillRect(0, 0, 320, 450, TFT_BLACK);
      tft.setCursor(0,0);
      GFXcanvas1 forecast_canvas(320,450);
      forecast_canvas.setFont(&FreeSerif9pt7b);
      forecast_canvas.setCursor(0, 17);

      forecast_screen_number +=1;

      for (; current_forecast_line < SCREEN_HEIGHT_LINES*forecast_screen_number && current_forecast_line < forecast_lines; ++current_forecast_line) {
        forecast_canvas.println(forecast_arr[current_forecast_line].as<const char*>());
      }

      tft.drawBitmap(0,0,forecast_canvas.getBuffer(),320,450,TFT_CYAN,TFT_BLACK);
      // have to conditionally transition here

      if (current_forecast_line < forecast_lines) {
        // haven't shown all the lines yet
        gizmo_state = STATE_WAIT_NEXT; 
      }
      else { // done
        gizmo_state = STATE_WAIT;
      }
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
    case STATE_SHOW_LOCAL_NWS:
    {
      tft.fillRect(0, 0, 320, 450, TFT_BLACK);
      tft.setCursor(0,0);
      GFXcanvas1 local_nws_canvas(320,450);
      local_nws_canvas.setFont(&FreeMonoBold12pt7b);
      local_nws_canvas.setCursor(0, 17);
      String sensor_string = httpsGetInsecure(microd_url);
      DeserializationError err = deserializeJson(doc,sensor_string.c_str());
      if(err) {
//          draw_message(err.c_str());
          DEBUG_WEATHERGIZMO(err.c_str());
      }

      local_nws_canvas.printf("For Location %s:\n\n",nws_tla);

      JsonObject obj = doc.as<JsonObject>();
      for (JsonPair kv : obj) {
        const char* key = kv.key().c_str();
        if (strncmp(key, nws_tla, 3) == 0) {
          if(strncmp(&(key[4]), "las", 3) == 0) {
            local_nws_canvas.printf("%s:\n %s\n",&(key[4]), kv.value().as<const char *>());
          }
          else {
            local_nws_canvas.printf("%s:    %s\n",&(key[4]), kv.value().as<const char *>());
          }
        }
      }

      tft.drawBitmap(0,0,local_nws_canvas.getBuffer(),320,450,TFT_CYAN,TFT_BLACK);
      // have to transition here
      gizmo_state = STATE_WAIT;
      break;
    }

    case STATE_WAIT:
      break;
    case STATE_WAIT_NEXT:
      break;
    case STATE_SLEEP:
      int num_to_render = 2;
      int rendered = 0;
      for (int i=0; i < NUM_BITMAPS; ++i) {
        if (dirty[i]) {
//          dirty[i]=false;
//          tft.drawBitmap(data_pos[i][0],data_pos[i][1],data_canvases[i]->getBuffer(),data_pos[i][2],data_pos[i][3],data_canvas_colors[i][0],data_canvas_colors[i][1]);
          rendered+=1;
          if (rendered >= num_to_render) {
            break;
          }
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
