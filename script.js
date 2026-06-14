// Menggunakan Firebase SDK Modular (v9+)
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.11.0/firebase-app.js";
import { getDatabase, ref, onValue, update } from "https://www.gstatic.com/firebasejs/10.11.0/firebase-database.js";

// =========================================================================
// 1. KONFIGURASI FIREBASE
// TODO: Ganti nilai di dalam firebaseConfig ini dengan data dari Firebase Console Anda!
// =========================================================================
const firebaseConfig = {
    apiKey: "AIzaSyASTd9dlNjxe8QZL2LJRatx8qUntt2D80g",
    authDomain: "asap-f023f.firebaseapp.com",
    databaseURL: "https://asap-f023f-default-rtdb.asia-southeast1.firebasedatabase.app",
    projectId: "asap-f023f",
    storageBucket: "asap-f023f.firebasestorage.app",
    messagingSenderId: "844762514053",
    appId: "1:844762514053:web:d6672b17d489d3c65b5fec"
};

// Inisialisasi Firebase
const app = initializeApp(firebaseConfig);
const db = getDatabase(app);

// Referensi Database
const connectedRef = ref(db, ".info/connected");
const airQualityRef = ref(db, "airQuality");
const configRef = ref(db, "config/thresholds");

// =========================================================================
// 2. INISIALISASI CHART.JS
// =========================================================================
const ctx = document.getElementById('historyChart').getContext('2d');

// Array untuk menyimpan history (maksimal 10)
let historyLabels    = [];
let historyDataAsap  = [];
let historyDataPM25  = [];
let historyDataPM10  = [];
const MAX_HISTORY = 10;

const historyChart = new Chart(ctx, {
    type: 'line',
    data: {
        labels: historyLabels,
        datasets: [
            {
                label: 'Kadar Asap (ppm)',
                data: historyDataAsap,
                borderColor: 'rgba(161, 161, 170, 1)', // Abu-abu
                backgroundColor: 'rgba(161, 161, 170, 0.1)',
                borderWidth: 2,
                tension: 0.3,
                fill: true
            },
            {
                label: 'PM2.5 (µg/m³)',
                data: historyDataPM25,
                borderColor: 'rgba(56, 189, 248, 1)',
                backgroundColor: 'rgba(56, 189, 248, 0.1)',
                borderWidth: 2,
                tension: 0.3,
                fill: true
            },
            {
                label: 'PM10 (µg/m³)',
                data: historyDataPM10,
                borderColor: 'rgba(163, 230, 53, 1)',
                backgroundColor: 'rgba(163, 230, 53, 0.08)',
                borderWidth: 2,
                tension: 0.3,
                fill: true
            }
        ]
    },
    options: {
        responsive: true,
        maintainAspectRatio: false,
        color: '#94a3b8',
        scales: {
            y: {
                beginAtZero: true,
                grid: { color: '#334155' },
                ticks: { color: '#94a3b8' }
            },
            x: {
                grid: { color: '#334155' },
                ticks: { color: '#94a3b8' }
            }
        },
        plugins: {
            legend: {
                position: 'top',
                labels: { 
                    color: '#f8fafc', 
                    boxWidth: 15,
                    font: { size: 11 }
                }
            }
        }
    }
});

function updateChart(waktu, asap, pm25, pm10) {
    historyLabels.push(waktu);
    historyDataAsap.push(asap);
    historyDataPM25.push(pm25);
    historyDataPM10.push(pm10);

    if (historyLabels.length > MAX_HISTORY) {
        historyLabels.shift();
        historyDataAsap.shift();
        historyDataPM25.shift();
        historyDataPM10.shift();
    }

    historyChart.update();
}

// =========================================================================
// 3. LOGIKA KONEKSI & MONITORING REAL-TIME
// =========================================================================

// =========================================================================
// ISPU - Permen LHK No. 14 Tahun 2020
// Tabel batas bawah dan atas untuk PM2.5 (µg/m³) dan ISPU
// =========================================================================
// Format: { IaLow, IaHigh, CaLow, CaHigh }
// Ia = ISPU, Ca = Konsentrasi ambien (µg/m³)
const ISPU_BREAKPOINTS_PM25 = [
    { IaLow:   1, IaHigh:  50, CaLow:   0,   CaHigh:  15.5  }, // Baik
    { IaLow:  51, IaHigh: 100, CaLow:  15.5, CaHigh:  55.4  }, // Sedang
    { IaLow: 101, IaHigh: 200, CaLow:  55.4, CaHigh: 150.4  }, // Tidak Sehat
    { IaLow: 201, IaHigh: 300, CaLow: 150.4, CaHigh: 250.4  }, // Sangat Tidak Sehat
    { IaLow: 301, IaHigh: 500, CaLow: 250.4, CaHigh: 500.4  }, // Berbahaya
];

