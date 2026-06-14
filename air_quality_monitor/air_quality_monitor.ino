/**
 * ============================================================
 *   SISTEM MONITORING & CONTROL KUALITAS UDARA - SMOKING ROOM
 *   Berbasis IoT dengan ESP32 + Firebase Realtime Database
 *
 *   Versi 3.0 — Perbaikan bug + Kalibrasi Otomatis MQ-135:
 *     - MQ-135 : kalibrasi otomatis Ro di udara bersih (startup)
 *                + simpan/muat Ro dari NVS (Preferences)
 *     - PMS5003: PM1.0 + PM2.5 + PM10, sliding window 5 sampel
 *                (bug fix: buffer full detection & key PM1.0)
 *     - DHT22  : filter nilai NaN berturut-turut
 *     - Firebase: kondisiBuruk konsisten (PM2.5 + PM10 + CO)
 *     - Threshold berbasis Permen LHK No.14/2020
 *
 * ============================================================
 *  Hardware:
 *    - ESP32
 *    - Sensor PMS5003  (TX → GPIO16 / RX2, RX → GPIO17 / TX2)
 *    - Sensor MQ-135   (AOUT → GPIO34)
 *    - Sensor DHT22    (DATA → GPIO27)
 *    - Relay Fan       (IN   → GPIO14)  Active HIGH
 *    - Relay Purifier  (IN   → GPIO12)  Active HIGH
 * ============================================================
 *
 *  KALIBRASI OTOMATIS MQ-135:
 *  -------------------------------------------------
 *  Saat boot pertama kali (atau jika belum ada nilai Ro yang
 *  tersimpan di flash), sistem akan otomatis masuk mode kalibrasi:
 *
 *  1. Pastikan sensor SUDAH warm-up minimal 24 jam (burn-in awal).
 *  2. Letakkan sensor di udara BERSIH (luar ruangan / ber-AC).
 *  3. Nyalakan ESP32 — sistem akan mengukur Rs selama
 *     AUTOCALIB_DURATION_SEC detik, lalu menghitung Ro = Rs / 3.6.
 *  4. Nilai Ro disimpan ke NVS flash; tidak perlu kalibrasi ulang
 *     setelah restart kecuali kamu menekan tombol kalibrasi paksa.
 *
 *  Kalibrasi ulang paksa:
 *    - Tahan tombol BOOT (GPIO0) saat power-on hingga LED berkedip,
 *      ATAU
 *    - Set /config/calibration/trigger = true di Firebase.
 *
 *  Kalibrasi dari jarak jauh (Firebase):
 *    - Set node  /config/calibration/trigger = true
 *    - Sistem akan masuk kalibrasi pada siklus berikutnya,
 *      reset node menjadi false setelah selesai,
 *      dan mengunggah Ro baru ke /config/calibration/ro_ohm.
 * ============================================================
 */

#include <WiFi.h>
#include <FirebaseESP32.h>
#include <DHT.h>
#include <HardwareSerial.h>
#include <Preferences.h>   // NVS flash storage untuk Ro
#include <time.h>
#include <math.h>

// ============================================================
//  KONFIGURASI WIFI & FIREBASE
// ============================================================
#define WIFI_SSID        "Ziazaidan"
#define WIFI_PASSWORD    "01051977"
#define FIREBASE_HOST    "https://asap-f023f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH    "8bUCYVgs9C0mvIeIBuH0mUFaxtfWQOwOfFG1ElYS"
#define NTP_SERVER       "pool.ntp.org"
#define GMT_OFFSET_SEC   25200   // WIB = UTC+7
#define DAYLIGHT_OFFSET  0

// ============================================================
//  PIN
// ============================================================
#define PIN_MQ135        34
#define PIN_DHT22        27
#define PIN_RELAY_FAN    14
#define PIN_RELAY_PURIF  12
#define PIN_BOOT_BTN     0    // Tombol BOOT bawaan ESP32

// ============================================================
//  KONSTANTA WAKTU
// ============================================================
#define INTERVAL_SEND        2000UL    // ms — kirim ke Firebase
#define INTERVAL_WIFI_CHECK  10000UL  // ms — cek koneksi WiFi
#define INTERVAL_FB_CHECK    15000UL  // ms — cek koneksi Firebase
#define DHT_TYPE             DHT22

// ============================================================
//  THRESHOLD DEFAULT (Permen LHK No.14/2020)
// ============================================================
#define DEFAULT_ASAP_MAX          7    // ppm CO  — batas Sedang→Tidak Sehat per ISPU
#define DEFAULT_PM25_TIDAK_SEHAT  56   // µg/m³   — batas Tidak Sehat PM2.5 per ISPU
#define DEFAULT_PM10_TIDAK_SEHAT  151  // µg/m³   — batas Tidak Sehat PM10 per ISPU

