// VOICE ASSISTANT - UI v3 ("Midnight Gradient" theme)
// Gradient background, chat bubbles, live mic meter, "thinking" animation, equalizer.
// Hold K0 (or BOOT) -> speak -> release. Knob = volume, knob PUSH = new conversation.
// Needs the fonts.cpp tab (#include <U8g2lib.h>) for the Hebrew font.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>
#include <Arduino_GFX_Library.h>

// ---- Your private settings (fill in locally - NEVER commit real values) ----
const char* WIFI_SSID  = "YOUR_WIFI_NAME";
const char* WIFI_PASS  = "YOUR_WIFI_PASSWORD";
const char* OPENAI_KEY = "sk-PASTE_YOUR_KEY_HERE";

// ---- Models ----
const char* STT_MODEL = "gpt-4o-mini-transcribe";  // speech -> text
const char* LLM_MODEL = "gpt-6-luna";              // the "brain"
const char* TTS_MODEL = "gpt-4o-mini-tts";         // text -> speech
const char* TTS_VOICE = "coral";                   // try: alloy, ash, coral, nova, sage...

// ---- Assistant personality ----
const char* SYSTEM_PROMPT =
  "You are a friendly voice assistant running on a small electronic device with a small screen. "
  "Always answer in the same language the user spoke (Hebrew or English). "
  "Keep answers very short: 1 to 2 short sentences. No lists, no markdown, no emojis, "
  "because your answer will be read aloud. "
  "If the question needs current information (news, weather, prices, sports), use web search. "
  "Never read URLs, links or source names aloud.";

// ---- Audio pins ----
#define MIC_SCK  17
#define MIC_WS   16
#define MIC_SD   18
#define MIC_LR   15
#define AMP_BCLK 11
#define AMP_LRC  12
#define AMP_DIN  10
#define AMP_SD   13      // amp on/off: HIGH = on, LOW = off (no hiss)
#define BTN       0      // BOOT button (talk)
#define RGB_PIN  48

// ---- Display + controls pins ----
#define TFT_SCL  4
#define TFT_SDA  5
#define TFT_RES  6
#define TFT_DC   7
#define TFT_CS   1
#define TFT_BLK  2
#define ENC_A    42
#define ENC_B    41
#define ENC_PUSH 40      // knob press = new conversation
#define KEY_K0   39      // talk button

// ---- Audio settings ----
const int   MIC_RATE      = 16000;
const int   AMP_RATE      = 24000;   // OpenAI TTS "pcm" = 24 kHz, 16-bit, mono
const int   MAX_SEC       = 8;
const int   MAX_REPLY_SEC = 40;
const int   MIC_SHIFT     = 14;
const float TARGET_PEAK   = 12000;
const float MAX_GAIN      = 8.0;
const float HPF_ALPHA     = 0.970;

// ---- Volume (knob) ----
const int   VOL_STEPS     = 10;      // 0..10
const float VOL_PER_STEP  = 0.10;    // max 1.00
int volStep = 4;                     // start at 0.40

size_t MAX_SAMPLES = MIC_RATE * MAX_SEC;
I2SClass mic, amp;
int16_t *rec;
uint8_t *reply;
size_t   replyCap;
String   lastResponseId;
int16_t  silence[512] = {0};

// ======================= DISPLAY BASICS =======================

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCL, TFT_SDA, GFX_NOT_DEFINED);
// 4th argument = IPS color inversion. This module needs it OFF (true = inverted colors).
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1, false, 240, 320);
extern "C" const uint8_t u8g2_font_unifont_t_hebrew[];

// Two tasks draw on the screen (main loop + animation task on core 0),
// so every drawing function takes this lock first.
SemaphoreHandle_t gfxLock;
#define GFX_LOCK()   xSemaphoreTake(gfxLock, portMAX_DELAY)
#define GFX_UNLOCK() xSemaphoreGive(gfxLock)

// 8-bit RGB -> 16-bit RGB565 (5 bits red, 6 green, 5 blue)
#define RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// ---- "Midnight Gradient" theme ----
#define C_WHITE    RGB565(255, 255, 255)
#define C_LAV      RGB565(170, 160, 230)   // soft lavender (secondary text)
#define C_DIM      RGB565(110, 100, 160)   // dim lavender
#define C_HEAD     RGB565(6, 8, 26)        // header band
#define C_CARD     RGB565(20, 22, 56)      // status card
#define C_CARDLINE RGB565(60, 60, 120)     // card border
#define C_CYAN     RGB565(0, 220, 255)
#define C_PINK     RGB565(255, 80, 200)
#define C_BLUE     RGB565(90, 150, 255)
#define C_YELLOW   RGB565(255, 210, 60)
#define C_DYELLOW  RGB565(80, 66, 20)
#define C_GREEN    RGB565(60, 230, 140)
#define C_DGREEN   RGB565(20, 70, 50)
#define C_RED      RGB565(255, 70, 90)
#define C_DRED     RGB565(80, 20, 30)
#define C_QFILL    RGB565(0, 80, 100)      // your question bubble (teal)
#define C_AFILL    RGB565(36, 30, 84)      // assistant answer bubble (indigo)
#define C_ABORDER  RGB565(150, 130, 255)

