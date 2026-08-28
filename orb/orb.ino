// ============================================================
//  ORB-TAMA - a pocket tamagotchi for the JCZN/Guition
//  ESP32-2424S012 (ESP32-C3, 1.28" round GC9A01 240x240 IPS,
//  CST816D touch). NO BUTTONS REQUIRED - everything is touch.
//
//  Care: drag=pet, tap=giggle, burger=feed, ball=games (pop /
//  catch / memory chooser), drop=clean, tap poop=clean one,
//  hold ~1s=stats overlay, swipe left=feed, swipe right=clean.
//
//  Life: hatches with a PERSONALITY (lazy/hyper/picky/cuddly),
//  evolves BABY->TEEN->ADULT with care-based FORM (chubby /
//  athletic / sparkly), sleeps at night if WiFi/NTP is on (with
//  an ambient clock face), naps when exhausted, gets zoomies,
//  gets curious, and complains in speech bubbles when neglected.
//  Battery % arc appears once BATTERY_PIN is enabled (probe
//  logs at boot).
// ============================================================

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <WiFi.h>
#include <time.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// ----------------------- user settings ----------------------
#define TIME_SCALE        30       // 1 = real time; 30 = 1 pet-day per 48 real minutes
#define BACKLIGHT_BRIGHT  255
#define BACKLIGHT_DIM     30
#define ROTATION          2        // flip to 0 if image is upside-down

// optional WiFi + NTP (empty SSID = stay offline; night sleep then
// falls back to energy-based naps only)
const char *WIFI_SSID = "";
const char *WIFI_PASS = "";
const char *TZ_RULE   = "CET-1CEST,M3.5.0,M10.5.0/3"; // Amsterdam
#define NIGHT_START_HOUR 22
#define NIGHT_END_HOUR   7

// battery sense: leave -1 until the boot-time probe (analogRead on
// GPIO0/1) identifies the right pin, then set it + calibrate raws.
#define BATTERY_PIN      -1
#define BAT_EMPTY_RAW    1500     // ~3.0V on the divider pin
#define BAT_FULL_RAW     2150     // ~4.2V on the divider pin

// ---- board pins (JCZN ESP32-2424S012) ----
#define PIN_SCLK 6
#define PIN_MOSI 7
#define PIN_DC   2
#define PIN_CS   10
#define PIN_RST  -1
#define PIN_BL   3

// ---- touch CST816D ----
#define T_SDA 4
#define T_SCL 5
#define T_RST 1
#define TP_ADDR 0x15
#define TOUCH_INVERT_X 1     // verified correct on this board+rotation
#define TOUCH_INVERT_Y 1
#define TOUCH_SWAP_XY  0

// ----------------------- graphics ---------------------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_DC, PIN_CS, PIN_SCLK, PIN_MOSI);
Arduino_GFX *gfx     = new Arduino_GC9A01(bus, PIN_RST, ROTATION, true /* IPS */);

static uint16_t g_framebuffer[240 * 240];   // heap can't fit a malloc'ed canvas

class StaticCanvas : public Arduino_Canvas
{
public:
  StaticCanvas(int16_t w, int16_t h, Arduino_G *out) : Arduino_Canvas(w, h, out) {}
  bool begin(int32_t speed = GFX_NOT_DEFINED) override
  {
    (void)speed;
    _framebuffer = g_framebuffer;
    return true;
  }
};

StaticCanvas *cv = new StaticCanvas(240, 240, gfx);

#define RGB565(r,g,b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

const uint16_t COL_BG      = RGB565(5, 6, 10);
const uint16_t COL_BODY    = RGB565(15, 17, 26);
const uint16_t COL_RIM     = RGB565(36, 42, 58);
const uint16_t COL_SCLERA  = RGB565(238, 242, 250);
const uint16_t COL_PUPIL   = RGB565(8, 8, 12);
const uint16_t COL_TRACK   = RGB565(28, 31, 42);
const uint16_t COL_ICON    = RGB565(210, 216, 230);
const uint16_t COL_ICONDIM = RGB565(80, 86, 102);
const uint16_t COL_HEART   = RGB565(255, 92, 128);
const uint16_t COL_POOP    = RGB565(122, 82, 48);
const uint16_t COL_GOOD    = RGB565(80, 220, 130);
const uint16_t COL_BAD     = RGB565(255, 82, 82);

