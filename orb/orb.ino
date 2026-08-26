// ============================================================
//  ORB-TAMA - a pocket tamagotchi for the JCZN/Guition
//  ESP32-2424S012 (ESP32-C3, 1.28" round GC9A01 240x240 IPS,
//  CST816D touch). NO BUTTONS REQUIRED - everything is touch.
//
//  How to care for your Orb:
//    - DRAG your finger over it .... petting (hearts, +fun)
//    - TAP it ...................... it giggles (+tiny fun)
//    - BURGER icon (bottom left) ... feed (+fullness)
//    - BALL icon (bottom middle) ... play bubble-pop game
//                                    (+fun, -energy)
//    - DROP icon (bottom right) .... clean up everything
//    - TAP a poop .................. clean just that one
//    - HOLD finger ~1s ............. stats & age overlay
//
//  It gets hungry/bored/tired on its own, poops after meals,
//  falls asleep (and dims the screen) when exhausted, hatches
//  from an egg on first boot and evolves as it ages. State is
//  saved to flash every ~20s, so it survives power-off and
//  even re-flashing.
//
//  Set TIME_SCALE below to speed up its life (60 = 1 day per
//  24 minutes - great for watching it evolve).
// ============================================================

#include <Arduino_GFX_Library.h>
#include <Preferences.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

// ----------------------- user settings ----------------------
#define TIME_SCALE        60       // 1 = real time; 60 = 1 pet-day per 24 real minutes
#define BACKLIGHT_BRIGHT  255
#define BACKLIGHT_DIM     30
#define ROTATION          2        // flip to 0 if image is upside-down

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

// The heap's largest free block (~114.7KB) is too small for a malloc'ed
// 240x240x2 canvas, so we back it with a linker-placed static buffer.
static uint16_t g_framebuffer[240 * 240];

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
static inline uint16_t hsv565(float h, float s, float v)
{
  h = fmodf(h, 360.0f); if (h < 0) h += 360;
  float c = v * s, x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1)), m = v - c;
  float r, g, b;
  if      (h < 60)  { r = c; g = x; b = 0; }
  else if (h < 120) { r = x; g = c; b = 0; }
  else if (h < 180) { r = 0; g = c; b = x; }
  else if (h < 240) { r = 0; g = x; b = c; }
  else if (h < 300) { r = x; g = 0; b = c; }
  else              { r = c; g = 0; b = x; }
  return RGB565((int)((r + m) * 255), (int)((g + m) * 255), (int)((b + m) * 255));
}

static inline uint16_t lerpColor(uint16_t a, uint16_t b, uint8_t t) // t 0..255
{
  int ar = (a >> 11) << 3, ag = ((a >> 5) & 0x3F) << 2, ab = (a & 0x1F) << 3;
  int br = (b >> 11) << 3, bg = ((b >> 5) & 0x3F) << 2, bb = (b & 0x1F) << 3;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return RGB565(r, g, bl);
}

// ------------------------ integer sqrt ----------------------
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

static inline uint16_t *fbRow(int y) { return g_framebuffer + y * 240; }

// ----------------------- touch ------------------------------
bool tpTouch = false;        // true while finger is down (updated by pollTouch)
uint16_t tpX = 120, tpY = 120;
bool tpAlive = false;

void readRegs(uint8_t reg, uint8_t *buf, uint8_t len)
{
  Wire.beginTransmission(TP_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return;
  if (Wire.requestFrom(TP_ADDR, len) != len) return;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  tpAlive = true;
}

void touchInit()
{
  pinMode(T_RST, OUTPUT);
  digitalWrite(T_RST, LOW); delay(20);
  digitalWrite(T_RST, HIGH); delay(60);
  Wire.begin(T_SDA, T_SCL, 400000U);
  Wire.setTimeOut(50);
}

uint16_t lastSampleX = 120, lastSampleY = 120;
float strokeAccum = 0;
uint32_t lastHeartAt = 0;

// returns: 0=nothing, 1=just pressed, 2=still/moving down, 3=just released
uint8_t pollTouch()
{
  static bool wasDown = false;
  uint8_t r[6];
  readRegs(0x01, r, 6);
  bool down = (r[1] >= 1 && r[1] < 3);

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
    if (!wasDown) {
      lastSampleX = x; lastSampleY = y;
      strokeAccum = 0;
      wasDown = true;
      return 1;
    }
    // accumulate stroke distance for petting
    int dx = (int)x - lastSampleX, dy = (int)y - lastSampleY;
    strokeAccum += sqrtf((float)(dx * dx + dy * dy));
    lastSampleX = x; lastSampleY = y;
    return 2;
  }
  tpTouch = false;
  if (wasDown) { wasDown = false; return 3; }
  return 0;
}