// Background gradient: top color -> bottom color
const uint8_t BG_TOP[3] = {10, 14, 40};    // midnight blue
const uint8_t BG_BOT[3] = {48, 18, 74};    // deep purple
uint16_t bgRow[240];                       // precomputed color of every row

const int SCR_W   = 320;
const int SCR_H   = 240;
const int CHAR_W  = 8;       // unifont glyphs are 8 px wide
const int LINE_H  = 18;
const int BUB_CHARS = 34;    // max chars per line inside a bubble

// Layout (pixels)
const int Y_HEADER = 0,  H_HEADER = 26;
const int Y_STATUS = 32, H_STATUS = 34;
const int Y_CHAT   = 72, Y_CHAT_END = 210;
const int Y_VOL    = 214, H_VOL   = 26;
// Animation box (inside the status card, right side)
const int AX = 196, AY = 33, AW = 112, AH = 32;

int qBottom = Y_CHAT;        // where the question bubble ends

// NOTE: must be defined BEFORE the first function, because the Arduino IDE
// auto-generates function prototypes above the first function.
struct Wrapped {
  int n;
  uint32_t line[6][BUB_CHARS + 2];
  int len[6];
  bool heb[6];
};
Wrapped wq, wa;              // question / answer (static, not on the stack)

// ---- background helpers ----
void bgInit() {
  for (int y = 0; y < SCR_H; y++) {
    float t = y / (float)(SCR_H - 1);
    uint8_t r = BG_TOP[0] + (BG_BOT[0] - BG_TOP[0]) * t;
    uint8_t g = BG_TOP[1] + (BG_BOT[1] - BG_TOP[1]) * t;
    uint8_t b = BG_TOP[2] + (BG_BOT[2] - BG_TOP[2]) * t;
    bgRow[y] = RGB565(r, g, b);
  }
}

// Repaint a rectangle with the gradient (used instead of "fill with black")
void bgFill(int x, int y, int w, int h) {
  for (int r = y; r < y + h && r < SCR_H; r++) {
    if (r >= 0) gfx->drawFastHLine(x, r, w, bgRow[r]);
  }
}

// Color between cyan (t=0) and pink (t=1), for accents
uint16_t accent(float t) {
  uint8_t r = 0   + (255 - 0)   * t;
  uint8_t g = 220 + (80 - 220)  * t;
  uint8_t b = 255 + (200 - 255) * t;
  return RGB565(r, g, b);
}

// Draw one line of text (caller must hold the lock). yTop = top of the line.
void uiText(int x, int yTop, const String& s, uint16_t color, uint8_t size = 1) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x, yTop + 13 * size);
  gfx->print(s);
}

void uiHeader() {
  GFX_LOCK();
  gfx->fillRect(0, Y_HEADER, SCR_W, H_HEADER, C_HEAD);
  gfx->fillCircle(14, 13, 7, C_CYAN);              // logo: ring
  gfx->fillCircle(14, 13, 4, C_HEAD);
  gfx->fillCircle(14, 13, 2, C_PINK);
  uiText(28, 5, "AI Assistant", C_WHITE);
  struct tm t;
  char buf[8] = "--:--";
  if (getLocalTime(&t, 10)) strftime(buf, sizeof(buf), "%H:%M", &t);
  uiText(SCR_W - 8 - 5 * CHAR_W, 5, buf, C_LAV);
  GFX_UNLOCK();
}

// Full screen: gradient, header, glowing accent line, status card
void uiFrame() {
  GFX_LOCK();
  bgFill(0, 0, SCR_W, SCR_H);
  for (int x = 0; x < SCR_W; x++) {                // cyan -> pink line under header
    uint16_t c = accent(x / (float)(SCR_W - 1));
    gfx->drawPixel(x, H_HEADER, c);
    gfx->drawPixel(x, H_HEADER + 1, c);
  }
  gfx->fillRoundRect(4, Y_STATUS - 1, SCR_W - 8, H_STATUS + 2, 12, C_CARD);
  gfx->drawRoundRect(4, Y_STATUS - 1, SCR_W - 8, H_STATUS + 2, 12, C_CARDLINE);
  GFX_UNLOCK();
}

// Status text on the left part of the card (animation box stays free)
void uiStatus(const char* text, uint16_t color) {
  GFX_LOCK();
  gfx->fillRect(10, Y_STATUS + 1, AX - 14, H_STATUS - 2, C_CARD);
  gfx->fillCircle(22, Y_STATUS + 17, 7, color);
  gfx->drawCircle(22, Y_STATUS + 17, 9, color);   // halo
  uiText(38, Y_STATUS + 1, text, color, 2);
  GFX_UNLOCK();
}