// ============================================================
//  KALIBRASI MQ-135
// ============================================================
/**
 * Rangkaian MQ-135 di modul biasanya:
 *   VCC (5V) — [Rs sensor] — AOUT — [RL=10kΩ ke GND]
 *
 * Formula:
 *   Vout = ADC * (3.3 / 4095)          → tegangan di AOUT
 *   Rs   = RL * (Vcc - Vout) / Vout   → resistansi sensor
 *   ratio = Rs / Ro                    → rasio terhadap udara bersih
 *   ppm  = a * pow(ratio, b)           → konversi ke ppm (kurva datasheet)
 */
#define MQ135_VCC              5.0      // Tegangan supply sensor (Volt)
#define MQ135_RL               10000.0  // Resistor beban di modul (Ohm) — biasanya 10kΩ
#define MQ135_RATIO_CLEAN_AIR  3.6      // Rs/Ro di udara bersih (dari datasheet MQ-135)

/**
 * Nilai Ro fallback jika kalibrasi belum pernah dilakukan.
 * Nilai 10000 Ω adalah perkiraan umum untuk MQ-135 baru.
 * Setelah kalibrasi otomatis, nilai ini TIDAK digunakan.
 */
#define MQ135_RO_DEFAULT       10000.0  // Ohm — hanya dipakai sebelum kalibrasi pertama

/**
 * Kurva sensitivitas CO dari datasheet MQ-135:
 *   ppm = A * (Rs/Ro) ^ B
 *   Berdasarkan kurva CO di datasheet:
 *     A = 605.18, B = -3.937
 */
#define MQ135_CO_A   605.18
#define MQ135_CO_B   -3.937

/**
 * Durasi kalibrasi otomatis (detik).
 * Selama ini sensor disampling setiap 500ms, hasilnya dirata-rata.
 * Minimal 60 detik dianjurkan. Default 120 detik (2 menit).
 */
#define AUTOCALIB_DURATION_SEC  120

/**
 * Batas bawah Rs yang masuk akal untuk MQ-135 (Ohm).
 * Jika Rs < nilai ini, kemungkinan sensor terbalik / kabel putus.
 */
#define MQ135_RS_MIN  1000.0   // Ohm
#define MQ135_RS_MAX  200000.0 // Ohm

// ============================================================
//  KALIBRASI PMS5003
// ============================================================
#define PMS_USE_HUMIDITY_CORRECTION  true
#define PMS_WINDOW_SIZE              5   // Jumlah sampel untuk moving average

// ============================================================
//  OBJEK GLOBAL
// ============================================================
FirebaseData   fbData;
FirebaseAuth   fbAuth;
FirebaseConfig fbConfig;

DHT         dht(PIN_DHT22, DHT_TYPE);
HardwareSerial pmsSerial(2);
Preferences prefs;  // NVS namespace untuk simpan Ro

// ============================================================
//  VARIABEL SENSOR & STATUS
// ============================================================
float    g_suhu         = 0.0;
float    g_kelembaban   = 0.0;
float    g_co_ppm       = 0.0;
uint16_t g_pm25_raw     = 0;
float    g_pm25         = 0.0;
uint16_t g_pm10_raw     = 0;
float    g_pm10         = 0.0;
uint16_t g_pm1_raw      = 0;
float    g_rs_mq135     = 0.0;
float    g_ro_mq135     = MQ135_RO_DEFAULT;  // Diisi dari NVS atau kalibrasi
bool     g_calibrated   = false;             // True jika Ro sudah valid dari kalibrasi

// Moving average buffer PM2.5 — FIX: simpan count terpisah
float    g_pm25Buffer[PMS_WINDOW_SIZE];
int      g_pm25BufIdx   = 0;
int      g_pm25BufCount = 0;   // FIX: jumlah sampel valid, bukan flag boolean

// Moving average buffer PM10
float    g_pm10Buffer[PMS_WINDOW_SIZE];
int      g_pm10BufIdx   = 0;
int      g_pm10BufCount = 0;

int g_threshAsapMax    = DEFAULT_ASAP_MAX;
int g_threshPM25Sehat  = DEFAULT_PM25_TIDAK_SEHAT;
int g_threshPM10Sehat  = DEFAULT_PM10_TIDAK_SEHAT;

float g_pm25_multiplier   = 1.67f; // Faktor kalibrasi PM2.5 (ref_val / raw_val = 15/9 ≈ 1.67)
float g_pm10_multiplier   = 2.20f; // Faktor kalibrasi PM10 (ref_val / raw_val = 22/10 = 2.20)
bool  g_use_humidity_corr = false; // Koreksi kelembapan OFF — agar stabil sejajar detektor fisik

bool g_relayFan   = false;
bool g_relayPurif = false;

unsigned long g_lastSend      = 0;
unsigned long g_lastWifiCheck = 0;
unsigned long g_lastFbCheck   = 0;

// ============================================================
//  PROTOTYPING
// ============================================================
struct IspuBP { float CaLow; float CaHigh; int IaLow; int IaHigh; };
static int _calcISPU(float Ca, const IspuBP* tbl, int n);

