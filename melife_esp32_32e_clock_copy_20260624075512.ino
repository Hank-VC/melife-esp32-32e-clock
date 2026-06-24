/**
 * This sketch is used to display an NTP clock on the MELIFE ESP32-32E "CYD".
 * Connects to Wi-Fi, updates time from specified NTP server, displays date and time. Updates clock on the minute. 
 * Re-syncs to NTP server hourly.
 * Hidden touch button in top right of clock screen that will display network info for 5 seconds.
 * This code was mostly developed using Google Gemini and altered as needed.
 */

#include <WiFi.h>
#include <time.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h> 

// Include standard GFX Sans-Serif font headers
#include <Fonts/FreeSans12pt7b.h>     // For Date, AM/PM, Info Page, and Timezone labels
#include <Fonts/FreeSansBold18pt7b.h> // Base 18pt font scaled by 3 for a max single-line clock

// --- Wi-Fi Credentials ---
const char* ssid = "YOUR_SSID";
// const char* password = "YOUR_WIFI_PASSWORD";
// Uncomment the above line and substitute string with your own password if needed.

// --- NTP Configuration for US Central Time (CST/CDT) ---
const char* ntpServer  = "pool.ntp.org";

// timeZone string is made of the following info: Standard Time, DST Time, and Transition Rules (Start and End).
// Use a timezone string appropriate for the area of use. Google one if needed.
// In the string used here, "CST6" is the Standard Time Name and offset, "CDT" is the Daylight Saving Time Name,
// "M3.2.0" is the DST Start Rule, and "M11.1.0" is the DST End Rule.
//  Research and use a timezone string for your area as needed.
const char* timeZone   = "CST6CDT,M3.2.0,M11.1.0"; 

// --- Sync Timing Control ---
const unsigned long SYNC_INTERVAL = 3600000UL; 
unsigned long lastSyncTime = 0;

// --- Melife / CYD Display Hardware Trace Pins ---
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1  

// --- YOUR VERIFIED HARDWIRED TOUCH MODULE CHIP TRACES ---
#define XPT_SCLK_PIN  25
#define XPT_MISO_PIN  39
#define XPT_MOSI_PIN  32
#define XPT_CS_PIN    33
#define LCD_BL_PIN    21

// Initialize Display Controller using standard hardware SPI settings
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

// Force Touch onto the completely separate hardware HSPI bus
SPIClass hspiTouch(HSPI);
XPT2046_Touchscreen touch(XPT_CS_PIN); 

int lastMinute = -1;

// Global buffers to permanently protect against stack allocation overflows
char timeString[32];
char dateString[32];
char tzString[16];

// --- Navigation State Machine ---
enum ViewState { SHOWING_CLOCK, SHOWING_INFO };
ViewState currentView = SHOWING_INFO; 
unsigned long infoPageStartTime = 0;

// --- Hardware Touchscreen Calibration Boundaries ---
#define TS_MINX 300
#define TS_MAXX 3800
#define TS_MINY 250
#define TS_MAXY 3700

SPISettings touchSPI(2500000, MSBFIRST, SPI_MODE0);

// Draws the system information layout view without blocking
void drawInfoPageText() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeSans12pt7b);
  tft.setTextSize(1);
  
  tft.setCursor(15, 50);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("SSID: ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(ssid);

  tft.setCursor(15, 95);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("IP: ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(WiFi.localIP().toString());

  tft.setCursor(15, 140);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("NTP: ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(ntpServer);

  tft.setCursor(15, 185);
  tft.setTextColor(ILI9341_CYAN);
  tft.print("Sync: ");
  tft.setTextColor(ILI9341_WHITE);
  tft.print(SYNC_INTERVAL / 3600000UL);
  tft.print(" Hour(s)");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("--- Booting Dual Hardware SPI Matrix ---");
  
  // Instantly force power to the screen backlight module pin 21
  pinMode(LCD_BL_PIN, OUTPUT);
  digitalWrite(LCD_BL_PIN, HIGH); 

  // Initialize display controller independently via VSPI default pins
  SPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.begin();
  tft.setRotation(1); // Set to Landscape Mode
  tft.fillScreen(ILI9341_BLACK);
  
  // Initialize the dedicated hardware SPI pins for the Touch controller
  hspiTouch.begin(XPT_SCLK_PIN, XPT_MISO_PIN, XPT_MOSI_PIN, XPT_CS_PIN);
  
  // Start the touch chip on the newly created hardware line
  touch.begin(hspiTouch);
  touch.setRotation(1); // Align touch orientation with the screen rotation

  // Boot UI
  tft.setFont(); 
  tft.setTextColor(ILI9341_WHITE);
  tft.setTextSize(2);
  tft.setCursor(10, 20);
  tft.print("Connecting to ");
  tft.print(ssid);

  // Connect to Wi-Fi
  // If using a Wi-Fi password, insert password variable instead of NULL
  WiFi.begin(ssid, NULL); 
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    Serial.print(".");
  }
  
  tft.fillScreen(ILI9341_BLACK);
  tft.setCursor(10, 20);
  tft.setTextColor(ILI9341_GREEN);
  tft.print("Wi-Fi Connected!");

  configTzTime(timeZone, ntpServer);
  
  tft.setCursor(10, 50);
  tft.setTextColor(ILI9341_WHITE);
  tft.print("Syncing NTP Time...");
  
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    delay(200);
  }
  
  lastSyncTime = millis();
  
  // Launch the initial info page popup
  drawInfoPageText();
  infoPageStartTime = millis();
  currentView = SHOWING_INFO;
  Serial.println("--- Boot Sequence Successful! ---");
}