// Breakpoint PM10 - Permen LHK No. 14 Tahun 2020
const ISPU_BREAKPOINTS_PM10 = [
    { IaLow:   1, IaHigh:  50, CaLow:   0,  CaHigh:  50   }, // Baik
    { IaLow:  51, IaHigh: 100, CaLow:  50,  CaHigh: 150   }, // Sedang
    { IaLow: 101, IaHigh: 200, CaLow: 150,  CaHigh: 350   }, // Tidak Sehat
    { IaLow: 201, IaHigh: 300, CaLow: 350,  CaHigh: 420   }, // Sangat Tidak Sehat
    { IaLow: 301, IaHigh: 500, CaLow: 420,  CaHigh: 500   }, // Berbahaya
];

/**
 * Fungsi generik menghitung ISPU berdasarkan nilai konsentrasi dan tabel breakpoint.
 * Rumus: Ia = ((IaHigh - IaLow) / (CaHigh - CaLow)) * (Ca - CaLow) + IaLow
 * @param {number} Ca  - Konsentrasi ambien (µg/m³)
 * @param {Array}  bps - Tabel breakpoint
 * @returns {{ ispu: number, kategori: string, warna: string }}
 */
function hitungISPU(Ca, bps) {
    if (Ca < 0) return { ispu: 0, kategori: '-', warna: 'var(--text-muted)' };

    let bp = bps.find(b => Ca <= b.CaHigh) || bps[bps.length - 1];

    const Ia = ((bp.IaHigh - bp.IaLow) / (bp.CaHigh - bp.CaLow)) * (Ca - bp.CaLow) + bp.IaLow;
    const ispuBulat = Math.round(Ia);

    let kategori, warna;
    if (ispuBulat <= 50)       { kategori = 'Baik';               warna = 'var(--accent-green)'; }
    else if (ispuBulat <= 100) { kategori = 'Sedang';             warna = '#a3e635'; }
    else if (ispuBulat <= 200) { kategori = 'Tidak Sehat';        warna = 'var(--accent-orange)'; }
    else if (ispuBulat <= 300) { kategori = 'Sangat Tidak Sehat'; warna = '#c026d3'; }
    else                       { kategori = 'Berbahaya';          warna = 'var(--accent-red)'; }

    return { ispu: ispuBulat, kategori, warna };
}

// Wrapper untuk keterbacaan
const hitungISPU_PM25 = (val) => hitungISPU(val, ISPU_BREAKPOINTS_PM25);
const hitungISPU_PM10 = (val) => hitungISPU(val, ISPU_BREAKPOINTS_PM10);

// DOM Elements
const elConnStatus    = document.getElementById('conn-status');
const elStatusBadge   = document.getElementById('status-level-badge');
const elSuhu          = document.getElementById('val-suhu');
const elKelembaban    = document.getElementById('val-kelembaban');
const elAsap          = document.getElementById('val-asap');
const elBarAsap       = document.getElementById('bar-asap');
const elPm25          = document.getElementById('val-pm25');
const elPm25Status    = document.getElementById('val-pm25-status');
const elPm10          = document.getElementById('val-pm10');
const elPm10Status    = document.getElementById('val-pm10-status');
const elLastUpdate    = document.getElementById('last-update');
// ISPU PM2.5
const elISPU_PM25     = document.getElementById('val-ispu-pm25');
const elISPUKat_PM25  = document.getElementById('val-ispu-kategori-pm25');
const elISPURing_PM25 = document.getElementById('ispu-ring-pm25');
// ISPU PM10
const elISPU_PM10     = document.getElementById('val-ispu-pm10');
const elISPUKat_PM10  = document.getElementById('val-ispu-kategori-pm10');
const elISPURing_PM10 = document.getElementById('ispu-ring-pm10');

// Bagian kontrol dan threshold telah dihapus


// Listener Koneksi
onValue(connectedRef, (snap) => {
    if (snap.val() === true) {
        elConnStatus.className = "badge-conn connected";
        elConnStatus.innerHTML = '<i class="fas fa-wifi"></i> Connected';
    } else {
        elConnStatus.className = "badge-conn disconnected";
        elConnStatus.innerHTML = '<i class="fas fa-times-circle"></i> Disconnected';
    }
});

