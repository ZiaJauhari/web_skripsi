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

#include <DHT.h>
#include <FirebaseESP32.h>
#include <HardwareSerial.h>
#include <Preferences.h> // NVS flash storage untuk Ro
#include <WiFi.h>
#include <math.h>
#include <time.h>

// ============================================================
//  KONFIGURASI WIFI & FIREBASE
// ============================================================
#define WIFI_SSID "Ziazaidan"
#define WIFI_PASSWORD "01051977"
#define FIREBASE_HOST                                                          \
  "https://asap-f023f-default-rtdb.asia-southeast1.firebasedatabase.app"
#define FIREBASE_AUTH "8bUCYVgs9C0mvIeIBuH0mUFaxtfWQOwOfFG1ElYS"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 25200 // WIB = UTC+7
#define DAYLIGHT_OFFSET 0

// ============================================================
//  PIN
// ============================================================
#define PIN_MQ135 34
#define PIN_DHT22 27
#define PIN_RELAY_FAN 14
#define PIN_RELAY_PURIF 12
#define PIN_BOOT_BTN 0 // Tombol BOOT bawaan ESP32

// ============================================================
//  KONSTANTA WAKTU
// ============================================================
#define INTERVAL_SEND 5000UL        // ms — kirim ke Firebase (5 detik)
#define INTERVAL_WIFI_CHECK 10000UL // ms — cek koneksi WiFi
#define INTERVAL_FB_CHECK 15000UL   // ms — cek koneksi Firebase
#define DHT_TYPE DHT22

// ============================================================
//  THRESHOLD DEFAULT (Permen LHK No.14/2020)
// ============================================================
#define DEFAULT_ASAP_MAX 7 // ppm CO  — batas Sedang→Tidak Sehat per ISPU
#define DEFAULT_PM25_TIDAK_SEHAT                                               \
  56 // µg/m³   — batas Tidak Sehat PM2.5 per ISPU
#define DEFAULT_PM10_TIDAK_SEHAT                                               \
  151 // µg/m³   — batas Tidak Sehat PM10 per ISPU

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
#define MQ135_VCC 5.0    // Tegangan supply sensor (Volt)
#define MQ135_RL 10000.0 // Resistor beban di modul (Ohm) — biasanya 10kΩ
#define MQ135_RATIO_CLEAN_AIR                                                  \
  3.6 // Rs/Ro di udara bersih (dari datasheet MQ-135)

/**
 * Nilai Ro fallback jika kalibrasi belum pernah dilakukan.
 * Nilai 10000 Ω adalah perkiraan umum untuk MQ-135 baru.
 * Setelah kalibrasi otomatis, nilai ini TIDAK digunakan.
 */
#define MQ135_RO_DEFAULT                                                       \
  10000.0 // Ohm — hanya dipakai sebelum kalibrasi pertama

/**
 * Kurva sensitivitas CO dari datasheet MQ-135:
 *   ppm = A * (Rs/Ro) ^ B
 *   Berdasarkan kurva CO di datasheet:
 *     A = 605.18, B = -3.937
 */
#define MQ135_CO_A 605.18
#define MQ135_CO_B -3.937

/**
 * Dead-zone ratio: Jika Rs/Ro >= nilai ini, CO dianggap 0 ppm.
 * Ini mencegah kurva yang sangat curam menghasilkan pembacaan
 * CO palsu di udara bersih akibat noise ADC atau drift sensor.
 * Nilai 3.3 memberi margin ~8% di bawah clean air ratio (3.6).
 */
#define MQ135_CO_DEAD_ZONE_RATIO 3.0

/**
 * Noise floor: Hasil CO di bawah nilai ini (ppm) dianggap 0.
 * Mengkompensasi ketidakstabilan MQ-135 pada konsentrasi rendah.
 */
#define MQ135_CO_NOISE_FLOOR 0.5

/**
 * Durasi kalibrasi otomatis (detik).
 * Selama ini sensor disampling setiap 500ms, hasilnya dirata-rata.
 * Minimal 60 detik dianjurkan. Default 120 detik (2 menit).
 */
#define AUTOCALIB_DURATION_SEC 120