// ----------------------- pet state --------------------------
enum PetState : uint8_t { ST_AWAKE = 0, ST_EATING, ST_PLAYING, ST_SLEEPING };
enum Stage : uint8_t { STAGE_BABY = 0, STAGE_TEEN, STAGE_ADULT };
const char *STAGE_NAME[] = { "BABY", "TEEN", "ADULT" };

struct {
  float hunger = 80, fun = 80, energy = 80;   // 0..100
  uint32_t ageSec = 0;
  bool hatched = false;
  PetState state = ST_AWAKE;
  Stage stage = STAGE_BABY;
} tama;

struct Poop { bool active; uint8_t slot; };
Poop poops[4];
const int16_t POOP_SPOTS[4][2] = { {44,146}, {196,146}, {86,174}, {154,174} };

struct Particle {
  bool active;
  int16_t x, y;
  int8_t vx, vy;
  uint8_t life, type;   // 0 heart, 1 zzz, 2 crumb, 3 sparkle, 4 popring
};
Particle parts[24];

float blinkPhase = 1.0f;
bool blinking = false;
uint32_t blinkStart = 0, nextBlink = 0;
float gazeX = 0, gazeY = 0, tgtX = 0, tgtY = 0;
uint32_t nextWander = 0;
float breathe = 0;
uint32_t happyUntil = 0;
uint32_t refuseUntil = 0;      // head-shake "!"
uint32_t eatStart = 0;
uint32_t celebrateUntil = 0;
uint32_t stateEnd = 0;         // generic end timestamp for EATING
bool infoOverlay = false;
bool virginBoot = false;
uint32_t bootAt = 0;

// mini-game
struct Game {
  bool active = false;
  uint8_t round, hits;
  int16_t tx, ty;
  uint32_t roundStart;
  bool waitNext;               // pause between rounds
  uint32_t waitStart;
} game;

// frame pacing / fps
uint32_t frameStart = 0, fpsFrames = 0, fpsT0 = 0;
float fpsShown = 0;

// persistence
Preferences prefs;
bool statsDirty = false;
uint32_t lastSaveAt = 0;

void saveTamagotchi()
{
  prefs.begin("tama", false);   // read-write
  prefs.putBool("hatched", tama.hatched);
  prefs.putUChar("hunger", (uint8_t)tama.hunger);
  prefs.putUChar("fun", (uint8_t)tama.fun);
  prefs.putUChar("energy", (uint8_t)tama.energy);
  prefs.putUInt("age", tama.ageSec);
  uint8_t pmask = 0;
  for (int i = 0; i < 4; i++) if (poops[i].active) pmask |= (1 << i);
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
  uint8_t pmask = prefs.getUChar("poops", 0);
  prefs.end();
  for (int i = 0; i < 4; i++) poops[i] = { (pmask & (1 << i)) != 0, (uint8_t)i };
  tama.stage = tama.ageSec < 3600UL ? STAGE_BABY
             : tama.ageSec < 43200UL ? STAGE_TEEN : STAGE_ADULT;
}

void spawnPart(uint8_t type, int16_t x, int16_t y, int8_t vx, int8_t vy, uint8_t life)
{
  for (auto &p : parts) {
    if (!p.active) {
      p = { true, x, y, vx, vy, life, type };
      return;
    }
  }
}

void clearPoops()
{
  for (auto &p : poops) p.active = false;
  statsDirty = true;
}

// ------------------- sim (fixed timestep) -------------------
uint32_t lastTick = 0;
uint32_t pendingMealPoop = 0;