void   connectWiFi();
void   connectFirebase();
void   syncThresholds();
void   checkRemoteCalibTrigger();
void   runAutoCalibration(bool uploadToFirebase);
bool   loadRoFromNVS();
void   saveRoToNVS(float ro);
bool   readPMS5003(uint16_t &pm1raw, uint16_t &pm25raw, uint16_t &pm10raw);
void   readDHT22();
void   readMQ135();
float  calculateRs(int adcRaw);
float  calculateCO_ppm(float rs);
float  correctParticleHumidity(float pmRaw, float humidity);
float  movingAvgPM25(float newVal);
float  movingAvgPM10(float newVal);
void   controlRelay(bool fan, bool purifier);
void   sendDataToFirebase();
String getTimestamp();
String getCategoryPM25(float pm25);
String getCategoryPM10(float pm10);
String getCategoryCO(float co_ppm);
int    hitungISPU_PM25(float pm25);
int    hitungISPU_PM10(float pm10);
int    getISPUValue();

// ============================================================
//  SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==========================================");
  Serial.println("  Air Quality Monitor - Smoking Room v3.0");
  Serial.println("==========================================\n");

  // --- Init pin ---
  pinMode(PIN_RELAY_FAN,   OUTPUT);
  pinMode(PIN_RELAY_PURIF, OUTPUT);
  pinMode(PIN_BOOT_BTN,    INPUT_PULLUP);
  digitalWrite(PIN_RELAY_FAN,   LOW);
  digitalWrite(PIN_RELAY_PURIF, LOW);

  // --- Init sensor ---
  dht.begin();
  Serial.println("[DHT22] Inisialisasi selesai.");

  pmsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("[PMS5003] UART2 siap.");

  // Inisialisasi buffer moving average ke nol
  memset(g_pm25Buffer, 0, sizeof(g_pm25Buffer));
  memset(g_pm10Buffer, 0, sizeof(g_pm10Buffer));

  // --- MQ-135 warm-up (30 detik) ---
  Serial.print("[MQ135] Pemanasan awal (30 detik)");
  for (int i = 0; i < 6; i++) {
    delay(5000);
    Serial.print(".");
  }
  Serial.println(" Selesai!");

  // --- Coba muat Ro dari NVS flash ---
  bool forceCalib = (digitalRead(PIN_BOOT_BTN) == LOW);  // Tombol BOOT ditahan
  if (forceCalib) {
    Serial.println("\n[KALIB] Tombol BOOT ditahan → Kalibrasi ulang paksa!");
  }

  if (!forceCalib && loadRoFromNVS()) {
    Serial.printf("[KALIB] Ro dimuat dari flash: %.0f Ω\n", g_ro_mq135);
    g_calibrated = true;
  } else {
    // Belum ada nilai Ro tersimpan → jalankan kalibrasi otomatis
    Serial.println("[KALIB] Nilai Ro belum tersimpan. Memulai kalibrasi otomatis...");
    Serial.println("        Pastikan sensor berada di UDARA BERSIH!\n");

    // Sambungkan WiFi & Firebase lebih dulu agar timestamp tersedia
    connectWiFi();
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
    struct tm ti;
    for (int i = 0; i < 10 && !getLocalTime(&ti); i++) delay(500);
    connectFirebase();
    syncThresholds();

    runAutoCalibration(true);  // upload hasil ke Firebase
  }

  // --- Koneksi (jika belum terhubung dari path kalibrasi) ---
  connectWiFi();

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET, NTP_SERVER);
  Serial.print("[NTP] Sinkronisasi waktu");
  struct tm timeinfo;
  for (int i = 0; i < 10; i++) {
    if (getLocalTime(&timeinfo)) {
      Serial.println(" OK.");
      break;
    }
    Serial.print(".");
    delay(500);
  }

  connectFirebase();
  syncThresholds();

  Serial.println("\n[SYSTEM] Setup selesai. Mulai monitoring...");
  Serial.printf("[KALIB] Ro aktif: %.0f Ω | Kalibrasi: %s\n\n",
                g_ro_mq135, g_calibrated ? "✓ Terverifikasi" : "⚠ Default");
}