/**
 * Batas bawah Rs yang masuk akal untuk MQ-135 (Ohm).
 * Jika Rs < nilai ini, kemungkinan sensor terbalik / kabel putus.
 */
#define MQ135_RS_MIN 1000.0   // Ohm
#define MQ135_RS_MAX 200000.0 // Ohm

// ============================================================
//  KALIBRASI PMS5003
// ============================================================
#define PMS_USE_HUMIDITY_CORRECTION true
#define PMS_WINDOW_SIZE 5 // Jumlah sampel untuk moving average

// ============================================================
//  OBJEK GLOBAL
// ============================================================
FirebaseData fbData;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;

DHT dht(PIN_DHT22, DHT_TYPE);
HardwareSerial pmsSerial(2);
Preferences prefs; // NVS namespace untuk simpan Ro

// ============================================================
//  VARIABEL SENSOR & STATUS
// ============================================================
float g_suhu = 0.0;
float g_kelembaban = 0.0;
float g_co_ppm = 0.0;
uint16_t g_pm25_raw = 0;
float g_pm25 = 0.0;
uint16_t g_pm10_raw = 0;
float g_pm10 = 0.0;
uint16_t g_pm1_raw = 0;
float g_rs_mq135 = 0.0;
float g_ro_mq135 = MQ135_RO_DEFAULT; // Diisi dari NVS atau kalibrasi
bool g_calibrated = false;           // True jika Ro sudah valid dari kalibrasi

// Moving average buffer PM2.5 — FIX: simpan count terpisah
float g_pm25Buffer[PMS_WINDOW_SIZE];
int g_pm25BufIdx = 0;
int g_pm25BufCount = 0; // FIX: jumlah sampel valid, bukan flag boolean

// Moving average buffer PM10
float g_pm10Buffer[PMS_WINDOW_SIZE];
int g_pm10BufIdx = 0;
int g_pm10BufCount = 0;

int g_threshAsapMax = DEFAULT_ASAP_MAX;
int g_threshPM25Sehat = DEFAULT_PM25_TIDAK_SEHAT;
int g_threshPM10Sehat = DEFAULT_PM10_TIDAK_SEHAT;

float g_pm25_multiplier =
    3.14f; // Faktor kalibrasi PM2.5 (ref_val / raw_val = 15/4.78 ≈ 3.14)
float g_pm10_multiplier =
    3.09f; // Faktor kalibrasi PM10 (ref_val / raw_val = 21/6.80 ≈ 3.09)
bool g_use_humidity_corr =
    false; // Koreksi kelembapan OFF — agar stabil sejajar detektor fisik

bool g_relayFan = false;
bool g_relayPurif = false;

unsigned long g_lastSend = 0;
unsigned long g_lastWifiCheck = 0;
unsigned long g_lastFbCheck = 0;

// ============================================================
//  PROTOTYPING
// ============================================================
struct IspuBP {
  float CaLow;
  float CaHigh;
  int IaLow;
  int IaHigh;
};
static int _calcISPU(float Ca, const IspuBP *tbl, int n);