void simTick()   // every 250ms
{
  float dm = 0.25f / 60.0f * TIME_SCALE;       // simulated minutes elapsed
  tama.ageSec += (uint32_t)(0.25f * TIME_SCALE);

  bool anyPoop = false;
  for (auto &p : poops) anyPoop |= p.active;

  if (tama.state != ST_SLEEPING) {
    tama.hunger -= 0.40f * dm;
    tama.fun -= (anyPoop ? 0.56f : 0.28f) * dm;
    tama.energy -= (tama.state == ST_PLAYING ? 0.0f : 0.15f) * dm;
  } else {
    tama.energy += 0.85f * dm;                 // sleeping recharges
  }

  if (tama.hunger < 0) tama.hunger = 0;
  if (tama.fun < 0) tama.fun = 0;
  if (tama.energy > 100) tama.energy = 100;

  // meal -> eventual poop
  if (pendingMealPoop && random(100) < 3) {
    pendingMealPoop--;
    for (auto &p : poops) {
      if (!p.active) { p.active = true; statsDirty = true; break; }
    }
  }

  // exhaustion -> sleep
  if (tama.state == ST_AWAKE && tama.energy <= 4) {
    tama.state = ST_SLEEPING;
    statsDirty = true;
    Serial.println("[TAMA] fell asleep");
  }
  if (tama.state == ST_SLEEPING) {
    analogWrite(PIN_BL, BACKLIGHT_DIM);
    if (tama.energy >= 97) {
      tama.state = ST_AWAKE;
      happyUntil = millis() + 1200;
      analogWrite(PIN_BL, BACKLIGHT_BRIGHT);
      Serial.println("[TAMA] woke up rested");
    }
  }

  // evolution
  Stage st = tama.ageSec < 3600UL ? STAGE_BABY
           : tama.ageSec < 43200UL ? STAGE_TEEN : STAGE_ADULT;
  if (st != tama.stage) {
    tama.stage = st;
    celebrateUntil = millis() + 2200;
    for (int i = 0; i < 10; i++)
      spawnPart(3, 120 + random(-60, 61), 90 + random(-50, 51), 0, -2, 30);
    Serial.printf("[TAMA] evolved to %s!\n", STAGE_NAME[st]);
  }

  if (statsDirty && millis() - lastSaveAt > 20000) saveTamagotchi();

  // periodic status log for remote verification
  static uint32_t lastStatLog = 0;
  if (millis() - lastStatLog > 60000) {
    lastStatLog = millis();
    Serial.printf("[TAMA] age=%us h=%u f=%u e=%u stage=%s poops=%u\n",
                  (unsigned)tama.ageSec, (uint8_t)tama.hunger, (uint8_t)tama.fun,
                  (uint8_t)tama.energy, STAGE_NAME[tama.stage],
                  poops[0].active + poops[1].active + poops[2].active + poops[3].active);
  }
}

// ------------------- interaction ----------------------------
const int16_t ICON[3][2] = { {58,186}, {120,198}, {182,186} };  // feed play clean

bool hitIcon(int idx)
{
  int dx = tpX - ICON[idx][0], dy = tpY - ICON[idx][1];
  return dx * dx + dy * dy <= 22 * 22;
}

bool hitPoop(int16_t &sx, int16_t &sy)
{
  for (auto &p : poops) {
    if (!p.active) continue;
    int dx = tpX - POOP_SPOTS[p.slot][0], dy = tpY - POOP_SPOTS[p.slot][1];
    if (dx * dx + dy * dy <= 16 * 16) { sx = POOP_SPOTS[p.slot][0]; sy = POOP_SPOTS[p.slot][1]; return true; }
  }
  return false;
}

void headShake()
{
  refuseUntil = millis() + 800;
}

void startFeeding()
{
  if (tama.state != ST_AWAKE) return;
  if (tama.hunger > 92) { headShake(); return; }
  tama.state = ST_EATING;
  eatStart = millis();
  stateEnd = eatStart + 1900;
}

void startGame()
{
  if (tama.state != ST_AWAKE) return;
  if (tama.energy < 15) { headShake(); return; }   // too tired
  game.active = true;
  game.round = 0;
  game.hits = 0;
  game.waitNext = false;
  tama.state = ST_PLAYING;
  nextGameTarget();
}

void nextGameTarget()
{
  for (int tries = 0; tries < 20; tries++) {
    float a = random(0, 628) * 0.01f;
    float rr = 30 + random(0, 46);
    int x = 120 + (int)(cosf(a) * rr);
    int y = 105 + (int)(sinf(a) * rr);
    int dxF = x - ICON[0][0], dyF = y - ICON[0][1];
    if (dxF * dxF + dyF * dyF < 30 * 30) continue;
    game.tx = x; game.ty = y;
    break;
  }
  game.roundStart = millis();
  game.waitNext = false;
}

void endGame()
{
  game.active = false;
  tama.fun += 8 + game.hits * 4;
  if (tama.fun > 100) tama.fun = 100;
  tama.energy -= 7;
  if (tama.energy < 0) tama.energy = 0;
  tama.state = ST_AWAKE;
  happyUntil = millis() + 1400;
  statsDirty = true;
  spawnPart(3, 120, 60, 0, -1, 26);
  Serial.printf("[TAMA] game over hits=%u\n", game.hits);
}