void uiVolume() {
  GFX_LOCK();
  bgFill(0, Y_VOL, SCR_W, H_VOL);
  uiText(8, Y_VOL + 5, "Vol", C_LAV);
  int bx = 44, bw = 260, bh = 12, by = Y_VOL + 7;
  gfx->fillRoundRect(bx, by, bw, bh, 6, C_CARD);
  gfx->drawRoundRect(bx, by, bw, bh, 6, C_CARDLINE);
  int fw = (bw - 6) * volStep / VOL_STEPS;
  for (int i = 0; i < fw; i++) {                   // gradient fill cyan -> pink
    gfx->drawFastVLine(bx + 3 + i, by + 3, bh - 6, accent(i / (float)(bw - 6)));
  }
  GFX_UNLOCK();
}

// ======================= HEBREW / UTF-8 TEXT =======================

int utf8ToCps(const String& s, uint32_t* out, int maxN) {
  int n = 0;
  const uint8_t* p = (const uint8_t*)s.c_str();
  while (*p && n < maxN) {
    uint32_t c = *p++;
    int extra = 0;
    if (c >= 0xF0)      { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    for (int k = 0; k < extra && *p; k++) c = (c << 6) | (*p++ & 0x3F);
    out[n++] = c;
  }
  return n;
}

String cpsToUtf8(const uint32_t* cps, int n) {
  String s;
  for (int i = 0; i < n; i++) {
    uint32_t c = cps[i];
    if (c < 0x80) {
      s += (char)c;
    } else if (c < 0x800) {
      s += (char)(0xC0 | (c >> 6));
      s += (char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      s += (char)(0xE0 | (c >> 12));
      s += (char)(0x80 | ((c >> 6) & 0x3F));
      s += (char)(0x80 | (c & 0x3F));
    } else {
      s += (char)(0xF0 | (c >> 18));
      s += (char)(0x80 | ((c >> 12) & 0x3F));
      s += (char)(0x80 | ((c >> 6) & 0x3F));
      s += (char)(0x80 | (c & 0x3F));
    }
  }
  return s;
}

bool isHeb(uint32_t c) { return c >= 0x0590 && c <= 0x05FF; }
bool isLtrChar(uint32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}
bool isJoiner(uint32_t c) {
  return c == '.' || c == ',' || c == ':' || c == '/' || c == '-' || c == '%';
}

// Reverse Hebrew for left->right drawing, keep numbers/English in normal order
void toVisual(uint32_t* v, int n) {
  bool heb = false;
  for (int i = 0; i < n; i++) if (isHeb(v[i])) { heb = true; break; }
  if (!heb) return;
  for (int i = 0, j = n - 1; i < j; i++, j--) { uint32_t t = v[i]; v[i] = v[j]; v[j] = t; }
  for (int i = 0; i < n; i++) {
    if (v[i] == '(') v[i] = ')'; else if (v[i] == ')') v[i] = '(';
  }
  int i = 0;
  while (i < n) {
    if (isLtrChar(v[i])) {
      int j = i;
      while (j < n && (isLtrChar(v[j]) || (isJoiner(v[j]) && j + 1 < n && isLtrChar(v[j + 1])))) j++;
      for (int a = i, b = j - 1; a < b; a++, b--) { uint32_t t = v[a]; v[a] = v[b]; v[b] = t; }
      i = j;
    } else {
      i++;
    }
  }
}

void wrapText(const String& text, int maxLines, Wrapped& w) {
  static uint32_t cps[700];
  int n = utf8ToCps(text, cps, 700);
  w.n = 0;
  int start = 0;
  while (start < n && w.n < maxLines) {
    while (start < n && cps[start] == ' ') start++;
    if (start >= n) break;
    int end = start + BUB_CHARS;
    if (end >= n) {
      end = n;
    } else {
      int k = end;
      while (k > start && cps[k] != ' ') k--;
      if (k > start) end = k;
    }
    int len = end - start;
    if (len > BUB_CHARS) len = BUB_CHARS;
    memcpy(w.line[w.n], cps + start, len * sizeof(uint32_t));
    if (w.n == maxLines - 1 && end < n) {           // last line but text continues
      if (len > BUB_CHARS - 2) len = BUB_CHARS - 2;
      w.line[w.n][len++] = '.';
      w.line[w.n][len++] = '.';
    }
    bool heb = false;
    for (int i = 0; i < len; i++) if (isHeb(w.line[w.n][i])) { heb = true; break; }
    toVisual(w.line[w.n], len);
    w.len[w.n] = len;
    w.heb[w.n] = heb;
    w.n++;
    start = end;
  }
}

// Draw a rounded chat bubble with a small "tail". Returns the y below it. Caller holds the lock.
int drawBubble(const Wrapped& w, int y, bool rightSide, uint16_t fill, uint16_t border, uint16_t textColor) {
  if (w.n == 0) return y;
  int maxLen = 0;
  for (int i = 0; i < w.n; i++) if (w.len[i] > maxLen) maxLen = w.len[i];
  int bw = maxLen * CHAR_W + 16;
  if (bw < 40) bw = 40;
  int bh = w.n * LINE_H + 10;
  int x = rightSide ? (SCR_W - 8 - bw) : 8;
  gfx->fillRoundRect(x, y, bw, bh, 10, fill);
  gfx->drawRoundRect(x, y, bw, bh, 10, border);
  // tail: small triangle at the bottom corner
  if (rightSide) gfx->fillTriangle(x + bw - 14, y + bh - 1, x + bw - 2, y + bh + 5, x + bw - 6, y + bh - 1, border);
  else           gfx->fillTriangle(x + 14, y + bh - 1, x + 2, y + bh + 5, x + 6, y + bh - 1, border);
  for (int i = 0; i < w.n; i++) {
    int tx = w.heb[i] ? (x + bw - 8 - w.len[i] * CHAR_W) : (x + 8);
    uiText(tx, y + 4 + i * LINE_H, cpsToUtf8(w.line[i], w.len[i]), textColor);
  }
  return y + bh + 6;
}

void uiClearChat() {
  GFX_LOCK();
  bgFill(0, Y_CHAT, SCR_W, Y_CHAT_END - Y_CHAT);
  GFX_UNLOCK();
  qBottom = Y_CHAT;
}

// User question: bubble on the RIGHT (like a sent message)
void uiQuestion(const String& q) {
  wrapText(q, 2, wq);
  GFX_LOCK();
  bgFill(0, Y_CHAT, SCR_W, Y_CHAT_END - Y_CHAT);
  qBottom = drawBubble(wq, Y_CHAT, true, C_QFILL, C_CYAN, C_WHITE);
  GFX_UNLOCK();
}

// Assistant answer: bubble on the LEFT, below the question
void uiAnswer(const String& a, uint16_t border = C_ABORDER, uint16_t textColor = C_WHITE) {
  int y = qBottom + 2;
  int lines = (Y_CHAT_END - y - 16) / LINE_H;
  if (lines > 5) lines = 5;
  if (lines < 1) lines = 1;
  wrapText(a, lines, wa);
  GFX_LOCK();
  bgFill(0, y, SCR_W, Y_CHAT_END - y);
  drawBubble(wa, y, false, C_AFILL, border, textColor);
  GFX_UNLOCK();
}

void uiHint() {
  GFX_LOCK();
  bgFill(0, Y_CHAT, SCR_W, Y_CHAT_END - Y_CHAT);
  uiText(52, Y_CHAT + 28,            "Hold K0 and ask anything", C_LAV);
  uiText(52, Y_CHAT + 28 + LINE_H,   "Knob: volume",             C_DIM);
  uiText(52, Y_CHAT + 28 + 2*LINE_H, "Press knob: new chat",     C_DIM);
  GFX_UNLOCK();
  qBottom = Y_CHAT;
}

// ======================= ANIMATIONS (core 0 task) =======================

enum AnimMode { ANIM_NONE, ANIM_LISTEN, ANIM_THINK, ANIM_SPEAK };
volatile AnimMode animMode = ANIM_NONE;
volatile float micLevel = 0;   // 0..1, set while recording
volatile float outLevel = 0;   // 0..1, set while speaking

// Listening: segmented level meter (green -> yellow -> red)
void drawMeter(float &shown) {
  float lvl = micLevel;
  shown = (lvl > shown) ? lvl : shown * 0.85;      // fast attack, slow decay
  const int SEG = 13, sw = 6, gap = 2;
  int lit = (int)(shown * SEG + 0.5);
  int x0 = AX + (AW - SEG * (sw + gap)) / 2;
  for (int i = 0; i < SEG; i++) {
    uint16_t on  = (i < 8) ? C_GREEN  : (i < 11 ? C_YELLOW  : C_RED);
    uint16_t off = (i < 8) ? C_DGREEN : (i < 11 ? C_DYELLOW : C_DRED);
    int h = 8 + i;                                  // bars grow slightly
    gfx->fillRect(x0 + i * (sw + gap), AY + AH - 28, sw, 28 - h, C_CARD);
    gfx->fillRoundRect(x0 + i * (sw + gap), AY + AH - 2 - h, sw, h, 2, i < lit ? on : off);
  }
}

// Thinking: three bouncing dots (cyan -> pink)
void drawDots(uint32_t frame) {
  int active = (frame / 4) % 3;
  int cx0 = AX + AW / 2 - 24;
  gfx->fillRect(AX, AY, AW, AH, C_CARD);
  for (int i = 0; i < 3; i++) {
    bool on = (i == active);
    int cy = AY + AH / 2 + (on ? -5 : 2);
    uint16_t c = accent(i / 2.0);
    gfx->fillCircle(cx0 + i * 24, cy, on ? 6 : 4, on ? c : C_CARDLINE);
  }
}

// Speaking: equalizer bars (follow the answer loudness, cyan -> pink)
void drawEq(uint32_t frame) {
  const int BARS = 12, bw = 6, gap = 3;
  int x0 = AX + (AW - BARS * (bw + gap)) / 2;
  float lvl = outLevel;
  for (int i = 0; i < BARS; i++) {
    float wave = 0.35 + 0.65 * fabs(sinf(frame * 0.35f + i * 0.9f));   // decorative motion
    int h = 3 + (int)((AH - 6) * lvl * wave);
    if (h > AH - 2) h = AH - 2;
    int x = x0 + i * (bw + gap);
    gfx->fillRect(x, AY, bw, AH, C_CARD);
    gfx->fillRoundRect(x, AY + (AH - h) / 2, bw, h, 3, accent(i / (float)(BARS - 1)));
  }
}

void animTask(void*) {
  AnimMode last = ANIM_NONE;
  uint32_t frame = 0;
  float shown = 0;
  for (;;) {
    AnimMode m = animMode;
    GFX_LOCK();
    if (m != last) {                               // mode changed: clear the box
      gfx->fillRect(AX, AY, AW, AH, C_CARD);
      last = m; frame = 0; shown = 0;
    }
    if (m == ANIM_LISTEN)      drawMeter(shown);
    else if (m == ANIM_THINK)  drawDots(frame);
    else if (m == ANIM_SPEAK)  drawEq(frame);
    GFX_UNLOCK();
    frame++;
    vTaskDelay(pdMS_TO_TICKS(m == ANIM_NONE ? 100 : 40));
  }
}

// ======================= CONTROLS =======================

const int8_t QEM[16] = {0, -1, 1, 0,  1, 0, 0, -1,  -1, 0, 0, 1,  0, 1, -1, 0};
volatile int32_t encRaw = 0;
volatile uint8_t encState = 0;
int32_t lastEncPos = 0;

void IRAM_ATTR encISR() {
  uint8_t s = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  encRaw += QEM[(encState << 2) | s];
  encState = s;
}

bool pollEncoder() {
  int32_t pos = encRaw >> 2;
  if (pos == lastEncPos) return false;
  volStep += (pos - lastEncPos);               // swap to (lastEncPos - pos) to reverse
  lastEncPos = pos;
  if (volStep < 0) volStep = 0;
  if (volStep > VOL_STEPS) volStep = VOL_STEPS;
  return true;
}

bool talkHeld() { return digitalRead(BTN) == LOW || digitalRead(KEY_K0) == LOW; }

bool pushClicked() {
  static bool last = false;
  static unsigned long t = 0;
  bool now = digitalRead(ENC_PUSH) == LOW;
  bool fired = false;
  if (now && !last && millis() - t > 250) { fired = true; t = millis(); }
  last = now;
  return fired;
}

// ======================= STREAM HELPERS =======================

class UploadStream : public Stream {
public:
  const uint8_t* seg[3];
  size_t len[3];
  int idx = 0;
  size_t pos = 0;
  void skipEmpty() { while (idx < 3 && pos >= len[idx]) { idx++; pos = 0; } }
  int available() {
    if (idx >= 3) return 0;
    size_t r = len[idx] - pos;
    for (int i = idx + 1; i < 3; i++) r += len[i];
    return (int)r;
  }
  int read() { skipEmpty(); if (idx >= 3) return -1; return seg[idx][pos++]; }
  int peek() { skipEmpty(); if (idx >= 3) return -1; return seg[idx][pos]; }
  size_t readBytes(char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
      skipEmpty();
      if (idx >= 3) break;
      size_t chunk = len[idx] - pos;
      if (chunk > n - got) chunk = n - got;
      memcpy(buf + got, seg[idx] + pos, chunk);
      pos += chunk; got += chunk;
    }
    return got;
  }
  size_t write(uint8_t) { return 0; }
  void flush() {}
};

class BufferStream : public Stream {
public:
  uint8_t* buf;
  size_t cap;
  size_t len = 0;
  size_t write(uint8_t b) { if (len < cap) { buf[len++] = b; return 1; } return 0; }
  size_t write(const uint8_t* data, size_t n) {
    size_t k = n;
    if (len + k > cap) k = cap - len;
    memcpy(buf + len, data, k);
    len += k;
    return k;
  }
  int available() { return 0; }
  int read() { return -1; }
  int peek() { return -1; }
  void flush() {}
};

// ======================= SMALL HELPERS =======================

void led(uint8_t r, uint8_t g, uint8_t b) { rgbLedWrite(RGB_PIN, r, g, b); }

void showError(const char* msg) {
  animMode = ANIM_NONE;
  led(40, 0, 0);
  uiStatus("Error", C_RED);
  uiAnswer(msg, C_RED, C_RED);
  delay(2000);
  led(0, 0, 0);
  uiStatus("Ready", C_GREEN);
}

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF; }

