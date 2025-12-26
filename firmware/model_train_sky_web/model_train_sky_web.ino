/*
  ESP32-S3 Feather controller (based on your working sketch)

  Changes:
  - Added Sky Sim slider (seconds per simulated hour)
  - Added Sky Sim toggle button
  - Sky rendering is smooth between LEDs (blends two adjacent pixels)
  - Sun color shifts toward orange/red near ends (sunrise/sunset)
  - Sky is never intentionally cleared when train runs; periodic refresh keeps it "staying on"

  Pins:
  - Train DC motor: GPIO 12 (ON/OFF)
  - LED Ring: GPIO 10 (ON/OFF)
  - Sky: APA102 (16 LEDs) on MOSI/SCK
*/

#include <WiFi.h>
#include <WebServer.h>
#include <FastLED.h>
#include <time.h>

// -------- WiFi credentials --------
const char* WIFI_SSID = "YOUR_WIFI_SSID";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

// -------- Time / timezone --------
const char* TZ_INFO = "EST5EDT,M3.2.0/2,M11.1.0/2"; // America/Toronto rules
const char* NTP1 = "pool.ntp.org";
const char* NTP2 = "time.nist.gov";

// -------- Pins --------
static const uint8_t TRAIN_MOTOR_PIN = 12; // DC motor control (ON/OFF)
static const uint8_t LED_RING_PIN    = 10; // ON/OFF

// -------- APA102 Sky --------
static const uint8_t NUM_LEDS = 16;
CRGB leds[NUM_LEDS];

// Use Feather SPI pins (DATA=MOSI, CLK=SCK)
#ifndef MOSI
  #define MOSI 35
#endif
#ifndef SCK
  #define SCK 36
#endif
static const uint8_t APA_DATA_PIN = MOSI;
static const uint8_t APA_CLK_PIN  = SCK;

// -------- State --------
bool trainOn = false;
bool ringOn  = false;
bool skyOn   = true;

// Sky simulation
bool skySimOn = false;

// Slider controls this: real seconds per simulated hour
// (Integer slider -> float stored)
float simSecondsPerHour = 2.0f;

// Sim base time when sim is enabled
unsigned long simStartMs = 0;
int simStartHour = 12;
int simStartMinute = 0;

WebServer server(80);

// -------- Time helpers --------
bool getLocalTimeHM(int &hour24, int &minute) {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 50)) return false;
  hour24 = timeinfo.tm_hour;
  minute = timeinfo.tm_min;
  return true;
}

// Get sky time: real or simulated
bool getSkyTimeHM(int &hour24, int &minute) {
  if (!skySimOn) {
    // If real time temporarily fails, do not "turn off" sky; just keep last displayed
    return getLocalTimeHM(hour24, minute);
  }

  // simulated time: advance smoothly
  unsigned long now = millis();
  float elapsedSec = (now - simStartMs) / 1000.0f;

  // Guard against bad slider value
  float secPerHour = simSecondsPerHour;
  if (secPerHour < 1.0f) secPerHour = 1.0f;
  if (secPerHour > 60.0f) secPerHour = 60.0f;

  float simMinutesPerSecond = 60.0f / secPerHour;
  int deltaMinutes = (int)(elapsedSec * simMinutesPerSecond);

  int baseMinutes = simStartHour * 60 + simStartMinute;
  int totalMinutes = baseMinutes + deltaMinutes;

  totalMinutes %= (24 * 60);
  if (totalMinutes < 0) totalMinutes += (24 * 60);

  hour24 = totalMinutes / 60;
  minute = totalMinutes % 60;
  return true;
}

static inline float clamp01(float x) {
  if (x < 0.0f) return 0.0f;
  if (x > 1.0f) return 1.0f;
  return x;
}

static inline uint8_t toU8(float x) {
  if (x < 0.0f) x = 0.0f;
  if (x > 255.0f) x = 255.0f;
  return (uint8_t)(x + 0.5f);
}

// Sun color: yellow mid-day, more orange/red near ends
CRGB sunColorForT(float t01) {
  // t01 is 0..1 across day (sunrise->sunset)
  // "edge" is 1 at ends, 0 at middle
  float edge = fabsf(2.0f * t01 - 1.0f);  // 0 mid, 1 ends

  // Base "noon" color
  CRGB noon = CRGB(255, 200, 0);

  // Sunset/sunrise color (more orange/red)
  CRGB edgeCol = CRGB(255, 60, 0);

  // Blend factor: push stronger near edges
  float k = clamp01(edge * edge); // quadratic for nicer curve
  uint8_t kb = toU8(k * 255.0f);

  return blend(noon, edgeCol, kb);
}