void handleTap(uint16_t x, uint16_t y)
{
  int16_t sx, sy;

  if (virginBoot) return;                       // still hatching
  if (infoOverlay) { infoOverlay = false; return; }

  if (tama.state == ST_PLAYING) {               // bubble-pop taps
    if (game.active) handleTapGame(x, y);
    return;
  }
  if (tama.state == ST_SLEEPING) {          // wake up
    tama.state = ST_AWAKE;
    analogWrite(PIN_BL, BACKLIGHT_BRIGHT);
    happyUntil = millis() + 600;
    statsDirty = true;
    return;
  }
  if (tama.state != ST_AWAKE) return;

  for (int i = 0; i < 3; i++) {
    if (hitIcon(i)) {
      if (i == 0) startFeeding();
      else if (i == 1) startGame();
      else {
        if (!poops[0].active && !poops[1].active && !poops[2].active && !poops[3].active) headShake();
        else { clearPoops(); celebrateUntil = millis() + 800; }
      }
      return;
    }
  }
  if (hitPoop(sx, sy)) {                    // swipe-free single cleanup
    for (auto &p : poops)
      if (p.active && POOP_SPOTS[p.slot][0] == sx && POOP_SPOTS[p.slot][1] == sy) p.active = false;
    statsDirty = true;
    spawnPart(3, sx, sy, 0, -1, 20);
    return;
  }
  // plain poke on the creature
  int dx = x - 120, dy = y - 104;
  if (dx * dx + dy * dy < 95 * 95) {
    happyUntil = millis() + 900;
    tama.fun += 1;
    if (tama.fun > 100) tama.fun = 100;
    statsDirty = true;
  }
}

void handleTouchFrame()
{
  uint8_t ev = pollTouch();
  static uint32_t downAt = 0;
  static bool longFired = false;
  static bool moved = false;

  if (ev == 1) {                            // pressed
    downAt = millis();
    longFired = false;
    moved = false;
  } else if (ev == 2) {                     // held / moving
    if (strokeAccum > 26) moved = true;
    // petting: drag across the creature
    if (tama.state == ST_AWAKE && moved && !infoOverlay &&
        millis() - lastHeartAt > 260) {
      int dx = tpX - 120, dy = tpY - 104;
      if (dx * dx + dy * dy < 95 * 95) {
        lastHeartAt = millis();
        happyUntil = millis() + 900;
        tama.fun += 0.9f;
        if (tama.fun > 100) tama.fun = 100;
        statsDirty = true;
        spawnPart(0, tpX, tpY - 12, 0, -2, 22);
        strokeAccum = 0;
      }
    }
    if (!longFired && millis() - downAt > 700 &&
        strokeAccum < 14 && tama.state == ST_AWAKE) {
      longFired = true;
      infoOverlay = !infoOverlay;
    }
  } else if (ev == 3) {                     // released
    if (!longFired && !moved && millis() - downAt < 500) {
      handleTap(tpX, tpY);
    }
  }
}

// =========================== SETUP ==========================
void setup()
{
  Serial.begin(115200);
  delay(120);
  Serial.println("\n[ORB-TAMA] booting");

  pinMode(PIN_BL, OUTPUT);
  analogWrite(PIN_BL, 0);

  if (!gfx->begin(80000000)) Serial.println("[ORB] gfx begin FAILED");
  cv->begin(GFX_SKIP_OUTPUT_BEGIN);
  gfx->fillScreen(0x0000);
  analogWrite(PIN_BL, BACKLIGHT_BRIGHT);

  touchInit();
  loadTamagotchi();

  bootAt = millis();
  fpsT0 = millis();
}

// ============================ LOOP ==========================
void loop()
{
  frameStart = micros();
  uint32_t nowMs = millis();
  handleTouchFrame();

  if (nowMs - lastTick >= 250) {
    lastTick = nowMs;
    simTick();
  }

  if (tama.state == ST_EATING && (int32_t)(stateEnd - nowMs) < 0) {
    tama.state = ST_AWAKE;
    tama.hunger += 24;
    if (tama.hunger > 100) tama.hunger = 100;
    pendingMealPoop++;
    happyUntil = nowMs + 1000;
    statsDirty = true;
  }
  if (tama.state == ST_PLAYING && game.active) tickGame();

  updateLook(nowMs);
  renderScene(nowMs);
  cv->flush();

  fpsFrames++;
  if (nowMs - fpsT0 >= 1000) {
    fpsShown = fpsFrames * 1000.0f / (nowMs - fpsT0);
    fpsFrames = 0; fpsT0 = nowMs;
    Serial.printf("[TAMA] %s %.1ffps h=%u\n", STAGE_NAME[tama.stage], fpsShown, ESP.getFreeHeap());
  }

  int32_t budget = 22000 - (int32_t)(micros() - frameStart);
  if (budget > 0) delayMicroseconds(budget);
}