// ============================================================
//  LOOP UTAMA
// ============================================================
void loop() {
  unsigned long now = millis();

  // --- Cek koneksi WiFi ---
  if (now - g_lastWifiCheck >= INTERVAL_WIFI_CHECK) {
    g_lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Terputus! Reconnecting...");
      connectWiFi();
    }
  }

  // --- Cek koneksi Firebase ---
  if (now - g_lastFbCheck >= INTERVAL_FB_CHECK) {
    g_lastFbCheck = now;
    if (Firebase.ready()) {
      checkRemoteCalibTrigger();  // Cek permintaan kalibrasi dari Firebase
    } else {
      Serial.println("[Firebase] Tidak siap! Reconnecting...");
      connectFirebase();
    }
  }

  // --- Siklus baca & kirim ---
  if (now - g_lastSend >= INTERVAL_SEND) {
    g_lastSend = now;

    // 1. Baca semua sensor
    readDHT22();
    readMQ135();

    uint16_t pm1raw = 0, pm25raw = 0, pm10raw = 0;
    bool pmOK = readPMS5003(pm1raw, pm25raw, pm10raw);
    if (pmOK) {
      g_pm1_raw  = pm1raw;
      g_pm25_raw = pm25raw;
      g_pm10_raw = pm10raw;

      float pm25corr = correctParticleHumidity((float)g_pm25_raw, g_kelembaban);
      g_pm25 = movingAvgPM25(pm25corr) * g_pm25_multiplier;

      float pm10corr = correctParticleHumidity((float)g_pm10_raw, g_kelembaban);
      g_pm10 = movingAvgPM10(pm10corr) * g_pm10_multiplier;
    } else {
      Serial.println("[PMS5003] Gagal baca, nilai sebelumnya dipertahankan.");
    }

    // 2. Sync threshold dari Firebase
    syncThresholds();

    // 3. Logika kontrol relay — trigger jika PM2.5 ATAU PM10 ATAU CO melebihi batas
    bool kondisiBuruk = (g_pm25   > (float)g_threshPM25Sehat) ||
                        (g_pm10   > (float)g_threshPM10Sehat) ||
                        (g_co_ppm > (float)g_threshAsapMax);

    g_relayFan   = kondisiBuruk;
    g_relayPurif = kondisiBuruk;

    // 4. Terapkan relay
    controlRelay(g_relayFan, g_relayPurif);

    // 5. Kirim ke Firebase
    sendDataToFirebase();

    // 6. Debug Serial Monitor
    Serial.println("========== Sensor Update ==========");
    Serial.printf("  Suhu          : %.1f °C\n",               g_suhu);
    Serial.printf("  Kelembaban    : %.1f %%\n",                g_kelembaban);
    Serial.printf("  MQ135 Rs      : %.0f Ω\n",                g_rs_mq135);
    Serial.printf("  MQ135 Ro      : %.0f Ω (%s)\n",           g_ro_mq135,
                                                                 g_calibrated ? "kalibrasi" : "default");
    Serial.printf("  CO (ppm)      : %.2f | Batas: %d ppm\n",  g_co_ppm, g_threshAsapMax);
    Serial.printf("  Kategori CO   : %s\n",                     getCategoryCO(g_co_ppm).c_str());
    Serial.printf("  PM1.0 raw     : %d µg/m³\n",               g_pm1_raw);
    Serial.printf("  PM2.5 raw     : %d µg/m³\n",               g_pm25_raw);
    Serial.printf("  PM2.5 koreksi : %.1f µg/m³ | Batas: %d\n",g_pm25, g_threshPM25Sehat);
    Serial.printf("  Kategori PM2.5: %s\n",                     getCategoryPM25(g_pm25).c_str());
    Serial.printf("  PM10  raw     : %d µg/m³\n",               g_pm10_raw);
    Serial.printf("  PM10  koreksi : %.1f µg/m³ | Batas: %d\n",g_pm10, g_threshPM10Sehat);
    Serial.printf("  Kategori PM10 : %s\n",                     getCategoryPM10(g_pm10).c_str());
    Serial.printf("  Kondisi       : %s\n",                     kondisiBuruk ? "BURUK" : "BAIK");
    Serial.printf("  Relay Fan     : %s\n",                     g_relayFan   ? "ON" : "OFF");
    Serial.printf("  Relay Purif   : %s\n",                     g_relayPurif ? "ON" : "OFF");
    Serial.println("====================================\n");
  }
}

// ============================================================
//  KALIBRASI OTOMATIS MQ-135
// ============================================================
/**
 * Mengukur Rs rata-rata selama AUTOCALIB_DURATION_SEC detik
 * di udara bersih, lalu menghitung Ro = Rs_avg / RATIO_CLEAN_AIR.
 *
 * Proses:
 *   1. Sampling ADC setiap 500ms selama durasi yang ditentukan.
 *   2. Hitung rata-rata Rs dari semua sampel yang valid.
 *   3. Ro = Rs_avg / MQ135_RATIO_CLEAN_AIR (3.6 per datasheet).
 *   4. Validasi: Rs harus masuk rentang [MQ135_RS_MIN, MQ135_RS_MAX].
 *   5. Simpan ke NVS flash & (opsional) upload ke Firebase.
 *
 * @param uploadToFirebase  true = upload hasil ke /config/calibration/
 */
