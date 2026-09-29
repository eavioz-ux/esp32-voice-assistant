// ESP32-S3 AI VOICE ASSISTANT
// Hold BOOT -> speak -> release. The assistant answers through the speaker.
// LED: blue = listening, yellow = thinking, green = speaking, red = error
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP_I2S.h>

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
  "You are a friendly voice assistant running on a small electronic device. "
  "Always answer in the same language the user spoke (Hebrew or English). "
  "Keep answers short: 1 to 3 sentences. No lists, no markdown, no emojis, "
  "because your answer will be read aloud. "
  "If the question needs current information (news, weather, prices, sports), use web search. "
  "Never read URLs, links or source names aloud.";

// ---- Pins ----
#define MIC_SCK  17
#define MIC_WS   16
#define MIC_SD   18
#define MIC_LR   15
#define AMP_BCLK 11
#define AMP_LRC  12
#define AMP_DIN  10
#define BTN       0
#define RGB_PIN  48

// ---- Audio settings ----
const int   MIC_RATE      = 16000;   // microphone sample rate
const int   AMP_RATE      = 24000;   // OpenAI TTS "pcm" output is 24 kHz, 16-bit, mono
const int   MAX_SEC       = 8;       // max question length (seconds)
const int   MAX_REPLY_SEC = 40;      // max answer length kept in memory
const int   MIC_SHIFT     = 14;
const float TARGET_PEAK   = 12000;
const float MAX_GAIN      = 3.0;
const float HPF_ALPHA     = 0.970;
const float PLAY_VOLUME   = 0.2;     // 0.0-1.0. Above ~0.3 the speaker distorts

size_t MAX_SAMPLES = MIC_RATE * MAX_SEC;
I2SClass mic, amp;
int16_t *rec;              // recorded question (PSRAM)
uint8_t *reply;            // downloaded answer audio (PSRAM)
size_t   replyCap;
String   lastResponseId;   // lets the AI remember the conversation
int16_t  silence[512] = {0};

// ======================= STREAM HELPERS =======================

// Sends 3 memory pieces one after another without copying them
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

// Collects downloaded bytes into a big PSRAM buffer
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