void connectWiFi();
void connectFirebase();
void syncThresholds();
void checkRemoteCalibTrigger();
void runAutoCalibration(bool uploadToFirebase);
bool loadRoFromNVS();
void saveRoToNVS(float ro);
bool readPMS5003(uint16_t &pm1raw, uint16_t &pm25raw, uint16_t &pm10raw);
void readDHT22();
void readMQ135();
float calculateRs(int adcRaw);
float calculateCO_ppm(float rs);
float correctParticleHumidity(float pmRaw, float humidity);
float movingAvgPM25(float newVal);
float movingAvgPM10(float newVal);
void controlRelay(bool fan, bool purifier);
void sendDataToFirebase();
void sendHistoryToFirebase();
String getTimestamp();
String getCategoryPM25(float pm25);
String getCategoryPM10(float pm10);
String getCategoryCO(float co_ppm);
int hitungISPU_PM25(float pm25);
int hitungISPU_PM10(float pm10);
int hitungISPU_CO(float co_ppm);
int getISPUValue();
String getKategoriISPU(int ispu);
String getParameterKritis();

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
  pinMode(PIN_RELAY_FAN, OUTPUT);
  pinMode(PIN_RELAY_PURIF, OUTPUT);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);
  digitalWrite(PIN_RELAY_FAN, LOW);
  digitalWrite(PIN_RELAY_PURIF, LOW);

  // --- Init sensor ---
  dht.begin();
  Serial.println("[DHT22] Inisialisasi selesai.");

  pmsSerial.begin(9600, SERIAL_8N1, 16, 17);
  Serial.println("[PMS5003] UART2 siap.");

  // Inisialisasi buffer moving average ke nol
  memset(g_pm25Buffer, 0, sizeof(g_pm25Buffer));
  memset(g_pm10Buffer, 0, sizeof(g_pm10Buffer));

  // --- Sambungkan WiFi & NTP ---
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

  // --- Sambungkan Firebase & Sync Thresholds ---
  connectFirebase();
  syncThresholds();

  // --- Coba muat Ro dari NVS flash ---
  bool forceCalib = (digitalRead(PIN_BOOT_BTN) == LOW); // Tombol BOOT ditahan
  if (forceCalib) {
    Serial.println("\n[KALIB] Tombol BOOT ditahan → Kalibrasi ulang paksa!");
  }

  if (!forceCalib && loadRoFromNVS()) {
    Serial.printf("[KALIB] Ro dimuat dari flash: %.0f Ω\n", g_ro_mq135);
    g_calibrated = true;
  } else {
    // Belum ada nilai Ro tersimpan atau tombol ditahan → jalankan kalibrasi
    // otomatis (120 detik)
    Serial.println("[KALIB] Nilai Ro belum tersimpan atau paksa. Memulai "
                   "kalibrasi otomatis...");
    Serial.println("        Pastikan sensor berada di UDARA BERSIH!\n");
    runAutoCalibration(true); // upload hasil ke Firebase
  }

  // --- PROSES PREHEATING (60 Detik) ---
  Serial.println("[SYSTEM] Memulai pemanasan sensor (Preheating) 60 detik...");
  for (int countdown = 60; countdown > 0; countdown--) {
    // Kirim status preheating ke Firebase
    if (Firebase.ready()) {
      FirebaseJson jsonPreheat;
      jsonPreheat.set("preheating", true);
      jsonPreheat.set("countdown", countdown);
      Firebase.updateNode(fbData, "/airQuality", jsonPreheat);
    }
    Serial.printf("[SYSTEM] Pemanasan: %d detik tersisa...\n", countdown);
    delay(1000);
  }

  // Selesai preheating, set flag preheating ke false
  if (Firebase.ready()) {
    FirebaseJson jsonPreheatDone;
    jsonPreheatDone.set("preheating", false);
    jsonPreheatDone.set("countdown", 0);
    Firebase.updateNode(fbData, "/airQuality", jsonPreheatDone);
  }

  Serial.println("\n[SYSTEM] Setup selesai. Mulai monitoring...");
  Serial.printf("[KALIB] Ro aktif: %.0f Ω | Kalibrasi: %s\n\n", g_ro_mq135,
                g_calibrated ? "✓ Terverifikasi" : "⚠ Default");
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
      checkRemoteCalibTrigger(); // Cek permintaan kalibrasi dari Firebase
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
      g_pm1_raw = pm1raw;
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

    // 3. Hitung ISPU dan tentukan kontrol relay berdasarkan kategori ISPU
    int ispuAkhir = getISPUValue();
    String kategoriISPU = getKategoriISPU(ispuAkhir);
    String paramKritis = getParameterKritis();

    // Logika kontrol exhaust fan & purifier:
    // Baik/Sedang → OFF, Tidak Sehat/Sangat Tidak Sehat/Berbahaya → ON
    g_relayFan = (ispuAkhir > 100);
    g_relayPurif = (ispuAkhir > 100);

    // 4. Terapkan relay
    controlRelay(g_relayFan, g_relayPurif);

    // 5. Kirim ke Firebase
    sendDataToFirebase();

    // 6. Kirim histori ke Firebase
    sendHistoryToFirebase();

    // 7. Debug Serial Monitor
    Serial.println("========== Sensor Update ==========");
    Serial.printf("  Suhu          : %.1f °C\n", g_suhu);
    Serial.printf("  Kelembaban    : %.1f %%\n", g_kelembaban);
    Serial.printf("  MQ135 Rs      : %.0f Ω\n", g_rs_mq135);
    Serial.printf("  MQ135 Ro      : %.0f Ω (%s)\n", g_ro_mq135,
                  g_calibrated ? "kalibrasi" : "default");
    Serial.printf("  CO (ppm)      : %.2f\n", g_co_ppm);
    Serial.printf("  PM2.5         : %.1f µg/m³\n", g_pm25);
    Serial.printf("  PM10          : %.1f µg/m³\n", g_pm10);
    Serial.printf("  ISPU PM2.5    : %d\n", hitungISPU_PM25(g_pm25));
    Serial.printf("  ISPU PM10     : %d\n", hitungISPU_PM10(g_pm10));
    Serial.printf("  ISPU CO       : %d\n", hitungISPU_CO(g_co_ppm));
    Serial.printf("  ISPU Akhir    : %d\n", ispuAkhir);
    Serial.printf("  Kategori ISPU : %s\n", kategoriISPU.c_str());
    Serial.printf("  Param Kritis  : %s\n", paramKritis.c_str());
    Serial.printf("  Exhaust Fan   : %s\n", g_relayFan ? "ON" : "OFF");
    Serial.printf("  Air Purifier  : %s\n", g_relayPurif ? "ON" : "OFF");
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
  Serial.printf("[KALIB] Kalibrasi otomatis MQ-135 (%d detik)...\n",
                AUTOCALIB_DURATION_SEC);
  Serial.println("[KALIB]   Pastikan sensor di udara BERSIH sekarang!");
  Serial.println("[KALIB] ================================================");

  int totalSamples = AUTOCALIB_DURATION_SEC * 2; // 1 sample per 500ms
  double rsSum = 0.0;
  int validCount = 0;

  for (int i = 0; i < totalSamples; i++) {
    // Rata-rata 10 pembacaan ADC per sampel untuk meredam noise
    long adcSum = 0;
    for (int j = 0; j < 10; j++) {
      adcSum += analogRead(PIN_MQ135);
      delay(5);
    }
    int adcRaw = (int)(adcSum / 10);
    float rs = calculateRs(adcRaw);

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
      Serial.printf("[KALIB]   Progres: %d/%d | Rs rata-rata: %.0f Ω\n", i + 1,
                    totalSamples, rsNow);
      if (uploadToFirebase && Firebase.ready()) {
        int progressPercent = (i + 1) * 100 / totalSamples;
        Firebase.setInt(fbData, "/config/calibration/progress", progressPercent);
        Firebase.setString(fbData, "/config/calibration/status", "Calibrating (" + String(progressPercent) + "%)");
      }
    }

    delay(500 - 50); // 500ms total per sampel (10 * 5ms sudah dipakai)
  }

  if (validCount < 10) {
    Serial.println("[KALIB] ✗ Terlalu sedikit sampel valid. Kalibrasi GAGAL.");
    Serial.println("[KALIB]   Periksa koneksi sensor & coba lagi.");
    Serial.printf("[KALIB]   Menggunakan Ro default: %.0f Ω\n", g_ro_mq135);
    if (uploadToFirebase && Firebase.ready()) {
      FirebaseJson jCalib;
      jCalib.set("trigger", false);
      jCalib.set("progress", 0);
      jCalib.set("status", "Gagal (Sampel tidak valid)");
      Firebase.updateNode(fbData, "/config/calibration", jCalib);
    }
    return;
  }

  float rsAvg = (float)(rsSum / validCount);
  float roNew = rsAvg / MQ135_RATIO_CLEAN_AIR;

  Serial.println("\n[KALIB] ================================================");
  Serial.printf("[KALIB] Hasil kalibrasi:\n");
  Serial.printf("[KALIB]   Sampel valid  : %d / %d\n", validCount,
                totalSamples);
  Serial.printf("[KALIB]   Rs rata-rata  : %.0f Ω\n", rsAvg);
  Serial.printf("[KALIB]   Ro baru       : %.0f Ω  (Rs / %.1f)\n", roNew,
                MQ135_RATIO_CLEAN_AIR);
  Serial.println("[KALIB] ================================================\n");

  g_ro_mq135 = roNew;
  g_calibrated = true;

  // Simpan ke NVS flash
  saveRoToNVS(roNew);

  // Upload hasil ke Firebase
  if (uploadToFirebase && Firebase.ready()) {
    FirebaseJson jCalib;
    jCalib.set("ro_ohm", roNew);
    jCalib.set("rs_avg_ohm", rsAvg);
    jCalib.set("samples_valid", validCount);
    jCalib.set("timestamp", getTimestamp());
    jCalib.set("trigger", false); // Reset flag trigger
    jCalib.set("progress", 0);
    jCalib.set("status", "Terkalibrasi");

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
  prefs.begin("mq135", true); // true = read-only
  float ro = prefs.getFloat("ro_ohm", -1.0f);
  prefs.end();

  if (ro < MQ135_RS_MIN || ro > MQ135_RS_MAX) {
    return false; // Belum tersimpan atau nilai tidak masuk akal
  }
  g_ro_mq135 = ro;
  return true;
}