void writeWavHeader(uint8_t* h, uint32_t dataBytes) {
  memcpy(h, "RIFF", 4);      put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); put32(h + 16, 16);
  put16(h + 20, 1);  put16(h + 22, 1);
  put32(h + 24, MIC_RATE);   put32(h + 28, MIC_RATE * 2);
  put16(h + 32, 2);  put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, dataBytes);
}

void ampOn()  { digitalWrite(AMP_SD, HIGH); delay(5); }
void ampOff() { digitalWrite(AMP_SD, LOW); }

void playSilence(int ms) {
  for (int i = 0; i < ms / 10 + 1; i++) amp.write((uint8_t*)silence, sizeof(silence));
}

void beep(int ms, float freq) {
  ampOn();
  int16_t buf[512];
  float phase = 0, step = 2 * PI * freq / AMP_RATE;
  for (int done = 0; done < AMP_RATE * ms / 1000; done += 256) {
    for (int i = 0; i < 256; i++) {
      int16_t v = (int16_t)(2000 * sin(phase));
      buf[2 * i] = v;
      buf[2 * i + 1] = v;
      phase += step;
      if (phase > 2 * PI) phase -= 2 * PI;
    }
    amp.write((uint8_t*)buf, sizeof(buf));
  }
  playSilence(100);
  ampOff();
}