// Moon color: bluish
CRGB moonColor() {
  return CRGB(0, 80, 255);
}

// Render sky smoothly between pixels (two adjacent LEDs blend)
void renderSkySmooth() {
  static int lastA = -999;
  static int lastB = -999;
  static uint8_t lastWa = 0;
  static uint8_t lastWb = 0;
  static bool lastIsDay = false;
  static unsigned long lastForceMs = 0;

  int h, m;
  if (!getSkyTimeHM(h, m)) {
    // If time isn't available, do nothing (do NOT clear)
    return;
  }

  const int dayStart = 6;
  const int dayEnd   = 18;
  bool isDay = (h >= dayStart && h < dayEnd);

  float hourFrac = (float)h + (float)m / 60.0f;

  // Compute t 0..1 across the active span (day or night)
  float t = 0.0f;
  if (isDay) {
    t = (hourFrac - dayStart) / (float)(dayEnd - dayStart);
  } else {
    float hf = hourFrac;
    if (hf < dayStart) hf += 24.0f; // wrap night segment
    t = (hf - dayEnd) / (float)((dayStart + 24) - dayEnd);
  }
  t = clamp01(t);

  // position in [0..NUM_LEDS-1], fractional
  float posf = t * (NUM_LEDS - 1);
  int a = (int)floorf(posf);
  int b = a + 1;
  if (b >= NUM_LEDS) b = NUM_LEDS - 1;

  float frac = posf - (float)a; // 0..1
  uint8_t wb = toU8(frac * 255.0f);
  uint8_t wa = 255 - wb;

  // Only redraw if something changed, but also force refresh periodically
  unsigned long now = millis();
  bool changed = (a != lastA) || (b != lastB) || (wa != lastWa) || (wb != lastWb) || (isDay != lastIsDay);
  bool force = (now - lastForceMs) > 2000; // force a show every 2s so it never "goes stale"

  if (!changed && !force) return;

  lastForceMs = now;
  lastA = a; lastB = b; lastWa = wa; lastWb = wb; lastIsDay = isDay;

  // Clear and set two pixels with brightness weights
  for (int i = 0; i < NUM_LEDS; i++) leds[i] = CRGB::Black;

  CRGB c = isDay ? sunColorForT(t) : moonColor();

  // Scale by weights (CRGB::nscale8_video keeps a minimum visible glow)
  CRGB ca = c; ca.nscale8_video(wa);
  CRGB cb = c; cb.nscale8_video(wb);

  leds[a] += ca;
  leds[b] += cb;

  FastLED.show();
}

void clearSky() {
  FastLED.clear(true);
}