void runAutoCalibration(bool uploadToFirebase) {
  Serial.println("\n[KALIB] ================================================");
  Serial.printf ("[KALIB] Kalibrasi otomatis MQ-135 (%d detik)...\n", AUTOCALIB_DURATION_SEC);
  Serial.println("[KALIB]   Pastikan sensor di udara BERSIH sekarang!");
  Serial.println("[KALIB] ================================================");

  int    totalSamples = AUTOCALIB_DURATION_SEC * 2;  // 1 sample per 500ms
  double rsSum        = 0.0;
  int    validCount   = 0;

  for (int i = 0; i < totalSamples; i++) {
    // Rata-rata 10 pembacaan ADC per sampel untuk meredam noise
    long adcSum = 0;
    for (int j = 0; j < 10; j++) {
      adcSum += analogRead(PIN_MQ135);
      delay(5);
    }
    int  adcRaw = (int)(adcSum / 10);
    float rs    = calculateRs(adcRaw);

    if (rs >= MQ135_RS_MIN && rs <= MQ135_RS_MAX) {
      rsSum += rs;
      validCount++;
    } else {
      Serial.printf("[KALIB]   ⚠ Sampel %d tidak valid: Rs=%.0f Ω (ADC=%d)\n",
                    i + 1, rs, adcRaw);
    }

    // Progress log setiap 10 sampel (5 detik)
    if ((i + 1) % 10 == 0) {
      float rsNow = (validCount > 0) ? (float)(rsSum / validCount) : 0;
      Serial.printf("[KALIB]   Progres: %d/%d | Rs rata-rata: %.0f Ω\n",
                    i + 1, totalSamples, rsNow);
    }

    delay(500 - 50);  // 500ms total per sampel (10 * 5ms sudah dipakai)
  }

  if (validCount < 10) {
    Serial.println("[KALIB] ✗ Terlalu sedikit sampel valid. Kalibrasi GAGAL.");
    Serial.println("[KALIB]   Periksa koneksi sensor & coba lagi.");
    Serial.printf ("[KALIB]   Menggunakan Ro default: %.0f Ω\n", g_ro_mq135);
    return;
  }

  float rsAvg   = (float)(rsSum / validCount);
  float roNew   = rsAvg / MQ135_RATIO_CLEAN_AIR;

  Serial.println("\n[KALIB] ================================================");
  Serial.printf ("[KALIB] Hasil kalibrasi:\n");
  Serial.printf ("[KALIB]   Sampel valid  : %d / %d\n", validCount, totalSamples);
  Serial.printf ("[KALIB]   Rs rata-rata  : %.0f Ω\n",  rsAvg);
  Serial.printf ("[KALIB]   Ro baru       : %.0f Ω  (Rs / %.1f)\n", roNew, MQ135_RATIO_CLEAN_AIR);
  Serial.println("[KALIB] ================================================\n");

  g_ro_mq135   = roNew;
  g_calibrated = true;

  // Simpan ke NVS flash
  saveRoToNVS(roNew);

  // Upload hasil ke Firebase
  if (uploadToFirebase && Firebase.ready()) {
    FirebaseJson jCalib;
    jCalib.set("ro_ohm",       roNew);
    jCalib.set("rs_avg_ohm",   rsAvg);
    jCalib.set("samples_valid",validCount);
    jCalib.set("timestamp",    getTimestamp());
    jCalib.set("trigger",      false);  // Reset flag trigger

    if (Firebase.updateNode(fbData, "/config/calibration", jCalib)) {
      Serial.println("[KALIB] Hasil kalibrasi diunggah ke Firebase ✓");
    } else {
      Serial.printf("[KALIB] Gagal upload: %s\n", fbData.errorReason().c_str());
    }
  }
}

// ============================================================
//  SIMPAN / MUAT Ro DARI NVS FLASH
// ============================================================
bool loadRoFromNVS() {
  prefs.begin("mq135", true);   // true = read-only
  float ro = prefs.getFloat("ro_ohm", -1.0f);
  prefs.end();

  if (ro < MQ135_RS_MIN || ro > MQ135_RS_MAX) {
    return false;  // Belum tersimpan atau nilai tidak masuk akal
  }
  g_ro_mq135 = ro;
  return true;
}

void saveRoToNVS(float ro) {
  prefs.begin("mq135", false);  // false = read-write
  prefs.putFloat("ro_ohm", ro);
  prefs.end();
  Serial.printf("[KALIB] Ro = %.0f Ω disimpan ke flash NVS.\n", ro);
}

// ============================================================
//  CEK TRIGGER KALIBRASI DARI FIREBASE
// ============================================================
/**
 * Dipanggil setiap INTERVAL_FB_CHECK.
 * Jika /config/calibration/trigger = true, jalankan kalibrasi
 * dan upload hasilnya, lalu reset flag menjadi false.
 */
void checkRemoteCalibTrigger() {
  FirebaseData tempData;
  if (Firebase.getBool(tempData, "/config/calibration/trigger")) {
    if (tempData.boolData()) {
      Serial.println("\n[KALIB] Trigger kalibrasi diterima dari Firebase!");
      runAutoCalibration(true);
    }
  }
}

// ============================================================
//  KONEKSI WIFI
// ============================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.printf("[WiFi] Menghubungkan ke \"%s\"", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setAutoReconnect(true);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (++attempt > 40) {
      Serial.println("\n[WiFi] Timeout! Lanjutkan tanpa WiFi.");
      return;
    }
  }
  Serial.println(" Terhubung!");
  Serial.printf("[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());
}

// ============================================================
//  KONEKSI FIREBASE
// ============================================================
void connectFirebase() {
  fbConfig.host = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  fbData.setBSSLBufferSize(1024, 1024);
  Serial.print("[Firebase] Menghubungkan");
  int attempt = 0;
  while (!Firebase.ready()) {
    delay(500);
    Serial.print(".");
    if (++attempt > 20) {
      Serial.println("\n[Firebase] Timeout!");
      return;
    }
  }
  Serial.println(" Siap!");
}