void loop() {
  unsigned long currentMillis = millis();
  struct tm timeinfo;
  
  // Hourly NTP Re-sync Logic
  if (currentMillis - lastSyncTime >= SYNC_INTERVAL) {
    if (WiFi.status() == WL_CONNECTED) {
      configTzTime(timeZone, ntpServer); 
      lastSyncTime = currentMillis;      
    }
  }

  // --- 1. TOUCH PANEL SCANNER VIA DEDICATED HSPI BUS ---
  if (touch.touched()) {
    TS_Point p = touch.getPoint();

    if (p.z > 500) {
      int16_t touchX = map(p.x, 250, 3800, 0, 320); 
      int16_t touchY = map(p.y, 250, 3750, 0, 240);

      if (touchX >= 260 && touchX <= 320 && touchY >= 0 && touchY <= 60) {
        if (currentView == SHOWING_CLOCK) {
          drawInfoPageText();
          infoPageStartTime = currentMillis; 
          currentView = SHOWING_INFO;        
          return;
        }
      }
    }
  }

  // --- 2. NON-BLOCKING TIMEOUT TRACKER ---
  if (currentView == SHOWING_INFO) {
    if (currentMillis - infoPageStartTime >= 5000) {
      currentView = SHOWING_CLOCK;
      lastMinute = -1; // Forces the clock face to draw instantly
      tft.fillScreen(ILI9341_BLACK);
    }
    return; 
  }

  // --- 3. CLOCK DISPLAY ROUTINE ---
  if (!getLocalTime(&timeinfo)) {
    return; 
  }

  if (timeinfo.tm_min != lastMinute) {
    
    // Variables needed for bounds calculation
    int16_t x1, y1;
    uint16_t w, h;
    
    int hour12 = timeinfo.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;

    // --- STEP A: ERASE OLD TEXT ONLY (PREVENTS FLASHING) ---
    if (lastMinute != -1) {
      // Clear old Date box
      tft.setFont(&FreeSans12pt7b);
      tft.setTextSize(1);
      tft.getTextBounds(dateString, (320 - w) / 2, 45, &x1, &y1, &w, &h);
      tft.fillRect(x1 - 2, y1 - 2, w + 4, h + 4, ILI9341_BLACK);

      // Clear old Clock box
      tft.setFont(&FreeSansBold18pt7b);
      tft.setTextSize(3);
      tft.getTextBounds(timeString, 0, 140, &x1, &y1, &w, &h);
      
      // Override the box width to ensure the leftmost "1" is completely erased
      // when transitioning from double-digit (10, 11, 12) to single-digit (1) hour.
      int oldHour = atoi(timeString); 
      if ((oldHour >= 10 && oldHour <= 12) && hour12 == 1) {
        tft.fillRect(20, y1 - 5, 280, h + 10, ILI9341_BLACK); 
      } else {
        int oldClockX = (oldHour >= 10) ? 25 : 55;
        tft.getTextBounds(timeString, oldClockX, 140, &x1, &y1, &w, &h);
        tft.fillRect(x1 - 5, y1 - 5, w + 10, h + 10, ILI9341_BLACK);
      }

      // Clear old AM/PM box
      tft.setFont(&FreeSans12pt7b);
      tft.setTextSize(1);
      const char* oldAmpm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";
      tft.getTextBounds(oldAmpm, 128, 185, &x1, &y1, &w, &h);
      tft.fillRect(x1 - 2, y1 - 2, w + 4, h + 4, ILI9341_BLACK);

      // Clear old Timezone box
      tft.getTextBounds(tzString, 125, 225, &x1, &y1, &w, &h);
      tft.fillRect(x1 - 2, y1 - 2, w + 4, h + 4, ILI9341_BLACK);
    } else {
      tft.fillRect(0, 10, 320, 225, ILI9341_BLACK);
    }

    // --- STEP B: COMPUTE NEW STRINGS ---
    lastMinute = timeinfo.tm_min;
    const char* ampm = (timeinfo.tm_hour >= 12) ? "PM" : "AM";
    
    sprintf(timeString, "%d:%02d", hour12, timeinfo.tm_min);
    sprintf(dateString, "%02d-%02d-%04d", timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_year + 1900);
    sprintf(tzString, "%s", timeinfo.tm_isdst > 0 ? "CDT" : "CST");

    // --- STEP C: DRAW NEW TEXT CENTERING IT DYNAMICALLY ---
    // 1. Centered Date
    tft.setFont(&FreeSans12pt7b);
    tft.setTextSize(1);
    tft.getTextBounds(dateString, 0, 45, &x1, &y1, &w, &h);
    int dateX = (320 - w) / 2;    
    tft.setCursor(dateX, 45);        
    tft.setTextColor(ILI9341_CYAN);
    tft.print(dateString);

    // 2. Clock Time
    tft.setFont(&FreeSansBold18pt7b);
    tft.setTextSize(3); 
    int clockX = (hour12 >= 10) ? 25 : 55; 
    tft.setCursor(clockX, 140); 
    tft.setTextColor(ILI9341_YELLOW);
    tft.print(timeString);
    
    // 3. Centered AM/PM
    tft.setFont(&FreeSans12pt7b); 
    tft.setTextSize(1);
    tft.getTextBounds(ampm, 0, 185, &x1, &y1, &w, &h);
    int ampmX = (320 - w) / 2;          
    tft.setCursor(ampmX, 185); 
    tft.setTextColor(ILI9341_ORANGE);
    tft.print(ampm);
    
    // 4. Centered Timezone
    tft.setFont(&FreeSans12pt7b); 
    tft.setTextSize(1);
    tft.getTextBounds(tzString, 0, 225, &x1, &y1, &w, &h);
    int tzX = (320 - w) / 2;          
    tft.setCursor(tzX, 225);      
    tft.setTextColor(ILI9341_WHITE);
    tft.print(tzString);
  }
}