// ------------------------- mini-game ------------------------
void tickGame()
{
  uint32_t nowMs = millis();
  if (game.waitNext) {
    if (nowMs - game.waitStart > 450) {
      if (game.round >= 5) endGame();
      else nextGameTarget();
    }
    return;
  }
  // timeout?
  if (nowMs - game.roundStart > 3200) {
    game.round++;
    game.waitNext = true;
    game.waitStart = nowMs;
  }
  // hit test happens on tap (see handleTapGame)
}

void handleTapGame(uint16_t x, uint16_t y)
{
  if (game.waitNext) return;
  int dx = x - game.tx, dy = y - game.ty;
  int r = 20 + (millis() - game.roundStart > 2400 ? -6 : 0); // shrinks late
  if (dx * dx + dy * dy <= r * r) {
    game.hits++;
    for (int i = 0; i < 6; i++)
      spawnPart(2, game.tx, game.ty, random(-3, 4), random(-4, 1), 14);
    spawnPart(4, game.tx, game.ty, 0, 0, 10);
    game.round++;
    game.waitNext = true;
    game.waitStart = millis();
  }
}

// --------------------- look / animation ---------------------
void updateLook(uint32_t tms)
{
  breathe = sinf(tms * (2 * PI / 4200.0f));

  bool chasing = (tama.state == ST_PLAYING && game.active && !game.waitNext);
  if (chasing) {
    tgtX = constrain((float)game.tx - 120, -70, 70) * 0.6f;
    tgtY = constrain((float)game.ty - 104, -60, 60) * 0.6f;
  } else if (tpTouchLive()) {
    tgtX = constrain((float)tpX - 120, -70, 70) * 0.55f;
    tgtY = constrain((float)tpY - 104, -60, 60) * 0.55f;
  } else if (refuseActive()) {
    tgtX = sinf(tms * 0.04f) * 14;
    tgtY = 0;
  } else {
    if ((int32_t)(tms - nextWander) >= 0) {
      nextWander = tms + 1500 + random(2800);
      if (random(10) < 3) { tgtX = 0; tgtY = 0; }
      else { tgtX = (float)random(-22, 23); tgtY = (float)random(-14, 15); }
    }
  }
  gazeX += (tgtX - gazeX) * 0.18f;
  gazeY += (tgtY - gazeY) * 0.18f;

  // blinking (not while sleeping - eyes stay shut)
  if (tama.state != ST_SLEEPING) {
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
        nextBlink = tms + 2200 + random(4500);
      }
    } else blinkPhase = 1.0f;
  }

  // particles live in real time
  for (auto &p : parts) {
    if (!p.active) continue;
    p.x += p.vx; p.y += p.vy;
    if (p.life) p.life--;
    if (!p.life) p.active = false;
  }
}

bool tpTouchLive()
{
  return tpTouch;   // maintained directly by pollTouch
}

bool refuseActive()
{
  return (int32_t)(refuseUntil - millis()) > 0;
}

// ========================== RENDER ==========================
void renderScene(uint32_t tms)
{
  uint16_t *fb = cv->getFramebuffer();
  for (uint32_t i = 0; i < 240 * 240; i++) fb[i] = COL_BG;

  drawGaugeRing();

  // body disc + rim light
  cv->fillCircle(120, 108, 84, COL_BODY);
  for (float a = 100; a <= 175; a += 4) {
    float rad = a * DEG_TO_RAD;
    cv->fillCircle(120 + cosf(rad) * 82, 108 - sinf(rad) * 82, 1, COL_RIM);
  }

  drawPoops(tms);
  drawPet(tms);
  drawParticles();
  drawIcons();
  if (infoOverlay) drawInfo();
  if (virginBoot) drawEggIntro(tms);
  if (game.active) drawGameOverlay(tms);
}

// ---- stat ring: three colored arc segments around the bezel ----
void drawGaugeRing()
{
  arcTrack(150, 100, COL_TRACK);   // hunger sector (upper left)
  arcTrack(30, 100, COL_TRACK);    // fun sector (upper right)
  arcTrack(270, 100, COL_TRACK);   // energy sector (bottom)

  uint8_t hq = (uint8_t)(tama.hunger * 2.55f);
  uint8_t fq = (uint8_t)(tama.fun * 2.55f);
  uint8_t eq = (uint8_t)(tama.energy * 2.55f);
  uint16_t hc = lerpColor(COL_BAD, COL_GOOD, hq);
  uint16_t fc = lerpColor(RGB565(120, 120, 140), COL_HEART, fq);
  uint16_t ec = lerpColor(COL_BAD, RGB565(90, 190, 255), eq);

  arcValue(150, hq, hc);
  arcValue(30, fq, fc);
  arcValue(270, eq, ec);
}