// ============================================================
//  SINKRONISASI THRESHOLD
// ============================================================
void syncThresholds() {
  if (!Firebase.ready()) return;
  FirebaseData tempData;
  if (Firebase.getInt(tempData, "/config/thresholds/asap_max"))
    g_threshAsapMax = tempData.intData();
  if (Firebase.getInt(tempData, "/config/thresholds/pm25_tidak_sehat"))
    g_threshPM25Sehat = tempData.intData();
  if (Firebase.getInt(tempData, "/config/thresholds/pm10_tidak_sehat"))
    g_threshPM10Sehat = tempData.intData();

  // Sync PM calibration parameters from Firebase (if they exist)
  if (Firebase.getFloat(tempData, "/config/calibration/pm25_multiplier"))
    g_pm25_multiplier = tempData.floatData();
  if (Firebase.getFloat(tempData, "/config/calibration/pm10_multiplier"))
    g_pm10_multiplier = tempData.floatData();
  if (Firebase.getBool(tempData, "/config/calibration/use_humidity_corr"))
    g_use_humidity_corr = tempData.boolData();

  Serial.printf("[Config] Threshold → CO: %d ppm | PM2.5: %d µg/m³ | PM10: %d µg/m³\n",
                g_threshAsapMax, g_threshPM25Sehat, g_threshPM10Sehat);
  Serial.printf("[Config] PM Calib → PM2.5 Mult: %.2f | PM10 Mult: %.2f | Humid Corr: %s\n",
                g_pm25_multiplier, g_pm10_multiplier, g_use_humidity_corr ? "ON" : "OFF");
}

// ============================================================
//  BACA PMS5003
// ============================================================
/**
 * Struktur paket PMS5003 (32 byte):
 * Byte  0-1 : Header 0x42 0x4D
 * Byte  2-3 : Frame length
 * Byte  4-5 : PM1.0  "standard particle" (CF=1)
 * Byte  6-7 : PM2.5  "standard particle" (CF=1)
 * Byte  8-9 : PM10   "standard particle" (CF=1)
 * Byte 10-11: PM1.0  "atmospheric environment"  ← gunakan ini
 * Byte 12-13: PM2.5  "atmospheric environment"  ← gunakan ini
 * Byte 14-15: PM10   "atmospheric environment"  ← gunakan ini
 * Byte 30-31: checksum
 */
bool readPMS5003(uint16_t &pm1raw, uint16_t &pm25raw, uint16_t &pm10raw) {
  const int     PACKET_SIZE = 32;
  const uint8_t HEADER_1   = 0x42;
  const uint8_t HEADER_2   = 0x4D;
  uint8_t buf[PACKET_SIZE];

  unsigned long startTime = millis();
  while (pmsSerial.available() < PACKET_SIZE) {
    if (millis() - startTime > 2000) return false;
    delay(10);
  }

  while (pmsSerial.available()) {
    if (pmsSerial.read() == HEADER_1) {
      if (pmsSerial.peek() == HEADER_2) {
        buf[0] = HEADER_1;
        pmsSerial.readBytes(&buf[1], PACKET_SIZE - 1);

        // Validasi checksum
        uint16_t checksum = 0;
        for (int i = 0; i < 30; i++) checksum += buf[i];
        uint16_t csReceived = (buf[30] << 8) | buf[31];
        if (checksum != csReceived) {
          Serial.println("[PMS5003] Checksum error!");
          return false;
        }

        // "Atmospheric environment" — lebih akurat untuk indoor
        pm1raw  = (buf[10] << 8) | buf[11];
        pm25raw = (buf[12] << 8) | buf[13];
        pm10raw = (buf[14] << 8) | buf[15];

        Serial.printf("[PMS5003] PM1.0: %d | PM2.5: %d | PM10: %d µg/m³ (raw)\n",
                      pm1raw, pm25raw, pm10raw);
        return true;
      }
    }
  }
  return false;
}

// ============================================================
//  BACA DHT22
// ============================================================
void readDHT22() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h)) {
    Serial.println("[DHT22] Gagal baca! Nilai lama dipertahankan.");
    return;
  }
  g_suhu       = t;
  g_kelembaban = h;
}

// ============================================================
//  HITUNG Rs MQ-135 DARI ADC
// ============================================================
float calculateRs(int adcRaw) {
  if (adcRaw <= 0) adcRaw = 1;
  float vout = adcRaw * (3.3f / 4095.0f);
  if (vout <= 0.0f) vout = 0.001f;
  float rs = MQ135_RL * (MQ135_VCC - vout) / vout;
  return rs;
}

// ============================================================
//  HITUNG CO PPM DARI Rs
// ============================================================
float calculateCO_ppm(float rs) {
  float ratio = rs / g_ro_mq135;   // FIX: pakai g_ro_mq135 (bukan define lama)
  if (ratio <= 0.0f) ratio = 0.001f;
  float ppm = MQ135_CO_A * pow(ratio, MQ135_CO_B);
  
  // Kurangi baseline CO di udara bersih (dari datasheet Rs/Ro = 3.6)
  float ppmBaseline = MQ135_CO_A * pow(MQ135_RATIO_CLEAN_AIR, MQ135_CO_B);
  ppm -= ppmBaseline;
  if (ppm < 0.0f) ppm = 0.0f;
  
  return ppm;
}