// Listener Data Sensor (airQuality)
onValue(airQualityRef, (snapshot) => {
    const data = snapshot.val();
    if (data) {
        // Update Text Values
        elSuhu.innerText = data.suhu !== undefined ? data.suhu.toFixed(1) : '0';
        elKelembaban.innerText = data.kelembaban !== undefined ? data.kelembaban.toFixed(1) : '0';
        
        const rawAsap = data.kadarAsap || 0;
        const kadarAsap = Number(rawAsap).toFixed(1);
        const pm25Value = data.partikelDebu?.PM25 || 0;
        
        elAsap.innerText = kadarAsap;
        elPm25.innerText = pm25Value;

        // Update Progress Bar Asap (Warna menyesuaikan nilai)
        elBarAsap.style.width = `${Math.min(kadarAsap, 100)}%`;
        if (kadarAsap < 30) elBarAsap.style.backgroundColor = 'var(--accent-green)';
        else if (kadarAsap < 70) elBarAsap.style.backgroundColor = 'var(--accent-orange)';
        else elBarAsap.style.backgroundColor = 'var(--accent-red)';

        // Update Status PM2.5
        const statPM25 = data.partikelDebu?.status || '-';
        elPm25Status.innerText = statPM25;
        elPm25Status.style.color = statPM25.toLowerCase() === 'sehat'
            ? 'var(--accent-green)' : 'var(--accent-red)';

        // Baca nilai PM10
        const pm10Value = data.partikelDebu?.PM10 || 0;
        elPm10.innerText = pm10Value;
        const statPM10 = data.partikelDebu?.statusPM10 || '-';
        elPm10Status.innerText = statPM10;
        elPm10Status.style.color = statPM10.toLowerCase() === 'sehat'
            ? 'var(--accent-green)' : 'var(--accent-red)';

        // Hitung dan Tampilkan ISPU PM2.5
        const ispuPM25 = hitungISPU_PM25(pm25Value);
        elISPU_PM25.innerText   = ispuPM25.ispu;
        elISPU_PM25.style.color = ispuPM25.warna;
        elISPUKat_PM25.innerText   = ispuPM25.kategori;
        elISPUKat_PM25.style.color = ispuPM25.warna;
        if (elISPURing_PM25) {
            const deg25 = Math.round(Math.min(ispuPM25.ispu / 500, 1) * 360);
            elISPURing_PM25.style.background =
                `conic-gradient(${ispuPM25.warna} ${deg25}deg, var(--border-color) ${deg25}deg)`;
        }

        // Hitung dan Tampilkan ISPU PM10
        const ispuPM10 = hitungISPU_PM10(pm10Value);
        elISPU_PM10.innerText   = ispuPM10.ispu;
        elISPU_PM10.style.color = ispuPM10.warna;
        elISPUKat_PM10.innerText   = ispuPM10.kategori;
        elISPUKat_PM10.style.color = ispuPM10.warna;
        if (elISPURing_PM10) {
            const deg10 = Math.round(Math.min(ispuPM10.ispu / 500, 1) * 360);
            elISPURing_PM10.style.background =
                `conic-gradient(${ispuPM10.warna} ${deg10}deg, var(--border-color) ${deg10}deg)`;
        }

        // Update Main Air Status Badge
        const statusLevel = (data.statusLevel || '').toUpperCase();
        elStatusBadge.innerText = statusLevel;
        elStatusBadge.className = ''; // reset class
        if (statusLevel === 'BAIK') {
            elStatusBadge.classList.add('status-baik');
        } else if (statusLevel === 'BURUK' || statusLevel === 'TIDAK SEHAT') {
            elStatusBadge.classList.add('status-buruk');
        }



        // Format dan tampilkan waktu (Timestamp)
        let timeString = "-";
        if (data.timestamp) {
            const dateObj = new Date(data.timestamp);
            if (!isNaN(dateObj.getTime())) {
                timeString = dateObj.toLocaleTimeString('id-ID', { hour12: false });
                elLastUpdate.innerText = `${dateObj.toLocaleDateString('id-ID')} ${timeString}`;
            } else {
                // Fallback jika format timestamp tidak terbaca
                let rawTime = String(data.timestamp);
                timeString = rawTime.includes('T') ? rawTime.split('T')[1].substring(0,8) : rawTime.substring(0,8);
                elLastUpdate.innerText = rawTime;
            }
        }

        // Update Grafik (Chart.js)
        updateChart(timeString, kadarAsap, pm25Value, pm10Value);
    }
});