void arcDot(int r, float angDeg, uint16_t c)
{
  float rad = angDeg * DEG_TO_RAD;
  int x = 120 + (int)(cosf(rad) * r);
  int y = 108 - (int)(sinf(rad) * r);
  cv->fillCircle(x, y, 2, c);
}

void arcTrack(int centerDeg, int span, uint16_t c)
{
  float half = span * 0.5f;
  for (float a = centerDeg - half; a <= centerDeg + half; a += 5) arcDot(113, a, c);
}

void arcValue(int centerDeg, uint8_t q, uint16_t c)
{
  if (q < 3) return;
  float span = 100.0f * (q / 255.0f);
  float half = span * 0.5f;
  for (float a = centerDeg - half; a <= centerDeg + half; a += 3.5f) arcDot(113, a, c);
}

// --------------------------- poops --------------------------
void drawPoops(uint32_t tms)
{
  for (auto &p : poops) {
    if (!p.active) continue;
    int x = POOP_SPOTS[p.slot][0], y = POOP_SPOTS[p.slot][1];
    cv->fillCircle(x, y + 3, 7, COL_POOP);
    cv->fillCircle(x - 3, y - 2, 5, COL_POOP);
    cv->fillCircle(x + 3, y - 3, 4, COL_POOP);
    cv->fillCircle(x, y - 7, 3, COL_POOP);
    // little stink wisp
    if ((tms >> 8) & 1) cv->drawLine(x + 8, y - 8, x + 11, y - 12, RGB565(110, 130, 90));
  }
}

// ============================ PET ===========================
void drawPet(uint32_t tms)
{
  float scale = tama.stage == STAGE_BABY ? 0.72f
              : tama.stage == STAGE_TEEN ? 0.88f : 1.0f;

  float hue = fmodf(tms * 0.006f + tama.stage * 90.0f, 360.0f);
  uint16_t irisCol  = hsv565(hue, 0.72f, 0.85f);
  uint16_t irisDark = hsv565(hue, 0.85f, 0.45f);

  bool happy = happyUntil && (int32_t)(happyUntil - tms) > 0;
  bool celebrating = (int32_t)(celebrateUntil - tms) > 0;
  bool sleeping = tama.state == ST_SLEEPING;
  bool sad = !sleeping && !happy &&
             (tama.hunger < 30 || tama.fun < 30 || tama.energy < 20);

  float squash = 1.0f;
  if (sleeping) squash = 0.12f;
  else if (happy || celebrating) squash = 0.62f + 0.06f * sinf(tms * 0.02f);
  else if (sad) squash = 0.82f;

  float droop = sad ? 0.88f : 1.0f;         // sad = heavy eyelids
  int ry = (int)((42.0f * squash * droop) * scale * (sleeping ? 1.0f : (1.0f - 0.96f * (1.0f - blinkPhase))));
  int rx = (int)(44.0f * scale * (1.0f + 0.02f * breathe));
  if (ry < 2) ry = 2;
  int irisR = (int)(21 * scale), pupR = (int)(11 * scale);

  int eyeCy = 104 + (int)(3.0f * breathe) + (sleeping ? 4 : 0);
  int gx = (int)gazeX, gy = (int)gazeY;

  drawAntenna(tms, irisCol, scale);

  if (sleeping) {
    // closed sleepy eyes: gentle curved lashes
    drawShutEye(76, eyeCy, (int)(40 * scale));
    drawShutEye(164, eyeCy, (int)(40 * scale));
    if (random(10) < 2) spawnPart(1, 168, 66, 1, -2, 40);
  } else {
    drawEyeInt(76, eyeCy, rx, ry, gx, gy, irisR, pupR, irisCol, irisDark, sad);
    drawEyeInt(164, eyeCy, rx, ry, gx, gy, irisR, pupR, irisCol, irisDark, sad);
    if ((tama.fun < 12 || tama.hunger < 10) && random(60) == 0)
      spawnPart(5, 76 - (int)(rx * 0.8f), eyeCy - 6, -1, 2, 30);   // tear drip
  }

  drawMouth(tms, sleeping, happy || celebrating, sad);

  if (refuseActive()) {
    cv->fillCircle(120, 52, 13, COL_SCLERA);
    cv->setFont(&FreeSansBold12pt7b);
    drawCenteredString("!", 120, 52, COL_BAD);
  }
}