// ============================================================
//  BACA MQ-135
// ============================================================
void readMQ135() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(PIN_MQ135);
    delay(5);
  }
  int adcRaw = (int)(sum / 10);

  g_rs_mq135 = calculateRs(adcRaw);
  g_co_ppm   = calculateCO_ppm(g_rs_mq135);
  g_co_ppm   = constrain(g_co_ppm, 0.0f, 100.0f);

  Serial.printf("[MQ135] ADC: %d | Rs: %.0f Ω | Rs/Ro: %.3f | CO: %.2f ppm\n",
                adcRaw, g_rs_mq135,
                g_rs_mq135 / g_ro_mq135,
                g_co_ppm);
}

// ============================================================
//  KOREKSI KELEMBAPAN PARTIKULAT
// ============================================================
float correctParticleHumidity(float pmRaw, float humidity) {
  if (!g_use_humidity_corr || humidity < 40.0f) {
    return pmRaw;
  }
  float cf        = 1.0f + 0.25f * (humidity / 100.0f);
  float corrected = pmRaw / cf;
  return corrected;
}

// ============================================================
//  MOVING AVERAGE PM2.5 — FIX: gunakan counter integer, bukan bool
// ============================================================
float movingAvgPM25(float newVal) {
  g_pm25Buffer[g_pm25BufIdx] = newVal;
  g_pm25BufIdx = (g_pm25BufIdx + 1) % PMS_WINDOW_SIZE;
  if (g_pm25BufCount < PMS_WINDOW_SIZE) g_pm25BufCount++;

  float sum = 0.0f;
  for (int i = 0; i < g_pm25BufCount; i++) sum += g_pm25Buffer[i];
  return sum / (float)g_pm25BufCount;
}

// ============================================================
//  MOVING AVERAGE PM10 — FIX: sama dengan PM2.5
// ============================================================
float movingAvgPM10(float newVal) {
  g_pm10Buffer[g_pm10BufIdx] = newVal;
  g_pm10BufIdx = (g_pm10BufIdx + 1) % PMS_WINDOW_SIZE;
  if (g_pm10BufCount < PMS_WINDOW_SIZE) g_pm10BufCount++;

  float sum = 0.0f;
  for (int i = 0; i < g_pm10BufCount; i++) sum += g_pm10Buffer[i];
  return sum / (float)g_pm10BufCount;
}

// ============================================================
//  KONTROL RELAY (Active HIGH)
// ============================================================
void controlRelay(bool fan, bool purifier) {
  digitalWrite(PIN_RELAY_FAN,   fan      ? HIGH : LOW);
  digitalWrite(PIN_RELAY_PURIF, purifier ? HIGH : LOW);
}