// ======================= 1) RECORD (+ live mic level) =======================

size_t recordWhileHeld() {
  size_t n = 0;
  int32_t block[512];
  while (talkHeld() && n < MAX_SAMPLES) {
    size_t bytes = mic.readBytes((char*)block, sizeof(block));
    size_t count = bytes / 4;
    double sum = 0, sumSq = 0;
    int cnt = 0;
    for (size_t i = 0; i < count; i += 2) {      // slot A only
      if (n >= MAX_SAMPLES) break;
      int32_t s = block[i] >> MIC_SHIFT;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      rec[n++] = (int16_t)s;
      sum += s; sumSq += (double)s * s; cnt++;
    }
    if (cnt > 0) {
      // RMS -> 0..1 on a log scale: ~10 (silence) = 0, ~1000 (loud) = 1
      double mean = sum / cnt;
      double rms = sqrt(fmax(sumSq / cnt - mean * mean, 1.0));
      float lvl = (log10(rms) - 1.0) / 2.0;
      micLevel = lvl < 0 ? 0 : (lvl > 1 ? 1 : lvl);
    }
  }
  micLevel = 0;
  if (n == 0) return 0;

  float prevX = rec[0], prevY = 0;               // 80 Hz high-pass
  int32_t peak = 1;
  for (size_t i = 0; i < n; i++) {
    float x = rec[i];
    float y = HPF_ALPHA * (prevY + x - prevX);
    prevX = x; prevY = y;
    if (y > 32767) y = 32767;
    if (y < -32768) y = -32768;
    rec[i] = (int16_t)y;
    if (i > 256 && abs((int32_t)rec[i]) > peak) peak = abs((int32_t)rec[i]);
  }
  float gain = TARGET_PEAK / peak;               // normalize
  if (gain > MAX_GAIN) gain = MAX_GAIN;
  for (size_t i = 0; i < n; i++) {
    int32_t v = (int32_t)(rec[i] * gain);
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    rec[i] = (int16_t)v;
  }
  return n;
}