// ----------------------- utility ----------------------------
// Non-blocking, USB-independent logger: if the host is absent/busy
// (flaky cable, no monitor open) we DROP the message instead of
// stalling the render loop. The pet must never wait on a PC.
static inline void dbg(const char *fmt, ...)
{
  if (Serial.availableForWrite() < 24) return;
  char buf[96];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

static inline uint16_t hsv565(float h, float s, float v)
{
  h = fmodf(h, 360.0f); if (h < 0) h += 360;
  float c = v * s, x = c * (1 - fastFabs(fmodf(h / 60.0f, 2) - 1)), m = v - c;
  float r, g, b;
  if      (h < 60)  { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else              { r = c; g = 0; b = x; }
  return RGB565((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}

static inline uint16_t lerpColor(uint16_t a, uint16_t b, uint8_t t)
{
  int ar = (a >> 11) << 3, ag = ((a >> 5) & 0x3F) << 2, ab = (a & 0x1F) << 3;
  int br = (b >> 11) << 3, bg = ((b >> 5) & 0x3F) << 2, bb = (b & 0x1F) << 3;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return RGB565(r, g, bl);
}

static inline int isqrt32(uint32_t n)
{
  uint32_t x = n, c = 0, d = 1u << 30;
  while (d > n) d >>= 2;
  while (d) {
    if (x >= c + d) { x -= c + d; c = (c >> 1) + d; }
    else c >>= 1;
    d >>= 2;
  }
  return (int)c;
}

// ---- fast sin/cos LUT (64 entries, 10-bit fixed point) ----
// Eliminates expensive libm sinf/cosf calls from render-critical paths.
#define SIN_LUT_BITS 6
#define SIN_LUT_SIZE (1 << SIN_LUT_BITS)
#define SIN_LUT_SHIFT 10
static int16_t sinLUT[SIN_LUT_SIZE];
static bool sinLUTInit = false;

static void initSinLUT()
{
  for (int i = 0; i < SIN_LUT_SIZE; i++) {
    float a = (float)i * (2.0f * PI / SIN_LUT_SIZE);
    sinLUT[i] = (int16_t)(sinf(a) * (1 << SIN_LUT_SHIFT));
  }
  sinLUTInit = true;
}

// fastSin/fastCos: angle in 0..360 degrees, returns -1024..+1024
static inline int16_t fastSin10(int angDeg)
{
  if (!sinLUTInit) initSinLUT();
  int a = ((angDeg % 360) + 360) % 360;            // normalize to 0..359
  int idx = (a * SIN_LUT_SIZE) / 360;              // map to LUT index
  return sinLUT[idx];
}

static inline int16_t fastCos10(int angDeg)
{
  return fastSin10(angDeg + 90);
}

// fastSinRad / fastCosRad: angle in radians (uses LUT via degree conversion)
static inline int16_t fastSinRad10(float rad)
{
  int deg = (int)(rad * (180.0f / PI)) % 360;
  return fastSin10(deg);
}

static inline int16_t fastCosRad10(float rad)
{
  return fastSinRad10(rad + PI / 2);
}

// fast roundf: avoids libm roundf
static inline int fastRound(float f) { return (int)(f + 0.5f); }

// fast fabsf inline
static inline float fastFabs(float f) { return f < 0 ? -f : f; }

static inline uint16_t *fbRow(int y) { return g_framebuffer + y * 240; }

// ----------------------- touch ------------------------------
bool tpTouch = false;
uint16_t tpX = 120, tpY = 120;
uint16_t pressX = 120, pressY = 120;
bool tpAlive = false;
uint32_t lastTouchAt = 0;

bool readRegs(uint8_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(TP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(TP_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  tpAlive = true;
  return true;
}

void touchInit()
{
  pinMode(T_RST, OUTPUT);
  digitalWrite(T_RST, LOW); delay(20);
  digitalWrite(T_RST, HIGH); delay(60);
  Wire.begin(T_SDA, T_SCL, 100000U);
  Wire.setTimeOut(50);
}

void touchReinit()
{
  pinMode(T_SCL, OUTPUT);
  for (int i = 0; i < 10; i++) {
    digitalWrite(T_SCL, LOW); delayMicroseconds(5);
    digitalWrite(T_SCL, HIGH); delayMicroseconds(5);
  }
  Wire.end();
  pinMode(T_RST, OUTPUT);
  digitalWrite(T_RST, LOW); delay(20);
  digitalWrite(T_RST, HIGH); delay(60);
  Wire.begin(T_SDA, T_SCL, 100000U);
  Wire.setTimeOut(50);
  tpAlive = false;
  dbg("[TP] controller re-initialized\n");
}

uint16_t lastSampleX = 120, lastSampleY = 120;
float strokeAccum = 0;
uint32_t lastHeartAt = 0;

uint8_t pollTouch()          // 0 none, 1 down, 2 move, 3 up
{
  static bool wasDown = false;
  static bool broken = false;
  static uint32_t lastReinit = 0;

  if (broken) {
    if (millis() - lastReinit > 1500) { touchReinit(); broken = false; }
    return 0;
  }
  uint8_t r[6] = {0};
  uint32_t t0 = micros();
  bool ok = readRegs(0x01, r, 6);
  uint32_t elapsed = micros() - t0;
  if (elapsed > 30000) {
    broken = true;
    lastReinit = millis();
    strokeAccum = 0;
    tpTouch = false;
    dbg("[TP] bus hang (%lums) -> reinit\n", (unsigned long)(elapsed / 1000));
    return 0;
  }
  bool down = ok && (r[1] >= 1 && r[1] < 3);

  if (down) {
    uint16_t x = ((r[2] & 0x0F) << 8) | r[3];
    uint16_t y = ((r[4] & 0x0F) << 8) | r[5];
#if TOUCH_SWAP_XY
    uint16_t tmp = x; x = y; y = tmp;
#endif
#if TOUCH_INVERT_X
    x = 239 - x;
#endif
#if TOUCH_INVERT_Y
    y = 239 - y;
#endif
    tpX = x; tpY = y;
    tpTouch = true;
    lastTouchAt = millis();
    if (!wasDown) {
      lastSampleX = x; lastSampleY = y;
      pressX = x; pressY = y;
      strokeAccum = 0;
      wasDown = true;
      return 1;
    }
    int dx = (int)x - lastSampleX, dy = (int)y - lastSampleY;
    strokeAccum += sqrtf((float)(dx * dx + dy * dy));
    lastSampleX = x; lastSampleY = y;
    return 2;
  }
  tpTouch = false;
  if (wasDown) { wasDown = false; strokeAccum = 0; return 3; }
  return 0;
}

// ----------------------- pet identity -----------------------
enum Personality : uint8_t { P_LAZY = 0, P_HYPER, P_PICKY, P_CUDDLY, P_COUNT };
const char *P_NAME[P_COUNT] = { "LAZY", "HYPER", "PICKY", "CUDDLY" };
const float P_HUE[P_COUNT] = { 210, 25, 110, 330 };

enum Form : uint8_t { F_BALANCED = 0, F_CHUBBY, F_ATHLETIC, F_SPARKLY };
const char *FORM_NAME[4] = { "", "CHUBBY", "ATHLETIC", "SPARKLY" };
const float FORM_SCALE[4] = { 1.0f, 1.06f, 0.94f, 1.0f };

enum PetState : uint8_t { ST_AWAKE = 0, ST_EATING, ST_PLAYING, ST_SLEEPING, ST_DEAD };
enum Stage : uint8_t { STAGE_BABY = 0, STAGE_TEEN, STAGE_ADULT };
const char *STAGE_NAME[] = { "BABY", "TEEN", "ADULT" };

struct TamaData {
  float hunger = 80, fun = 80, energy = 80;
  uint32_t ageSec = 0;
  bool hatched = false;
  PetState state = ST_AWAKE;
  Stage stage = STAGE_BABY;
  Form form = F_BALANCED;
  Personality pers = P_LAZY;
  uint16_t fedCount = 0, playCount = 0, petCount = 0;   // care history
} tama;

// ------------------------ poops -----------------------------
bool statsDirty = false;            // declared early: clearPoops uses it
struct Poop { bool active; uint8_t slot; };
Poop poops[4];
const int16_t POOP_SPOTS[4][2] = { {44,146}, {196,146}, {86,174}, {154,174} };
bool poopsActive(int i) { return poops[i].active; }
void poopSet(int i, bool v) { poops[i].active = v; poops[i].slot = (uint8_t)i; }
void clearPoops() { for (int i = 0; i < 4; i++) poopSet(i, false); statsDirty = true; }

// ---------------------- persistence -------------------------
Preferences prefs;
uint32_t lastSaveAt = 0;
bool virginBoot = false;

void saveTamagotchi()
{
  prefs.begin("tama", false);
  // batch all writes before end() to minimize NVS overhead
  prefs.putBool("hatched", tama.hatched);
  prefs.putUChar("hunger", (uint8_t)tama.hunger);
  prefs.putUChar("fun", (uint8_t)tama.fun);
  prefs.putUChar("energy", (uint8_t)tama.energy);
  prefs.putUInt("age", tama.ageSec);
  prefs.putUChar("pers", tama.pers);
  prefs.putUChar("form", tama.form);
  prefs.putUShort("fed", tama.fedCount);
  prefs.putUShort("play", tama.playCount);
  prefs.putUShort("pet", tama.petCount);
  prefs.putBool("dead", tama.state == ST_DEAD);
  uint8_t pmask = 0;
  for (int i = 0; i < 4; i++) if (poopsActive(i)) pmask |= (1 << i);
  prefs.putUChar("poops", pmask);
  prefs.end();
  statsDirty = false;
  lastSaveAt = millis();
}

void loadTamagotchi()
{
  prefs.begin("tama", true);
  virginBoot = !prefs.getBool("hatched", false);
  tama.hatched = !virginBoot;
  tama.hunger = prefs.getUChar("hunger", 80);
  tama.fun = prefs.getUChar("fun", 80);
  tama.energy = prefs.getUChar("energy", 80);
  tama.ageSec = prefs.getUInt("age", 0);
  if (prefs.isKey("pers")) tama.pers = (Personality)prefs.getUChar("pers", P_LAZY);
  else { tama.pers = (Personality)random(P_COUNT); dbg("[TAMA] rolled personality: %s\n", P_NAME[tama.pers]); }
  tama.form = (Form)prefs.getUChar("form", F_BALANCED);
  tama.fedCount = prefs.getUShort("fed", 0);
  tama.playCount = prefs.getUShort("play", 0);
  tama.petCount = prefs.getUShort("pet", 0);
  uint8_t pmask = prefs.getUChar("poops", 0);
  bool wasDead = prefs.getBool("dead", false);
  prefs.end();
  for (int i = 0; i < 4; i++) poopSet(i, (pmask & (1 << i)) != 0);
  tama.stage = tama.ageSec < 3600UL ? STAGE_BABY
             : tama.ageSec < 43200UL ? STAGE_TEEN : STAGE_ADULT;
  if (wasDead && tama.hatched) {
    tama.state = ST_DEAD;
    dbg("[TAMA] restored dead state from NVS\n");
  }
}

// ------------------------ poops -----------------------------
// (moved before persistence; see above)

// ------------------------ particles -------------------------
struct Particle {
  bool active;
  int16_t x, y;
  int8_t vx, vy;
  uint8_t life, type;   // 0 heart 1 zzz 2 crumb 3 sparkle 4 popring 5 tear
};
Particle parts[24];

void spawnPart(uint8_t type, int16_t x, int16_t y, int8_t vx, int8_t vy, uint8_t life)
{
  for (auto &p : parts) {
    if (!p.active) { p = { true, x, y, vx, vy, life, type }; return; }
  }
}

// ----------------------- mini-game --------------------------
struct Game {
  bool active = false;
  uint8_t type;            // 0 pop, 1 catch
  uint8_t round = 0;
  uint8_t score, hits;
  int16_t tx, ty;
  uint32_t roundStart;
  bool waitNext;
  uint32_t waitStart;
  uint32_t startMs;
  // catch
  struct { int16_t x, y; int8_t vy; bool on; } drops[3];
} game;

// game chooser (2 games)
bool chooserOpen = false;
const int16_t CHOICE[2][2] = { {70,118}, {170,118} };

// ---------------------- mood / anim state -------------------
float blinkPhase = 1.0f;
bool blinking = false;
uint32_t blinkStart = 0, nextBlink = 0;
float gazeX = 0, gazeY = 0, tgtX = 0, tgtY = 0;
uint32_t nextWander = 0;
float breathe = 0;
uint32_t happyUntil = 0;
uint32_t celebrateUntil = 0;
uint32_t grumpyUntil = 0;
uint32_t curiousUntil = 0;
uint32_t zoomUntil = 0, nextZoom = 0;
uint32_t angryUntil = 0;
struct BubbleState { uint8_t type = 0; uint32_t until = 0; } bubble;
uint32_t lastBubble = 0;
uint32_t stateEnd = 0;
uint32_t eatStart = 0;
uint32_t wakeGraceAt = 0;
uint32_t infoOpenAt = 0;
bool infoOverlay = false;
bool orientPending = false;   // orientation self-test: verified once, now disabled
                              // (re-enable with true if orientation ever needs rechecking)
uint32_t bootAt = 0;
uint32_t refuseUntil = 0;
uint32_t diedAt = 0;

// ---------------------- time / wifi / battery ---------------
bool wifiConnecting = false;
uint32_t wifiStartAt = 0;
bool timeSynced = false;
uint32_t lastTimeCheck = 0;
int curHour = -1, curMin = -1, curSec = -1, curDow = -1, curDay = -1, curMon = -1;
float batteryPct = -1;
uint32_t lastBatRead = 0;
int probeRaw0 = -1, probeRaw1 = -1;      // battery pin discovery values

bool isNight()
{
  return timeSynced && (curHour >= NIGHT_START_HOUR || curHour < NIGHT_END_HOUR);
}

void startWifi()
{
  if (!WIFI_SSID[0] || wifiConnecting) return;
  wifiConnecting = true;
  wifiStartAt = millis();
  dbg("[NET] wifi connecting...\n");
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("orb-tama");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

void updateTime()
{
  if (!timeSynced) return;
  if (millis() - lastTimeCheck < 1000) return;
  lastTimeCheck = millis();
  struct tm ti;
  if (!getLocalTime(&ti, 0)) return;
  curHour = ti.tm_hour; curMin = ti.tm_min; curSec = ti.tm_sec;
  curDow = ti.tm_wday; curDay = ti.tm_mday; curMon = ti.tm_mon + 1;
}

void readBattery()
{
#if BATTERY_PIN >= 0
  static bool announced = false;
  pinMode(BATTERY_PIN, INPUT);
  analogSetPinAttenuation(BATTERY_PIN, ADC_11db);
  int raw = analogRead(BATTERY_PIN);
  if (!announced) {
    announced = true;
    dbg("[BAT] pin%d raw=%d\n", BATTERY_PIN, raw);
  }
  if (raw > 700) {
    batteryPct = constrain((raw - BAT_EMPTY_RAW) * 100.0f / (BAT_FULL_RAW - BAT_EMPTY_RAW), 0.0f, 100.0f);
  } else batteryPct = -1;
#endif
}

void probeBatteryPins()
{
  // discovery helper: no battery pin documented publicly - log ADC on the
  // two candidates once at boot. Attach a LiPo and read the serial log.
  pinMode(0, INPUT);
  analogSetPinAttenuation(0, ADC_11db);
  probeRaw0 = analogRead(0);
  pinMode(1, INPUT);
  analogSetPinAttenuation(1, ADC_11db);
  probeRaw1 = analogRead(1);
  dbg("[BAT] probe pin0=%d pin1=%d\n", probeRaw0, probeRaw1);
  pinMode(T_RST, OUTPUT);   // restore touch reset
  digitalWrite(T_RST, HIGH);
}

// frame pacing / fps (declared early: setup uses them)
uint32_t frameStart = 0, fpsFrames = 0, fpsT0 = 0, lastTick = 0;
float fpsShown = 0;
uint32_t pendingMealPoop = 0;

// =========================== SETUP ==========================
void setup()
{
  Serial.begin(115200);
  delay(120);
  dbg("\n[ORB-TAMA] booting\n");

  pinMode(PIN_BL, OUTPUT);
  analogWrite(PIN_BL, 0);

  if (!gfx->begin(80000000)) dbg("[ORB] gfx begin FAILED\n");
  cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  gfx->fillScreen(0x0000);
  analogWrite(PIN_BL, BACKLIGHT_BRIGHT);

  touchInit();
  loadTamagotchi();
  probeBatteryPins();
#if BATTERY_PIN >= 0
  readBattery();
#endif

  // welcome-back care package: a neglected pet is always approachable
  // (skip if dead — no boost for the departed)
  bool rescued = false;
  if (tama.hunger < 35) { tama.hunger = 45; rescued = true; }
  if (tama.fun < 35)    { tama.fun = 45;    rescued = true; }
  if (tama.energy < 35) { tama.energy = 45; rescued = true; }
  if (rescued && tama.state != ST_DEAD) {
    statsDirty = true;
    dbg("[TAMA] welcome-back boost h=%u f=%u e=%u\n",
        (uint8_t)tama.hunger, (uint8_t)tama.fun, (uint8_t)tama.energy);
  }

  startWifi();
  bootAt = millis();
  fpsT0 = millis();
}

// ============================ LOOP ==========================
void loop()
{
  frameStart = micros();
  uint32_t nowMs = millis();

  // debug: send 'k' over serial to instantly kill the pet
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'k' && tama.state != ST_DEAD) {
      tama.hunger = 0;
      tama.state = ST_DEAD;
      diedAt = millis();
      statsDirty = true;
      saveTamagotchi();
      dbg("[TAMA] killed via serial\n");
    }
  }

  handleTouchFrame();
  nowMs = millis();    // refresh after touch handling (may reset bootAt)

  if (nowMs - lastTick >= 250) {
    lastTick = nowMs;
    uint32_t tSim = micros();
    simTick();
    uint32_t dSim = micros() - tSim;
    if (dSim > 50000) dbg("[PERF] simTick %lums\n", (unsigned long)(dSim / 1000));
  }

  // (stats panel closes ONLY by tapping it - no auto-close that can race)

  // network bookkeeping
  if (wifiConnecting && nowMs - wifiStartAt > 8000) {
    wifiConnecting = false;
    dbg("[NET] wifi timeout\n");
  }
  if (WiFi.status() == WL_CONNECTED) {
    if (!timeSynced && nowMs - lastTimeCheck > 2000) {
      lastTimeCheck = nowMs;
      struct tm ti;
      if (getLocalTime(&ti, 0)) {
        timeSynced = true;
        curHour = ti.tm_hour; curMin = ti.tm_min; curSec = ti.tm_sec;
        curDow = ti.tm_wday; curDay = ti.tm_mday; curMon = ti.tm_mon + 1;
        dbg("[NET] NTP synced\n");
      }
    }
    updateTime();
  } else if (timeSynced) {
    // lost wifi: keep last known time, it drifts but is harmless
  }
  if (millis() - lastBatRead > 5000) { lastBatRead = millis(); readBattery(); }

  if (tama.state == ST_EATING && (int32_t)(stateEnd - nowMs) < 0) {
    tama.state = ST_AWAKE;
    tama.hunger += 24;
    if (tama.hunger > 100) tama.hunger = 100;
    pendingMealPoop++;
    tama.fedCount++;
    happyUntil = nowMs + 1000;
    statsDirty = true;
    dbg("[TAMA] fed h=%u\n", (uint8_t)tama.hunger);
  }
  if (tama.state == ST_PLAYING && game.active) tickGame();

  updateLook(nowMs);
  uint32_t tRen = micros();
  renderScene(nowMs);
  uint32_t dRen = micros() - tRen;
  if (dRen > 50000) dbg("[PERF] render %lums\n", (unsigned long)(dRen / 1000));

  // forensic: if the stats panel should be visible, verify its pixels exist
  if (infoOverlay) {
    static uint32_t lastDump = 0;
    if (nowMs - lastDump > 700) {
      lastDump = nowMs;
      uint16_t *fb = cv->getFramebuffer();
      dbg("[UI] fb ctr=%04X hdr=%04X bdr=%04X pill=%04X\n",
          fb[116 * 240 + 120], fb[24 * 240 + 120], fb[2 * 240 + 2], fb[12 * 240 + 120]);
    }
  }
  uint32_t tFl = micros();
  cv->flush();
  uint32_t dFl = micros() - tFl;
  if (dFl > 50000) dbg("[PERF] flush %lums\n", (unsigned long)(dFl / 1000));

  // per-second render/flush breakdown
  static uint32_t perfT0 = 0, perfSumR = 0, perfSumF = 0, perfFrames = 0;
  perfSumR += dRen; perfSumF += dFl; perfFrames++;
  if (nowMs - perfT0 > 2000) {
    perfT0 = nowMs;
    dbg("[PERF] avg render=%lums flush=%lums frames=%u\n",
        (unsigned long)(perfSumR / perfFrames), (unsigned long)(perfSumF / perfFrames), perfFrames);
    perfSumR = 0; perfSumF = 0; perfFrames = 0;
  }

  fpsFrames++;
  if (nowMs - fpsT0 >= 1000) {
    fpsShown = fpsFrames * 1000.0f / (nowMs - fpsT0);
    fpsFrames = 0; fpsT0 = nowMs;
    dbg("[TAMA] %s %s %.1ffps h=%u\n", STAGE_NAME[tama.stage],
        P_NAME[tama.pers], fpsShown, ESP.getFreeHeap());
  }

  int32_t budget = 22000 - (int32_t)(micros() - frameStart);
  if (budget > 0) delayMicroseconds(budget);

  uint32_t frameCost = micros() - frameStart;
  if (frameCost > 200000)
    dbg("[TP] slow frame %lums\n", (unsigned long)(frameCost / 1000));
}

// ------------------------- simulation -----------------------
// returns a Form value; uint8_t because the Arduino auto-prototype
// block is emitted before user type declarations, so custom return
// types in signatures can't be used
uint8_t calcForm()
{
  uint32_t m = max(tama.fedCount, max(tama.playCount, tama.petCount));
  if (m == 0) return F_BALANCED;
  int best = 0;
  if (tama.fedCount == m) best++;
  if (tama.playCount == m) best++;
  if (tama.petCount == m) best++;
  if (best > 1) return F_BALANCED;              // tie -> balanced
  if (tama.fedCount == m) return F_CHUBBY;
  if (tama.playCount == m) return F_ATHLETIC;
  return F_SPARKLY;
}

void simTick()
{
  float dm = 0.25f / 60.0f * TIME_SCALE;
  tama.ageSec += (uint32_t)(0.25f * TIME_SCALE);

  // personality multipliers
  float enD = 1.0f, huD = 1.0f, fuD = 1.0f;
  if (tama.pers == P_LAZY)   { enD = 0.7f; huD = 1.2f; }
  if (tama.pers == P_HYPER)  { fuD = 0.6f; enD = 1.3f; }
  if (tama.pers == P_PICKY)  { huD = 1.15f; }
  if (tama.pers == P_CUDDLY) { fuD = 0.5f; }

  bool anyPoop = false;
  for (int i = 0; i < 4; i++) anyPoop |= poopsActive(i);

  bool night = isNight();
  float sleepMul = (tama.state == ST_SLEEPING) ? 0.20f : 1.0f;
  tama.hunger -= 0.40f * huD * dm * sleepMul;
  tama.fun    -= (anyPoop ? 0.56f : 0.28f) * fuD * dm * sleepMul;
  if (tama.state == ST_SLEEPING) {
    tama.energy += 0.07f * dm;
  } else if (tama.state != ST_PLAYING) {
    tama.energy -= 0.15f * enD * dm;
  }
  if (tama.hunger < 0) tama.hunger = 0;
  if (tama.fun < 0) tama.fun = 0;
  if (tama.energy > 100) tama.energy = 100;

  // death: hunger at 0 = starvation
  if (tama.hunger <= 0 && tama.state != ST_DEAD) {
    tama.state = ST_DEAD;
    diedAt = millis();
    statsDirty = true;
    saveTamagotchi();
    dbg("[TAMA] died of hunger at age %us\n", (unsigned)tama.ageSec);
    return;
  }
  if (tama.state == ST_DEAD) return;

  // anger: fun at 0 = furious (lasts until fun recovers above 5)
  if (tama.fun <= 0) {
    angryUntil = millis() + 300000;  // 5 min minimum anger
    if (tama.state == ST_AWAKE) {
      headShake();
    }
  }
  if (tama.fun > 5 && (int32_t)(millis() - angryUntil) > 0) {
    angryUntil = 0;  // anger fades once fun recovers
  }

  if (pendingMealPoop && random(100) < 3) {
    pendingMealPoop--;
    for (int i = 0; i < 4; i++) if (!poopsActive(i)) { poopSet(i, true); statsDirty = true; break; }
  }

  // sleep logic: exhaustion OR night bedtime
  if (tama.state == ST_AWAKE &&
      ((tama.energy <= 8) || (night && tama.energy < 70)) &&
      (int32_t)(millis() - wakeGraceAt) > 10000) {
    tama.state = ST_SLEEPING;
    statsDirty = true;
    dbg("[TAMA] fell asleep (%s)\n", night ? "night" : "tired");
  }
  if (tama.state == ST_SLEEPING) {
    analogWrite(PIN_BL, BACKLIGHT_DIM);
    bool wake = night ? (curHour >= NIGHT_END_HOUR && tama.energy >= 40)
                      : (tama.energy >= 40);
    if (wake) {
      tama.state = ST_AWAKE;
      wakeGraceAt = millis();
      happyUntil = millis() + 1200;
      analogWrite(PIN_BL, BACKLIGHT_BRIGHT);
      dbg("[TAMA] woke up rested\n");
    }
  }

  // zoomies: full fun + full energy = the zoomies
  if (tama.state == ST_AWAKE && !night &&
      tama.fun >= 90 && tama.energy >= 90 && (int32_t)(millis() - nextZoom) >= 0) {
    zoomUntil = millis() + 2600;
    tama.fun -= 8;
    tama.energy -= 6;
    nextZoom = millis() + 60000 + random(90000);
    dbg("[TAMA] ZOOMIES!\n");
  }

  // evolution with care-based form
  Stage st = tama.ageSec < 3600UL ? STAGE_BABY
           : tama.ageSec < 43200UL ? STAGE_TEEN : STAGE_ADULT;
  if (st != tama.stage) {
    tama.stage = st;
    Form f = (Form)calcForm();
    if (f != F_BALANCED) {
      tama.form = f;
      dbg("[TAMA] evolved to %s %s!\n", STAGE_NAME[st], FORM_NAME[f]);
    } else {
      dbg("[TAMA] evolved to %s!\n", STAGE_NAME[st]);
    }
    celebrateUntil = millis() + 2200;
    for (int i = 0; i < 10; i++)
      spawnPart(3, 120 + random(-60, 61), 90 + random(-50, 51), 0, -2, 30);
    statsDirty = true;
  }

  if (statsDirty && millis() - lastSaveAt > 60000) saveTamagotchi();

  static uint32_t lastStatLog = 0;
  if (millis() - lastStatLog > 60000) {
    lastStatLog = millis();
    dbg("[TAMA] age=%us h=%u f=%u e=%u %s/%s poops=%u fed=%u ply=%u pet=%u bat0=%d bat1=%d\n",
        (unsigned)tama.ageSec, (uint8_t)tama.hunger, (uint8_t)tama.fun,
        (uint8_t)tama.energy, STAGE_NAME[tama.stage], P_NAME[tama.pers],
        poopsActive(0) + poopsActive(1) + poopsActive(2) + poopsActive(3),
        tama.fedCount, tama.playCount, tama.petCount, probeRaw0, probeRaw1);
  }
}

// ------------------------ interaction -----------------------
const int16_t ICON[3][2] = { {58,186}, {120,198}, {182,186} };  // feed games clean

bool hitIcon(int idx)
{
  int dx = tpX - ICON[idx][0], dy = tpY - ICON[idx][1];
  return dx * dx + dy * dy <= 26 * 26;
}

bool hitPt(int px, int py, int cx, int cy, int r)
{
  int dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy <= r * r;
}

bool hitPoop()
{
  for (int i = 0; i < 4; i++) {
    if (poopsActive(i) && hitPt(tpX, tpY, POOP_SPOTS[i][0], POOP_SPOTS[i][1], 16)) return true;
  }
  return false;
}

void headShake() { refuseUntil = millis() + 800; }

void startFeeding()
{
  if (tama.state != ST_AWAKE || chooserOpen) return;
  if (tama.hunger > 92 || (int32_t)(millis() - angryUntil) < 0) { headShake(); return; }
  tama.state = ST_EATING;
  eatStart = millis();
  stateEnd = eatStart + 1900;
}

void startGame(uint8_t type)
{
  if (tama.state != ST_AWAKE) return;
  if (tama.energy < 15 || (int32_t)(millis() - angryUntil) < 0) { headShake(); return; }
  game.active = true;
  game.type = type;
  game.score = 0;
  game.hits = 0;
  game.startMs = millis();
  game.waitNext = false;
  if (type == 0) {                 // pop
    game.round = 0;
    nextGameTarget();
  } else if (type == 1) {          // catch
    for (int i = 0; i < 3; i++) game.drops[i].on = false;
    game.roundStart = millis();
  }
  tama.state = ST_PLAYING;
}

void endGame()
{
  game.active = false;
  chooserOpen = false;
  tama.playCount++;
  if (game.type == 0) tama.fun += 8 + game.hits * 4;
  else if (game.type == 1) tama.fun += 6 + game.score * 4;
  else tama.fun += 4 + game.score * 3;
  if (tama.fun > 100) tama.fun = 100;
  tama.energy -= 7;
  if (tama.energy < 0) tama.energy = 0;
  tama.state = ST_AWAKE;
  happyUntil = millis() + 1400;
  statsDirty = true;
  spawnPart(3, 120, 60, 0, -1, 26);
  dbg("[TAMA] game over type=%u score=%u\n", game.type, game.score);
}

void nextGameTarget()
{
  for (int tries = 0; tries < 20; tries++) {
    float a = random(0, 628) * 0.01f;
    float rr = 30 + random(0, 46);
    int x = 120 + (int)(cosf(a) * rr);
    int y = 105 + (int)(sinf(a) * rr);
    if (hitPt(x, y, ICON[0][0], ICON[0][1], 30)) continue;
    game.tx = x; game.ty = y;
    break;
  }
  game.roundStart = millis();
  game.waitNext = false;
}

void handleTap(uint16_t x, uint16_t y)
{
  if (virginBoot) return;
  if (infoOverlay) { infoOverlay = false; dbg("[UI] stats closed by tap\n"); return; }

  // death: tap to re-hatch (with 2s cooldown so tombstone is visible)
  if (tama.state == ST_DEAD) {
    if (diedAt && (int32_t)(millis() - diedAt) < 2000) return;  // grace period
    diedAt = 0;
    tama = TamaData();                      // reset all stats to defaults
    tama.hatched = false;
    virginBoot = true;
    bootAt = millis();                       // reset egg timer
    clearPoops();
    statsDirty = true;
    saveTamagotchi();
    dbg("[TAMA] re-hatching after death\n");
    return;
  }

  // anger: block most interactions, only petting helps
  bool angry = (int32_t)(millis() - angryUntil) < 0;

  if (chooserOpen) {               // picking a game
    for (int g = 0; g < 2; g++) {
      if (hitPt(x, y, CHOICE[g][0], CHOICE[g][1], 40)) {
        chooserOpen = false;
        startGame(g);
        return;
      }
    }
    chooserOpen = false;           // tap elsewhere cancels
    return;
  }

  if (tama.state == ST_PLAYING) { if (game.active) handleTapGame(x, y); return; }

  if (tama.state == ST_SLEEPING) {
    tama.state = ST_AWAKE;
    wakeGraceAt = millis();
    grumpyUntil = millis() + 900;
    analogWrite(PIN_BL, BACKLIGHT_BRIGHT);
    happyUntil = millis() + 600;
    statsDirty = true;
    return;
  }
  if (tama.state != ST_AWAKE) return;

  // double-tap opens the stats panel
  static uint32_t lastTapAt = 0;
  static uint16_t lastTapX = 0, lastTapY = 0;
  if (millis() - lastTapAt < 350 && abs((int)x - lastTapX) < 45 && abs((int)y - lastTapY) < 45) {
    lastTapAt = 0;
    infoOpenAt = millis();
    infoOverlay = true;
    dbg("[UI] stats opened (double-tap)\n");
    return;
  }
  lastTapAt = millis();
  lastTapX = x; lastTapY = y;

  for (int i = 0; i < 3; i++) {
    if (hitIcon(i)) {
      if (i == 0) { if (!angry) startFeeding(); else headShake(); }
      else if (i == 1) {
        if (game.active) return;
        if (!angry) chooserOpen = !chooserOpen; else headShake();
      } else {
        bool any = false;
        for (int k = 0; k < 4; k++) any |= poopsActive(k);
        if (!any) {
          happyUntil = millis() + 800;
          for (int k = 0; k < 4; k++) spawnPart(3, ICON[2][0] + random(-14, 15), ICON[2][1] - 10, 0, -2, 18);
        } else { clearPoops(); celebrateUntil = millis() + 800; }
      }
      return;
    }
  }
  if (hitPoop()) {
    for (int i = 0; i < 4; i++)
      if (poopsActive(i) && hitPt(tpX, tpY, POOP_SPOTS[i][0], POOP_SPOTS[i][1], 16))
        poopSet(i, false);
    statsDirty = true;
    spawnPart(3, x, y, 0, -1, 20);
    return;
  }
  if (hitPt(x, y, 120, 104, 95)) {
    happyUntil = millis() + 1000;
    tama.fun += 2;
    if (tama.fun > 100) tama.fun = 100;
    statsDirty = true;
  }
}

void handleTouchFrame()
{
  uint8_t ev = pollTouch();
  static uint32_t downAt = 0;
  static bool moved = false;

  if (ev == 1) {
    downAt = millis();
    moved = false;
  } else if (ev == 2) {
    if (strokeAccum > 26) moved = true;
    int driftX = abs((int)tpX - pressX), driftY = abs((int)tpY - pressY);
    int maxDrift = max(driftX, driftY);
    // petting: needs real movement away from the press point
    if (tama.state == ST_AWAKE && !chooserOpen && moved && !infoOverlay &&
        maxDrift > 14 && millis() - lastHeartAt > 180) {
      int dx = tpX - 120, dy = tpY - 104;
      if (dx * dx + dy * dy < 95 * 95) {
        lastHeartAt = millis();
        happyUntil = millis() + 1200;
        tama.fun += (tama.pers == P_CUDDLY ? 2.0f : 1.6f);
        if (tama.fun > 100) tama.fun = 100;
        tama.petCount++;
        statsDirty = true;
        spawnPart(0, tpX, tpY - 12, 0, -2, 22);
        strokeAccum = 0;
      }
    }
    // drag over poop = clean it
    if (tama.state == ST_AWAKE && hitPoop()) {
      for (int i = 0; i < 4; i++)
        if (poopsActive(i) && hitPt(tpX, tpY, POOP_SPOTS[i][0], POOP_SPOTS[i][1], 16))
          poopSet(i, false);
      statsDirty = true;
      spawnPart(3, tpX, tpY, 0, -1, 20);
    }
  } else if (ev == 3) {
    if (orientPending) { orientPending = false; return; }
    if (moved) return;
    // swipe gestures: left = feed, right = clean
    int ddx = (int)tpX - pressX, ddy = (int)tpY - pressY;
    if (abs(ddx) >= 55 && abs(ddy) <= 45) {
      bool angry = (int32_t)(millis() - angryUntil) < 0;
      if (ddx < 0) { if (!angry) startFeeding(); else headShake(); }
      else {
        bool any = false;
        for (int k = 0; k < 4; k++) any |= poopsActive(k);
        if (any) clearPoops();
      }
      return;
    }
    handleTap(tpX, tpY);
  }
}

// ------------------------- mini-games -----------------------
void tickGame()
{
  uint32_t nowMs = millis();
  if (game.type == 0) {                 // pop
    if (game.waitNext) {
      if (nowMs - game.waitStart > 450) {
        if (game.round >= 5) endGame();
        else nextGameTarget();
      }
      return;
    }
    if (nowMs - game.roundStart > 3200) {
      game.round++;
      game.waitNext = true;
      game.waitStart = nowMs;
    }
  } else if (game.type == 1) {          // catch
    if (nowMs - game.startMs > 22000) { endGame(); return; }
    for (int i = 0; i < 3; i++) {
      if (!game.drops[i].on) {
        if (random(100) < 4) {          // spawn
          game.drops[i].on = true;
          game.drops[i].x = 60 + random(121);
          game.drops[i].y = -8;
          game.drops[i].vy = 1 + random(2);
        }
      } else {
        game.drops[i].y += game.drops[i].vy * 2;
        if (game.drops[i].y > 235) game.drops[i].on = false;   // missed
      }
    }
  }
}

void handleTapGame(uint16_t x, uint16_t y)
{
  if (game.type == 0) {                 // pop
    if (game.waitNext) return;
    int dx = x - game.tx, dy = y - game.ty;
    int r = 24 + (millis() - game.roundStart > 2400 ? -8 : 0);
    if (dx * dx + dy * dy <= r * r) {
      game.hits++;
      for (int i = 0; i < 6; i++)
        spawnPart(2, game.tx, game.ty, random(-3, 4), random(-4, 1), 14);
      spawnPart(4, game.tx, game.ty, 0, 0, 10);
      game.round++;
      game.waitNext = true;
      game.waitStart = millis();
    }
  } else if (game.type == 1) {          // catch
    for (int i = 0; i < 3; i++) {
      if (game.drops[i].on && hitPt(x, y, game.drops[i].x, game.drops[i].y, 26)) {
        game.drops[i].on = false;
        game.score++;
        spawnPart(4, game.drops[i].x, game.drops[i].y, 0, 0, 10);
        spawnPart(3, game.drops[i].x, game.drops[i].y, 0, -1, 16);
        if (game.score >= 5) endGame();
        return;
      }
    }
  }
}

// --------------------- look / animation ---------------------
void updateLook(uint32_t tms)
{
  breathe = sinf(tms * (2 * PI / 4200.0f));
  bool zooming = (int32_t)(zoomUntil - tms) > 0;
  bool playing = (tama.state == ST_PLAYING && game.active);
  bool night = isNight();

  if (zooming) {
    // override: eyes dart side to side during zoomies
    tgtX = sinf(tms * 0.04f) * 60;
    tgtY = 0;
  } else if (playing) {
    if (game.type == 0 && !game.waitNext) {
      tgtX = constrain((float)game.tx - 120, -70, 70) * 0.6f;
      tgtY = constrain((float)game.ty - 104, -60, 60) * 0.6f;
    } else { tgtX = 0; tgtY = 0; }
  } else if (tpTouch) {
    tgtX = constrain((float)tpX - 120, -70, 70) * 0.55f;
    tgtY = constrain((float)tpY - 104, -60, 60) * 0.55f;
  } else if ((int32_t)(refuseUntil - tms) > 0) {
    tgtX = sinf(tms * 0.04f) * 14;
    tgtY = 0;
  } else if ((int32_t)(grumpyUntil - tms) > 0) {
    tgtX = -16; tgtY = 0;                // averted gaze
  } else {
    if ((int32_t)(tms - nextWander) >= 0) {
      nextWander = tms + 1500 + random(2800);
      if (random(10) < 3) { tgtX = 0; tgtY = 0; }
      else { tgtX = (float)random(-22, 23); tgtY = (float)random(-14, 15); }
    }
  }
  gazeX += (tgtX - gazeX) * 0.18f;
  gazeY += (tgtY - gazeY) * 0.18f;

  // blinking (not while sleeping)
  if (tama.state != ST_SLEEPING) {
    uint32_t blinkRange = (tama.pers == P_HYPER) ? 2600 : 4500;
    if (!blinking && (int32_t)(tms - nextBlink) >= 0) {
      blinking = true;
      blinkStart = tms;
    }
    if (blinking) {
      uint32_t dt = tms - blinkStart;
      blinkPhase = (dt < 90) ? 1.0f - (dt / 90.0f)
                 : (dt < 180) ? (dt - 90) / 90.0f : 1.0f;
      if (dt >= 180) {
        blinking = false;
        blinkPhase = 1.0f;
        nextBlink = tms + blinkRange + random(blinkRange);
      }
    } else blinkPhase = 1.0f;
  }

  // curiosity
  if (tama.state == ST_AWAKE && !night && !zooming && !playing &&
      (int32_t)(tms - lastTouchAt) > 20000 && (int32_t)(curiousUntil - tms) <= 0) {
    curiousUntil = tms + 1600;
  }

  // speech bubbles (need-based, rate-limited)
  if (bubble.until < tms && tama.state == ST_AWAKE && !night &&
      (int32_t)(tms - lastBubble) > 30000) {
    uint8_t bt = 0;
    if (tama.hunger < 30) bt = 1;
    else if (tama.fun < 30) bt = 2;
    else if (tama.energy < 12) bt = 3;
    else if (random(100) < 20) bt = 5;
    if (bt) { bubble.type = bt; bubble.until = tms + 4500; lastBubble = tms; }
  }
  if (bubble.until < tms) bubble.type = 0;

  // particles (real time)
  for (auto &p : parts) {
    if (!p.active) continue;
    p.x += p.vx; p.y += p.vy;
    if (p.life) p.life--;
    if (!p.life) p.active = false;
  }
}

// ========================== RENDER ==========================
void renderScene(uint32_t tms)
{
  uint16_t *fb = cv->getFramebuffer();
  // fast clear: fill first row, then memcpy in doubling chunks
  fb[0] = COL_BG;
  for (int n = 1; n < 240 * 240; n *= 2) {
    int todo = (240 * 240 - n < n) ? 240 * 240 - n : n;
    for (int i = 0; i < todo; i++) fb[n + i] = fb[i];
  }

  if (orientPending) {
    if (tms - bootAt > 10000) orientPending = false;
    else { drawOrientationTest(); return; }
  }
  if (virginBoot) { drawEggIntro(tms); return; }
  if (tama.state == ST_DEAD) { drawDeath(tms); return; }

  // night sleep = ambient clock face
  if (tama.state == ST_SLEEPING && isNight()) {
    drawNightClock();
    return;
  }

  drawGaugeRing();
  cv->fillCircle(120, 108, 84, COL_BODY);
  // rim dots: use LUT instead of per-dot sinf/cosf
  for (int a = 100; a <= 175; a += 4) {
    int16_t cs = fastCos10(a);
    int16_t sn = fastSin10(a);
    int rx = 120 + ((int32_t)82 * cs >> SIN_LUT_SHIFT);
    int ry = 108 - ((int32_t)82 * sn >> SIN_LUT_SHIFT);
    cv->fillCircle(rx, ry, 1, COL_RIM);
  }

  drawPoops(tms);
  drawPet(tms);
  drawParticles();
  drawIcons();
  if (chooserOpen) drawChooser();
  if (infoOverlay) drawInfo();
  if (game.active) drawGameOverlay(tms);
}

// ---- stat ring + battery ----
static inline void arcDot(int r, float angDeg, uint16_t c)
{
  int16_t cs = fastCos10((int)angDeg);
  int16_t sn = fastSin10((int)angDeg);
  int x = 120 + (int)((int32_t)r * cs >> SIN_LUT_SHIFT);
  int y = 108 - (int)((int32_t)r * sn >> SIN_LUT_SHIFT);
  cv->fillCircle(x, y, 2, c);
}

static inline void arcTrack(int centerDeg, int span, uint16_t c)
{
  int half = span / 2;
  for (int a = centerDeg - half; a <= centerDeg + half; a += 5) arcDot(113, a, c);
}

static inline void arcValue(int centerDeg, uint8_t q, uint16_t c)
{
  if (q < 3) return;
  int span = (int)(100.0f * (q / 255.0f));
  int half = span / 2;
  for (int a = centerDeg - half; a <= centerDeg + half; a += 4) arcDot(113, a, c);
}

void drawGaugeRing()
{
  arcTrack(150, 100, COL_TRACK);
  arcTrack(30, 100, COL_TRACK);
  arcTrack(270, 100, COL_TRACK);
  arcTrack(90, 100, COL_TRACK);          // battery sector (top)

  uint8_t hq = (uint8_t)(tama.hunger * 2.55f);
  uint8_t fq = (uint8_t)(tama.fun * 2.55f);
  uint8_t eq = (uint8_t)(tama.energy * 2.55f);
  arcValue(150, hq, lerpColor(COL_BAD, COL_GOOD, hq));
  arcValue(30, fq, lerpColor(RGB565(120, 120, 140), COL_HEART, fq));
  arcValue(270, eq, lerpColor(COL_BAD, RGB565(90, 190, 255), eq));
  if (batteryPct >= 0) {
    uint8_t bq = (uint8_t)(batteryPct * 2.55f);
    arcValue(90, bq, lerpColor(COL_BAD, COL_GOOD, bq));
  }
}

// --------------------------- poops --------------------------
void drawPoops(uint32_t tms)
{
  for (int i = 0; i < 4; i++) {
    if (!poopsActive(i)) continue;
    int x = POOP_SPOTS[i][0], y = POOP_SPOTS[i][1];
    cv->fillCircle(x, y + 3, 7, COL_POOP);
    cv->fillCircle(x - 3, y - 2, 5, COL_POOP);
    cv->fillCircle(x + 3, y - 3, 4, COL_POOP);
    cv->fillCircle(x, y - 7, 3, COL_POOP);
    if ((tms >> 8) & 1) cv->drawLine(x + 8, y - 8, x + 11, y - 12, RGB565(110, 130, 90));
  }
}

// ============================ PET ===========================
void drawPet(uint32_t tms)
{
  float scale = (tama.stage == STAGE_BABY ? 0.72f : tama.stage == STAGE_TEEN ? 0.88f : 1.0f)
              * FORM_SCALE[tama.form];

  float hue = fmodf(tms * 0.006f + tama.stage * 90.0f + P_HUE[tama.pers], 360.0f);
  uint16_t irisCol  = hsv565(hue, 0.72f, 0.85f);
  uint16_t irisDark = hsv565(hue, 0.85f, 0.45f);

  bool happy = happyUntil && (int32_t)(happyUntil - tms) > 0;
  bool celebrating = (int32_t)(celebrateUntil - tms) > 0;
  bool grumpy = (int32_t)(grumpyUntil - tms) > 0;
  bool curious = (int32_t)(curiousUntil - tms) > 0;
  bool zooming = (int32_t)(zoomUntil - tms) > 0;
  bool sleeping = tama.state == ST_SLEEPING;
  bool angry = (int32_t)(millis() - angryUntil) < 0;
  bool sad = !sleeping && !happy && !grumpy && !zooming && !angry &&
             (tama.hunger < 25 || tama.fun < 20 || tama.energy < 15);

  // angry overrides iris to red
  if (angry) {
    irisCol = RGB565(220, 50, 50);
    irisDark = RGB565(160, 30, 30);
  }

  float squash = 1.0f;
  if (sleeping) squash = 0.12f;
  else if (happy || celebrating) squash = 0.62f + 0.06f * sinf(tms * 0.02f);
  else if (angry) squash = 0.92f + 0.03f * sinf(tms * 0.03f);  // vibrating with rage
  else if (sad) squash = 0.82f;

  float droop = sad ? 0.88f : 1.0f;
  int ry = (int)((42.0f * squash * droop) * scale *
                 (sleeping ? 1.0f : (1.0f - 0.96f * (1.0f - blinkPhase))));
  int rx = (int)(44.0f * scale * (1.0f + 0.02f * breathe));
  if (ry < 2) ry = 2;
  int irisR = (int)(21 * scale), pupR = (int)(11 * scale);
  if (curious) { irisR = (int)(irisR * 1.05f); pupR = (int)(pupR * 0.8f); }

  int shift = zooming ? (int)(sinf(tms * 0.045f) * 72) : 0;
  int eyeCy = 104 + (int)(3.0f * breathe) + (sleeping ? 4 : 0);
  int gx = (int)gazeX, gy = (int)gazeY;

  drawAntenna(tms, irisCol, scale, shift);

  if (sleeping) {
    drawShutEye(76 + shift, eyeCy, (int)(40 * scale));
    drawShutEye(164 + shift, eyeCy, (int)(40 * scale));
    if (random(12) < 2) spawnPart(1, 168 + shift, 66, 1, -2, 40);
  } else {
    drawEyeInt(76 + shift, eyeCy, rx, ry, gx, gy, irisR, pupR, irisCol, irisDark, sad);
    drawEyeInt(164 + shift, eyeCy, rx, ry, gx, gy, irisR, pupR, irisCol, irisDark, sad);
    if ((tama.fun < 12 || tama.hunger < 10) && random(60) == 0)
      spawnPart(5, 76 - (int)(rx * 0.8f) + shift, eyeCy - 6, -1, 2, 30);
  }

  drawMouth(tms, sleeping, happy || celebrating, sad, grumpy, angry);

  if (zooming && random(10) < 3)
    spawnPart(2, 120 + random(-80, 81), 100 + random(-40, 41), random(-3, 4), random(-2, 2), 12);
  if (tama.form == F_SPARKLY && random(40) == 0)
    spawnPart(3, 120 + random(-50, 51), 60 + random(-20, 21), 0, -1, 20);

  if (refuseActive()) {
    cv->fillCircle(120, 52, 13, COL_SCLERA);
    cv->setFont(&FreeSansBold12pt7b);
    drawCenteredString("!", 120, 52, COL_BAD);
  }
  if (angry) {
    // anger veins above head
    cv->drawLine(98, 62, 106, 56, RGB565(220, 50, 50));
    cv->drawLine(106, 56, 114, 62, RGB565(220, 50, 50));
    cv->drawLine(126, 62, 134, 56, RGB565(220, 50, 50));
    cv->drawLine(134, 56, 142, 62, RGB565(220, 50, 50));
  }
  if (bubble.type) drawBubble();
}

void drawAntenna(uint32_t tms, uint16_t c, float scale, int shift)
{
  if (tama.stage == STAGE_BABY) {
    cv->fillCircle(120 + shift, 54, 3, c);
    return;
  }
  int swayDeg = (int)(tms * 0.004f * (180.0f / PI)) % 360;
  int16_t swaySin = fastSin10(swayDeg);
  int16_t swayCos = fastCos10(swayDeg);
  float swayF = swaySin * 0.35f / (1 << SIN_LUT_SHIFT);
  float len = (tama.form == F_ATHLETIC) ? 24.0f
            : (tama.stage == STAGE_ADULT) ? 20.0f : 15.0f;
  float bx = 120 + shift, by = 58;
  float tx = bx + swayF * len;
  float ty = by - swayCos * 0.35f * len / (1 << SIN_LUT_SHIFT);  // cos(small) ≈ cosDeg
  cv->drawLine(bx, by, tx, ty, COL_RIM);
  cv->drawLine(bx + 1, by, tx, ty, COL_RIM);
  cv->fillCircle(tx, ty, 4, (tama.form == F_SPARKLY) ? RGB565(255, 255, 255) : c);
  cv->fillCircle(tx - 1, ty - 1, 2, COL_SCLERA);
}

void drawShutEye(int cx, int cy, int rxw)
{
  // parabolic approximation of sin(pi*x/rxw): max error ~3% for this use case
  int rr = rxw * rxw;
  for (int dx = -rxw; dx <= rxw; dx++) {
    int ddx = dx * dx;
    int yy = cy + (int)((int32_t)3 * (rr - ddx) / rr);  // ~sin curve, integer only
    cv->fillCircle(cx + dx, yy, 2, RGB565(60, 66, 84));
  }
}

void drawMouth(uint32_t tms, bool sleeping, bool happy, bool sad, bool grumpy, bool angry)
{
  if (sleeping || grumpy) return;
  if (tama.state == ST_EATING) {
    float open = fastFabs(sinf(tms * 0.022f));
    int mw = 8 + (int)(9 * open);
    cv->fillEllipse(120, 162, mw, 5, RGB565(26, 14, 20));
    if (mw > 10) cv->fillEllipse(120, 163, mw - 4, 2, RGB565(255, 118, 140));
    if (open > 0.82 && random(100) < 25)
      spawnPart(2, 120 + random(-mw, mw + 1), 168, random(-2, 3), -2, 14);
    return;
  }
  if (angry) {
    // sharp V-frown
    for (int i = 0; i <= 12; i++) {
      int x = 108 + i * 2;
      int y = 178 + (i < 6 ? i * 2 : (12 - i) * 2);
      cv->fillCircle(x, y, 2, RGB565(200, 60, 60));
    }
    // steam puffs
    if (random(100) < 8) {
      spawnPart(3, 80 + random(80), 70 + random(10), random(-1, 2), -2, 18);
    }
    return;
  }
  if (sad) {
    for (int a = 35; a <= 145; a += 3) {
      int16_t cs = fastCos10(a), sn = fastSin10(a);
      int x = 120 + ((int32_t)30 * cs >> SIN_LUT_SHIFT);
      int y = 184 - ((int32_t)30 * sn >> SIN_LUT_SHIFT);
      cv->fillCircle(x, y, 3, RGB565(120, 126, 142));
    }
    return;
  }
  if (happy) {
    for (int a = 35; a <= 145; a += 2) {
      int16_t cs = fastCos10(a), sn = fastSin10(a);
      int x = 120 + ((int32_t)42 * cs >> SIN_LUT_SHIFT);
      int y = 132 + ((int32_t)42 * sn >> SIN_LUT_SHIFT);
      if (y > 205) continue;
      cv->fillCircle(x, y, 4, RGB565(235, 240, 248));
    }
    return;
  }
  cv->fillRoundRect(112, 160, 16, 3, 1, RGB565(90, 96, 112));
}

void drawEyeInt(int cx, int cy, int rx, int ry, int gx, int gy, int irisR, int pupR,
                uint16_t irisCol, uint16_t irisDark, bool sad)
{
  int rx2 = rx * rx, ry2 = ry * ry;
  int irisR2 = irisR * irisR, pupR2 = pupR * pupR;
  int ix = cx + gx, iy = cy + gy;
  int hx = ix - (irisR * 9) / 20, hy = iy - (irisR * 9) / 20;
  int hlR2 = 18;
  int blushY = cy + (ry * 55) / 100 + 6;
  int sadCutoffY = sad ? (cy - ry / 3) : -1;  // precompute per-pixel branch

  int y0 = cy - ry - 1, y1 = cy + ry + 1;
  if (y0 < 0) y0 = 0;
  if (y1 > 239) y1 = 239;

  for (int py = y0; py <= y1; py++) {
    int dy = py - cy;
    int dy2 = dy * dy;
    if (dy2 > ry2) continue;
    int q = ((ry2 - dy2) << 10) / ry2;
    int hw = (rx * isqrt32((uint32_t)q << 10)) >> 10;
    if (hw < 1) continue;
    int x0 = cx - hw, x1 = cx + hw;
    if (x0 < 0) x0 = 0;
    if (x1 > 239) x1 = 239;
    uint16_t *row = fbRow(py);
    bool sadRow = sad && py < sadCutoffY;
    for (int px = x0; px <= x1; px++) {
      uint16_t col = sadRow ? RGB565(205, 212, 224) : COL_SCLERA;
      int dix = px - ix, diy = py - iy;
      int di = dix * dix + diy * diy;
      if (di < irisR2) {
        col = (diy > 2) ? irisDark : irisCol;
        if (di < pupR2) col = COL_PUPIL;
        else {
          int dhx = px - hx, dhy = py - hy;
          if (dhx * dhx + dhy * dhy < hlR2) col = RGB565(255, 255, 255);
        }
      }
      row[px] = col;
    }
  }
  if (!sad) cv->fillCircle(cx, blushY, 4, RGB565(255, 130, 150));
}

// ------------------------ speech bubble ---------------------
void drawBubble()
{
  int bx = 120, by = 26;
  cv->fillRoundRect(bx - 19, by - 12, 38, 24, 8, COL_SCLERA);
  cv->fillTriangle(bx - 6, by + 10, bx + 6, by + 10, bx, by + 17, COL_SCLERA);
  switch (bubble.type) {
    case 1: {  // burger
      cv->fillRoundRect(bx - 8, by - 5, 16, 5, 2, RGB565(120, 80, 40));
      cv->fillRect(bx - 8, by, 16, 2, RGB565(235, 220, 120));
      cv->fillRoundRect(bx - 8, by + 2, 16, 4, 2, RGB565(120, 80, 40));
      break;
    }
    case 2: {  // ball
      cv->fillCircle(bx, by, 6, RGB565(90, 190, 255));
      cv->drawLine(bx - 6, by, bx + 6, by, RGB565(230, 245, 255));
      break;
    }
    case 3:  // zzz
      cv->setFont(NULL);
      cv->setCursor(bx - 6, by + 7);
      cv->setTextColor(RGB565(60, 70, 95));
      cv->print("zZ");
      break;
    case 4:  // question
      cv->setFont(&FreeSansBold12pt7b);
      drawCenteredString("?", bx, by, RGB565(40, 45, 60));
      break;
    case 5:  // hi
      cv->setFont(&FreeSansBold12pt7b);
      drawCenteredString("hi!", bx, by, RGB565(40, 45, 60));
      break;
  }
}

// ------------------------ particles -------------------------
void drawParticles()
{
  bool fontSet = false;
  for (auto &p : parts) {
    if (!p.active) continue;
    if (p.x < -10 || p.x > 250 || p.y < -10 || p.y > 250) continue; // off-screen skip
    switch (p.type) {
      case 0: drawHeart(p.x, p.y, 1, COL_HEART); break;
      case 1:
        if (!fontSet) { cv->setFont(NULL); fontSet = true; }
        cv->setCursor(p.x, p.y);
        cv->setTextColor(RGB565(150, 160, 185));
        cv->print("z");
        break;
      case 2: cv->fillCircle(p.x, p.y, 2, RGB565(240, 200, 120)); break;
      case 3: drawSparkle(p.x, p.y, COL_GOOD); break;
      case 4: {
        uint16_t c = lerpColor(RGB565(255, 255, 255), COL_BG, (uint8_t)(255 - p.life * 25));
        cv->drawCircle(p.x, p.y, (uint8_t)(24 - p.life * 2), c);
        break;
      }
      case 5: cv->fillEllipse(p.x, p.y, 2, 3, RGB565(140, 180, 255)); break;
    }
  }
}

void drawHeart(int x, int y, int s, uint16_t c)
{
  cv->fillCircle(x - 3 * s, y - 2 * s, 3 * s, c);
  cv->fillCircle(x + 3 * s, y - 2 * s, 3 * s, c);
  cv->fillTriangle(x - 6 * s, y, x + 6 * s, y, x, y + 7 * s, c);
}

void drawSparkle(int x, int y, uint16_t c)
{
  cv->drawLine(x - 5, y, x + 5, y, c);
  cv->drawLine(x, y - 5, x, y + 5, c);
  cv->fillCircle(x, y, 2, c);
}

// -------------------------- icons ---------------------------
void drawIcons()
{
  bool sleeping = tama.state == ST_SLEEPING;
  bool playing = tama.state == ST_PLAYING;
  uint16_t c = sleeping ? COL_ICONDIM : COL_ICON;

  cv->fillRoundRect(ICON[0][0] - 11, ICON[0][1] - 9, 22, 7, 3, c);
  cv->fillRect(ICON[0][0] - 11, ICON[0][1] - 1, 22, 3, c);
  cv->fillRoundRect(ICON[0][0] - 11, ICON[0][1] + 3, 22, 6, 3, c);

  cv->fillCircle(ICON[1][0], ICON[1][1] - 2, 9, playing ? COL_GOOD : c);
  cv->drawLine(ICON[1][0] - 9, ICON[1][1] - 2, ICON[1][0] + 9, ICON[1][1] - 2, COL_BG);
  cv->drawArc(ICON[1][0], ICON[1][1] - 2, 12, 9, 200, 340, c);

  cv->fillTriangle(ICON[2][0], ICON[2][1] - 12, ICON[2][0] - 8, ICON[2][1] + 2, ICON[2][0] + 8, ICON[2][1] + 2, c);
  cv->fillCircle(ICON[2][0], ICON[2][1] + 2, 8, c);
  cv->fillCircle(ICON[2][0] - 3, ICON[2][1], 3, COL_BG);
}

// ---------------------- game chooser ------------------------
void drawChooser()
{
  cv->fillCircle(120, 116, 100, RGB565(8, 10, 16));
  cv->drawCircle(120, 116, 100, COL_RIM);
  for (int g = 0; g < 2; g++) {
    int cx = CHOICE[g][0], cy = CHOICE[g][1];
    cv->fillCircle(cx, cy, 28, RGB565(24, 28, 40));
    cv->drawCircle(cx, cy, 28, COL_RIM);
    if (g == 0) {              // pop: pulsing dot
      cv->fillCircle(cx, cy, 10, RGB565(90, 190, 255));
      cv->fillCircle(cx - 3, cy - 3, 4, RGB565(210, 240, 255));
      cv->setFont(&FreeSansBold9pt7b);
      drawCenteredString("pop", cx, cy + 40, RGB565(150, 160, 185));
    } else {                   // catch: falling burger
      cv->fillRoundRect(cx - 8, cy - 6, 16, 5, 2, RGB565(120, 80, 40));
      cv->fillRect(cx - 8, cy - 1, 16, 2, RGB565(235, 220, 120));
      cv->fillRoundRect(cx - 8, cy + 1, 16, 4, 2, RGB565(120, 80, 40));
      cv->fillCircle(cx, cy + 14, 2, RGB565(240, 200, 120));
      cv->setFont(&FreeSansBold9pt7b);
      drawCenteredString("catch", cx, cy + 40, RGB565(150, 160, 185));
    }
  }
  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString("pick a game", 120, 190, RGB565(130, 138, 158));
}

// ---------------------- game overlays -----------------------
void drawGameOverlay(uint32_t tms)
{
  if (game.type == 0) {
    if (game.waitNext) return;
    uint32_t el = millis() - game.roundStart;
    int pulse = 20 + ((int)(sinf(tms * 0.02f) * 1.6f)); // ~19..21, integer step
    cv->fillCircle(game.tx, game.ty, pulse, RGB565(90, 190, 255));
    cv->fillCircle(game.tx - pulse / 3, game.ty - pulse / 3, pulse / 3, RGB565(210, 240, 255));
    float frac = 1.0f - (float)el / 3200.0f;
    int arcDeg = (int)(360.0f * frac);
    for (int a = -90; a < -90 + arcDeg; a += 6) {
      int16_t cs = fastCos10(a), sn = fastSin10(a);
      int dx = (int)((int32_t)(pulse + 5) * cs >> SIN_LUT_SHIFT);
      int dy = (int)((int32_t)(pulse + 5) * sn >> SIN_LUT_SHIFT);
      cv->fillCircle(game.tx + dx, game.ty - dy, 1, COL_GOOD);
    }
    cv->setFont(&FreeSansBold9pt7b);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/5", game.hits);
    drawCenteredString(buf, 120, 40, RGB565(150, 160, 185));
  } else if (game.type == 1) {            // catch
    for (int i = 0; i < 3; i++) {
      if (!game.drops[i].on) continue;
      int x = game.drops[i].x, y = game.drops[i].y;
      cv->fillRoundRect(x - 8, y - 6, 16, 5, 2, RGB565(120, 80, 40));
      cv->fillRect(x - 8, y - 1, 16, 2, RGB565(235, 220, 120));
      cv->fillRoundRect(x - 8, y + 1, 16, 4, 2, RGB565(120, 80, 40));
    }
    cv->setFont(&FreeSansBold9pt7b);
    char buf[8];
    snprintf(buf, sizeof(buf), "%u/5", game.score);
    drawCenteredString(buf, 120, 40, RGB565(150, 160, 185));
  }
}

// ---------------------- info overlay ------------------------
void drawInfo()
{
  // full-screen white flash for the first 300ms: unmissable proof of open
  if (millis() - infoOpenAt < 300) {
    uint16_t *fb = cv->getFramebuffer();
    fb[0] = RGB565(255, 255, 255);
    for (int n = 1; n < 240 * 240; n *= 2) {
      int todo = (240 * 240 - n < n) ? 240 * 240 - n : n;
      for (int i = 0; i < todo; i++) fb[n + i] = fb[i];
    }
  }

  // flashing border while open
  if ((millis() >> 9) & 1) cv->drawRect(1, 1, 238, 238, RGB565(255, 255, 255));
  else cv->drawRect(1, 1, 238, 238, RGB565(90, 170, 255));

  // bright panel - impossible to miss
  cv->fillCircle(120, 116, 98, RGB565(30, 34, 48));
  cv->drawCircle(120, 116, 98, RGB565(90, 110, 150));
  cv->fillCircle(120, 116, 93, RGB565(22, 25, 36));
  cv->fillRoundRect(120 - 88, 16, 176, 16, 6, RGB565(60, 80, 120));
  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString("PET STATS", 120, 24, RGB565(230, 238, 250));

  char line[48];
  cv->setFont(&FreeSansBold12pt7b);
  if (tama.form != F_BALANCED)
    snprintf(line, sizeof(line), "%s %s", STAGE_NAME[tama.stage], FORM_NAME[tama.form]);
  else
    snprintf(line, sizeof(line), "%s", STAGE_NAME[tama.stage]);
  drawCenteredString(line, 120, 46, RGB565(240, 244, 252));

  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString(P_NAME[tama.pers], 120, 68, RGB565(170, 180, 200));

  bar(88, "FULL", tama.hunger, COL_GOOD, COL_BAD);
  bar(114, "FUN ", tama.fun, COL_HEART, RGB565(120, 120, 140));
  bar(140, "Zzz ", tama.energy, RGB565(90, 190, 255), COL_BAD);

  cv->setFont(&FreeSansBold9pt7b);
  uint32_t days = tama.ageSec / 86400;
  uint32_t daysInStage = (tama.stage == STAGE_BABY) ? (tama.ageSec / 3600)
                 : (tama.stage == STAGE_TEEN) ? ((tama.ageSec - 3600) / 3600)
                 : ((tama.ageSec - 43200) / 3600);
  const char *stageMilestone = (tama.stage == STAGE_BABY) ? "TEEN" : (tama.stage == STAGE_TEEN) ? "ADULT" : "mature";
  snprintf(line, sizeof(line), "day %u  (%uh to %s)", (unsigned)days, (unsigned)daysInStage, stageMilestone);
  drawCenteredString(line, 120, 156, RGB565(180, 188, 205));
  float sp = stagePct();
  cv->fillRect(120 - 100, 166, 200, 4, RGB565(30, 34, 44));
  cv->fillRect(120 - 100, 166, (int)(200 * sp), 4, lerpColor(COL_BAD, COL_GOOD, (uint8_t)(sp * 255)));

  if (timeSynced) {
    const char *DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    snprintf(line, sizeof(line), "%02d:%02d  %s %d/%02d",
             curHour, curMin, DOW[curDow], curDay, curMon);
    drawCenteredString(line, 120, 186, RGB565(150, 200, 160));
  } else {
    drawCenteredString("OFFLINE (no wifi)", 120, 186, RGB565(150, 158, 175));
  }
  if (batteryPct >= 0) {
    snprintf(line, sizeof(line), "BAT %d%%", (int)batteryPct);
  } else {
    snprintf(line, sizeof(line), "USB power");
  }
  drawCenteredString(line, 120, 200, RGB565(180, 188, 205));
  drawCenteredString("tap to close", 120, 208, RGB565(140, 148, 165));
}

float stagePct()
{
  if (tama.stage == STAGE_BABY) return tama.ageSec / 3600.0f;
  if (tama.stage == STAGE_TEEN) return (tama.ageSec - 3600) / 39600.0f;
  return 1.0f;   // adult: no milestone
}

void bar(int y, const char *label, float v, uint16_t hi, uint16_t lo)
{
  cv->setFont(&FreeSansBold9pt7b);
  cv->setCursor(38, y + 6);
  cv->setTextColor(RGB565(140, 148, 165));
  cv->print(label);
  cv->drawRoundRect(86, y - 4, 116, 12, 5, COL_RIM);
  int w = (int)(112 * (v / 100.0f));
  if (w > 0) cv->fillRoundRect(88, y - 2, w, 8, 4, lerpColor(lo, hi, (uint8_t)(v * 2.55f)));
}

// ---------------------- night clock -------------------------
void drawNightClock()
{
  if (curHour < 0) return;
  cv->drawCircle(120, 120, 116, RGB565(20, 22, 30));
  cv->drawCircle(120, 120, 114, RGB565(14, 15, 20));
  // tick marks: precompute 12 unique positions, mirror for remaining 48
  for (int i = 0; i < 60; i++) {
    int deg = i * 6;                            // 0..354
    int16_t ca = fastCos10(deg), sa = fastSin10(deg);
    bool major = (i % 5 == 0);
    int rOut = major ? 106 : 109;
    uint16_t c = major ? RGB565(120, 128, 148) : RGB565(50, 55, 70);
    int x0 = 120 + ((int32_t)101 * ca >> SIN_LUT_SHIFT);
    int y0 = 120 + ((int32_t)101 * sa >> SIN_LUT_SHIFT);
    int x1 = 120 + ((int32_t)rOut * ca >> SIN_LUT_SHIFT);
    int y1 = 120 + ((int32_t)rOut * sa >> SIN_LUT_SHIFT);
    cv->drawLine(x0, y0, x1, y1, c);
    if (major) {
      int x2 = 120 + ((int32_t)102 * ca >> SIN_LUT_SHIFT);
      int y2 = 120 + ((int32_t)102 * sa >> SIN_LUT_SHIFT);
      cv->drawLine(x2, y2, x1, y1, c);
    }
  }
  float sec = curSec + (millis() % 1000) / 1000.0f;
  float minv = curMin + sec / 60.0f;
  float hrsv = (curHour % 12) + minv / 60.0f;
  clockHand(hrsv * 30 * DEG_TO_RAD, 50, 4, RGB565(150, 158, 178));
  clockHand(minv * 6 * DEG_TO_RAD, 78, 3, RGB565(120, 128, 148));
  // second hand
  int sdeg = (int)(sec * 6) % 360;
  int16_t cs = fastCos10(sdeg), ss = fastSin10(sdeg);
  int sx0 = 120 - ((int32_t)16 * cs >> SIN_LUT_SHIFT);
  int sy0 = 120 - ((int32_t)16 * ss >> SIN_LUT_SHIFT);
  int sx1 = 120 + ((int32_t)90 * cs >> SIN_LUT_SHIFT);
  int sy1 = 120 + ((int32_t)90 * ss >> SIN_LUT_SHIFT);
  cv->drawLine(sx0, sy0, sx1, sy1, RGB565(150, 60, 70));
  cv->fillCircle(120, 120, 5, RGB565(150, 60, 70));
  cv->fillCircle(120, 120, 2, RGB565(200, 205, 220));
  const char *DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  char line[24];
  snprintf(line, sizeof(line), "%s %d", DOW[curDow], curDay);
  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString(line, 120, 176, RGB565(70, 76, 94));
}

void clockHand(float rad, float len, float wid, uint16_t color)
{
  int deg = (int)(rad * (180.0f / PI)) % 360;
  int16_t ca = fastCos10(deg), sa = fastSin10(deg);
  float x0 = 120 - ca * 10.0f / (1 << SIN_LUT_SHIFT);
  float y0 = 120 - sa * 10.0f / (1 << SIN_LUT_SHIFT);
  float x1 = 120 + ca * len / (1 << SIN_LUT_SHIFT);
  float y1 = 120 + sa * len / (1 << SIN_LUT_SHIFT);
  cv->drawLine((int)x0, (int)y0, (int)x1, (int)y1, color);
  float dx = x1 - x0, dy = y1 - y0;
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 1) return;
  float ox = -dy / l * wid * 0.5f, oy = dx / l * wid * 0.5f;
  cv->drawLine((int)(x0 + ox), (int)(y0 + oy), (int)(x1 + ox), (int)(y1 + oy), color);
  cv->drawLine((int)(x0 - ox), (int)(y0 - oy), (int)(x1 - ox), (int)(y1 - oy), color);
}

// ---------------------- egg intro ---------------------------
void drawEggIntro(uint32_t tms)
{
  uint32_t el = tms - bootAt;
  cv->fillCircle(120, 116, 96, RGB565(8, 10, 16));

  // egg wobbles more over time
  float wobbleAmt = min((float)el / 60000.0f, 1.0f) * 10.0f;
  int ex = 120 + (int)(sinf(el * 0.008f) * wobbleAmt);

  // egg body
  cv->fillEllipse(ex, 118, 34, 44, RGB565(225, 228, 238));
  cv->fillEllipse(ex - 8, 104, 10, 14, RGB565(245, 246, 252));

  // cracks appear progressively
  if (el > 120000) cv->drawLine(ex - 14, 106, ex - 4, 114, RGB565(90, 96, 112));
  if (el > 180000) {
    cv->drawLine(ex - 4, 114, ex + 6, 108, RGB565(90, 96, 112));
    cv->drawLine(ex + 6, 108, ex + 2, 124, RGB565(90, 96, 112));
  }
  if (el > 240000) {
    cv->drawLine(ex + 2, 124, ex - 10, 130, RGB565(90, 96, 112));
    cv->drawLine(ex - 10, 130, ex + 4, 138, RGB565(90, 96, 112));
  }

  // text
  cv->setFont(&FreeSansBold9pt7b);
  if (el < 180000)
    drawCenteredString("something is moving", 120, 186, RGB565(130, 138, 158));
  else
    drawCenteredString("almost...", 120, 186, RGB565(130, 138, 158));

  // hatch after 5 minutes
  if (el > 300000) {
    virginBoot = false;
    tama.hatched = true;
    statsDirty = true;
    saveTamagotchi();
    happyUntil = millis() + 1500;
    for (int i = 0; i < 12; i++)
      spawnPart(3, 120 + random(-70, 71), 110 + random(-60, 61), 0, -2, 30);
    dbg("[TAMA] hatched as %s!\n", P_NAME[tama.pers]);
  }
}

// ---------------------- death screen -------------------------
void drawDeath(uint32_t tms)
{
  uint16_t *fb = cv->getFramebuffer();
  // dark dirt background
  for (uint32_t i = 0; i < 240 * 240; i++) fb[i] = RGB565(8, 6, 12);

  // ground
  cv->fillRect(0, 180, 240, 60, RGB565(28, 20, 14));
  for (int x = 0; x < 240; x += 3) {
    int h = 2 + ((x * 7 + 13) % 5);
    cv->fillRect(x, 180 - h, 2, h, RGB565(22, 16, 10));
  }

  // tombstone body
  cv->fillRoundRect(80, 90, 80, 90, 6, RGB565(90, 95, 110));
  cv->fillRoundRect(84, 94, 72, 82, 4, RGB565(70, 75, 90));
  // tombstone top arch
  cv->fillCircle(120, 94, 38, RGB565(90, 95, 110));
  cv->fillCircle(120, 94, 34, RGB565(70, 75, 90));
  // cross
  cv->fillRect(117, 100, 6, 30, RGB565(110, 115, 130));
  cv->fillRect(108, 110, 24, 6, RGB565(110, 115, 130));

  // "R.I.P." text
  cv->setFont(&FreeSansBold18pt7b);
  drawCenteredString("R.I.P.", 120, 82, RGB565(140, 145, 165));

  // pet info
  cv->setFont(&FreeSansBold9pt7b);
  char line[40];
  snprintf(line, sizeof(line), "%s %s", STAGE_NAME[tama.stage], P_NAME[tama.pers]);
  drawCenteredString(line, 120, 200, RGB565(100, 105, 120));
  uint32_t days = tama.ageSec / 86400;
  snprintf(line, sizeof(line), "lived %u day%s", (unsigned)days, days == 1 ? "" : "s");
  drawCenteredString(line, 120, 214, RGB565(80, 85, 100));

  // floating ghost wisps
  for (int i = 0; i < 3; i++) {
    int wx = 90 + i * 30 + (int)(sinf(tms * 0.001f + i * 2.1f) * 12);
    float phase = fmodf(tms * 0.02f + i * 40.0f, 100.0f);
    int wy = 60 - (int)phase;
    uint8_t alpha = (uint8_t)(100 - (int)phase);
    uint16_t gc = lerpColor(RGB565(60, 60, 80), RGB565(8, 6, 12), (uint8_t)(255 - alpha));
    cv->fillCircle(wx, wy, 3 + (alpha >> 6), gc);
  }

  // "tap to hatching" prompt
  bool blink = ((tms >> 9) & 1);
  if (blink)
    drawCenteredString("tap to hatching", 120, 232, RGB565(70, 75, 90));
}

// ---------------------- orientation test --------------------
void drawOrientationTest()
{
  uint16_t *fb = cv->getFramebuffer();
  for (uint32_t i = 0; i < 240 * 240; i++) fb[i] = RGB565(10, 10, 14);

  cv->setFont(&FreeSansBold12pt7b);
  drawCenteredString("TOP", 120, 22, RGB565(255, 255, 255));
  drawCenteredString("BOTTOM", 120, 218, RGB565(255, 255, 255));
  cv->setFont(&FreeSansBold18pt7b);
  drawCenteredString("L", 24, 120, RGB565(120, 200, 255));
  drawCenteredString("R", 216, 120, RGB565(120, 200, 255));
  int cx = 120, cy = 116;
  cv->fillTriangle(cx, cy - 22, cx - 14, cy + 2, cx + 14, cy + 2, RGB565(255, 200, 60));
  cv->fillRect(cx - 3, cy + 2, 6, 18, RGB565(255, 200, 60));
  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString("tap to continue", 120, 176, RGB565(140, 150, 170));
}

// ------------------------ text helper -----------------------
void drawCenteredString(const char *s, int cx, int cy, uint16_t color)
{
  int16_t x1, y1; uint16_t w, h;
  cv->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  cv->setCursor(cx - w / 2 - x1, cy - (h >> 1) - y1);
  cv->setTextColor(color);
  cv->print(s);
}

bool refuseActive() { return (int32_t)(refuseUntil - millis()) > 0; }