// ============================================================
//  KATEGORI ISPU PM2.5 (Permen LHK No.14/2020)
// ============================================================
String getCategoryPM25(float pm25) {
  if (pm25 <= 15.5f)  return "Baik";
  if (pm25 <= 55.4f)  return "Sedang";
  if (pm25 <= 150.4f) return "Tidak Sehat";
  if (pm25 <= 250.4f) return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  KATEGORI ISPU PM10 (Permen LHK No.14/2020)
// ============================================================
String getCategoryPM10(float pm10) {
  if (pm10 <= 50.0f)  return "Baik";
  if (pm10 <= 150.0f) return "Sedang";
  if (pm10 <= 350.0f) return "Tidak Sehat";
  if (pm10 <= 420.0f) return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  KATEGORI ISPU CO dalam ppm (Permen LHK No.14/2020)
//  Batas: Baik<3.4, Sedang<6.9, Tidak Sehat<12.9, dst.
// ============================================================
String getCategoryCO(float co_ppm) {
  if (co_ppm <= 3.4f)  return "Baik";
  if (co_ppm <= 6.9f)  return "Sedang";
  if (co_ppm <= 12.9f) return "Tidak Sehat";
  if (co_ppm <= 25.7f) return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  HITUNG NILAI ISPU NUMERIK - Permen LHK No.14/2020
//  Rumus: Ia = ((IaHigh-IaLow)/(CaHigh-CaLow))*(Ca-CaLow)+IaLow
// ============================================================
static int _calcISPU(float Ca, const IspuBP* tbl, int n) {
  for (int i = 0; i < n; i++) {
    if (Ca <= tbl[i].CaHigh) {
      float ia = ((float)(tbl[i].IaHigh - tbl[i].IaLow) /
                  (tbl[i].CaHigh  - tbl[i].CaLow)) *
                 (Ca - tbl[i].CaLow) + tbl[i].IaLow;
      return (int)(ia + 0.5f);
    }
  }
  return 500;  // melampaui batas tertinggi
}

int hitungISPU_PM25(float pm25) {
  static const IspuBP tbl[] = {
    {  0.0f,  15.5f,   1,  50 },
    { 15.5f,  55.4f,  51, 100 },
    { 55.4f, 150.4f, 101, 200 },
    {150.4f, 250.4f, 201, 300 },
    {250.4f, 500.4f, 301, 500 }
  };
  return _calcISPU(pm25, tbl, 5);
}

int hitungISPU_PM10(float pm10) {
  static const IspuBP tbl[] = {
    {  0.0f,  50.0f,   1,  50 },
    { 50.0f, 150.0f,  51, 100 },
    {150.0f, 350.0f, 101, 200 },
    {350.0f, 420.0f, 201, 300 },
    {420.0f, 500.0f, 301, 500 }
  };
  return _calcISPU(pm10, tbl, 5);
}

// Ambil nilai ISPU tertinggi dari parameter yang tersedia
int getISPUValue() {
  int i25 = hitungISPU_PM25(g_pm25);
  int i10 = hitungISPU_PM10(g_pm10);
  return max(i25, i10);
}

// ============================================================
//  KIRIM DATA KE FIREBASE
// ============================================================
void sendDataToFirebase() {
  if (!Firebase.ready()) {
    Serial.println("[Firebase] Tidak siap, data tidak dikirim.");
    return;
  }

  bool kondisiBuruk = (g_pm25   > (float)g_threshPM25Sehat) ||
                      (g_pm10   > (float)g_threshPM10Sehat) ||
                      (g_co_ppm > (float)g_threshAsapMax);

  String statusRelay = (g_relayFan || g_relayPurif) ? "Aktif" : "Standby";
  String ts          = getTimestamp();

  // Hitung ISPU numerik (ambil tertinggi dari PM2.5 & PM10)
  int ispuVal = getISPUValue();

  // Status partikel PM2.5
  String statusPM25 = (g_pm25 <= (float)g_threshPM25Sehat) ? "Baik" : "Tidak Sehat";
  // Status partikel PM10
  String statusPM10 = (g_pm10 <= (float)g_threshPM10Sehat) ? "Baik" : "Tidak Sehat";

  FirebaseJson jsonSensor;

  // ── Field utama (sesuai yang dibaca web) ──────────────────────
  jsonSensor.set("suhu",        g_suhu);
  jsonSensor.set("kelembaban",  g_kelembaban);

  // "kadarAsap" = nilai ppm CO — key yang dibaca web di data.kadarAsap
  jsonSensor.set("kadarAsap",   g_co_ppm);

  // partikelDebu — key PM25 & PM10 langsung (sesuai web: data.partikelDebu.PM25 / PM10)
  jsonSensor.set("partikelDebu/PM25",       g_pm25);          // µg/m³ terkoreksi
  jsonSensor.set("partikelDebu/PM10",       g_pm10);          // µg/m³ terkoreksi
  jsonSensor.set("partikelDebu/status",     statusPM25);      // dibaca web: data.partikelDebu.status
  jsonSensor.set("partikelDebu/statusPM10", statusPM10);      // dibaca web: data.partikelDebu.statusPM10

  // ISPU — key dibaca web: data.ISPU
  jsonSensor.set("ISPU", ispuVal);

  // statusLevel — web cek .toUpperCase() → "BAIK" / "BURUK"
  jsonSensor.set("statusLevel", kondisiBuruk ? "Buruk" : "Baik");

  // ── Field detail tambahan ──────────────────────────────────────
  jsonSensor.set("partikelDebu/PM25_raw",      (int)g_pm25_raw);
  jsonSensor.set("partikelDebu/PM25_kategori", getCategoryPM25(g_pm25));
  jsonSensor.set("partikelDebu/PM10_raw",      (int)g_pm10_raw);
  jsonSensor.set("partikelDebu/PM10_kategori", getCategoryPM10(g_pm10));
  jsonSensor.set("partikelDebu/PM1_raw",       (int)g_pm1_raw);
  jsonSensor.set("partikelDebu/ISPU_PM25",     hitungISPU_PM25(g_pm25));
  jsonSensor.set("partikelDebu/ISPU_PM10",     hitungISPU_PM10(g_pm10));

  jsonSensor.set("co/ppm",       g_co_ppm);
  jsonSensor.set("co/kategori",  getCategoryCO(g_co_ppm));

  jsonSensor.set("relay/fan",      g_relayFan);
  jsonSensor.set("relay/purifier", g_relayPurif);
  jsonSensor.set("relay/status",   statusRelay);

  jsonSensor.set("calibration/ro_ohm",        g_ro_mq135);
  jsonSensor.set("calibration/is_calibrated", g_calibrated);

  jsonSensor.set("timestamp", ts);

  if (Firebase.updateNode(fbData, "/airQuality", jsonSensor)) {
    Serial.println("[Firebase] Data berhasil dikirim.");
    Serial.printf("[Firebase] ISPU: %d | PM2.5: %.1f | PM10: %.1f | CO: %.2f ppm\n",
                  ispuVal, g_pm25, g_pm10, g_co_ppm);
  } else {
    Serial.printf("[Firebase] Gagal: %s\n", fbData.errorReason().c_str());
  }
}

// ============================================================
//  GENERATE TIMESTAMP
// ============================================================
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return String("uptime_") + String(millis() / 1000) + "s";
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
  return String(buf);
}