// ======================= 2) SPEECH -> TEXT =======================

String transcribe(int16_t* pcm, size_t n) {
  String B = "----esp32boundary7MA4YWxk";
  String head = "--";
  head += B;
  head += "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n";
  head += STT_MODEL;
  head += "\r\n--";
  head += B;
  head += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";
  String tail = "\r\n--";
  tail += B;
  tail += "--\r\n";

  static uint8_t prefix[512];
  size_t hl = head.length();
  memcpy(prefix, head.c_str(), hl);
  writeWavHeader(prefix + hl, n * 2);

  UploadStream up;
  up.seg[0] = prefix;                       up.len[0] = hl + 44;
  up.seg[1] = (const uint8_t*)pcm;          up.len[1] = n * 2;
  up.seg[2] = (const uint8_t*)tail.c_str(); up.len[2] = tail.length();
  size_t total = up.len[0] + up.len[1] + up.len[2];

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, "https://api.openai.com/v1/audio/transcriptions");
  http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
  http.addHeader("Content-Type", String("multipart/form-data; boundary=") + B);

  int code = http.sendRequest("POST", &up, total);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  if (code != 200) {
    Serial.printf("STT error %d (%s) %s\n", code, http.errorToString(code).c_str(), resp.c_str());
    return "";
  }
  JsonDocument doc;
  if (deserializeJson(doc, resp)) { Serial.println("STT JSON error: " + resp); return ""; }
  return doc["text"].as<String>();
}