// -------- Web UI --------
const char PAGE_INDEX[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Train Controller</title>
  <style>
    body { font-family: sans-serif; max-width: 520px; margin: 24px auto; padding: 0 12px; }
    .card { border: 1px solid #ddd; border-radius: 10px; padding: 14px; margin: 12px 0; }
    .row { display: flex; gap: 10px; margin: 10px 0; flex-wrap: wrap; }
    button { flex: 1; padding: 14px; font-size: 16px; min-width: 150px; }
    .small { color: #666; font-size: 13px; }
    input[type="range"] { width: 100%; }
    pre { white-space: pre-wrap; }
  </style>
</head>
<body>
  <h2>Train Controller</h2>

  <div class="card">
    <div class="row">
      <button onclick="toggle('train')">Train ON/OFF</button>
      <button onclick="toggle('ring')">LEDs ON/OFF</button>
      <button onclick="toggle('sky')">Sky ON/OFF</button>
      <button onclick="toggle('skysim')">Sky Sim ON/OFF</button>
    </div>

    <div style="margin-top:12px;">
      <div>Sky Sim speed: <span id="simVal">2</span> sec / hour</div>
      <input id="sim" type="range" min="1" max="30" value="2" oninput="setSim(this.value)" />
      <div class="small">Lower = faster. 2 means 1 simulated hour every 2 seconds.</div>
    </div>
  </div>

  <div class="card">
    <button onclick="refresh()">Refresh status</button>
    <pre id="status"></pre>
  </div>

<script>
function api(path) { return fetch(path, {cache: "no-store"}).then(r => r.text()); }

function toggle(what) {
  api(`/toggle?what=${what}`).then(refresh);
}

function setSim(v) {
  document.getElementById('simVal').textContent = v;
  api(`/sim?sec=${v}`);
}

function refresh() {
  api('/status').then(t => document.getElementById('status').textContent = t);
}

refresh();
</script>
</body>
</html>
)HTML";

void handleRoot() {
  server.send(200, "text/html", PAGE_INDEX);
}

// /toggle?what=train|ring|sky|skysim
void handleToggle() {
  String what = server.arg("what");

  if (what == "train") {
    trainOn = !trainOn;
    digitalWrite(TRAIN_MOTOR_PIN, trainOn ? HIGH : LOW);

  } else if (what == "ring") {
    ringOn = !ringOn;
    digitalWrite(LED_RING_PIN, ringOn ? HIGH : LOW);

  } else if (what == "sky") {
    skyOn = !skyOn;
    if (!skyOn) clearSky();

  } else if (what == "skysim") {
    skySimOn = !skySimOn;

    // When enabling sim, seed it from real time if available
    int h, m;
    if (getLocalTimeHM(h, m)) {
      simStartHour = h;
      simStartMinute = m;
    } else {
      simStartHour = 12;
      simStartMinute = 0;
    }
    simStartMs = millis();
  }

  server.send(200, "text/plain", "OK");
}

// /sim?sec=1..30   (seconds per simulated hour)
void handleSim() {
  int sec = server.arg("sec").toInt();
  if (sec < 1) sec = 1;
  if (sec > 30) sec = 30;

  // Preserve continuity: rebase sim start so it doesn't "jump"
  if (skySimOn) {
    int h, m;
    if (getSkyTimeHM(h, m)) {
      simStartHour = h;
      simStartMinute = m;
      simStartMs = millis();
    }
  }

  simSecondsPerHour = (float)sec;
  server.send(200, "text/plain", "OK");
}

void handleStatus() {
  int h = -1, m = -1;
  bool ok = getSkyTimeHM(h, m);

  char buf[320];
  if (ok) {
    snprintf(buf, sizeof(buf),
      "sky_time: %02d:%02d%s\ntrain: %s\nleds: %s\nsky: %s\nsky_sim: %s\nsim_speed: %.0f sec / hour\n",
      h, m, skySimOn ? " (sim)" : "",
      trainOn ? "on" : "off",
      ringOn ? "on" : "off",
      skyOn ? "on" : "off",
      skySimOn ? "on" : "off",
      simSecondsPerHour
    );
  } else {
    snprintf(buf, sizeof(buf),
      "sky_time: (not synced yet)\ntrain: %s\nleds: %s\nsky: %s\nsky_sim: %s\nsim_speed: %.0f sec / hour\n",
      trainOn ? "on" : "off",
      ringOn ? "on" : "off",
      skyOn ? "on" : "off",
      skySimOn ? "on" : "off",
      simSecondsPerHour
    );
  }

  server.send(200, "text/plain", buf);
}

// -------- Setup / Loop --------
unsigned long lastSkyUpdateMs = 0;

void setup() {
  // Serial + IP printout
  Serial.begin(115200);
  delay(1200);

  // Outputs
  pinMode(TRAIN_MOTOR_PIN, OUTPUT);
  pinMode(LED_RING_PIN, OUTPUT);

  digitalWrite(TRAIN_MOTOR_PIN, LOW);
  digitalWrite(LED_RING_PIN, LOW);

  // APA102
  FastLED.addLeds<APA102, APA_DATA_PIN, APA_CLK_PIN, BGR>(leds, NUM_LEDS);
  FastLED.setBrightness(60);
  // Optional: makes low-level blends look steadier
  FastLED.setDither(false);

  clearSky();

  // WiFi connect
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 10000) {
    delay(200);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false); // reduces latency
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi not connected yet (server still starts).");
  }

  // Time
  configTzTime(TZ_INFO, NTP1, NTP2);

  // Web
  server.on("/", handleRoot);
  server.on("/toggle", handleToggle);
  server.on("/status", handleStatus);
  server.on("/sim", handleSim);
  server.begin();

  Serial.println("Web server started.");
}

void loop() {
  server.handleClient();

  // Update sky ~20 times per second so the blending looks smooth
  // (Not heavy: only 16 LEDs)
  if (skyOn) {
    unsigned long now = millis();
    if (now - lastSkyUpdateMs > 50) {   // 50ms -> smooth motion
      lastSkyUpdateMs = now;
      renderSkySmooth();
    }
  }
}
