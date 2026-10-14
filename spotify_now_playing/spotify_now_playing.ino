/*
  Spotify "Now Playing" display  -  version 2.1
  ESP32 + 2.8" 240x320 SPI TFT (ST7789 driver)

  New in version 2:
    - Equaliser animation next to the song info
    - Smooth transitions when the song changes
    - Smooth (anti-aliased) Poppins fonts
    - Synced lyrics from LRCLIB (lrclib.net), when available
    - "Up next" from your Spotify queue
    - Faster drawing (40 MHz SPI, flicker-free strip rendering, network on its own core)

  Fixed in version 2.1:
    - "Not enough memory for image": the old song is freed before the new cover
      downloads, free memory is checked first, and if the 300 px cover still
      doesn't fit the small 64 px cover is used (scaled up smoothly)
    - "Queue JSON error: IncompleteInput": only the first song of the queue is
      read, instead of the whole (very large) queue
    - "LRCLIB JSON error: EmptyInput": slow replies are waited for, and failed
      requests are tried once more
    - Low-memory cover drawing keeps the rounded corners and shadow

  Put BOTH files in the same sketch folder:
    spotify_now_playing.ino   (this file)
    fonts.h                   (the fonts)

  Libraries (Arduino Library Manager):
    - Adafruit GFX Library
    - Adafruit ST7735 and ST7789 Library
    - ArduinoJson (version 7)
    - TJpg_Decoder (by Bodmer)

  Fits the default partition scheme (about 90% of flash). If you add more
  features later, switch Tools > Partition Scheme to "Huge APP (3MB No OTA/1MB SPIFFS)".
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <base64.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <ArduinoJson.h>
#include <TJpg_Decoder.h>
#include <vector>
#include <algorithm>
#include <functional>
#include "fonts.h"

// ================== YOUR SETTINGS ==================
const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

const char* CLIENT_ID     = "YOUR_SPOTIFY_CLIENT_ID";
const char* CLIENT_SECRET = "YOUR_SPOTIFY_CLIENT_SECRET";
const char* REFRESH_TOKEN = "YOUR_SPOTIFY_REFRESH_TOKEN";

// ================== DISPLAY PINS ===================
#define TFT_CS   13
#define TFT_RST  12
#define TFT_DC   14
#define TFT_MOSI 27
#define TFT_SCK  26
#define TFT_MISO 33

const uint8_t  SCREEN_MADCTL = 0x28;       // orientation + red/blue fix
const uint32_t SPI_SPEED     = 40000000;   // 40 MHz. Use 27000000 or 20000000 if you see glitches

// ================== FEATURES =======================
const bool     SHOW_LYRICS      = true;
const long     LYRICS_OFFSET_MS = 300;     // show each lyric line slightly early
const uint32_t POLL_MS          = 2500;    // how often to ask Spotify what's playing
const uint32_t QUEUE_REFRESH_MS = 30000;   // how often to refresh "up next"

// ================== LAYOUT =========================
const int SCREEN_W = 320;
const int SCREEN_H = 240;

// Progress bar and times (both layouts)
const int BAR_X = 16, BAR_Y = 206, BAR_W = 288, BAR_H = 4;
const int TIME_BASELINE = 228;
const int PROG_RX = 6, PROG_RY = 197, PROG_RW = 308, PROG_RH = 39;

// Layout without lyrics: big cover on the left, info on the right
const int CL_COVER   = 150, CL_COVER_X = 16, CL_COVER_Y = 24;
const int CL_PANEL_X = 182, CL_PANEL_W = 126;
const int CL_STATUS_Y = 24;
const int CL_TEXT_Y  = 46, CL_TEXT_H = 134;

// Layout with lyrics: small cover and info on top, lyrics below
const int LL_COVER   = 75, LL_COVER_X = 16, LL_COVER_Y = 14;
const int LL_TEXT_X  = 104, LL_TEXT_W = 204;
const int LL_STATUS_Y = 14;
const int LL_HEAD_Y  = 34, LL_HEAD_H = 61;
const int LYR_X = 6, LYR_Y = 100, LYR_W = 308, LYR_H = 94;
const int LYR_TEXT_X = 16, LYR_TEXT_W = 288;
const int LYR_ACTIVE_TOP = LYR_Y + 26;
const int LYR_GAP = 6;

const int SHADOW = 9;           // soft shadow size around the cover
const int STRIP_PIXELS = 320 * 20;   // drawing buffer (12.5 KB). Smaller leaves more memory for covers
const uint16_t WHITE = 0xFFFF;
const uint16_t BLACK = 0x0000;

// ================== TYPES ==========================
struct LyricLine {
  uint32_t ms;
  String text;
};

struct TrackData {
  String id, title, artist;
  long durationMs = 0;
  uint8_t* jpg = nullptr;
  size_t jpgSize = 0;
  std::vector<LyricLine> lyrics;
  ~TrackData() { if (jpg) free(jpg); }
};

struct Palette {
  uint16_t top, bottom, accent, dim, track, future, past;
};

// A horizontal strip of the screen we draw into before sending it in one go
struct Canvas {
  uint16_t* buf;
  int x, y, w, h;
};
typedef std::function<void(Canvas&)> PaintFn;

enum ScreenMode { MODE_NONE, MODE_MESSAGE, MODE_LOADING, MODE_TRACK };
enum Layout { LAYOUT_COVER, LAYOUT_LYRICS };
enum JpgMode { JPG_MEASURE, JPG_TO_BUFFER, JPG_TO_SCREEN };

// Everything the network task hands over to the display
struct Shared {
  bool showTrack = false;
  String msg1, msg2;
  uint32_t msgSeq = 0;
  bool loading = false;
  TrackData* ready = nullptr;
  String upTitle, upArtist;
  uint32_t upSeq = 0;
  bool playing = false;
  long progressMs = 0;
  uint32_t progressAt = 0;
};

// ================== GLOBALS ========================
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);
Preferences prefs;
SemaphoreHandle_t stateLock;
Shared shared;

// Network task
String accessToken = "";
String refreshToken = "";
uint32_t tokenExpiresAt = 0;
String preparedId = "";
uint32_t lastQueueFetch = 0;
String lastUpTitle = "", lastUpArtist = "";
String lastMsg1 = "", lastMsg2 = "";

// Display
uint16_t* strip = nullptr;
uint16_t bgRow[SCREEN_H];
uint16_t bgPat[SCREEN_H][2];
Palette pal;
Palette messagePal;
ScreenMode mode = MODE_NONE;
Layout layout = LAYOUT_COVER;
TrackData* track = nullptr;
String dispTitle = "", dispArtist = "", dispUpTitle = "", dispUpArtist = "";
uint32_t shownMsgSeq = 0, shownUpSeq = 0;
bool playing = false;
long progressMs = 0;
uint32_t progressAt = 0;
long durationMs = 0;
float eqLevel[4] = { 3, 3, 3, 3 };
uint32_t lastEqFrame = 0, lastProgressDraw = 0, lastDotsFrame = 0, lastLyricFrame = 0;
int lastBarPx = -1;
long lastSecShown = -1;
int lyricIdx = -1, lyricPrevIdx = -1;
bool lyricAnim = false;
uint32_t lyricAnimStart = 0;
float lyricScrollFrom = 0;

// Album cover
uint16_t* coverBuf = nullptr;
int coverW = 0, coverH = 0;
JpgMode jpgMode = JPG_TO_SCREEN;
int directX = 0, directY = 0, directSize = 0;   // cover drawn straight to the screen
uint32_t hueR[12], hueG[12], hueB[12], hueW[12];
uint32_t avgR, avgG, avgB, avgN;
uint8_t shadowLut[SHADOW * SHADOW + 1];
uint16_t tileOrder[300];

// ================== SMALL HELPERS ==================
int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

float easeOut(float t) {
  t = 1.0f - t;
  return 1.0f - t * t * t;
}

// ================== COLOURS ========================
uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

void splitColor(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
  r = (c >> 8) & 0xF8;  r |= r >> 5;
  g = (c >> 3) & 0xFC;  g |= g >> 6;
  b = (c << 3) & 0xF8;  b |= b >> 5;
}

// Mix two colours. alpha 0 = all bg, 255 = all fg
uint16_t mix565(uint16_t bg, uint16_t fg, uint8_t alpha) {
  int a = alpha;
  int rb = (bg >> 11) & 0x1F, gb = (bg >> 5) & 0x3F, bb = bg & 0x1F;
  int rf = (fg >> 11) & 0x1F, gf = (fg >> 5) & 0x3F, bf = fg & 0x1F;
  int r = rb + (((rf - rb) * a + 127) / 255);
  int g = gb + (((gf - gb) * a + 127) / 255);
  int b = bb + (((bf - bb) * a + 127) / 255);
  return (r << 11) | (g << 5) | b;
}

Palette makePalette(uint8_t r, uint8_t g, uint8_t b) {
  // Normalise brightness so dark and pale covers both give a rich colour
  int hi = max(r, max(g, b));
  if (hi < 16) { r = g = b = 100; hi = 100; }
  float s = 215.0f / hi;
  r = min(255, (int)(r * s));
  g = min(255, (int)(g * s));
  b = min(255, (int)(b * s));
  uint16_t base = rgb(r, g, b);

  Palette p;
  p.top    = mix565(base, BLACK, 100);
  p.bottom = mix565(base, BLACK, 228);
  p.accent = mix565(base, WHITE, 75);
  p.dim    = mix565(p.bottom, WHITE, 175);
  p.track  = mix565(p.bottom, WHITE, 55);
  p.future = mix565(p.bottom, WHITE, 140);
  p.past   = mix565(p.bottom, WHITE, 100);
  return p;
}

Palette makeMessagePalette() {
  Palette p;
  p.top    = rgb(40, 42, 62);
  p.bottom = rgb(10, 10, 16);
  p.accent = rgb(150, 160, 255);
  p.dim    = rgb(165, 165, 180);
  p.track  = rgb(50, 50, 62);
  p.future = rgb(120, 120, 135);
  p.past   = rgb(90, 90, 100);
  return p;
}

// Background gradient. bgPat holds a 2x2 dither pattern so the gradient
// looks smooth instead of showing colour bands.
void setPalette(const Palette& p) {
  static const float thr[2][2] = { { 0.125f, 0.625f }, { 0.875f, 0.375f } };
  pal = p;
  uint8_t r1, g1, b1, r2, g2, b2;
  splitColor(p.top, r1, g1, b1);
  splitColor(p.bottom, r2, g2, b2);
  for (int y = 0; y < SCREEN_H; y++) {
    float t = (float)y / 210.0f;
    if (t > 1) t = 1;
    t = t * t * (3 - 2 * t);
    float r = r1 + (r2 - r1) * t, g = g1 + (g2 - g1) * t, b = b1 + (b2 - b1) * t;
    bgRow[y] = rgb((uint8_t)(r + 0.5f), (uint8_t)(g + 0.5f), (uint8_t)(b + 0.5f));
    for (int k = 0; k < 2; k++) {
      float d = thr[y & 1][k];
      int r5 = min(31, (int)(r * 31 / 255 + d));
      int g6 = min(63, (int)(g * 63 / 255 + d));
      int b5 = min(31, (int)(b * 31 / 255 + d));
      bgPat[y][k] = (r5 << 11) | (g6 << 5) | b5;
    }
  }
}

// ================== TEXT ===========================
// The fonts only have plain English letters, so convert accented letters
// and smart quotes, and drop anything else (for example Telugu or Tamil script).
String toDisplayText(const String& in) {
  static const char latin[] =
    "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs"
    "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
  String out = "";
  size_t i = 0;
  while (i < in.length()) {
    uint8_t c = in[i];
    if (c < 0x80) {
      if (c >= 32 && c < 127) out += (char)c;
      i += 1;
    } else if ((c & 0xE0) == 0xC0 && i + 1 < in.length()) {
      uint16_t cp = ((c & 0x1F) << 6) | (in[i + 1] & 0x3F);
      if (cp >= 0xC0 && cp <= 0xFF) out += latin[cp - 0xC0];
      i += 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < in.length()) {
      uint16_t cp = ((c & 0x0F) << 12) | ((in[i + 1] & 0x3F) << 6) | (in[i + 2] & 0x3F);
      if (cp == 0x2018 || cp == 0x2019) out += '\'';
      else if (cp == 0x201C || cp == 0x201D) out += '"';
      else if (cp == 0x2013 || cp == 0x2014) out += '-';
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      i += 4;
    } else {
      i += 1;
    }
  }
  out.trim();
  while (out.indexOf("  ") >= 0) out.replace("  ", " ");
  return out;
}

const AAGlyph* glyphFor(const AAFont& f, char ch) {
  uint8_t c = (uint8_t)ch;
  if (c < f.first || c > f.last) c = '?';
  return &f.glyphs[c - f.first];
}

int textWidth(const AAFont& f, const String& s) {
  int32_t w16 = 0;
  for (size_t i = 0; i < s.length(); i++) w16 += glyphFor(f, s[i])->advance16;
  return (w16 + 8) / 16;
}

// Shorten text with "..." so it fits maxW pixels
String fitText(const AAFont& f, const String& s, int maxW) {
  if (textWidth(f, s) <= maxW) return s;
  int32_t dots16 = glyphFor(f, '.')->advance16 * 3;
  int32_t w16 = 0;
  int cut = 0;
  for (size_t i = 0; i < s.length(); i++) {
    w16 += glyphFor(f, s[i])->advance16;
    if ((w16 + dots16 + 8) / 16 > maxW) break;
    cut = i + 1;
  }
  String out = s.substring(0, cut);
  out.trim();
  return out + "...";
}

// Split text into at most 2 rows that fit maxW. Returns the number of rows.
int wrapText(const AAFont& f, const String& s, int maxW, String rows[2]) {
  if (textWidth(f, s) <= maxW) {
    rows[0] = s;
    return 1;
  }
  int32_t w16 = 0;
  int lastSpace = -1, lastFit = 0;
  for (size_t i = 0; i < s.length(); i++) {
    if (s[i] == ' ' && (w16 + 8) / 16 <= maxW) lastSpace = i;
    w16 += glyphFor(f, s[i])->advance16;
    if ((w16 + 8) / 16 <= maxW) lastFit = i + 1;
    else if (lastSpace >= 0) break;
  }
  String rest;
  if (lastSpace > 0) {
    rows[0] = s.substring(0, lastSpace);
    rest = s.substring(lastSpace + 1);
  } else {
    rows[0] = s.substring(0, lastFit);
    rest = s.substring(lastFit);
  }
  rest.trim();
  if (rest.startsWith("- ")) rest = rest.substring(2);   // "Song - From X" reads better as "Song / From X"
  rows[0].trim();
  if (rows[0].length() > 2 && rows[0].substring(rows[0].length() - 2) == String(" -")) rows[0] = rows[0].substring(0, rows[0].length() - 2);
  rows[1] = fitText(f, rest, maxW);
  return 2;
}

// ================== DRAWING ENGINE =================
// Draws a rectangle of the screen strip by strip: background first, then
// whatever paint() adds, then the finished strip goes to the display in one
// fast transfer. Nothing flickers because each pixel is only sent once.
void renderRect(int x, int y, int w, int h, PaintFn paint) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0 || !strip) return;

  int rows = STRIP_PIXELS / w;
  for (int sy = y; sy < y + h; sy += rows) {
    int sh = min(rows, y + h - sy);
    Canvas c = { strip, x, sy, w, sh };
    for (int r = 0; r < sh; r++) {
      const uint16_t* pat = bgPat[sy + r];
      uint16_t* row = strip + r * w;
      for (int i = 0; i < w; i++) row[i] = pat[(x + i) & 1];
    }
    paint(c);
    tft.drawRGBBitmap(x, sy, strip, w, sh);
  }
}

void cvRect(Canvas& c, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
  int x0 = max(x, c.x), y0 = max(y, c.y);
  int x1 = min(x + w, c.x + c.w), y1 = min(y + h, c.y + c.h);
  for (int yy = y0; yy < y1; yy++) {
    uint16_t* p = c.buf + (yy - c.y) * c.w + (x0 - c.x);
    for (int xx = x0; xx < x1; xx++, p++) {
      *p = (alpha == 255) ? color : mix565(*p, color, alpha);
    }
  }
}

void cvCircle(Canvas& c, float cx, float cy, float r, uint16_t color, uint8_t alpha) {
  int x0 = max((int)floorf(cx - r - 1), c.x), x1 = min((int)ceilf(cx + r + 1), c.x + c.w);
  int y0 = max((int)floorf(cy - r - 1), c.y), y1 = min((int)ceilf(cy + r + 1), c.y + c.h);
  for (int yy = y0; yy < y1; yy++) {
    for (int xx = x0; xx < x1; xx++) {
      float dx = xx + 0.5f - cx, dy = yy + 0.5f - cy;
      float cov = r - sqrtf(dx * dx + dy * dy) + 0.5f;
      if (cov <= 0) continue;
      if (cov > 1) cov = 1;
      uint16_t* p = c.buf + (yy - c.y) * c.w + (xx - c.x);
      *p = mix565(*p, color, (uint8_t)(cov * alpha));
    }
  }
}

// Rounded bar
void cvPill(Canvas& c, int x, int y, int w, int h, uint16_t color, uint8_t alpha) {
  float r = h / 2.0f;
  if (w < h) w = h;
  cvCircle(c, x + r, y + r, r, color, alpha);
  cvCircle(c, x + w - r, y + r, r, color, alpha);
  cvRect(c, x + (int)r, y, w - 2 * (int)r, h, color, alpha);
}

// Anti-aliased text. tracking16 adds extra letter spacing in 1/16 pixels.
void cvTextTracked(Canvas& c, const AAFont& f, const String& s, int x, int baseline,
                   uint16_t color, uint8_t opacity, int tracking16) {
  if (opacity == 0) return;
  if (baseline - f.ascent >= c.y + c.h || baseline + f.descent < c.y) return;
  int32_t pen16 = x * 16;
  for (size_t i = 0; i < s.length(); i++) {
    const AAGlyph* g = glyphFor(f, s[i]);
    int gx = (pen16 + 8) / 16 + g->xOffset;
    int gy = baseline + g->yOffset;
    pen16 += g->advance16 + tracking16;
    if (gx >= c.x + c.w) break;
    if (gx + g->width <= c.x || g->width == 0) continue;
    const uint8_t* bits = f.bitmap + g->offset;
    for (int row = 0; row < g->height; row++) {
      int py = gy + row;
      if (py < c.y || py >= c.y + c.h) continue;
      uint16_t* line = c.buf + (py - c.y) * c.w;
      for (int col = 0; col < g->width; col++) {
        int px = gx + col;
        if (px < c.x || px >= c.x + c.w) continue;
        int k = row * g->width + col;
        uint8_t nib = (k & 1) ? (bits[k >> 1] & 0x0F) : (bits[k >> 1] >> 4);
        if (!nib) continue;
        uint8_t a = (uint8_t)((nib * 17 * opacity) / 255);
        line[px - c.x] = mix565(line[px - c.x], color, a);
      }
    }
  }
}

void cvText(Canvas& c, const AAFont& f, const String& s, int x, int baseline, uint16_t color, uint8_t opacity) {
  cvTextTracked(c, f, s, x, baseline, color, opacity, 0);
}

void cvTextCentered(Canvas& c, const AAFont& f, const String& s, int cx, int baseline, uint16_t color, uint8_t opacity) {
  cvText(c, f, s, cx - textWidth(f, s) / 2, baseline, color, opacity);
}

// ================== SCREEN PARTS ===================
long currentProgress() {
  long p = progressMs;
  if (playing) p += (long)(millis() - progressAt);
  if (durationMs > 0 && p > durationMs) p = durationMs;
  if (p < 0) p = 0;
  return p;
}

String formatTime(long ms) {
  long s = ms / 1000;
  char buf[12];
  snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
  return String(buf);
}

int barPixels(long p) {
  if (durationMs <= 0) return 0;
  return clampi((int)((long long)BAR_W * p / durationMs), 0, BAR_W);
}

void paintProgress(Canvas& c, long p, uint8_t op) {
  int filled = barPixels(p);
  cvPill(c, BAR_X, BAR_Y, BAR_W, BAR_H, pal.track, op);
  uint16_t fillColor = playing ? WHITE : pal.dim;
  if (filled > 0) cvPill(c, BAR_X, BAR_Y, max(filled, BAR_H), BAR_H, fillColor, op);
  cvCircle(c, BAR_X + filled, BAR_Y + BAR_H / 2.0f, 5.0f, fillColor, op);
  cvText(c, FONT_SMALL, formatTime(p), BAR_X, TIME_BASELINE, pal.dim, op);
  String total = formatTime(durationMs);
  cvText(c, FONT_SMALL, total, BAR_X + BAR_W - textWidth(FONT_SMALL, total), TIME_BASELINE, pal.dim, op);
}

void drawProgress(long p, uint8_t op) {
  renderRect(PROG_RX, PROG_RY, PROG_RW, PROG_RH, [&](Canvas& c) { paintProgress(c, p, op); });
  lastBarPx = barPixels(p);
  lastSecShown = p / 1000;
}

// Equaliser bars + "NOW PLAYING" / "PAUSED"
void paintStatus(Canvas& c, int x, int y, uint8_t op) {
  for (int i = 0; i < 4; i++) {
    int h = (int)(eqLevel[i] + 0.5f);
    cvRect(c, x + i * 5, y + 16 - h, 3, h, pal.accent, op);
  }
  cvTextTracked(c, FONT_LABEL, playing ? "NOW PLAYING" : "PAUSED", x + 27, y + 14, pal.dim, op, 16);
}

void drawStatus(uint8_t op) {
  int x = (layout == LAYOUT_COVER) ? CL_PANEL_X : LL_TEXT_X;
  int y = (layout == LAYOUT_COVER) ? CL_STATUS_Y : LL_STATUS_Y;
  int w = (layout == LAYOUT_COVER) ? CL_PANEL_W : LL_TEXT_W;
  renderRect(x, y, w, 18, [&](Canvas& c) { paintStatus(c, x, y, op); });
}

void updateEq() {
  static const float sp1[4] = { 7.1f, 9.3f, 6.2f, 8.4f };
  static const float ph1[4] = { 0.0f, 1.7f, 3.1f, 4.4f };
  static const float sp2[4] = { 2.3f, 1.7f, 2.9f, 1.3f };
  static const float ph2[4] = { 0.5f, 2.2f, 1.1f, 3.6f };
  float t = millis() / 1000.0f;
  for (int i = 0; i < 4; i++) {
    float target = 3;
    if (playing) {
      float a = 0.5f + 0.5f * sinf(t * sp1[i] + ph1[i]);
      float b = 0.55f + 0.45f * sinf(t * sp2[i] + ph2[i]);
      target = 3 + 11 * a * b;
    }
    eqLevel[i] += (target - eqLevel[i]) * 0.35f;
  }
}

// Title, artist and up next - layout without lyrics
void paintCoverPanel(Canvas& c, int dx, uint8_t op) {
  int x = CL_PANEL_X;
  String rows[2];
  int n = wrapText(FONT_TITLE, dispTitle, CL_PANEL_W, rows);
  int base = CL_TEXT_Y + 19;
  for (int i = 0; i < n; i++) cvText(c, FONT_TITLE, rows[i], x + dx, base + i * 23, WHITE, op);
  int artistBase = base + (n - 1) * 23 + 22;
  if (n == 1) {
    // Short title: the artist line may use two rows
    String ar[2];
    int an = wrapText(FONT_BODY, dispArtist, CL_PANEL_W, ar);
    for (int i = 0; i < an; i++) cvText(c, FONT_BODY, ar[i], x + dx * 13 / 10, artistBase + i * 18, pal.dim, op);
  } else {
    cvText(c, FONT_BODY, fitText(FONT_BODY, dispArtist, CL_PANEL_W), x + dx * 13 / 10, artistBase, pal.dim, op);
  }

  if (dispUpTitle.length() > 0) {
    int ub = CL_TEXT_Y + 96;
    int ux = x + dx * 16 / 10;
    cvTextTracked(c, FONT_LABEL, "UP NEXT", ux, ub, pal.accent, op, 16);
    cvText(c, FONT_SMALL, fitText(FONT_SMALL, dispUpTitle, CL_PANEL_W), ux, ub + 17, mix565(pal.bottom, WHITE, 230), op);
    cvText(c, FONT_SMALL, fitText(FONT_SMALL, dispUpArtist, CL_PANEL_W), ux, ub + 32, pal.dim, op);
  }
}

// Title, artist and up next - layout with lyrics
void paintLyricsHeader(Canvas& c, int dx, uint8_t op) {
  int x = LL_TEXT_X;
  cvText(c, FONT_TITLE, fitText(FONT_TITLE, dispTitle, LL_TEXT_W), x + dx, LL_HEAD_Y + 18, WHITE, op);
  cvText(c, FONT_BODY, fitText(FONT_BODY, dispArtist, LL_TEXT_W), x + dx * 13 / 10, LL_HEAD_Y + 37, pal.dim, op);
  if (dispUpTitle.length() > 0) {
    int ux = x + dx * 16 / 10;
    int ub = LL_HEAD_Y + 55;
    cvTextTracked(c, FONT_LABEL, "NEXT", ux, ub, pal.accent, op, 16);
    int lw = textWidth(FONT_LABEL, "NEXT") + 12;
    String next = dispUpTitle;
    if (dispUpArtist.length() > 0) next += String("  \x7f  ") + dispUpArtist;
    cvText(c, FONT_SMALL, fitText(FONT_SMALL, next, LL_TEXT_W - lw), ux + lw, ub, pal.dim, op);
  }
}

void drawTextBlock(int dx, uint8_t op) {
  if (layout == LAYOUT_COVER) {
    renderRect(CL_PANEL_X - 2, CL_TEXT_Y, CL_PANEL_W + 8, CL_TEXT_H, [&](Canvas& c) { paintCoverPanel(c, dx, op); });
  } else {
    renderRect(LL_TEXT_X - 2, LL_HEAD_Y, LL_TEXT_W + 8, LL_HEAD_H, [&](Canvas& c) { paintLyricsHeader(c, dx, op); });
  }
}

// Album cover with rounded corners and a soft shadow
uint8_t cornerAlpha(int dx, int dy, int size, int radius) {
  int cx = -1, cy = -1;
  if (dx < radius) cx = radius; else if (dx >= size - radius) cx = size - radius;
  if (dy < radius) cy = radius; else if (dy >= size - radius) cy = size - radius;
  if (cx < 0 || cy < 0) return 255;
  float px = dx + 0.5f - (dx < radius ? radius : (size - radius));
  float py = dy + 0.5f - (dy < radius ? radius : (size - radius));
  float cov = radius - sqrtf(px * px + py * py) + 0.5f;
  if (cov <= 0) return 0;
  if (cov >= 1) return 255;
  return (uint8_t)(cov * 255);
}

int coverRadius(int size) {
  return size >= 120 ? 7 : 5;
}

// Soft shadow, shifted slightly down
void paintShadow(Canvas& c, int x, int y, int size, uint8_t op) {
  int sy = y + 4;
  int x0 = max(x - SHADOW, c.x), x1 = min(x + size + SHADOW, c.x + c.w);
  int y0 = max(sy - SHADOW, c.y), y1 = min(sy + size + SHADOW, c.y + c.h);
  for (int py = y0; py < y1; py++) {
    int oy = py < sy ? sy - py : (py >= sy + size ? py - (sy + size - 1) : 0);
    uint16_t* line = c.buf + (py - c.y) * c.w;
    for (int px = x0; px < x1; px++) {
      int ox = px < x ? x - px : (px >= x + size ? px - (x + size - 1) : 0);
      int d2 = ox * ox + oy * oy;
      if (d2 == 0 && py >= y && py < y + size) continue;   // under the cover
      if (d2 > SHADOW * SHADOW) continue;
      uint8_t a = (uint8_t)(shadowLut[d2] * op / 255);
      line[px - c.x] = mix565(line[px - c.x], BLACK, a);
    }
  }
}

// Cover pixel at (dx, dy) of a size x size square. The decoded cover may be
// smaller (the 64 px low-memory cover) or a bit larger, so it's scaled smoothly.
uint16_t coverPixel(int dx, int dy, int size) {
  if (coverW == size && coverH == size) return coverBuf[dy * coverW + dx];
  // Sample position in 1/256 pixels
  int fx = max(0, (int)(((2 * dx + 1) * coverW * 128) / size) - 128);
  int fy = max(0, (int)(((2 * dy + 1) * coverH * 128) / size) - 128);
  int x0 = fx >> 8, y0 = fy >> 8, ax = fx & 255, ay = fy & 255;
  if (x0 >= coverW - 1) { x0 = coverW - 1; ax = 0; }
  if (y0 >= coverH - 1) { y0 = coverH - 1; ay = 0; }
  int x1 = min(x0 + 1, coverW - 1), y1 = min(y0 + 1, coverH - 1);
  uint16_t top = mix565(coverBuf[y0 * coverW + x0], coverBuf[y0 * coverW + x1], ax);
  uint16_t bot = mix565(coverBuf[y1 * coverW + x0], coverBuf[y1 * coverW + x1], ax);
  return mix565(top, bot, ay);
}

void paintCover(Canvas& c, int x, int y, int size, uint8_t op) {
  int radius = coverRadius(size);
  paintShadow(c, x, y, size, op);

  // The cover itself
  uint16_t placeholder = mix565(pal.top, WHITE, 40);
  int cy0 = max(y, c.y), cy1 = min(y + size, c.y + c.h);
  int cx0 = max(x, c.x), cx1 = min(x + size, c.x + c.w);
  for (int py = cy0; py < cy1; py++) {
    uint16_t* line = c.buf + (py - c.y) * c.w;
    for (int px = cx0; px < cx1; px++) {
      int dx = px - x, dy = py - y;
      uint8_t a = cornerAlpha(dx, dy, size, radius);
      if (!a) continue;
      a = (uint8_t)(a * op / 255);
      uint16_t col = coverBuf ? coverPixel(dx, dy, size) : placeholder;
      line[px - c.x] = mix565(line[px - c.x], col, a);
    }
  }

  // Music note on the placeholder
  if (!coverBuf) {
    uint16_t noteColor = mix565(placeholder, WHITE, 120);
    float r = size * 0.11f;
    float nx = x + size * 0.42f, ny = y + size * 0.66f;
    cvCircle(c, nx, ny, r, noteColor, op);
    cvRect(c, (int)(nx + r - 3), (int)(y + size * 0.26f), 3, (int)(size * 0.40f), noteColor, op);
    cvRect(c, (int)(nx + r - 3), (int)(y + size * 0.26f), (int)(size * 0.16f), 3, noteColor, op);
  }
}

// One lyric line. Returns its height.
int lyricBlockHeight(int i, bool active) {
  const AAFont& f = active ? FONT_LYRIC_ON : FONT_LYRIC;
  String text = track->lyrics[i].text;
  if (text.length() == 0) return active ? 22 : 19;
  String rows[2];
  int n = wrapText(f, text, LYR_TEXT_W, rows);
  return n * (active ? 22 : 19);
}

int paintLyricLine(Canvas& c, int i, float top, bool active, float anim, uint8_t op) {
  const AAFont& f = active ? FONT_LYRIC_ON : FONT_LYRIC;
  int rowH = active ? 22 : 19;
  int baseOff = active ? 16 : 14;
  String text = track->lyrics[i].text;
  if (text.length() == 0) text = "\x7f \x7f \x7f";
  String rows[2];
  int n = wrapText(f, text, LYR_TEXT_W, rows);

  // Dim lines are tinted from the background right behind them so they stay readable
  uint16_t rowBg = bgRow[clampi((int)(top + baseOff - 6), 0, SCREEN_H - 1)];
  uint16_t futureC = mix565(rowBg, WHITE, 150);
  uint16_t pastC = mix565(rowBg, WHITE, 105);
  uint16_t color;
  if (active) color = (lyricAnim && i == lyricIdx) ? mix565(futureC, WHITE, (uint8_t)(anim * 255)) : WHITE;
  else if (i < lyricIdx) color = (lyricAnim && i == lyricPrevIdx) ? mix565(WHITE, pastC, (uint8_t)(anim * 255)) : pastC;
  else color = futureC;

  for (int r = 0; r < n; r++) {
    int base = (int)(top + baseOff + r * rowH + 0.5f);
    // Fade lines out near the top and bottom edges of the lyrics area
    float ft = (base - 11 - LYR_Y) / 7.0f;
    float fb = (LYR_Y + LYR_H - (base + 3)) / 7.0f;
    float fade = min(ft, fb);
    if (fade <= 0) continue;
    if (fade > 1) fade = 1;
    cvText(c, f, rows[r], LYR_TEXT_X, base, color, (uint8_t)(fade * op));
  }
  return n * rowH;
}

void paintLyrics(Canvas& c, float scroll, float anim, uint8_t op) {
  if (!track || track->lyrics.empty()) return;
  int n = track->lyrics.size();
  bool hasActive = lyricIdx >= 0;
  int ci = hasActive ? lyricIdx : 0;
  float top = LYR_ACTIVE_TOP + scroll;

  float y = top;
  for (int i = ci; i < n && y < LYR_Y + LYR_H; i++) {
    y += paintLyricLine(c, i, y, hasActive && i == ci, anim, op) + LYR_GAP;
  }
  y = top;
  for (int i = ci - 1; i >= 0 && y > LYR_Y; i--) {
    y -= lyricBlockHeight(i, false) + LYR_GAP;
    paintLyricLine(c, i, y, false, anim, op);
  }
}

void drawLyrics(float scroll, float anim, uint8_t op) {
  renderRect(LYR_X, LYR_Y, LYR_W, LYR_H, [&](Canvas& c) { paintLyrics(c, scroll, anim, op); });
}

int findLyricIndex(long p) {
  if (!track || track->lyrics.empty()) return -1;
  uint32_t t = (uint32_t)max(0L, p + LYRICS_OFFSET_MS);
  int lo = 0, hi = track->lyrics.size() - 1, found = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (track->lyrics[mid].ms <= t) { found = mid; lo = mid + 1; }
    else hi = mid - 1;
  }
  return found;
}

void updateLyrics(long p, uint32_t now) {
  int idx = findLyricIndex(p);
  if (idx != lyricIdx) {
    if (lyricIdx >= 0 && idx == lyricIdx + 1) {
      lyricScrollFrom = lyricBlockHeight(lyricIdx, true) + LYR_GAP;
      lyricPrevIdx = lyricIdx;
      lyricAnim = true;
      lyricAnimStart = now;
      lyricIdx = idx;
    } else {
      lyricAnim = false;
      lyricIdx = idx;
      drawLyrics(0, 1, 255);
    }
  }
  if (lyricAnim && now - lastLyricFrame >= 16) {
    lastLyricFrame = now;
    float t = (now - lyricAnimStart) / 380.0f;
    if (t >= 1) { t = 1; lyricAnim = false; }
    float e = easeOut(t);
    drawLyrics(lyricScrollFrom * (1 - e), e, 255);
  }
}

// ================== JPEG ===========================
bool jpgOutput(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (jpgMode == JPG_MEASURE) {
    // Find the cover's most prominent colourful hue
    for (uint32_t i = 0; i < (uint32_t)w * h; i++) {
      uint8_t r, g, b;
      splitColor(bitmap[i], r, g, b);
      avgR += r; avgG += g; avgB += b; avgN++;
      int hi = max(r, max(g, b)), lo = min(r, min(g, b)), sat = hi - lo;
      if (sat < 28 || hi < 50) continue;
      int hue;
      if (hi == r) hue = 60 * (g - b) / sat;
      else if (hi == g) hue = 120 + 60 * (b - r) / sat;
      else hue = 240 + 60 * (r - g) / sat;
      if (hue < 0) hue += 360;
      int k = (hue / 30) % 12;
      uint32_t wgt = (uint32_t)sat * hi / 128;
      hueR[k] += r * wgt; hueG[k] += g * wgt; hueB[k] += b * wgt; hueW[k] += wgt;
    }
    return true;
  }
  if (jpgMode == JPG_TO_BUFFER) {
    for (int j = 0; j < h; j++) {
      int yy = y + j;
      if (yy >= coverH) break;
      for (int i = 0; i < w; i++) {
        int xx = x + i;
        if (xx >= coverW) break;
        coverBuf[yy * coverW + xx] = bitmap[j * w + i];
      }
    }
    return true;
  }

  // Straight to the screen (not enough memory for the fade): round the
  // corners here by blending them into the background
  if (y >= SCREEN_H) return false;
  int radius = coverRadius(directSize);
  for (int j = 0; j < h; j++) {
    int sy = y + j;
    if (sy < 0 || sy >= SCREEN_H) continue;
    for (int i = 0; i < w; i++) {
      int sx = x + i;
      int dx = sx - directX, dy = sy - directY;
      uint16_t bg = bgPat[sy][sx & 1];
      uint16_t& px = bitmap[j * w + i];
      if (dx >= directSize || dy >= directSize) { px = bg; continue; }
      uint8_t a = cornerAlpha(dx, dy, directSize, radius);
      if (a < 255) px = mix565(bg, px, a);
    }
  }
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  return true;
}

Palette paletteFromCover(TrackData* t) {
  if (!t->jpg) return makePalette(90, 100, 135);
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, t->jpg, t->jpgSize);
  if (w == 0) return makePalette(90, 100, 135);
  for (int k = 0; k < 12; k++) hueR[k] = hueG[k] = hueB[k] = hueW[k] = 0;
  avgR = avgG = avgB = avgN = 0;
  TJpgDec.setJpgScale(w >= 128 ? 8 : 1);   // the 64 px cover is already tiny
  jpgMode = JPG_MEASURE;
  TJpgDec.drawJpg(0, 0, t->jpg, t->jpgSize);

  int best = 0;
  for (int k = 1; k < 12; k++) if (hueW[k] > hueW[best]) best = k;
  if (hueW[best] > avgN * 4 && hueW[best] > 0) {
    return makePalette(hueR[best] / hueW[best], hueG[best] / hueW[best], hueB[best] / hueW[best]);
  }
  if (avgN == 0) return makePalette(90, 100, 135);
  return makePalette(avgR / avgN, avgG / avgN, avgB / avgN);
}

void freeCover() {
  if (coverBuf) free(coverBuf);
  coverBuf = nullptr;
  coverW = coverH = 0;
}

// Largest JPEG scale (1, 2, 4, 8) that still gives at least the target size
uint8_t jpgScaleFor(uint16_t w, int target) {
  uint8_t scale = 1;
  while (w / (scale * 2) >= target && scale < 8) scale *= 2;
  return scale;
}

// Decode the cover into memory, at or a bit above the target size
bool decodeCover(TrackData* t, int target) {
  freeCover();
  if (!t->jpg) return false;
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, t->jpg, t->jpgSize);
  if (w == 0 || h == 0) return false;
  uint8_t scale = jpgScaleFor(w, target);
  coverW = w / scale;
  coverH = h / scale;
  coverBuf = (uint16_t*)malloc(coverW * coverH * 2);
  if (!coverBuf) {
    Serial.printf("No memory for cover fade (%d bytes, largest free block %u)\n",
                  coverW * coverH * 2, (unsigned)ESP.getMaxAllocHeap());
    coverW = coverH = 0;
    return false;
  }
  TJpgDec.setJpgScale(scale);
  jpgMode = JPG_TO_BUFFER;
  TJpgDec.drawJpg(0, 0, t->jpg, t->jpgSize);
  return true;
}

// ================== SCREENS & TRANSITIONS ==========
void buildTables() {
  // Shadow strength by squared distance from the cover edge
  for (int d2 = 0; d2 <= SHADOW * SHADOW; d2++) {
    float f = 1.0f - sqrtf((float)d2) / SHADOW;
    shadowLut[d2] = (uint8_t)(125 * f * f);
  }
  // Order of tiles for the dissolve: a diagonal sweep with some randomness
  uint16_t keys[300];
  uint32_t seed = 2463534242UL;
  for (int i = 0; i < 300; i++) {
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    int tx = i % 20, ty = i / 20;
    keys[i] = (tx + ty) * 8 + (seed % 24);
    tileOrder[i] = i;
  }
  for (int i = 1; i < 300; i++) {
    uint16_t k = keys[i], v = tileOrder[i];
    int j = i - 1;
    while (j >= 0 && keys[j] > k) { keys[j + 1] = keys[j]; tileOrder[j + 1] = tileOrder[j]; j--; }
    keys[j + 1] = k; tileOrder[j + 1] = v;
  }
}

// Replace whatever is on screen with the current background, tile by tile
void dissolveToBackground() {
  for (int n = 0; n < 300; n++) {
    int t = tileOrder[n];
    int x = (t % 20) * 16, y = (t / 20) * 16;
    for (int r = 0; r < 16; r++) {
      const uint16_t* pat = bgPat[y + r];
      for (int i = 0; i < 16; i++) strip[r * 16 + i] = pat[i & 1];
    }
    tft.drawRGBBitmap(x, y, strip, 16, 16);
    if (n % 15 == 14) delay(12);
  }
}

// Smoothly change the background from one palette to another
void crossfadeBackground(const Palette& to) {
  uint16_t oldRow[SCREEN_H];
  memcpy(oldRow, bgRow, sizeof(oldRow));
  setPalette(to);
  for (int f = 1; f <= 9; f++) {
    uint8_t a = (uint8_t)(easeOut(f / 10.0f) * 255);
    for (int y = 0; y < SCREEN_H; y++) tft.drawFastHLine(0, y, SCREEN_W, mix565(oldRow[y], bgRow[y], a));
    delay(10);
  }
  renderRect(0, 0, SCREEN_W, SCREEN_H, [](Canvas& c) {});   // final, dithered
}

void showMessageScreen(const String& line1, const String& line2) {
  Serial.println("Screen: " + line1 + " / " + line2);
  delete track;
  track = nullptr;
  setPalette(messagePal);
  dissolveToBackground();
  for (int f = 1; f <= 6; f++) {
    uint8_t op = (uint8_t)(easeOut(f / 6.0f) * 255);
    renderRect(0, 90, SCREEN_W, 64, [&](Canvas& c) {
      cvTextCentered(c, FONT_TITLE, fitText(FONT_TITLE, line1, 296), SCREEN_W / 2, 117, WHITE, op);
      cvTextCentered(c, FONT_BODY, fitText(FONT_BODY, line2, 296), SCREEN_W / 2, 142, pal.dim, op);
    });
    delay(15);
  }
  mode = MODE_MESSAGE;
}

void enterLoading() {
  // The old song isn't shown any more: free its lyrics now so the network
  // task has as much memory as possible for the new cover
  delete track;
  track = nullptr;
  dissolveToBackground();
  mode = MODE_LOADING;
  lastDotsFrame = 0;
}

void drawLoadingDots(uint32_t now) {
  renderRect(136, 110, 48, 20, [&](Canvas& c) {
    for (int i = 0; i < 3; i++) {
      float s = 0.5f + 0.5f * sinf(now / 170.0f - i * 0.9f);
      cvCircle(c, 148 + i * 12, 120, 3.0f + s, WHITE, (uint8_t)(70 + 185 * s));
    }
  });
}

void startTrack(TrackData* t) {
  delete track;
  track = t;
  durationMs = t->durationMs;
  dispTitle = toDisplayText(t->title);
  if (dispTitle.length() == 0) dispTitle = "Now playing";
  dispArtist = toDisplayText(t->artist);
  Serial.printf("Showing: %s - %s (%d lyric lines, free heap %u)\n",
                dispTitle.c_str(), dispArtist.c_str(), (int)t->lyrics.size(), (unsigned)ESP.getFreeHeap());

  // 1. New background colour from the cover
  Palette np = paletteFromCover(t);
  if (mode == MODE_LOADING) {
    crossfadeBackground(np);
  } else {
    setPalette(np);
    dissolveToBackground();
  }

  layout = t->lyrics.empty() ? LAYOUT_COVER : LAYOUT_LYRICS;
  mode = MODE_TRACK;
  lyricAnim = false;
  lyricIdx = findLyricIndex(currentProgress());
  for (int i = 0; i < 4; i++) eqLevel[i] = 3;

  // 2. Cover fades in
  int size = (layout == LAYOUT_COVER) ? CL_COVER : LL_COVER;
  int cx = (layout == LAYOUT_COVER) ? CL_COVER_X : LL_COVER_X;
  int cy = (layout == LAYOUT_COVER) ? CL_COVER_Y : LL_COVER_Y;
  bool decoded = decodeCover(t, size);
  if (!decoded && t->jpg) {
    // Not enough memory for the fade: draw the shadow, then the cover
    // straight to the screen with its corners rounded on the way
    uint16_t w = 0, h = 0;
    TJpgDec.getJpgSize(&w, &h, t->jpg, t->jpgSize);
    uint8_t scale = jpgScaleFor(w, size);
    directX = cx;
    directY = cy;
    directSize = min(w, h) / scale;
    if (directSize == 0) directSize = size;
    renderRect(cx - SHADOW, cy - SHADOW, directSize + 2 * SHADOW, directSize + 2 * SHADOW + 4,
               [&](Canvas& c) { paintShadow(c, cx, cy, directSize, 255); });
    TJpgDec.setJpgScale(scale);
    jpgMode = JPG_TO_SCREEN;
    TJpgDec.drawJpg(cx, cy, t->jpg, t->jpgSize);
  } else {
    // With a decoded cover, or the placeholder when there's no picture
    for (int f = 1; f <= 8; f++) {
      uint8_t op = (uint8_t)(easeOut(f / 8.0f) * 255);
      renderRect(cx - SHADOW, cy - SHADOW, size + 2 * SHADOW, size + 2 * SHADOW + 4,
                 [&](Canvas& c) { paintCover(c, cx, cy, size, op); });
      delay(10);
    }
  }
  freeCover();
  free(t->jpg);
  t->jpg = nullptr;

  // 3. Text slides in, everything else fades in
  for (int f = 1; f <= 9; f++) {
    float e = easeOut(f / 9.0f);
    uint8_t op = (uint8_t)(e * 255);
    updateEq();
    drawTextBlock((int)((1 - e) * 26), op);
    drawStatus(op);
    drawProgress(currentProgress(), op);
    if (layout == LAYOUT_LYRICS) drawLyrics(0, 1, op);
  }
}

void redrawUpNext() {
  drawTextBlock(0, 255);
}

// ================== NETWORK: HELPERS ===============
void lockState() { xSemaphoreTake(stateLock, portMAX_DELAY); }
void unlockState() { xSemaphoreGive(stateLock); }

void postMessage(const String& a, const String& b) {
  preparedId = "";
  lockState();
  bool same = !shared.showTrack && a == lastMsg1 && b == lastMsg2;
  if (!same) {
    shared.msg1 = a;
    shared.msg2 = b;
    shared.msgSeq++;
    shared.showTrack = false;
    shared.loading = false;
  }
  unlockState();
  lastMsg1 = a;
  lastMsg2 = b;
}

String urlEncode(const String& s) {
  const char* hex = "0123456789ABCDEF";
  String out = "";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

// Remove " - From ...", " (feat. ...)" and similar extras from a song title
String cleanTitle(const String& title) {
  String t = title;
  int a = t.indexOf(" - ");
  if (a > 0) t = t.substring(0, a);
  int b = t.indexOf(" (");
  if (b > 0) t = t.substring(0, b);
  t.trim();
  return t;
}

// Reads a reply for ArduinoJson, waiting for slow data instead of giving up
// after a short pause (which showed up as "EmptyInput" / "IncompleteInput")
class NetStream : public Stream {
 public:
  NetStream(Client* c, uint32_t waitMs) : client(c), waitMs(waitMs) {}
  int available() override { return (peeked >= 0 ? 1 : 0) + (pos < len ? len - pos : 0) + client->available(); }
  int read() override {
    if (peeked >= 0) { int c = peeked; peeked = -1; return c; }
    return nextByte();
  }
  int peek() override {
    if (peeked < 0) peeked = nextByte();
    return peeked;
  }
  size_t write(uint8_t) override { return 0; }

 protected:
  virtual int nextByte() { return rawByte(); }

  int rawByte() {
    if (pos < len) return buf[pos++];
    if (!client) return -1;
    uint32_t start = millis();
    for (;;) {
      int n = client->available();
      if (n > 0) {
        int got = client->read(buf, min(n, (int)sizeof(buf)));
        if (got > 0) {
          len = got;
          pos = 0;
          return buf[pos++];
        }
      } else if (!client->connected()) {
        return -1;
      }
      if (millis() - start > waitMs) return -1;
      delay(2);
    }
  }

 private:
  Client* client;
  uint32_t waitMs;
  uint8_t buf[256];
  int pos = 0, len = 0;
  int peeked = -1;
};

// Spotify's queue reply holds up to 20 full songs (often 100+ KB), which
// didn't arrive in one piece. This hands ArduinoJson only the first song of
// the "queue" list and stops there.
class FirstQueueItemStream : public NetStream {
 public:
  using NetStream::NetStream;
  bool queueEmpty = false;

 protected:
  int nextByte() override {
    if (state == 0) return findFirstItem();
    if (state == 2) return -1;
    int c = rawByte();
    if (c < 0) { state = 2; return -1; }
    if (inString) {
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
    } else if (c == '"') {
      inString = true;
    } else if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      if (--depth == 0) state = 2;
    }
    return c;
  }

 private:
  int state = 0;   // 0 = looking for the queue, 1 = inside its first item, 2 = done
  int depth = 0;
  bool inString = false, escaped = false;

  int nextNonSpace() {
    int c;
    do { c = rawByte(); } while (c == ' ' || c == '\n' || c == '\r' || c == '\t');
    return c;
  }

  // Skip to  "queue" : [ {  and return that "{". Only the key is followed
  // by ":", so a song that is itself called "queue" is skipped over.
  int findFirstItem() {
    state = 2;
    const char* key = "\"queue\"";
    int c;
    for (;;) {
      int matched = 0;
      while (key[matched]) {
        c = rawByte();
        if (c < 0) return -1;
        if (c == key[matched]) matched++;
        else matched = (c == key[0]) ? 1 : 0;
      }
      c = nextNonSpace();
      if (c < 0) return -1;
      if (c == ':') break;
    }
    if (nextNonSpace() != '[') return -1;
    c = nextNonSpace();
    if (c == ']') { queueEmpty = true; return -1; }
    if (c != '{') return -1;
    state = 1;
    depth = 1;
    return c;
  }
};

// ================== NETWORK: WI-FI & LOGIN =========
bool connectWiFi() {
  postMessage("Connecting to Wi-Fi", WIFI_SSID);
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);

  Serial.println("Scanning for networks...");
  int count = WiFi.scanNetworks();
  bool found = false;
  for (int i = 0; i < count; i++) {
    Serial.printf("  %-32s signal %d dBm\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i));
    if (WiFi.SSID(i) == WIFI_SSID) found = true;
  }
  WiFi.scanDelete();
  if (!found) {
    Serial.printf("'%s' was NOT found. Check the name, or switch to 2.4 GHz.\n", WIFI_SSID);
    postMessage("Wi-Fi not found", "Check the name, use 2.4 GHz");
    return false;
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Network found but login failed (status %d). Check the password.\n", WiFi.status());
    postMessage("Wi-Fi login failed", "Check the password");
    return false;
  }
  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
  postMessage("Connected", "Waiting for Spotify...");
  return true;
}

void loadRefreshToken() {
  // Spotify sometimes issues a new refresh token. It's saved in flash so the
  // display keeps working after a reboot. Pasting a different token into the
  // sketch makes that one take over again.
  prefs.begin("spotify", false);
  String seed = prefs.getString("seed", "");
  if (seed != REFRESH_TOKEN) {
    prefs.putString("seed", REFRESH_TOKEN);
    prefs.putString("token", REFRESH_TOKEN);
  }
  refreshToken = prefs.getString("token", REFRESH_TOKEN);
}

bool refreshAccessToken() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) return false;
  String auth = base64::encode(String(CLIENT_ID) + ":" + CLIENT_SECRET);
  http.addHeader("Authorization", "Basic " + auth);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  int code = http.POST("grant_type=refresh_token&refresh_token=" + refreshToken);
  String body = http.getString();
  http.end();

  JsonDocument doc;
  deserializeJson(doc, body);
  if (code != 200) {
    Serial.printf("Token refresh failed (%d): %s\n", code, body.c_str());
    String error = doc["error"] | "";
    if (error == "invalid_grant") postMessage("Spotify login expired", "Run get_refresh_token.py again");
    else if (error == "invalid_client") postMessage("Wrong Client ID/Secret", "Check the sketch settings");
    return false;
  }
  accessToken = doc["access_token"] | "";
  long expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAt = millis() + (expiresIn - 60) * 1000UL;
  String newRefresh = doc["refresh_token"] | "";
  if (newRefresh.length() > 0 && newRefresh != refreshToken) {
    refreshToken = newRefresh;
    prefs.putString("token", refreshToken);
    Serial.println("Saved new refresh token");
  }
  Serial.println("Access token refreshed");
  return accessToken.length() > 0;
}

// ================== NETWORK: COVER, QUEUE, LYRICS ==
bool downloadImage(const String& url, uint8_t*& out, size_t& outSize) {
  out = nullptr;
  outSize = 0;
  if (url.length() == 0) return false;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Image download failed: %d\n", code);
    http.end();
    return false;
  }
  int length = http.getSize();
  if (length > 150000) {
    Serial.println("Image too large");
    http.end();
    return false;
  }
  // Check there's one free block big enough before asking for it. Keep some
  // spare so the secure connection still has room to work.
  size_t largest = ESP.getMaxAllocHeap();
  const size_t spare = 6144;
  size_t capacity;
  if (length > 0) {
    capacity = length;
    if (largest < capacity + spare) {
      Serial.printf("Not enough memory for image (%d bytes, largest free block %u)\n", length, (unsigned)largest);
      http.end();
      return false;
    }
  } else {
    capacity = largest > spare ? min((size_t)100000, largest - spare) : 0;
  }
  uint8_t* buffer = capacity >= 1024 ? (uint8_t*)malloc(capacity) : nullptr;
  if (!buffer) {
    Serial.printf("Not enough memory for image (largest free block %u)\n", (unsigned)largest);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  uint32_t lastData = millis();
  while ((http.connected() || stream->available()) && received < capacity && millis() - lastData < 10000) {
    size_t available = stream->available();
    if (available) {
      size_t toRead = min(available, capacity - received);
      received += stream->readBytes(buffer + received, toRead);
      lastData = millis();
    } else {
      if (length > 0 && received >= (size_t)length) break;
      delay(1);
    }
  }
  http.end();
  if (received == 0 || (length > 0 && received != (size_t)length)) {
    Serial.println("Image download incomplete");
    free(buffer);
    return false;
  }
  out = buffer;
  outSize = received;
  return true;
}

bool fetchQueue(String& title, String& artist) {
  title = "";
  artist = "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/queue")) return false;
  http.addHeader("Authorization", "Bearer " + accessToken);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Queue error: %d\n", code);
    http.end();
    return false;
  }
  JsonDocument filter;
  filter["name"] = true;
  filter["artists"][0]["name"] = true;
  filter["show"]["name"] = true;
  JsonDocument doc;
  FirstQueueItemStream body(http.getStreamPtr(), 8000);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();   // the rest of the queue is never downloaded
  if (body.queueEmpty) return true;
  if (err) {
    Serial.printf("Queue JSON error: %s\n", err.c_str());
    return false;
  }
  title = doc["name"] | "";
  const char* a = doc["artists"][0]["name"];
  if (!a) a = doc["show"]["name"];
  artist = a ? a : "";
  return true;
}

// One LRCLIB request, tried twice if the reply goes missing on the way
bool lrclibRequest(const String& path, JsonDocument& filter, JsonDocument& doc) {
  String url = "https://lrclib.net/api/" + path;
  for (int attempt = 1; attempt <= 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.useHTTP10(true);
    http.setConnectTimeout(8000);
    http.setTimeout(15000);   // LRCLIB can take a while when it has to look a song up
    if (!http.begin(client, url)) return false;
    http.setUserAgent("ESP32-NowPlaying/2.1 (personal Spotify display)");
    int code = http.GET();
    Serial.printf("LRCLIB %d  %s\n", code, path.c_str());
    if (code != 200) {
      http.end();
      if (code == 404 || (code >= 400 && code < 500)) return false;   // not there, no point retrying
      continue;
    }
    doc.clear();
    NetStream body(http.getStreamPtr(), 10000);
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    http.end();
    if (!err) return true;
    Serial.printf("LRCLIB JSON error: %s (free heap %u, attempt %d)\n", err.c_str(), (unsigned)ESP.getFreeHeap(), attempt);
  }
  return false;
}

// Count characters the fonts can show, to skip lyrics in other scripts
void countReadable(const String& s, uint32_t& total, uint32_t& readable) {
  size_t i = 0;
  while (i < s.length()) {
    uint8_t c = s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = ((c & 0x1F) << 6) | (i + 1 < s.length() ? (s[i + 1] & 0x3F) : 0); len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = ((c & 0x0F) << 12) | (i + 2 < s.length() ? (((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F)) : 0); len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = 0x10000; len = 4; }
    else { cp = 0xFFFD; len = 1; }
    if (cp != ' ') {
      total++;
      if (cp < 0x80 || (cp >= 0xC0 && cp <= 0xFF) || cp == 0x2018 || cp == 0x2019 ||
          cp == 0x201C || cp == 0x201D || cp == 0x2013 || cp == 0x2014) readable++;
    }
    i += len;
  }
}

bool parseLrc(const char* lrc, std::vector<LyricLine>& out) {
  out.clear();
  if (!lrc) return false;
  String all(lrc);
  uint32_t total = 0, readable = 0;
  int textLines = 0;
  int pos = 0;
  while (pos < (int)all.length()) {
    int nl = all.indexOf('\n', pos);
    if (nl < 0) nl = all.length();
    String line = all.substring(pos, nl);
    pos = nl + 1;
    line.trim();
    uint32_t times[8];
    int nt = 0;
    while (line.startsWith("[")) {
      int close = line.indexOf(']');
      if (close < 0) break;
      String tag = line.substring(1, close);
      line = line.substring(close + 1);
      int colon = tag.indexOf(':');
      if (colon > 0 && isDigit(tag[0]) && nt < 8) {
        times[nt++] = tag.substring(0, colon).toInt() * 60000UL + (uint32_t)(tag.substring(colon + 1).toFloat() * 1000);
      }
    }
    if (nt == 0) continue;
    line.trim();
    if (line.length() > 0) textLines++;
    countReadable(line, total, readable);
    String shown = toDisplayText(line);
    for (int k = 0; k < nt; k++) out.push_back(LyricLine{ times[k], shown });
  }
  if (out.empty() || textLines == 0) { out.clear(); return false; }
  if (total > 0 && readable * 100 < total * 85) {
    Serial.println("Lyrics are in a script the display can't show");
    out.clear();
    return false;
  }
  std::stable_sort(out.begin(), out.end(), [](const LyricLine& a, const LyricLine& b) { return a.ms < b.ms; });
  if (out[0].ms > 2500) out.insert(out.begin(), LyricLine{ 0, "" });
  return true;
}

// Exact LRCLIB lookup. Sets instrumental if LRCLIB says the song has no words.
bool lrclibGet(const String& title, const String& artist, const String& album, long sec,
               std::vector<LyricLine>& out, bool& instrumental) {
  JsonDocument filter;
  filter["syncedLyrics"] = true;
  filter["instrumental"] = true;
  JsonDocument doc;
  String q = "get?track_name=" + urlEncode(title) + "&artist_name=" + urlEncode(artist) +
             "&album_name=" + urlEncode(album) + "&duration=" + String(sec);
  if (!lrclibRequest(q, filter, doc)) return false;
  if (doc["instrumental"] | false) { instrumental = true; return false; }
  return parseLrc(doc["syncedLyrics"], out);
}

bool fetchLyrics(const String& title, const String& artist, const String& album, long durationMs, std::vector<LyricLine>& out) {
  Serial.printf("Looking for lyrics (free heap %u)\n", (unsigned)ESP.getFreeHeap());
  long sec = (durationMs + 500) / 1000;
  bool instrumental = false;

  // 1. Exact match with the full title
  if (lrclibGet(title, artist, album, sec, out, instrumental)) return true;
  if (instrumental) return false;

  // 2. Exact match with the cleaned-up title ("Song - From X" -> "Song")
  String clean = cleanTitle(title);
  if (clean.length() > 0 && clean != title) {
    if (lrclibGet(clean, artist, album, sec, out, instrumental)) return true;
    if (instrumental) return false;
  }

  // 3. Search, and try results with the same length
  JsonDocument searchFilter;
  searchFilter[0]["id"] = true;
  searchFilter[0]["duration"] = true;
  JsonDocument results;
  String sq = "search?track_name=" + urlEncode(clean.length() ? clean : title) + "&artist_name=" + urlEncode(artist);
  if (!lrclibRequest(sq, searchFilter, results)) return false;

  JsonDocument filter;
  filter["syncedLyrics"] = true;
  JsonDocument doc;
  int tries = 0;
  for (JsonVariant r : results.as<JsonArray>()) {
    float d = r["duration"] | 0.0f;
    if (fabsf(d - sec) > 3) continue;
    long id = r["id"] | 0L;
    if (id == 0) continue;
    if (lrclibRequest("get/" + String(id), filter, doc) && parseLrc(doc["syncedLyrics"], out)) return true;
    if (++tries >= 3) break;
  }
  return false;
}

// ================== NETWORK: SPOTIFY ===============
void prepareTrack(const String& id, const String& title, const String& artist, const String& firstArtist,
                  const String& album, const String& imageUrl, const String& smallImageUrl,
                  long duration, bool isEpisode) {
  lockState();
  shared.loading = true;
  unlockState();
  delay(50);   // let the display free the previous song first

  TrackData* t = new TrackData();
  t->id = id;
  t->title = title;
  t->artist = artist;
  t->durationMs = duration;
  if (!downloadImage(imageUrl, t->jpg, t->jpgSize) && smallImageUrl.length() > 0 && smallImageUrl != imageUrl) {
    Serial.println("Using the small cover instead");
    downloadImage(smallImageUrl, t->jpg, t->jpgSize);
  }

  String upT, upA;
  fetchQueue(upT, upA);
  lastQueueFetch = millis();
  lastUpTitle = upT;
  lastUpArtist = upA;

  if (SHOW_LYRICS && !isEpisode) {
    if (fetchLyrics(title, firstArtist, album, duration, t->lyrics)) Serial.printf("Lyrics: %d lines\n", (int)t->lyrics.size());
    else Serial.println("No lyrics found");
  }

  preparedId = id;
  lockState();
  if (shared.ready) delete shared.ready;
  shared.ready = t;
  shared.showTrack = true;
  shared.loading = false;
  shared.upTitle = upT;
  shared.upArtist = upA;
  shared.upSeq++;
  unlockState();
}

void refreshQueue() {
  lastQueueFetch = millis();
  String t, a;
  if (!fetchQueue(t, a)) return;
  if (t == lastUpTitle && a == lastUpArtist) return;
  lastUpTitle = t;
  lastUpArtist = a;
  lockState();
  shared.upTitle = t;
  shared.upArtist = a;
  shared.upSeq++;
  unlockState();
}

uint32_t pollSpotify() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.spotify.com/v1/me/player/currently-playing?additional_types=episode")) return 5000;
  http.addHeader("Authorization", "Bearer " + accessToken);
  const char* headerKeys[] = { "Retry-After" };
  http.collectHeaders(headerKeys, 1);
  int code = http.GET();

  if (code == 204) {
    http.end();
    postMessage("Nothing playing", "Play a song on Spotify");
    return 4000;
  }
  if (code == 401) {
    http.end();
    accessToken = "";
    return 200;
  }
  if (code == 429) {
    long wait = http.header("Retry-After").toInt();
    http.end();
    Serial.printf("Rate limited, waiting %ld s\n", wait);
    return (uint32_t)std::max(wait, 5L) * 1000UL;
  }
  if (code != 200) {
    Serial.printf("Spotify error: %d\n", code);
    http.end();
    return 8000;
  }

  JsonDocument filter;
  filter["is_playing"] = true;
  filter["progress_ms"] = true;
  filter["currently_playing_type"] = true;
  filter["item"]["id"] = true;
  filter["item"]["name"] = true;
  filter["item"]["duration_ms"] = true;
  filter["item"]["artists"][0]["name"] = true;
  filter["item"]["album"]["name"] = true;
  filter["item"]["album"]["images"][0]["url"] = true;
  filter["item"]["album"]["images"][0]["width"] = true;
  filter["item"]["show"]["name"] = true;
  filter["item"]["images"][0]["url"] = true;
  filter["item"]["images"][0]["width"] = true;
  JsonDocument doc;
  // Parse straight from the connection: the full reply is 10+ KB, mostly
  // country lists, and copying it into memory first fragments the heap
  NetStream body(http.getStreamPtr(), 8000);
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return 5000;
  }

  JsonVariant item = doc["item"];
  if (item.isNull()) {   // for example an ad
    postMessage("Nothing playing", "Play a song on Spotify");
    return 4000;
  }

  String type = doc["currently_playing_type"] | "track";
  bool isEpisode = (type == "episode");
  String title = item["name"] | "";
  String artist = "", firstArtist = "", album = "";
  JsonArray images;
  if (isEpisode) {
    artist = item["show"]["name"] | "";
    firstArtist = artist;
    images = item["images"].as<JsonArray>();
  } else {
    for (JsonVariant a : item["artists"].as<JsonArray>()) {
      String name = a["name"] | "";
      if (firstArtist.length() == 0) firstArtist = name;
      if (artist.length() > 0) artist += ", ";
      artist += name;
    }
    album = item["album"]["name"] | "";
    images = item["album"]["images"].as<JsonArray>();
  }
  String id = item["id"] | "";
  if (id.length() == 0) id = title + "|" + artist;

  // Spotify gives 640, 300 and 64 px covers. 300 px suits this screen best;
  // the 64 px one is the backup when memory is short.
  String imageUrl = "", smallUrl = "";
  int smallW = 100000;
  for (JsonVariant img : images) {
    int w = img["width"] | 0;
    String u = img["url"] | "";
    if (w == 300) imageUrl = u;
    if (w > 0 && w < smallW) { smallW = w; smallUrl = u; }
  }
  if (imageUrl.length() == 0 && images.size() > 0) imageUrl = images[0]["url"] | "";

  bool playingNow = doc["is_playing"] | false;
  long progress = doc["progress_ms"] | 0L;
  long duration = item["duration_ms"] | 0L;

  lockState();
  shared.playing = playingNow;
  shared.progressMs = progress;
  shared.progressAt = millis();
  unlockState();

  if (id != preparedId) {
    Serial.println("New song: " + title + " - " + artist);
    prepareTrack(id, title, artist, firstArtist, album, imageUrl, smallUrl, duration, isEpisode);
  } else if (millis() - lastQueueFetch > QUEUE_REFRESH_MS) {
    refreshQueue();
  }

  uint32_t next = POLL_MS;
  if (playingNow && duration > 0) {
    long remaining = duration - progress;
    if (remaining < (long)POLL_MS) next = (uint32_t)std::max(remaining, 0L) + 700;
  }
  return next;
}

uint32_t networkStep() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return 10000;
  }
  if (accessToken.length() == 0 || (int32_t)(millis() - tokenExpiresAt) >= 0) {
    if (!refreshAccessToken()) return 15000;
  }
  return pollSpotify();
}

void networkTask(void* param) {
  for (;;) {
    uint32_t wait = networkStep();
    vTaskDelay(pdMS_TO_TICKS(wait));
  }
}

// ================== MAIN ===========================
void setup() {
  Serial.begin(115200);
  stateLock = xSemaphoreCreateMutex();

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
  tft.init(240, 320);
  tft.setSPISpeed(SPI_SPEED);
  tft.setRotation(1);   // landscape 320 x 240
  uint8_t madctl = SCREEN_MADCTL;
  tft.sendCommand(ST77XX_MADCTL, &madctl, 1);
  tft.invertDisplay(false);

  strip = (uint16_t*)malloc(STRIP_PIXELS * 2);
  buildTables();
  messagePal = makeMessagePalette();
  setPalette(messagePal);

  TJpgDec.setCallback(jpgOutput);
  TJpgDec.setSwapBytes(false);

  showMessageScreen("Spotify Now Playing", "Starting up...");
  loadRefreshToken();

  // Network work runs on the other CPU core so animations never stall
  xTaskCreatePinnedToCore(networkTask, "network", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  // Take a snapshot of what the network task has for us
  TrackData* incoming = nullptr;
  bool wantLoading, showTrack, pl;
  long pm;
  uint32_t pa, mseq, useq;
  String m1, m2, u1, u2;

  lockState();
  incoming = shared.ready;
  shared.ready = nullptr;
  wantLoading = shared.loading;
  showTrack = shared.showTrack;
  mseq = shared.msgSeq;
  if (mseq != shownMsgSeq) { m1 = shared.msg1; m2 = shared.msg2; }
  useq = shared.upSeq;
  if (useq != shownUpSeq) { u1 = shared.upTitle; u2 = shared.upArtist; }
  pl = shared.playing;
  pm = shared.progressMs;
  pa = shared.progressAt;
  unlockState();

  playing = pl;
  progressMs = pm;
  progressAt = pa;

  if (useq != shownUpSeq) {
    shownUpSeq = useq;
    dispUpTitle = toDisplayText(u1);
    dispUpArtist = toDisplayText(u2);
    if (mode == MODE_TRACK && !incoming) redrawUpNext();
  }

  if (incoming) {
    startTrack(incoming);
    return;
  }
  if (!showTrack && mseq != shownMsgSeq) {
    shownMsgSeq = mseq;
    showMessageScreen(m1, m2);
    return;
  }
  if (wantLoading && (mode == MODE_TRACK || mode == MODE_MESSAGE)) enterLoading();

  uint32_t now = millis();
  if (mode == MODE_LOADING && now - lastDotsFrame >= 40) {
    lastDotsFrame = now;
    drawLoadingDots(now);
  }

  if (mode == MODE_TRACK) {
    static bool lastPlaying = false;
    bool playChanged = (playing != lastPlaying);
    lastPlaying = playing;

    if (now - lastEqFrame >= 33) {
      lastEqFrame = now;
      updateEq();
      drawStatus(255);
    }
    long p = currentProgress();
    if (playChanged || now - lastProgressDraw >= 200) {
      lastProgressDraw = now;
      if (playChanged || barPixels(p) != lastBarPx || p / 1000 != lastSecShown) drawProgress(p, 255);
    }
    if (layout == LAYOUT_LYRICS) updateLyrics(p, now);
  }
  delay(2);
}