// ======================= 3) THE BRAIN (LLM) =======================

bool askLLM(const String& question, String& answer) {
  JsonDocument req;
  req["model"] = LLM_MODEL;

  struct tm t;
  char now[64] = "unknown";
  if (getLocalTime(&t, 200)) strftime(now, sizeof(now), "%A %d %B %Y, %H:%M", &t);
  String sys = String(SYSTEM_PROMPT) + " Current date and time in Israel: " + now + ".";
  req["instructions"] = sys;

  req["tools"][0]["type"] = "web_search";
  req["input"] = question;
  req["max_output_tokens"] = 2000;
  if (lastResponseId.length() > 0) req["previous_response_id"] = lastResponseId;

  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(60000);
  http.begin(client, "https://api.openai.com/v1/responses");
  http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  String resp = (code > 0) ? http.getString() : "";
  http.end();

  if (code != 200) {
    Serial.printf("LLM error %d (%s) %s\n", code, http.errorToString(code).c_str(), resp.c_str());
    return false;
  }

  JsonDocument filter;
  filter["id"] = true;
  filter["status"] = true;
  filter["output"][0]["type"] = true;
  filter["output"][0]["content"][0]["type"] = true;
  filter["output"][0]["content"][0]["text"] = true;

  JsonDocument doc;
  if (deserializeJson(doc, resp, DeserializationOption::Filter(filter))) {
    Serial.println("LLM JSON error");
    return false;
  }

  answer = "";
  for (JsonObject item : doc["output"].as<JsonArray>()) {
    if (item["type"] != "message") continue;
    for (JsonObject c : item["content"].as<JsonArray>()) {
      if (c["type"] == "output_text") answer += c["text"].as<const char*>();
    }
  }
  if (answer.length() == 0) {
    Serial.printf("LLM returned no text (status: %s)\n", doc["status"] | "?");
    return false;
  }
  lastResponseId = doc["id"].as<String>();
  return true;
}

// ======================= 4) TEXT -> SPEECH + PLAY =======================