void showError() {
  led(40, 0, 0);
  delay(1500);
  led(0, 0, 0);
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

void playSilence(int ms) {
  // 512 samples at 24 kHz = ~21 ms per block
  for (int i = 0; i < ms / 21 + 1; i++) amp.write((uint8_t*)silence, sizeof(silence));
}

void beep(int ms, float freq) {
  int16_t buf[256];
  float phase = 0, step = 2 * PI * freq / AMP_RATE;
  for (int done = 0; done < AMP_RATE * ms / 1000; done += 256) {
    for (int i = 0; i < 256; i++) {
      buf[i] = (int16_t)(2000 * sin(phase));
      phase += step;
      if (phase > 2 * PI) phase -= 2 * PI;
    }
    amp.write((uint8_t*)buf, sizeof(buf));
  }
  playSilence(100);
}

// ======================= 1) RECORD =======================

size_t recordWhileHeld() {
  size_t n = 0;
  int32_t block[512];
  while (digitalRead(BTN) == LOW && n < MAX_SAMPLES) {
    size_t bytes = mic.readBytes((char*)block, sizeof(block));
    size_t count = bytes / 4;
    for (size_t i = 0; i < count; i += 2) {     // keep slot A only
      if (n >= MAX_SAMPLES) break;
      int32_t s = block[i] >> MIC_SHIFT;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      rec[n++] = (int16_t)s;
    }
  }
  if (n == 0) return 0;

  float prevX = rec[0], prevY = 0;              // 80 Hz high-pass
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
  float gain = TARGET_PEAK / peak;              // normalize
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
  // Build the request as JSON (ArduinoJson escapes quotes/Hebrew safely)
  JsonDocument req;
  req["model"] = LLM_MODEL;
  // Add the current date/time so the AI knows "now"
  struct tm t;
  char now[64] = "unknown";
  if (getLocalTime(&t, 200)) strftime(now, sizeof(now), "%A %d %B %Y, %H:%M", &t);
  String sys = String(SYSTEM_PROMPT) + " Current date and time in Israel: " + now + ".";
  req["instructions"] = sys;

  // Allow the AI to search the web when needed
  req["tools"][0]["type"] = "web_search";

  req["input"] = question;
  req["max_output_tokens"] = 2000;
  if (lastResponseId.length() > 0) {
    req["previous_response_id"] = lastResponseId;   // conversation memory
  }
  String body;
  serializeJson(req, body);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(60000);                       // web search can take a while
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

  // Keep only the fields we need from the (large) answer
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

  // Answer text lives in: output[i] (type "message") -> content[j] (type "output_text") -> text
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
  req["response_format"] = "pcm";               // raw 24 kHz 16-bit mono
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

  // Download the whole answer audio into PSRAM
  BufferStream bs;
  bs.buf = reply;
  bs.cap = replyCap;
  bs.len = 0;
  http.writeToStream(&bs);
  http.end();

  size_t n = bs.len / 2;                        // number of 16-bit samples
  if (n == 0) { Serial.println("TTS returned no audio"); return false; }
  Serial.printf("Answer audio: %.1f s\n", n / (float)AMP_RATE);

  // Play it, with volume scaling
  led(0, 40, 0);                                // green = speaking
  int16_t* s = (int16_t*)reply;
  int16_t tmp[512];
  for (size_t off = 0; off < n; off += 512) {
    size_t k = n - off;
    if (k > 512) k = 512;
    for (size_t i = 0; i < k; i++) tmp[i] = (int16_t)(s[off + i] * PLAY_VOLUME);
    amp.write((uint8_t*)tmp, k * 2);
  }
  playSilence(150);
  return true;
}

// ======================= SETUP / LOOP =======================

void setup() {
  Serial.begin(115200);
  led(0, 0, 0);
  delay(1500);
  pinMode(BTN, INPUT_PULLUP);
  pinMode(MIC_LR, OUTPUT);
  digitalWrite(MIC_LR, LOW);

  Serial.printf("PSRAM: %u KB | Free RAM: %u KB\n",
                ESP.getPsramSize() / 1024, ESP.getFreeHeap() / 1024);

  rec = (int16_t*)ps_malloc(MAX_SAMPLES * 2);
  replyCap = (size_t)AMP_RATE * 2 * MAX_REPLY_SEC;       // ~1.9 MB
  reply = (uint8_t*)ps_malloc(replyCap);
  if (!rec || !reply) {
    Serial.println("PSRAM alloc FAILED - check Tools > PSRAM = OPI PSRAM");
    led(40, 0, 0);
    while (true) delay(1000);
  }

  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(500); Serial.print("."); }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi FAILED");
    led(40, 0, 0);
    while (true) delay(1000);
  }
  Serial.printf("\nWiFi OK (%d dBm)\n", WiFi.RSSI());

  // Get real date/time from the internet, Israel time zone (handles summer time)
  configTzTime("IST-2IDT,M3.4.4/26,M10.5.0", "pool.ntp.org", "time.google.com");
  struct tm t;
  if (getLocalTime(&t, 5000)) {
    char now[64];
    strftime(now, sizeof(now), "%d/%m/%Y %H:%M", &t);
    Serial.printf("Time: %s\n", now);
  } else {
    Serial.println("Time: not synced yet");
  }

  mic.setPins(MIC_SCK, MIC_WS, -1, MIC_SD);
  if (!mic.begin(I2S_MODE_STD, MIC_RATE, I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("MIC FAILED"); while (true) delay(1000);
  }
  amp.setPins(AMP_BCLK, AMP_LRC, AMP_DIN);
  if (!amp.begin(I2S_MODE_STD, AMP_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO)) {
    Serial.println("AMP FAILED"); while (true) delay(1000);
  }

  beep(150, 880);
  Serial.println("Ready: hold BOOT, ask a question, release");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {          // auto-reconnect if WiFi drops
    WiFi.reconnect();
    delay(1000);
    return;
  }
  if (digitalRead(BTN) != LOW) { delay(5); return; }

  // ---- 1) Listen ----
  led(0, 0, 40);                                // blue
  size_t n = recordWhileHeld();
  while (digitalRead(BTN) == LOW) delay(10);
  if (n < MIC_RATE / 5) { led(0, 0, 0); Serial.println("Too short, ignored"); return; }

  // ---- 2) Transcribe ----
  led(30, 20, 0);                               // yellow = thinking
  unsigned long t0 = millis();
  String question = transcribe(rec, n);
  unsigned long tStt = millis() - t0;
  question.trim();
  if (question.length() == 0) { Serial.println("(heard nothing)"); showError(); return; }
  Serial.printf("\nYou (%lu ms): %s\n", tStt, question.c_str());

  // ---- 3) Think ----
  t0 = millis();
  String answer;
  bool ok = askLLM(question, answer);
  if (!ok && lastResponseId.length() > 0) {     // retry once without conversation memory
    lastResponseId = "";
    ok = askLLM(question, answer);
  }
  unsigned long tLlm = millis() - t0;
  if (!ok) { showError(); return; }
  Serial.printf("AI  (%lu ms): %s\n", tLlm, answer.c_str());

  // ---- 4) Speak ----
  t0 = millis();
  if (!speak(answer)) { showError(); return; }
  Serial.printf("Speech download+play: %lu ms\n", millis() - t0);
  led(0, 0, 0);
}