void saveRoToNVS(float ro) {
  prefs.begin("mq135", false); // false = read-write
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
  if (WiFi.status() == WL_CONNECTED)
    return;
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
  if (!Firebase.ready())
    return;
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

  Serial.printf(
      "[Config] Threshold → CO: %d ppm | PM2.5: %d µg/m³ | PM10: %d µg/m³\n",
      g_threshAsapMax, g_threshPM25Sehat, g_threshPM10Sehat);
  Serial.printf("[Config] PM Calib → PM2.5 Mult: %.2f | PM10 Mult: %.2f | "
                "Humid Corr: %s\n",
                g_pm25_multiplier, g_pm10_multiplier,
                g_use_humidity_corr ? "ON" : "OFF");
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
  const int PACKET_SIZE = 32;
  const uint8_t HEADER_1 = 0x42;
  const uint8_t HEADER_2 = 0x4D;
  uint8_t buf[PACKET_SIZE];

  unsigned long startTime = millis();
  while (pmsSerial.available() < PACKET_SIZE) {
    if (millis() - startTime > 2000)
      return false;
    delay(10);
  }

  while (pmsSerial.available()) {
    if (pmsSerial.read() == HEADER_1) {
      if (pmsSerial.peek() == HEADER_2) {
        buf[0] = HEADER_1;
        pmsSerial.readBytes(&buf[1], PACKET_SIZE - 1);

        // Validasi checksum
        uint16_t checksum = 0;
        for (int i = 0; i < 30; i++)
          checksum += buf[i];
        uint16_t csReceived = (buf[30] << 8) | buf[31];
        if (checksum != csReceived) {
          Serial.println("[PMS5003] Checksum error!");
          return false;
        }

        // "Atmospheric environment" — lebih akurat untuk indoor
        pm1raw = (buf[10] << 8) | buf[11];
        pm25raw = (buf[12] << 8) | buf[13];
        pm10raw = (buf[14] << 8) | buf[15];

        Serial.printf(
            "[PMS5003] PM1.0: %d | PM2.5: %d | PM10: %d µg/m³ (raw)\n", pm1raw,
            pm25raw, pm10raw);
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
  g_suhu = t;
  g_kelembaban = h;
}

// ============================================================
//  HITUNG Rs MQ-135 DARI ADC
// ============================================================
float calculateRs(int adcRaw) {
  if (adcRaw <= 0)
    adcRaw = 1;
  float vout = adcRaw * (3.3f / 4095.0f);
  if (vout <= 0.0f)
    vout = 0.001f;
  float rs = MQ135_RL * (MQ135_VCC - vout) / vout;
  return rs;
}

// ============================================================
//  HITUNG CO PPM DARI Rs
// ============================================================
float calculateCO_ppm(float rs) {
  float ratio = rs / g_ro_mq135;
  if (ratio <= 0.0f)
    ratio = 0.001f;

  // Dead-zone: jika Rs/Ro masih dekat udara bersih, CO = 0.
  // Kurva MQ-135 sangat curam (B=-3.937), sehingga noise kecil
  // pada Rs/Ro di sekitar 3.6 bisa menghasilkan ppm palsu.
  if (ratio >= MQ135_CO_DEAD_ZONE_RATIO) {
    return 0.0f;
  }

  float ppm = MQ135_CO_A * pow(ratio, MQ135_CO_B);

  // Kurangi baseline CO di udara bersih (dari datasheet Rs/Ro = 3.6)
  float ppmBaseline = MQ135_CO_A * pow(MQ135_RATIO_CLEAN_AIR, MQ135_CO_B);
  ppm -= ppmBaseline;

  // Noise floor: nilai di bawah threshold dianggap 0
  if (ppm < MQ135_CO_NOISE_FLOOR)
    ppm = 0.0f;

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
  g_co_ppm = calculateCO_ppm(g_rs_mq135);
  g_co_ppm = constrain(g_co_ppm, 0.0f, 100.0f);

  Serial.printf("[MQ135] ADC: %d | Rs: %.0f Ω | Rs/Ro: %.3f | CO: %.2f ppm\n",
                adcRaw, g_rs_mq135, g_rs_mq135 / g_ro_mq135, g_co_ppm);
}

// ============================================================
//  KOREKSI KELEMBAPAN PARTIKULAT
// ============================================================
float correctParticleHumidity(float pmRaw, float humidity) {
  if (!g_use_humidity_corr || humidity < 40.0f) {
    return pmRaw;
  }
  float cf = 1.0f + 0.25f * (humidity / 100.0f);
  float corrected = pmRaw / cf;
  return corrected;
}

// ============================================================
//  MOVING AVERAGE PM2.5 — FIX: gunakan counter integer, bukan bool
// ============================================================
float movingAvgPM25(float newVal) {
  g_pm25Buffer[g_pm25BufIdx] = newVal;
  g_pm25BufIdx = (g_pm25BufIdx + 1) % PMS_WINDOW_SIZE;
  if (g_pm25BufCount < PMS_WINDOW_SIZE)
    g_pm25BufCount++;

  float sum = 0.0f;
  for (int i = 0; i < g_pm25BufCount; i++)
    sum += g_pm25Buffer[i];
  return sum / (float)g_pm25BufCount;
}

// ============================================================
//  MOVING AVERAGE PM10 — FIX: sama dengan PM2.5
// ============================================================
float movingAvgPM10(float newVal) {
  g_pm10Buffer[g_pm10BufIdx] = newVal;
  g_pm10BufIdx = (g_pm10BufIdx + 1) % PMS_WINDOW_SIZE;
  if (g_pm10BufCount < PMS_WINDOW_SIZE)
    g_pm10BufCount++;

  float sum = 0.0f;
  for (int i = 0; i < g_pm10BufCount; i++)
    sum += g_pm10Buffer[i];
  return sum / (float)g_pm10BufCount;
}

// ============================================================
//  KONTROL RELAY (Active HIGH)
// ============================================================
void controlRelay(bool fan, bool purifier) {
  digitalWrite(PIN_RELAY_FAN, fan ? HIGH : LOW);
  digitalWrite(PIN_RELAY_PURIF, purifier ? HIGH : LOW);
}

// ============================================================
//  KATEGORI ISPU PM2.5 (Permen LHK No.14/2020)
// ============================================================
String getCategoryPM25(float pm25) {
  if (pm25 <= 15.5f)
    return "Baik";
  if (pm25 <= 55.4f)
    return "Sedang";
  if (pm25 <= 150.4f)
    return "Tidak Sehat";
  if (pm25 <= 250.4f)
    return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  KATEGORI ISPU PM10 (Permen LHK No.14/2020)
// ============================================================
String getCategoryPM10(float pm10) {
  if (pm10 <= 50.0f)
    return "Baik";
  if (pm10 <= 150.0f)
    return "Sedang";
  if (pm10 <= 350.0f)
    return "Tidak Sehat";
  if (pm10 <= 420.0f)
    return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  KATEGORI ISPU CO dalam ppm (Permen LHK No.14/2020)
//  Batas: Baik<3.4, Sedang<6.9, Tidak Sehat<12.9, dst.
// ============================================================
String getCategoryCO(float co_ppm) {
  if (co_ppm <= 3.4f)
    return "Baik";
  if (co_ppm <= 6.9f)
    return "Sedang";
  if (co_ppm <= 12.9f)
    return "Tidak Sehat";
  if (co_ppm <= 25.7f)
    return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  HITUNG NILAI ISPU NUMERIK - Permen LHK No.14/2020
//  Rumus: Ia = ((IaHigh-IaLow)/(CaHigh-CaLow))*(Ca-CaLow)+IaLow
// ============================================================
static int _calcISPU(float Ca, const IspuBP *tbl, int n) {
  for (int i = 0; i < n; i++) {
    if (Ca <= tbl[i].CaHigh) {
      float ia = ((float)(tbl[i].IaHigh - tbl[i].IaLow) /
                  (tbl[i].CaHigh - tbl[i].CaLow)) *
                     (Ca - tbl[i].CaLow) +
                 tbl[i].IaLow;
      return (int)(ia + 0.5f);
    }
  }
  return 500; // melampaui batas tertinggi
}

int hitungISPU_PM25(float pm25) {
  static const IspuBP tbl[] = {{0.0f, 15.5f, 1, 50},
                               {15.5f, 55.4f, 51, 100},
                               {55.4f, 150.4f, 101, 200},
                               {150.4f, 250.4f, 201, 300},
                               {250.4f, 500.4f, 301, 500}};
  return _calcISPU(pm25, tbl, 5);
}

int hitungISPU_PM10(float pm10) {
  static const IspuBP tbl[] = {{0.0f, 50.0f, 1, 50},
                               {50.0f, 150.0f, 51, 100},
                               {150.0f, 350.0f, 101, 200},
                               {350.0f, 420.0f, 201, 300},
                               {420.0f, 500.0f, 301, 500}};
  return _calcISPU(pm10, tbl, 5);
}

int hitungISPU_CO(float co_ppm) {
  // Breakpoint CO untuk ISPU (Permen LHK No.14/2020)
  // Konversi dari mg/m³ ke ppm (1 ppm CO ≈ 1.145 mg/m³)
  static const IspuBP tbl[] = {
    {  0.0f,   4.0f,   1,  50 },
    {  4.0f,   8.0f,  51, 100 },
    {  8.0f,  15.0f, 101, 200 },
    { 15.0f,  30.0f, 201, 300 },
    { 30.0f,  45.0f, 301, 500 }
  };
  return _calcISPU(co_ppm, tbl, 5);
}

// Ambil nilai ISPU tertinggi dari PM2.5, PM10, dan CO
int getISPUValue() {
  int i25 = hitungISPU_PM25(g_pm25);
  int i10 = hitungISPU_PM10(g_pm10);
  int iCO = hitungISPU_CO(g_co_ppm);
  int maxVal = i25;
  if (i10 > maxVal) maxVal = i10;
  if (iCO > maxVal) maxVal = iCO;
  return maxVal;
}

// ============================================================
//  KATEGORI ISPU BERDASARKAN NILAI
// ============================================================
String getKategoriISPU(int ispu) {
  if (ispu <= 50)  return "Baik";
  if (ispu <= 100) return "Sedang";
  if (ispu <= 200) return "Tidak Sehat";
  if (ispu <= 300) return "Sangat Tidak Sehat";
  return "Berbahaya";
}

// ============================================================
//  PARAMETER PENCEMAR KRITIS (parameter dgn ISPU tertinggi)
// ============================================================
String getParameterKritis() {
  int i25 = hitungISPU_PM25(g_pm25);
  int i10 = hitungISPU_PM10(g_pm10);
  int iCO = hitungISPU_CO(g_co_ppm);

  int maxVal = i25;
  String param = "PM2.5";

  if (i10 > maxVal) {
    maxVal = i10;
    param = "PM10";
  }
  if (iCO > maxVal) {
    maxVal = iCO;
    param = "CO";
  }
  return param;
}

// ============================================================
//  KIRIM DATA KE FIREBASE
// ============================================================
void sendDataToFirebase() {
  if (!Firebase.ready()) {
    Serial.println("[Firebase] Tidak siap, data tidak dikirim.");
    return;
  }

  // Hitung semua ISPU
  int ispuPM25 = hitungISPU_PM25(g_pm25);
  int ispuPM10 = hitungISPU_PM10(g_pm10);
  int ispuCO   = hitungISPU_CO(g_co_ppm);
  int ispuAkhir = getISPUValue();
  String kategoriISPU = getKategoriISPU(ispuAkhir);
  String paramKritis  = getParameterKritis();
  String statusFan    = g_relayFan ? "ON" : "OFF";
  String statusPurif  = g_relayPurif ? "ON" : "OFF";
  String ts           = getTimestamp();

  FirebaseJson jsonSensor;

  // ── Sensor Data ─────────────────────────────────────────────
  jsonSensor.set("suhu", g_suhu);
  jsonSensor.set("kelembaban", g_kelembaban);
  jsonSensor.set("pm25", g_pm25);
  jsonSensor.set("pm10", g_pm10);
  jsonSensor.set("co", g_co_ppm);

  // ── ISPU per Parameter ──────────────────────────────────────
  jsonSensor.set("ispu/pm25", ispuPM25);
  jsonSensor.set("ispu/pm10", ispuPM10);
  jsonSensor.set("ispu/co", ispuCO);
  jsonSensor.set("ispu/akhir", ispuAkhir);
  jsonSensor.set("ispu/kategori", kategoriISPU);
  jsonSensor.set("ispu/paramKritis", paramKritis);

  // ── Status Exhaust Fan & Air Purifier ───────────────────────
  jsonSensor.set("exhaustFan", statusFan);
  jsonSensor.set("purifier", statusPurif);
  jsonSensor.set("relay/fan", g_relayFan);
  jsonSensor.set("relay/purifier", g_relayPurif);

  // ── Detail Tambahan ─────────────────────────────────────────
  jsonSensor.set("calibration/ro_ohm", g_ro_mq135);
  jsonSensor.set("calibration/is_calibrated", g_calibrated);

  jsonSensor.set("timestamp", ts);

  if (Firebase.updateNode(fbData, "/airQuality", jsonSensor)) {
    Serial.println("[Firebase] Data berhasil dikirim.");
    Serial.printf("[Firebase] ISPU Akhir: %d (%s) | Kritis: %s | Fan: %s | Purifier: %s\n",
                  ispuAkhir, kategoriISPU.c_str(), paramKritis.c_str(), statusFan.c_str(), statusPurif.c_str());
  } else {
    Serial.printf("[Firebase] Gagal: %s\n", fbData.errorReason().c_str());
  }
}

// ============================================================
//  KIRIM HISTORI DATA KE FIREBASE
// ============================================================
void sendHistoryToFirebase() {
  if (!Firebase.ready()) return;

  FirebaseJson jsonHistory;
  jsonHistory.set("pm25", g_pm25);
  jsonHistory.set("pm10", g_pm10);
  jsonHistory.set("co", g_co_ppm);
  jsonHistory.set("suhu", g_suhu);
  jsonHistory.set("kelembaban", g_kelembaban);
  jsonHistory.set("ispu_pm25", hitungISPU_PM25(g_pm25));
  jsonHistory.set("ispu_pm10", hitungISPU_PM10(g_pm10));
  jsonHistory.set("ispu_co", hitungISPU_CO(g_co_ppm));
  jsonHistory.set("ispu_akhir", getISPUValue());
  jsonHistory.set("kategori", getKategoriISPU(getISPUValue()));
  jsonHistory.set("paramKritis", getParameterKritis());
  jsonHistory.set("exhaustFan", g_relayFan ? "ON" : "OFF");
  jsonHistory.set("purifier", g_relayPurif ? "ON" : "OFF");
  jsonHistory.set("timestamp", getTimestamp());

  if (Firebase.pushJSON(fbData, "/airQuality/history", jsonHistory)) {
    Serial.println("[Firebase] Histori disimpan.");
  } else {
    Serial.printf("[Firebase] Gagal simpan histori: %s\n", fbData.errorReason().c_str());
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
