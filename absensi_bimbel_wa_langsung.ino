/*
  Absensi Bimbel - ESP32 + RFID MFRC522 + Buzzer -> WhatsApp (langsung, tanpa server)
  Alur: tap kartu -> cocokkan UID di tabel -> kirim pesan WA ke orang tua lewat
  API gateway (contoh format: Fonnte) -> buzzer berbunyi.

  Library: MFRC522 oleh GithubCommunity (miguelbalboa)

  Wiring MFRC522 -> ESP32 (3.3V):
    SDA/SS GPIO5, SCK GPIO18, MOSI GPIO23, MISO GPIO19, RST GPIO22, 3.3V, GND
  Buzzer lewat transistor NPN: GPIO27 -> resistor 1k -> basis;
    emitor -> GND; kolektor -> kabel (-) buzzer; kabel (+) buzzer -> 12V.

  JANGAN upload file ini ke GitHub/tempat publik: berisi token dan nomor HP.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

// ================== PENGATURAN ==================
const char* WIFI_SSID   = "NAMA_WIFI_BIMBEL";
const char* WIFI_PASS   = "PASSWORD_WIFI";
const char* NAMA_BIMBEL = "Bimbel Contoh";

// Gateway WhatsApp (format Fonnte: POST form, header Authorization berisi token)
const char* WA_API_URL  = "https://api.fonnte.com/send";
const char* WA_TOKEN    = "TOKEN_DARI_DASHBOARD_GATEWAY";

// Tabel murid: UID kartu (huruf besar, tanpa spasi), nama anak, nomor WA orang tua
struct Murid {
  const char* uid;
  const char* nama;
  const char* wa;   // format 08xxxxxxxxxx
};

Murid daftarMurid[] = {
  {"A1B2C3D4", "Ahmad",  "081234567890"},
  {"11223344", "Siti",   "081298765432"},
  // tambahkan murid lain di sini
};
const int JUMLAH_MURID = sizeof(daftarMurid) / sizeof(daftarMurid[0]);

#define SS_PIN     5
#define RST_PIN    22
#define BUZZER_PIN 27
#define LED_PIN    2

const long          GMT_OFFSET_SEC = 7 * 3600;       // WIB
const unsigned long DEBOUNCE_MS    = 4000;           // tap kartu sama berulang
const unsigned long COOLDOWN_NOTIME_MS = 30UL * 60UL * 1000UL; // jika jam belum sinkron
const unsigned long RETRY_INTERVAL = 15000;
const int           HTTP_TIMEOUT_MS = 8000;
// =================================================

MFRC522 rfid(SS_PIN, RST_PIN);

// Antrean kirim ulang saat offline (RAM)
const int QUEUE_MAX = 40;
int    qIdx[QUEUE_MAX];
time_t qTs[QUEUE_MAX];
int    qCount = 0;

// Status absen per murid
long          lastDayKey[64];     // sudah dikirim pada hari ke-berapa
unsigned long lastSentMs[64];     // cadangan bila jam belum sinkron

String        lastUid = "";
unsigned long lastScanMs = 0;
unsigned long lastRetryMs = 0;

// ---------- Buzzer ----------
void beep(int times, int onMs, int offMs = 100) {
  for (int i = 0; i < times; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_PIN, LOW);
    if (i < times - 1) delay(offMs);
  }
}
void bipSukses()      { beep(1, 250); }          // pesan terkirim
void bipSudahAbsen()  { beep(2, 50, 60); }       // sudah absen hari ini
void bipTakDikenal()  { beep(3, 80, 80); }       // kartu belum terdaftar
void bipAntre()       { beep(1, 700); }          // gagal kirim, masuk antrean

// ---------- WiFi & waktu ----------
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Menghubungkan WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi terhubung: " + WiFi.localIP().toString());
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  } else {
    Serial.println("\nWiFi gagal, mode offline.");
  }
}

time_t nowEpoch() {
  time_t t = time(nullptr);
  return (t > 1700000000) ? t : 0;   // 0 = jam belum sinkron
}

long dayKeyOf(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  return (long)(tmv.tm_year + 1900) * 400 + tmv.tm_yday;
}

String formatWaktu(time_t t) {
  if (t == 0) return "(jam belum tersedia)";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tmv);
  return String(buf) + " WIB";
}

// ---------- Cari murid ----------
int cariMurid(const String& uid) {
  for (int i = 0; i < JUMLAH_MURID; i++) {
    if (uid.equalsIgnoreCase(daftarMurid[i].uid)) return i;
  }
  return -1;
}

bool sudahAbsen(int idx, time_t ts) {
  if (ts != 0) return lastDayKey[idx] == dayKeyOf(ts);
  return lastSentMs[idx] != 0 && (millis() - lastSentMs[idx] < COOLDOWN_NOTIME_MS);
}

void tandaiAbsen(int idx, time_t ts) {
  if (ts != 0) lastDayKey[idx] = dayKeyOf(ts);
  lastSentMs[idx] = millis();
}

// ---------- Kirim WhatsApp ----------
String urlEncode(const String& s) {
  String out = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

// Return true jika gateway menerima pesan
bool kirimWA(int idx, time_t ts) {
  if (WiFi.status() != WL_CONNECTED) return false;

  String pesan = "Informasi dari " + String(NAMA_BIMBEL) + ":\n" +
                 String(daftarMurid[idx].nama) + " sudah hadir di bimbel pada " +
                 formatWaktu(ts) + ".";

  WiFiClientSecure client;
  client.setInsecure();               // lihat catatan di bawah file
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, WA_API_URL)) return false;

  http.addHeader("Authorization", WA_TOKEN);   // tanpa "Bearer"
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "target=" + urlEncode(daftarMurid[idx].wa) +
                "&message=" + urlEncode(pesan) +
                "&countryCode=62";

  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("WA %s -> HTTP %d : %s\n", daftarMurid[idx].nama, code, resp.c_str());

  // Gateway membalas JSON; anggap sukses jika status true
  return (code == 200 && resp.indexOf("\"status\":true") >= 0);
}

// ---------- Antrean ----------
void enqueue(int idx, time_t ts) {
  if (qCount >= QUEUE_MAX) {
    for (int i = 1; i < QUEUE_MAX; i++) { qIdx[i - 1] = qIdx[i]; qTs[i - 1] = qTs[i]; }
    qCount--;
  }
  qIdx[qCount] = idx;
  qTs[qCount] = ts;
  qCount++;
  Serial.printf("Masuk antrean (%d)\n", qCount);
}

void flushQueue() {
  if (qCount == 0 || WiFi.status() != WL_CONNECTED) return;
  while (qCount > 0) {
    if (!kirimWA(qIdx[0], qTs[0])) break;
    for (int i = 1; i < qCount; i++) { qIdx[i - 1] = qIdx[i]; qTs[i - 1] = qTs[i]; }
    qCount--;
    delay(1500);   // jeda antar pesan, jangan kirim beruntun
  }
}

// ---------- RFID ----------
String readUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  for (int i = 0; i < 64; i++) { lastDayKey[i] = -1; lastSentMs[i] = 0; }

  SPI.begin(18, 19, 23, SS_PIN);
  rfid.PCD_Init();
  Serial.print("MFRC522 versi: 0x");
  Serial.println(rfid.PCD_ReadRegister(MFRC522::VersionReg), HEX);

  connectWiFi();
  beep(2, 80, 80);
  Serial.println("Siap. Tempelkan kartu...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) connectWiFi();
  if (millis() - lastRetryMs > RETRY_INTERVAL) {
    lastRetryMs = millis();
    flushQueue();
  }

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) {
    delay(30);
    return;
  }

  String uid = readUid();
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  if (uid == lastUid && millis() - lastScanMs < DEBOUNCE_MS) return;
  lastUid = uid;
  lastScanMs = millis();

  Serial.println("UID: " + uid);   // salin UID ini ke tabel daftarMurid

  int idx = cariMurid(uid);
  if (idx < 0) { bipTakDikenal(); return; }

  time_t ts = nowEpoch();
  if (sudahAbsen(idx, ts)) { bipSudahAbsen(); return; }

  tandaiAbsen(idx, ts);              // tandai dulu supaya tidak dobel
  if (kirimWA(idx, ts)) {
    bipSukses();
  } else {
    enqueue(idx, ts);
    bipAntre();
  }
}

/*
  CATATAN:
  - Format request mengikuti dokumentasi Fonnte (POST ke api.fonnte.com/send,
    header Authorization berisi token tanpa "Bearer", field target & message).
    Jika memakai gateway lain, ubah WA_API_URL, header, field body, dan cara
    mengecek sukses di fungsi kirimWA().
  - Menambah murid: tap kartu, salin UID dari Serial Monitor, tambahkan baris
    di daftarMurid[]. Kapasitas status dibatasi 64 murid (ubah ukuran array
    lastDayKey/lastSentMs jika lebih).
  - Absen hanya dikirim sekali per murid per hari (jika jam NTP sudah sinkron).
  - Antrean di RAM hilang jika listrik mati.
  - client.setInsecure() tidak memverifikasi sertifikat; untuk lebih aman pakai
    setCACert() dengan root CA gateway.
  - Pola bunyi: 1x sedang = pesan terkirim, 2x sangat cepat = sudah absen hari
    ini, 3x cepat = kartu belum terdaftar, 1x panjang = gagal kirim (antre).
  - Beri tahu orang tua bahwa nomor mereka dipakai untuk notifikasi absensi.
*/
