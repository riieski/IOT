/*
  Absensi Bimbel v2.1 - ESP32 + RFID MFRC522 + Buzzer -> WhatsApp (Fonnte)

  FITUR
  - Absen BERANGKAT dan PULANG per hari. Scan kedua baru dihitung "pulang"
    setelah jeda (default 60 menit); scan sebelum itu ditolak (anti double scan).
  - Setup WiFi lewat HP: jika WiFi belum diatur / gagal, alat membuat WiFi
    "Absensi-Setup" (password: absen1234). Sambungkan HP, buka halaman setup.
  - Halaman admin (browser HP) saat alat tersambung ke WiFi bimbel:
    tambah/ubah/hapus murid, daftar kartu baru, riwayat hari ini,
    ganti token Fonnte, nama bimbel, jeda, password admin, ganti WiFi, tes kirim WA.
  - Semua data tersimpan di memori flash ESP32 (tidak perlu upload ulang).

  LOGIN ADMIN (awal): user  admin   password  admin1234   (SEGERA GANTI)
  RESET: tahan tombol BOOT 5 detik saat alat menyala -> WiFi dan password admin
         kembali ke awal (data murid tetap). Lepas tombol setelah bunyi bip 3x.

  LIBRARY: MFRC522 oleh GithubCommunity (miguelbalboa). Lainnya bawaan board esp32.

  WIRING MFRC522 -> ESP32 (3.3V):
    SDA/SS D5, SCK D18, MOSI D23, MISO D19, RST D22, 3.3V, GND
  BUZZER lewat transistor NPN: D27 -> resistor 1k -> basis; emitor -> GND;
    kolektor -> kabel (-) buzzer; kabel (+) buzzer -> 12V.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>
#include <vector>

// ================== KONSTANTA ==================
#define SS_PIN     5
#define RST_PIN    22
#define BUZZER_PIN 27
#define LED_PIN    2
#define BOOT_PIN   0

const long GMT_OFFSET_SEC = 7 * 3600;                 // WIB
const char* AP_SSID   = "Absensi-Setup";
const char* AP_PASS   = "absen1234";
const char* ADMIN_USER = "admin";
const char* ADMIN_PASS_DEFAULT = "admin1234";
const char* WA_API_URL = "https://api.fonnte.com/send";

const int MAX_MURID = 200;
const int QUEUE_MAX = 30;
const unsigned long DEBOUNCE_MS   = 3000;             // kartu menempel lama
const unsigned long RETRY_INTERVAL = 15000;
const int HTTP_TIMEOUT_MS = 8000;

// ================== TIPE DATA ==================
struct Murid { String uid; String nama; String wa; };
struct Rec   { String uid; time_t tB; time_t tP; };    // berangkat, pulang (hari ini)
struct Antre { String wa; String pesan; };

// ================== VARIABEL GLOBAL ==================
MFRC522 rfid(SS_PIN, RST_PIN);
WebServer server(80);
DNSServer dns;
Preferences prefs;

std::vector<Murid> murid;
std::vector<Rec> hariIni;
long todayKey = -1;

String cfgSsid, cfgPass, cfgToken, cfgBimbel, cfgAdminPw;
int cfgJedaMenit = 60;

bool apAktif = false;
unsigned long apStartMs = 0;
bool ntpStarted = false;
bool mdnsOk = false;

String lastUnknownUid = "";
String lastUnknownWaktu = "";
String lastWaResp = "";

Antre antre[QUEUE_MAX];
int qCount = 0;

String lastUid = "";
unsigned long lastScanMs = 0;
unsigned long lastRetryMs = 0;
unsigned long wifiBootStart = 0;
bool bootWifiDecided = false;
unsigned long lastRfidCheck = 0;

// ================== BUZZER ==================
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
void bipBerangkat()  { beep(1, 200); }
void bipPulang()     { beep(2, 200, 120); }
void bipDitolak()    { beep(4, 40, 60); }
void bipTakDikenal() { beep(3, 80, 80); }
void bipAntre()      { beep(1, 700); }
void bipWaktuBelum() { beep(2, 500, 200); }

// ================== UTIL STRING ==================
String esc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

String bersih(String s) {
  s.replace("|", " ");
  s.replace("\r", " ");
  s.replace("\n", " ");
  s.trim();
  return s;
}

String hanyaAngka(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) if (isDigit(s[i])) o += s[i];
  return o;
}

String normUid(String s) {
  s.trim();
  s.toUpperCase();
  String o;
  for (size_t i = 0; i < s.length(); i++) if (isxdigit(s[i])) o += s[i];
  return o;
}

String normWa(const String& s) {
  String d = hanyaAngka(s);
  if (d.startsWith("62")) d = "0" + d.substring(2);
  else if (d.startsWith("8")) d = "0" + d;
  return d;
}

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

// ================== WAKTU ==================
bool timeReady() { return time(nullptr) > 1700000000; }
time_t nowEpoch() { return time(nullptr); }

long dayKeyOf(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  return (long)(tmv.tm_year + 1900) * 400 + tmv.tm_yday;
}

String formatWaktu(time_t t) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[24];
  strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tmv);
  return String(buf) + " WIB";
}

String formatJam(time_t t) {
  if (t == 0) return "-";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", &tmv);
  return String(buf);
}

// ================== PENGATURAN (FLASH) ==================
void loadSettings() {
  prefs.begin("absen", true);
  cfgSsid    = prefs.getString("ssid", "");
  cfgPass    = prefs.getString("pass", "");
  cfgToken   = prefs.getString("token", "");
  cfgBimbel  = prefs.getString("bimbel", "Bimbel Anda");
  cfgAdminPw = prefs.getString("adminpw", ADMIN_PASS_DEFAULT);
  cfgJedaMenit = prefs.getInt("jeda", 60);
  prefs.end();
}

void saveStr(const char* key, const String& val) {
  prefs.begin("absen", false);
  prefs.putString(key, val);
  prefs.end();
}

void saveJeda(int menit) {
  prefs.begin("absen", false);
  prefs.putInt("jeda", menit);
  prefs.end();
}

// ================== DATA MURID (FLASH) ==================
void loadMurid() {
  murid.clear();
  File f = LittleFS.open("/murid.txt", "r");
  if (!f) return;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() < 5) continue;
    int a = line.indexOf('|');
    int b = line.indexOf('|', a + 1);
    if (a < 0 || b < 0) continue;
    Murid m;
    m.uid = line.substring(0, a);
    m.nama = line.substring(a + 1, b);
    m.wa = line.substring(b + 1);
    murid.push_back(m);
  }
  f.close();
}

void saveMurid() {
  File f = LittleFS.open("/murid.txt", "w");
  if (!f) return;
  for (size_t i = 0; i < murid.size(); i++) {
    f.println(murid[i].uid + "|" + murid[i].nama + "|" + murid[i].wa);
  }
  f.close();
}

int cariMurid(const String& uid) {
  for (size_t i = 0; i < murid.size(); i++) {
    if (murid[i].uid.equalsIgnoreCase(uid)) return (int)i;
  }
  return -1;
}

// ================== LOG ABSEN HARI INI (FLASH) ==================
Rec* cariRec(const String& uid) {
  for (size_t i = 0; i < hariIni.size(); i++) {
    if (hariIni[i].uid.equalsIgnoreCase(uid)) return &hariIni[i];
  }
  return nullptr;
}

void tulisLog(char tipe, const String& uid, time_t ts) {
  File f = LittleFS.open("/log.txt", "a");
  if (!f) return;
  f.printf("%ld|%s|%c|%lu\n", todayKey, uid.c_str(), tipe, (unsigned long)ts);
  f.close();
}

void muatLogHariIni() {
  hariIni.clear();
  String keep = "";
  File f = LittleFS.open("/log.txt", "r");
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      int a = line.indexOf('|');
      int b = line.indexOf('|', a + 1);
      int c = line.indexOf('|', b + 1);
      if (a < 0 || b < 0 || c < 0) continue;
      long dk = line.substring(0, a).toInt();
      if (dk != todayKey) continue;
      String uid = line.substring(a + 1, b);
      char tipe = line.charAt(b + 1);
      time_t ts = (time_t)strtoul(line.substring(c + 1).c_str(), NULL, 10);
      Rec* r = cariRec(uid);
      if (!r) {
        Rec n;
        n.uid = uid;
        n.tB = 0;
        n.tP = 0;
        hariIni.push_back(n);
        r = &hariIni.back();
      }
      if (tipe == 'B') r->tB = ts;
      else if (tipe == 'P') r->tP = ts;
      keep += line + "\n";
    }
    f.close();
  }
  File w = LittleFS.open("/log.txt", "w");
  if (w) {
    w.print(keep);
    w.close();
  }
}

void pastikanHari(time_t now) {
  long dk = dayKeyOf(now);
  if (todayKey == -1) {
    todayKey = dk;
    muatLogHariIni();
  } else if (dk != todayKey) {
    todayKey = dk;
    hariIni.clear();
    LittleFS.remove("/log.txt");
  }
}

// ================== WIFI ==================
bool sambungWiFi(unsigned long timeoutMs) {
  if (cfgSsid.length() == 0) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  Serial.print("Menghubungkan WiFi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void mulaiNTP() {
  if (ntpStarted) return;
  configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
  ntpStarted = true;
}

void mulaiAP() {
  WiFi.disconnect();
  delay(100);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS);
  dns.start(53, "*", WiFi.softAPIP());
  apAktif = true;
  apStartMs = millis();
  if (cfgSsid.length()) {
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  }
  Serial.println("Mode SETUP aktif. WiFi: " + String(AP_SSID) + "  password: " + String(AP_PASS));
  Serial.println("Buka http://" + WiFi.softAPIP().toString());
}

void hentikanAP() {
  dns.stop();
  WiFi.softAPdisconnect(true);
  apAktif = false;
}

// ================== KIRIM WHATSAPP ==================
bool kirimWA(const String& wa, const String& pesan) {
  if (WiFi.status() != WL_CONNECTED || cfgToken.length() == 0) return false;

  WiFiClientSecure client;
  client.setInsecure();   // tidak memverifikasi sertifikat (lihat catatan akhir file)
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, WA_API_URL)) return false;

  http.addHeader("Authorization", cfgToken);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "target=" + urlEncode(wa) + "&message=" + urlEncode(pesan) + "&countryCode=62";
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("WA %s -> HTTP %d : %s\n", wa.c_str(), code, resp.c_str());
  lastWaResp = "HTTP " + String(code) + " : " + resp.substring(0, 300);

  resp.replace(" ", "");
  return (code == 200 && resp.indexOf("\"status\":true") >= 0);
}

void enqueue(const String& wa, const String& pesan) {
  if (qCount >= QUEUE_MAX) {
    for (int i = 1; i < QUEUE_MAX; i++) antre[i - 1] = antre[i];
    qCount--;
  }
  antre[qCount].wa = wa;
  antre[qCount].pesan = pesan;
  qCount++;
  Serial.printf("Masuk antrean (%d)\n", qCount);
}

void flushQueue() {
  if (qCount == 0 || WiFi.status() != WL_CONNECTED) return;
  while (qCount > 0) {
    if (!kirimWA(antre[0].wa, antre[0].pesan)) break;
    for (int i = 1; i < qCount; i++) antre[i - 1] = antre[i];
    qCount--;
    delay(1500);   // jeda antar pesan
  }
}

String buatPesan(const Murid& m, char tipe, time_t ts) {
  String aksi = (tipe == 'B') ? "sudah hadir di bimbel" : "sudah pulang dari bimbel";
  return "Informasi dari " + cfgBimbel + ":\n" + m.nama + " " + aksi + " pada " + formatWaktu(ts) + ".";
}

// ================== PROSES KARTU ==================
void prosesKartu(const String& uid) {
  int idx = cariMurid(uid);
  if (idx < 0) {
    lastUnknownUid = uid;
    lastUnknownWaktu = timeReady() ? formatWaktu(nowEpoch()) : String("");
    Serial.println("Kartu belum terdaftar. Daftarkan lewat halaman admin.");
    bipTakDikenal();
    return;
  }
  if (!timeReady()) {
    Serial.println("Jam belum sinkron (butuh internet), scan diabaikan.");
    bipWaktuBelum();
    return;
  }

  time_t now = nowEpoch();
  pastikanHari(now);

  char tipe;
  Rec* r = cariRec(uid);
  if (!r) {
    Rec n;
    n.uid = uid;
    n.tB = now;
    n.tP = 0;
    hariIni.push_back(n);
    tulisLog('B', uid, now);
    tipe = 'B';
  } else if (r->tP == 0 && (long)(now - r->tB) >= (long)cfgJedaMenit * 60L) {
    r->tP = now;
    tulisLog('P', uid, now);
    tipe = 'P';
  } else {
    Serial.println("Ditolak: double scan atau sudah lengkap hari ini.");
    bipDitolak();
    return;
  }

  Murid m = murid[idx];
  String pesan = buatPesan(m, tipe, now);
  if (kirimWA(m.wa, pesan)) {
    if (tipe == 'B') bipBerangkat(); else bipPulang();
  } else {
    enqueue(m.wa, pesan);
    bipAntre();
  }
}

// ================== WEB: HELPER ==================
bool cekLogin() {
  if (server.authenticate(ADMIN_USER, cfgAdminPw.c_str())) return true;
  server.requestAuthentication(BASIC_AUTH, "Absensi Bimbel");
  return false;
}

void alihkan(const String& url) {
  server.sendHeader("Location", url);
  server.send(303, "text/plain", "");
}

void halamanAwal(const String& judul) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  server.sendContent("<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>");
  server.sendContent(esc(judul));
  server.sendContent("</title><style>"
    "body{font-family:sans-serif;margin:0;background:#f3f4f6;color:#111}"
    "nav{background:#1f2937;padding:10px}nav a{color:#fff;margin-right:12px;text-decoration:none;font-size:14px}"
    ".c{padding:12px;max-width:640px;margin:auto}"
    ".k{background:#fff;border-radius:8px;padding:12px;margin-bottom:12px}"
    "input,select{width:100%;padding:8px;margin:4px 0 10px;box-sizing:border-box;font-size:16px}"
    "button{padding:8px 14px;background:#2563eb;color:#fff;border:0;border-radius:6px;font-size:15px}"
    ".d{background:#dc2626}.w{color:#b45309}.s{font-size:13px;color:#555}"
    "table{width:100%;border-collapse:collapse;font-size:14px}"
    "td,th{border-bottom:1px solid #e5e7eb;padding:6px;text-align:left}"
    "</style></head><body><nav>"
    "<a href='/'>Beranda</a><a href='/murid'>Murid</a><a href='/riwayat'>Riwayat</a>"
    "<a href='/pengaturan'>Pengaturan</a><a href='/wifi'>WiFi</a></nav><div class='c'>");
}

void halamanAkhir() {
  server.sendContent("</div></body></html>");
  server.sendContent("");
}

void pesanHalaman(const String& judul, const String& isi, const String& kembali) {
  halamanAwal(judul);
  server.sendContent("<div class='k'><b>" + esc(judul) + "</b><p>" + isi + "</p><a href='" + kembali + "'>Kembali</a></div>");
  halamanAkhir();
}

// ================== WEB: HALAMAN ==================
void handleBeranda() {
  if (!cekLogin()) return;
  int pulang = 0;
  for (size_t i = 0; i < hariIni.size(); i++) if (hariIni[i].tP != 0) pulang++;

  halamanAwal("Beranda");
  String s = "<div class='k'><b>" + esc(cfgBimbel) + "</b><br>WiFi: ";
  if (WiFi.status() == WL_CONNECTED) s += esc(WiFi.SSID()) + " (" + WiFi.localIP().toString() + ")";
  else s += "<span class='w'>tidak tersambung</span>";
  s += "<br>Waktu: ";
  if (timeReady()) s += formatWaktu(nowEpoch());
  else s += "<span class='w'>belum sinkron (butuh internet)</span>";
  s += "<br>Jumlah murid: " + String(murid.size());
  s += "<br>Berangkat hari ini: " + String(hariIni.size()) + " | Pulang: " + String(pulang);
  s += "<br>Pesan WA mengantre: " + String(qCount);
  s += "<br>Jeda berangkat ke pulang: " + String(cfgJedaMenit) + " menit</div>";
  if (cfgToken.length() == 0) s += "<div class='k w'>Token Fonnte belum diisi. Isi di menu Pengaturan.</div>";
  if (cfgAdminPw == ADMIN_PASS_DEFAULT) s += "<div class='k w'>Password admin masih bawaan. Segera ganti di menu Pengaturan.</div>";
  if (apAktif) s += "<div class='k w'>Mode setup aktif (WiFi " + String(AP_SSID) + "). Atur WiFi bimbel di menu WiFi.</div>";
  server.sendContent(s);
  halamanAkhir();
}

void handleMurid() {
  if (!cekLogin()) return;
  int e = server.hasArg("edit") ? server.arg("edit").toInt() : -1;
  if (e < 0 || e >= (int)murid.size()) e = -1;

  String fUid = "", fNama = "", fWa = "";
  if (e >= 0) {
    fUid = murid[e].uid;
    fNama = murid[e].nama;
    fWa = murid[e].wa;
  } else {
    fUid = lastUnknownUid;
  }

  halamanAwal("Murid");
  String s = "<div class='k'><b>" + String(e >= 0 ? "Ubah murid" : "Tambah murid") + "</b>";
  if (e < 0) {
    s += "<p class='s'>Daftar kartu baru: tap kartu ke alat, lalu muat ulang halaman ini. UID kartu terakhir yang belum terdaftar terisi otomatis.</p>";
    if (lastUnknownUid.length()) {
      s += "<p class='s'>Kartu terakhir belum terdaftar: <b>" + esc(lastUnknownUid) + "</b> " + esc(lastUnknownWaktu) + "</p>";
    }
  }
  s += "<form method='post' action='/murid/simpan'>"
       "<input type='hidden' name='idx' value='" + String(e) + "'>"
       "UID kartu<input name='uid' value='" + esc(fUid) + "' placeholder='contoh: EB47D5E7'>"
       "Nama anak<input name='nama' maxlength='40' value='" + esc(fNama) + "'>"
       "WhatsApp orang tua<input name='wa' inputmode='numeric' value='" + esc(fWa) + "' placeholder='08xxxxxxxxxx'>"
       "<button>Simpan</button>";
  if (e >= 0) s += " <a href='/murid'>Batal</a>";
  s += "</form></div>";
  server.sendContent(s);

  server.sendContent("<div class='k'><b>Daftar murid (" + String(murid.size()) + ")</b><table>");
  for (size_t i = 0; i < murid.size(); i++) {
    Rec* r = cariRec(murid[i].uid);
    String st = "belum hadir";
    if (r) st = "B " + formatJam(r->tB) + " / P " + formatJam(r->tP);
    String row = "<tr><td><b>" + esc(murid[i].nama) + "</b><br><span class='s'>" + esc(murid[i].uid) +
                 " | " + esc(murid[i].wa) + "<br>" + st + "</span></td><td>"
                 "<a href='/murid?edit=" + String(i) + "'>Ubah</a>"
                 "<form method='post' action='/murid/hapus' onsubmit=\"return confirm('Hapus murid ini?')\">"
                 "<input type='hidden' name='idx' value='" + String(i) + "'>"
                 "<button class='d'>Hapus</button></form></td></tr>";
    server.sendContent(row);
  }
  server.sendContent("</table></div>");
  halamanAkhir();
}

void handleMuridSimpan() {
  if (!cekLogin()) return;
  String uid = normUid(server.arg("uid"));
  String nama = bersih(server.arg("nama"));
  String wa = normWa(server.arg("wa"));
  int idx = server.hasArg("idx") ? server.arg("idx").toInt() : -1;

  String err = "";
  if (uid.length() < 4) err = "UID tidak valid (minimal 4 karakter 0-9 atau A-F).";
  else if (nama.length() == 0) err = "Nama wajib diisi.";
  else if (wa.length() < 9) err = "Nomor WhatsApp tidak valid.";
  else {
    int dup = cariMurid(uid);
    if (dup >= 0 && dup != idx) err = "UID sudah terdaftar atas nama " + esc(murid[dup].nama) + ".";
    else if (idx < 0 && (int)murid.size() >= MAX_MURID) err = "Jumlah murid sudah maksimal.";
  }
  if (err.length()) {
    pesanHalaman("Gagal menyimpan", err, "/murid");
    return;
  }

  if (idx >= 0 && idx < (int)murid.size()) {
    murid[idx].uid = uid;
    murid[idx].nama = nama;
    murid[idx].wa = wa;
  } else {
    Murid m;
    m.uid = uid;
    m.nama = nama;
    m.wa = wa;
    murid.push_back(m);
  }
  saveMurid();
  if (lastUnknownUid.equalsIgnoreCase(uid)) lastUnknownUid = "";
  alihkan("/murid");
}

void handleMuridHapus() {
  if (!cekLogin()) return;
  int idx = server.arg("idx").toInt();
  if (idx >= 0 && idx < (int)murid.size()) {
    murid.erase(murid.begin() + idx);
    saveMurid();
  }
  alihkan("/murid");
}

void handleRiwayat() {
  if (!cekLogin()) return;
  halamanAwal("Riwayat");
  String s = "<div class='k'><b>Riwayat hari ini</b><table><tr><th>Nama</th><th>Berangkat</th><th>Pulang</th></tr>";
  for (size_t i = 0; i < hariIni.size(); i++) {
    int m = cariMurid(hariIni[i].uid);
    String nama = (m >= 0) ? murid[m].nama : hariIni[i].uid;
    s += "<tr><td>" + esc(nama) + "</td><td>" + formatJam(hariIni[i].tB) + "</td><td>" + formatJam(hariIni[i].tP) + "</td></tr>";
  }
  s += "</table></div><div class='k'><b>Belum hadir</b><br>";
  int n = 0;
  for (size_t i = 0; i < murid.size(); i++) {
    if (!cariRec(murid[i].uid)) {
      s += esc(murid[i].nama) + "<br>";
      n++;
    }
  }
  if (n == 0) s += "-";
  s += "</div>";
  server.sendContent(s);
  halamanAkhir();
}

void handlePengaturan() {
  if (!cekLogin()) return;
  halamanAwal("Pengaturan");
  String s = "<div class='k'><b>Pengaturan</b>"
    "<form method='post' action='/pengaturan/simpan'>"
    "Nama bimbel<input name='bimbel' maxlength='40' value='" + esc(cfgBimbel) + "'>"
    "Token Fonnte " + String(cfgToken.length() ? "(sudah terisi, kosongkan jika tidak diganti)" : "(belum diisi)") +
    "<input name='token' type='password' autocomplete='off'>"
    "Jeda berangkat ke pulang (menit)<input name='jeda' type='number' min='1' max='720' value='" + String(cfgJedaMenit) + "'>"
    "Password admin baru (minimal 6 karakter, kosongkan jika tidak diganti)"
    "<input name='adminpw' type='password' autocomplete='off'>"
    "<button>Simpan</button></form></div>"
    "<div class='k'><b>Tes kirim WhatsApp</b>"
    "<form method='post' action='/tes'>"
    "Nomor tujuan<input name='nomor' inputmode='numeric' placeholder='08xxxxxxxxxx'>"
    "<button>Kirim tes</button></form></div>";
  server.sendContent(s);
  halamanAkhir();
}

void handlePengaturanSimpan() {
  if (!cekLogin()) return;
  String bimbel = bersih(server.arg("bimbel"));
  if (bimbel.length()) {
    cfgBimbel = bimbel;
    saveStr("bimbel", cfgBimbel);
  }
  String token = server.arg("token");
  token.trim();
  if (token.length()) {
    cfgToken = token;
    saveStr("token", cfgToken);
  }
  int jeda = server.arg("jeda").toInt();
  if (jeda >= 1 && jeda <= 720) {
    cfgJedaMenit = jeda;
    saveJeda(jeda);
  }
  String pw = server.arg("adminpw");
  if (pw.length() >= 6) {
    cfgAdminPw = pw;
    saveStr("adminpw", cfgAdminPw);
    pesanHalaman("Tersimpan", "Pengaturan tersimpan. Password admin diganti, login ulang diminta saat membuka halaman berikutnya.", "/");
    return;
  }
  if (pw.length() > 0) {
    pesanHalaman("Sebagian tersimpan", "Password admin tidak diganti karena kurang dari 6 karakter. Pengaturan lain tersimpan.", "/pengaturan");
    return;
  }
  alihkan("/pengaturan");
}

void handleTes() {
  if (!cekLogin()) return;
  String nomor = normWa(server.arg("nomor"));
  if (nomor.length() < 9) {
    pesanHalaman("Gagal", "Nomor tidak valid.", "/pengaturan");
    return;
  }
  bool ok = kirimWA(nomor, "Tes dari alat absensi " + cfgBimbel + ".");
  String isi = ok ? "Pesan tes diterima gateway. Cek WhatsApp nomor tujuan." : "Gagal mengirim.";
  isi += "<br><span class='s'>" + esc(lastWaResp) + "</span>";
  pesanHalaman(ok ? "Berhasil" : "Gagal", isi, "/pengaturan");
}

void handleWifi() {
  if (!cekLogin()) return;
  int n = WiFi.scanNetworks();
  halamanAwal("WiFi");
  String s = "<div class='k'><b>Atur WiFi</b><p class='s'>WiFi saat ini: " +
             (cfgSsid.length() ? esc(cfgSsid) : String("(belum diatur)")) +
             ". ESP32 hanya mendukung WiFi 2,4 GHz.</p>"
             "<form method='post' action='/wifi/simpan'>"
             "Pilih WiFi<select name='pilih'><option value=''>-- pilih --</option>";
  String seen = "|";
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.length() == 0 || seen.indexOf("|" + name + "|") >= 0) continue;
    seen += name + "|";
    s += "<option value='" + esc(name) + "'>" + esc(name) + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
  s += "</select>Atau ketik nama WiFi<input name='manual' placeholder='nama WiFi'>"
       "Password WiFi<input name='pass' type='password' autocomplete='off'>"
       "<button>Simpan dan restart</button></form></div>";
  server.sendContent(s);
  halamanAkhir();
}

void handleWifiSimpan() {
  if (!cekLogin()) return;
  String ssid = server.arg("manual");
  ssid.trim();
  if (ssid.length() == 0) ssid = server.arg("pilih");
  if (ssid.length() == 0) {
    pesanHalaman("Gagal", "Nama WiFi belum dipilih atau diisi.", "/wifi");
    return;
  }
  cfgSsid = ssid;
  cfgPass = server.arg("pass");
  saveStr("ssid", cfgSsid);
  saveStr("pass", cfgPass);
  pesanHalaman("Tersimpan", "Alat akan restart dan mencoba tersambung ke WiFi baru. Alamat IP bisa berubah: lihat di daftar perangkat router, atau coba http://absensi.local. Jika gagal tersambung, WiFi Absensi-Setup muncul lagi.", "/");
  delay(1500);
  ESP.restart();
}

void handleNotFound() {
  if (apAktif) {
    alihkan("http://" + WiFi.softAPIP().toString() + "/");
  } else {
    server.send(404, "text/plain", "Tidak ditemukan");
  }
}

// ================== TOMBOL BOOT (RESET) ==================
void resetPengaturan() {
  prefs.begin("absen", false);
  prefs.remove("ssid");
  prefs.remove("pass");
  prefs.putString("adminpw", ADMIN_PASS_DEFAULT);
  prefs.end();
  beep(3, 400, 200);
  // Tunggu tombol dilepas dulu; menahan BOOT saat restart membuat ESP32 masuk mode flashing
  while (digitalRead(BOOT_PIN) == LOW) delay(50);
  delay(300);
  ESP.restart();
}

void cekTombolBoot() {
  static unsigned long tekan = 0;
  if (digitalRead(BOOT_PIN) == LOW) {
    if (tekan == 0) tekan = millis();
    else if (millis() - tekan > 5000) resetPengaturan();
  } else {
    tekan = 0;
  }
}

// ================== RFID ==================
String readUid() {
  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    if (rfid.uid.uidByte[i] < 0x10) uid += "0";
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  return uid;
}

// ================== INISIALISASI RFID & WIFI SAAT BOOT ==================
// Inisialisasi RFID diulang sampai modul merespon (penting saat listrik baru menyala,
// misalnya dari adaptor/charger, bukan dari USB komputer).
bool initRFID() {
  for (int i = 0; i < 5; i++) {
    rfid.PCD_Init();
    delay(100);
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.print("MFRC522 versi: 0x");
    Serial.println(v, HEX);
    if (v != 0x00 && v != 0xFF) return true;
    delay(300);
  }
  return false;
}

// WiFi dicoba di latar belakang supaya kartu tetap bisa discan selama proses ini.
void mulaiWiFiBoot() {
  wifiBootStart = millis();
  bootWifiDecided = false;
  if (cfgSsid.length() == 0) {
    mulaiAP();
    bootWifiDecided = true;
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  Serial.println("Menghubungkan WiFi di latar belakang...");
}

void cekWiFiBoot() {
  if (bootWifiDecided) return;
  if (WiFi.status() == WL_CONNECTED) {
    bootWifiDecided = true;
    Serial.println("WiFi terhubung: " + WiFi.localIP().toString());
    mulaiNTP();
    Serial.println("Halaman admin: http://" + WiFi.localIP().toString() + "  (user: admin)");
  } else if (millis() - wifiBootStart > 20000) {
    bootWifiDecided = true;
    Serial.println("WiFi belum tersambung.");
    mulaiAP();
  }
}

// ================== SETUP & LOOP ==================
void setup() {
  Serial.begin(115200);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(BOOT_PIN, INPUT_PULLUP);
  digitalWrite(BUZZER_PIN, LOW);

  if (!LittleFS.begin(true)) Serial.println("LittleFS gagal dimulai");
  loadSettings();
  loadMurid();

  delay(1000);   // beri waktu modul RFID stabil saat listrik baru menyala
  SPI.begin(18, 19, 23, SS_PIN);
  if (!initRFID()) {
    Serial.println("RFID tidak terbaca, cek wiring dan daya.");
    beep(5, 60, 60);
  }

  mulaiWiFiBoot();

  server.on("/", HTTP_GET, handleBeranda);
  server.on("/murid", HTTP_GET, handleMurid);
  server.on("/murid/simpan", HTTP_POST, handleMuridSimpan);
  server.on("/murid/hapus", HTTP_POST, handleMuridHapus);
  server.on("/riwayat", HTTP_GET, handleRiwayat);
  server.on("/pengaturan", HTTP_GET, handlePengaturan);
  server.on("/pengaturan/simpan", HTTP_POST, handlePengaturanSimpan);
  server.on("/tes", HTTP_POST, handleTes);
  server.on("/wifi", HTTP_GET, handleWifi);
  server.on("/wifi/simpan", HTTP_POST, handleWifiSimpan);
  server.onNotFound(handleNotFound);
  server.begin();

  beep(2, 80, 80);
  Serial.println("Siap. Tempelkan kartu...");
}

void loop() {
  server.handleClient();
  if (apAktif) dns.processNextRequest();
  cekTombolBoot();
  cekWiFiBoot();

  // Pastikan modul RFID tetap hidup; inisialisasi ulang jika tidak merespon
  if (millis() - lastRfidCheck > 3000) {
    lastRfidCheck = millis();
    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    if (v == 0x00 || v == 0xFF) {
      Serial.println("RFID tidak merespon, inisialisasi ulang");
      initRFID();
    } else {
      rfid.PCD_AntennaOn();
    }
  }

  // Pemeliharaan WiFi
  static unsigned long lastWifiCheck = 0;
  if (millis() - lastWifiCheck > 30000) {
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED && cfgSsid.length()) WiFi.reconnect();
  }
  if (WiFi.status() == WL_CONNECTED) {
    mulaiNTP();
    if (!mdnsOk) {
      mdnsOk = MDNS.begin("absensi");
      if (mdnsOk) MDNS.addService("http", "tcp", 80);
    }
    if (apAktif && millis() - apStartMs > 600000) hentikanAP();
  }

  // Ganti hari / muat log saat jam pertama kali tersedia
  static unsigned long lastDayCheck = 0;
  if (millis() - lastDayCheck > 30000) {
    lastDayCheck = millis();
    if (timeReady()) pastikanHari(nowEpoch());
  }

  // Kirim ulang antrean WA
  if (millis() - lastRetryMs > RETRY_INTERVAL) {
    lastRetryMs = millis();
    flushQueue();
  }

  // Baca kartu
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

  Serial.println("UID: " + uid);
  prosesKartu(uid);
}

/*
  CATATAN
  - Pola bunyi buzzer:
      1x sedang      = berangkat tercatat
      2x sedang      = pulang tercatat
      4x sangat cepat = ditolak (double scan sebelum jeda, atau sudah lengkap hari ini)
      3x cepat       = kartu belum terdaftar
      1x panjang     = tercatat, tapi WA gagal terkirim (masuk antrean, dikirim ulang otomatis)
      2x panjang     = jam belum sinkron (butuh internet), scan diabaikan
  - Scan memerlukan jam yang sinkron lewat internet (NTP). Setelah sekali sinkron,
    jam tetap berjalan walau internet putus; setelah listrik mati butuh internet lagi.
  - Antrean WA disimpan di RAM (hilang jika listrik mati). Catatan berangkat/pulang
    hari ini tersimpan di flash, jadi tidak ganda setelah restart.
  - Halaman admin memakai HTTP biasa dan hanya bisa dibuka dari WiFi yang sama.
  - client.setInsecure() tidak memverifikasi sertifikat server; untuk lebih aman
    pakai setCACert() dengan root CA gateway.
  - Beri tahu orang tua bahwa nomor mereka dipakai untuk notifikasi absensi.
*/