void drawAntenna(uint32_t tms, uint16_t c, float scale)
{
  if (tama.stage == STAGE_BABY) {
    cv->fillCircle(120, 54, 3, c);                       // little nub
    return;
  }
  float sway = sinf(tms * 0.004f) * 0.35f;
  float len = (tama.stage == STAGE_ADULT ? 20.0f : 15.0f);
  float bx = 120, by = 58;
  float tx = bx + sinf(sway) * len, ty = by - cosf(sway) * len;
  cv->drawLine(bx, by, tx, ty, COL_RIM);
  cv->drawLine(bx + 1, by, tx, ty, COL_RIM);
  cv->fillCircle(tx, ty, 4, c);
  cv->fillCircle(tx - 1, ty - 1, 2, COL_SCLERA);
}

void drawShutEye(int cx, int cy, int rxw)
{
  for (int dx = -rxw; dx <= rxw; dx++) {
    int yy = cy + (int)(sinf(dx * PI / (float)rxw) * 3.0f);
    cv->fillCircle(cx + dx, yy, 2, RGB565(60, 66, 84));
  }
}

void drawMouth(uint32_t tms, bool sleeping, bool happy, bool sad)
{
  if (sleeping) return;
  if (tama.state == ST_EATING) {
    float open = fabsf(sinf(tms * 0.022f));
    int mh = 3 + (int)(9 * open);
    cv->fillEllipse(120, 162, 11, mh, RGB565(30, 20, 24));
    return;
  }
  if (sad) {   // frown
    for (float a = -40; a <= 40; a += 3) {
      float rad = a * DEG_TO_RAD;
      int x = 120 + (int)(cosf(rad) * 30);
      int y = 184 + (int)(sinf(rad) * 30);
      cv->fillCircle(x, y, 3, RGB565(120, 126, 142));
    }
    return;
  }
  if (happy) { // smile
    for (float a = 35; a <= 145; a += 2) {
      float rad = a * DEG_TO_RAD;
      int x = 120 + (int)(cosf(rad) * 42);
      int y = 132 + (int)(sinf(rad) * 42);
      if (y > 205) continue;
      cv->fillCircle(x, y, 4, RGB565(235, 240, 248));
    }
    return;
  }
  // neutral: tiny flat mouth
  cv->fillRoundRect(112, 160, 16, 3, 1, RGB565(90, 96, 112));
}