bool speak(const String& text) {
  JsonDocument req;
  req["model"] = TTS_MODEL;
  req["voice"] = TTS_VOICE;
  req["input"] = text;
  req["response_format"] = "pcm";
  req["instructions"] = "Speak in a warm, clear, natural tone.";
  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.begin(client, "https://api.openai.com/v1/audio/speech");
  http.addHeader("Authorization", String("Bearer ") + OPENAI_KEY);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(body);
  if (code != 200) {
    String resp = (code > 0) ? http.getString() : "";
    http.end();
    Serial.printf("TTS error %d (%s) %s\n", code, http.errorToString(code).c_str(), resp.c_str());
    return false;
  }

  BufferStream bs;
  bs.buf = reply;
  bs.cap = replyCap;
  bs.len = 0;
  http.writeToStream(&bs);
  http.end();

  size_t n = bs.len / 2;
  if (n == 0) { Serial.println("TTS returned no audio"); return false; }
  Serial.printf("Answer audio: %.1f s\n", n / (float)AMP_RATE);

  led(0, 40, 0);
  uiStatus("Speaking", C_GREEN);
  animMode = ANIM_SPEAK;
  ampOn();
  int16_t* s = (int16_t*)reply;
  int16_t tmp[1024];                             // 512 stereo frames
  for (size_t off = 0; off < n; off += 512) {
    if (pollEncoder()) uiVolume();
    float vol = volStep * VOL_PER_STEP;
    size_t k = n - off;
    if (k > 512) k = 512;
    double sumSq = 0;
    for (size_t i = 0; i < k; i++) {
      int16_t raw = s[off + i];
      sumSq += (double)raw * raw;
      int16_t v = (int16_t)(raw * vol);
      tmp[2 * i] = v;
      tmp[2 * i + 1] = v;
    }
    float lvl = sqrt(sumSq / k) / 6000.0;        // drive the equalizer
    outLevel = lvl > 1 ? 1 : lvl;
    amp.write((uint8_t*)tmp, k * 4);
  }
  outLevel = 0;
  playSilence(150);
  ampOff();
  animMode = ANIM_NONE;
  return true;
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);
  led(0, 0, 0);
  delay(1000);
  pinMode(BTN, INPUT_PULLUP);
  pinMode(KEY_K0, INPUT_PULLUP);
  pinMode(ENC_PUSH, INPUT_PULLUP);
  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(MIC_LR, OUTPUT);
  digitalWrite(MIC_LR, LOW);
  pinMode(AMP_SD, OUTPUT);
  digitalWrite(AMP_SD, LOW);

  // ---- Display ----
  gfxLock = xSemaphoreCreateMutex();
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);
  if (!gfx->begin(20000000)) Serial.println("Display init FAILED");
  gfx->setFont(u8g2_font_unifont_t_hebrew);
  gfx->setUTF8Print(true);
  bgInit();
  uiFrame();
  uiHeader();
  uiStatus("Starting", C_YELLOW);
  uiVolume();

  // Animation task on core 0 (the main loop runs on core 1)
  xTaskCreatePinnedToCore(animTask, "anim", 4096, NULL, 1, NULL, 0);

  // ---- Encoder ----
  encState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encISR, CHANGE);

  // ---- Memory ----
  Serial.printf("PSRAM: %u KB | Free RAM: %u KB\n",
                ESP.getPsramSize() / 1024, ESP.getFreeHeap() / 1024);
  rec = (int16_t*)ps_malloc(MAX_SAMPLES * 2);
  replyCap = (size_t)AMP_RATE * 2 * MAX_REPLY_SEC;
  reply = (uint8_t*)ps_malloc(replyCap);
  if (!rec || !reply) {
    Serial.println("PSRAM alloc FAILED - check Tools > PSRAM = OPI PSRAM");
    uiStatus("No PSRAM", C_RED);
    led(40, 0, 0);
    while (true) delay(1000);
  }

  // ---- WiFi ----
  uiStatus("WiFi...", C_YELLOW);
  animMode = ANIM_THINK;                         // dots while connecting
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }
  animMode = ANIM_NONE;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED");
    uiStatus("No WiFi", C_RED);
    led(40, 0, 0);
    while (true) delay(1000);
  }
  Serial.printf("\nWiFi OK (%d dBm)\n", WiFi.RSSI());

  // ---- Time ----
  configTzTime("IST-2IDT,M3.4.4/26,M10.5.0", "pool.ntp.org", "time.google.com");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    char now[32];
    strftime(now, sizeof(now), "%d/%m/%Y %H:%M", &t);
    Serial.printf("Time: %s\n", now);
  }
  uiHeader();

  // ---- Audio ----
  mic.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!mic.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("MIC FAILED"); uiStatus("Mic error", C_RED); while (true) delay(1000);
  }
  amp.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  if (!amp.begin(I2S_MODE_STD, AMP_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("AMP FAILED"); uiStatus("Amp error", C_RED); while (true) delay(1000);
  }

  beep(150, 880);
  uiStatus("Ready", C_GREEN);
  uiHint();
  Serial.println("Ready: hold K0 or BOOT, ask a question, release");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    uiStatus("No WiFi", C_RED);
    WiFi.reconnect();
    delay(1000);
    if (WiFi.status() == WL_CONNECTED) uiStatus("Ready", C_GREEN);
    return;
  }

  static unsigned long tClock = 0;
  if (millis() - tClock > 20000) { tClock = millis(); uiHeader(); }
  if (pollEncoder()) uiVolume();
  if (pushClicked()) {
    lastResponseId = "";
    Serial.println("New conversation");
    uiStatus("New chat", C_CYAN);
    uiHint();
    beep(80, 660);
    delay(700);
    uiStatus("Ready", C_GREEN);
  }

  if (!talkHeld()) { delay(5); return; }

  // ---- 1) Listen ----
  led(0, 0, 40);
  uiStatus("Listening", C_BLUE);
  uiClearChat();
  animMode = ANIM_LISTEN;
  size_t n = recordWhileHeld();
  animMode = ANIM_NONE;
  while (talkHeld()) delay(10);
  if (n < MIC_RATE / 5) {
    led(0, 0, 0);
    uiStatus("Ready", C_GREEN);
    uiHint();
    Serial.println("Too short, ignored");
    return;
  }

  // ---- 2) Transcribe ----
  led(30, 20, 0);
  uiStatus("Thinking", C_YELLOW);
  animMode = ANIM_THINK;
  unsigned long t0 = millis();
  String question = transcribe(rec, n);
  unsigned long tStt = millis() - t0;
  question.trim();
  if (question.length() == 0) { Serial.println("(heard nothing)"); showError("I did not hear anything"); return; }
  Serial.printf("\nYou (%lu ms): %s\n", tStt, question.c_str());
  uiQuestion(question);

  // ---- 3) Think ----
  t0 = millis();
  String answer;
  bool ok = askLLM(question, answer);
  if (!ok && lastResponseId.length() > 0) {
    lastResponseId = "";
    ok = askLLM(question, answer);
  }
  unsigned long tLlm = millis() - t0;
  if (!ok) { showError("AI error - see Serial Monitor"); return; }
  Serial.printf("AI  (%lu ms): %s\n", tLlm, answer.c_str());
  uiAnswer(answer);

  // ---- 4) Speak (dots continue while the audio downloads) ----
  t0 = millis();
  if (!speak(answer)) { showError("Speech error - see Serial Monitor"); return; }
  Serial.printf("Speech download+play: %lu ms\n", millis() - t0);
  led(0, 0, 0);
  uiStatus("Ready", C_GREEN);
}