// integer eye rasterizer (proven fast version)
void drawEyeInt(int cx, int cy, int rx, int ry, int gx, int gy, int irisR, int pupR,
                uint16_t irisCol, uint16_t irisDark, bool sad)
{
  int rx2 = rx * rx, ry2 = ry * ry;
  int irisR2 = irisR * irisR, pupR2 = pupR * pupR;
  int ix = cx + gx, iy = cy + gy;
  int hx = ix - (irisR * 9) / 20, hy = iy - (irisR * 9) / 20;
  int hlR2 = 18;
  int blushY = cy + (ry * 55) / 100 + 6;

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
    for (int px = x0; px <= x1; px++) {
      int dx = px - cx;
      uint16_t col = sad && py < cy - ry / 3 ? RGB565(205, 212, 224) : COL_SCLERA;
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
  if (!sad) cv->fillCircle(cx, blushY, 4, RGB565(255, 130, 150));  // permanent blush
}

// ------------------------ particles -------------------------
void drawParticles()
{
  for (auto &p : parts) {
    if (!p.active) continue;
    switch (p.type) {
      case 0: drawHeart(p.x, p.y, 1, COL_HEART); break;
      case 1:
        cv->setFont(NULL);
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
      case 5: cv->fillEllipse(p.x, p.y, 2, 3, RGB565(140, 180, 255)); break; // tear
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

  // burger
  cv->fillRoundRect(ICON[0][0] - 11, ICON[0][1] - 9, 22, 7, 3, c);
  cv->fillRect(ICON[0][0] - 11, ICON[0][1] - 1, 22, 3, c);
  cv->fillRoundRect(ICON[0][0] - 11, ICON[0][1] + 3, 22, 6, 3, c);
  // ball
  cv->fillCircle(ICON[1][0], ICON[1][1] - 2, 9, playing ? COL_GOOD : c);
  cv->drawLine(ICON[1][0] - 9, ICON[1][1] - 2, ICON[1][0] + 9, ICON[1][1] - 2, COL_BG);
  cv->drawArc(ICON[1][0], ICON[1][1] - 2, 12, 9, 200, 340, c);
  // water drop (clean)
  cv->fillTriangle(ICON[2][0], ICON[2][1] - 12, ICON[2][0] - 8, ICON[2][1] + 2, ICON[2][0] + 8, ICON[2][1] + 2, c);
  cv->fillCircle(ICON[2][0], ICON[2][1] + 2, 8, c);
  cv->fillCircle(ICON[2][0] - 3, ICON[2][1], 3, COL_BG);
}

// ---------------------- game overlay ------------------------
void drawGameOverlay(uint32_t tms)
{
  if (game.waitNext) return;
  uint32_t el = millis() - game.roundStart;
  float pulse = 1.0f + 0.08f * sinf(tms * 0.02f);
  int r = (int)(17 * pulse);
  cv->fillCircle(game.tx, game.ty, r, RGB565(90, 190, 255));
  cv->fillCircle(game.tx - r / 3, game.ty - r / 3, r / 3, RGB565(210, 240, 255));
  // countdown ring
  float frac = 1.0f - (float)el / 3200.0f;
  for (float a = -90; a < -90 + 360 * frac; a += 6) {
    float rad = a * DEG_TO_RAD;
    cv->fillCircle(game.tx + cosf(rad) * (r + 5), game.ty - sinf(rad) * (r + 5), 1, COL_GOOD);
  }
  cv->setFont(&FreeSansBold9pt7b);
  char buf[8];
  snprintf(buf, sizeof(buf), "%u/5", game.hits);
  drawCenteredString(buf, 120, 40, RGB565(150, 160, 185));
}

// ---------------------- info overlay ------------------------
void drawInfo()
{
  cv->fillCircle(120, 116, 96, RGB565(8, 10, 16));
  cv->drawCircle(120, 116, 96, COL_RIM);
  cv->drawCircle(120, 116, 94, RGB565(18, 21, 30));

  char line[40];
  cv->setFont(&FreeSansBold12pt7b);
  snprintf(line, sizeof(line), "%s", STAGE_NAME[tama.stage]);
  drawCenteredString(line, 120, 52, COL_ICON);

  cv->setFont(&FreeSansBold9pt7b);
  uint32_t mins = tama.ageSec / 60;
  if (mins < 90) snprintf(line, sizeof(line), "age %umin", (unsigned)mins);
  else snprintf(line, sizeof(line), "age %uh%02um", (unsigned)(mins / 60), (unsigned)(mins % 60));
  drawCenteredString(line, 120, 76, RGB565(130, 138, 158));

  bar(52, "FULL", tama.hunger, COL_GOOD, COL_BAD);
  bar(84, "FUN ", tama.fun, COL_HEART, RGB565(120, 120, 140));
  bar(116, "Zzz ", tama.energy, RGB565(90, 190, 255), COL_BAD);

  int np = 0;
  for (auto &p : poops) np += p.active;
  snprintf(line, sizeof(line), np ? "%d mess%s to clean" : "all tidy!", np, np == 1 ? "" : "es");
  drawCenteredString(line, 120, 148, np ? RGB565(210, 160, 90) : RGB565(120, 200, 150));
  drawCenteredString("tap to close", 120, 172, RGB565(90, 96, 112));
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

// ---------------------- egg intro ---------------------------
void drawEggIntro(uint32_t tms)
{
  uint32_t el = tms - bootAt;
  cv->fillCircle(120, 116, 96, RGB565(8, 10, 16));
  float wob = sinf(el * 0.012f) * 0.12f;
  int ex = 120 + (int)(sinf(el * 0.012f) * 6);
  cv->fillEllipse(ex, 118, 34, 44, RGB565(225, 228, 238));
  cv->fillEllipse(ex - 8, 104, 10, 14, RGB565(245, 246, 252));
  // cracks appear progressively
  if (el > 900)  cv->drawLine(ex - 14, 106, ex - 4, 114, RGB565(90, 96, 112));
  if (el > 1500) { cv->drawLine(ex - 4, 114, ex + 6, 108, RGB565(90, 96, 112)); cv->drawLine(ex + 6, 108, ex + 2, 124, RGB565(90, 96, 112)); }
  (void)wob;
  cv->setFont(&FreeSansBold9pt7b);
  drawCenteredString(el > 1500 ? "almost..." : "something is moving", 120, 186, RGB565(130, 138, 158));
  if (el > 2100) {
    virginBoot = false;
    tama.hatched = true;
    statsDirty = true;
    saveTamagotchi();
    happyUntil = millis() + 1500;
    for (int i = 0; i < 12; i++)
      spawnPart(3, 120 + random(-70, 71), 110 + random(-60, 61), 0, -2, 30);
    Serial.println("[TAMA] hatched!");
  }
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
