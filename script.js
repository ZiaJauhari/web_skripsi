import { initializeApp } from "https://www.gstatic.com/firebasejs/10.11.0/firebase-app.js";
import { getDatabase, ref, onValue, query, limitToLast, set, update } from "https://www.gstatic.com/firebasejs/10.11.0/firebase-database.js";


// =========================================================================
// 1. CONFIGURASI FIREBASE
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
// Query history terbatas pada 20 entri terakhir untuk performa dan kenyamanan
const historyQueryRef = query(ref(db, "airQuality/history"), limitToLast(20));

// =========================================================================
// 2. DETEKSI KATEGORI ISPU & WARNA
// =========================================================================
function dapatkanKategoriISPU(ispu) {
    if (ispu < 0) return { kategori: '-', warna: 'var(--text-muted)' };

    let kategori, warna;
    if (ispu <= 50) {
        kategori = 'Baik';
        warna = 'var(--accent-green)';
    } else if (ispu <= 100) {
        kategori = 'Sedang';
        warna = '#a3e635'; // lime
    } else if (ispu <= 200) {
        kategori = 'Tidak Sehat';
        warna = 'var(--accent-orange)';
    } else if (ispu <= 300) {
        kategori = 'Sangat Tidak Sehat';
        warna = 'var(--accent-purple)';
    } else {
        kategori = 'Berbahaya';
        warna = 'var(--accent-red)';
    }

    return { kategori, warna };
}

// =========================================================================
// 3. INISIALISASI CHART.JS (3 GRAFIK TERPISAH)
// =========================================================================
const chartOptions = (label, color, bordercolor) => ({
    responsive: true,
    maintainAspectRatio: false,
    color: '#94a3b8',
    scales: {
        y: {
            beginAtZero: true,
            grid: { color: '#1e293b' },
            ticks: { 
                color: '#64748b',
                font: { family: "'Outfit', sans-serif" }
            }
        },
        x: {
            grid: { color: '#1e293b' },
            ticks: { 
                color: '#64748b',
                font: { family: "'Outfit', sans-serif" },
                maxRotation: 0,
                autoSkip: true,
                maxTicksLimit: 5
            }
        }
    },
    plugins: {
        legend: {
            display: false
        },
        tooltip: {
            backgroundColor: '#131c2e',
            titleColor: '#f8fafc',
            bodyColor: '#f8fafc',
            borderColor: '#1e293b',
            borderWidth: 1,
            titleFont: { family: "'Outfit', sans-serif" },
            bodyFont: { family: "'Outfit', sans-serif" }
        }
    }
});

// PM2.5 Chart
const ctxPM25 = document.getElementById('chartPM25').getContext('2d');
const chartPM25 = new Chart(ctxPM25, {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            data: [],
            borderColor: 'rgba(56, 189, 248, 1)', // Sky Blue
            backgroundColor: 'rgba(56, 189, 248, 0.1)',
            borderWidth: 2,
            tension: 0.3,
            fill: true,
            pointBackgroundColor: 'rgba(56, 189, 248, 1)',
            pointRadius: 2
        }]
    },
    options: chartOptions('PM2.5', 'rgba(56, 189, 248, 0.1)', 'rgba(56, 189, 248, 1)')
});

// PM10 Chart
const ctxPM10 = document.getElementById('chartPM10').getContext('2d');
const chartPM10 = new Chart(ctxPM10, {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            data: [],
            borderColor: 'rgba(163, 230, 53, 1)', // Lime
            backgroundColor: 'rgba(163, 230, 53, 0.08)',
            borderWidth: 2,
            tension: 0.3,
            fill: true,
            pointBackgroundColor: 'rgba(163, 230, 53, 1)',
            pointRadius: 2
        }]
    },
    options: chartOptions('PM10', 'rgba(163, 230, 53, 0.08)', 'rgba(163, 230, 53, 1)')
});

// CO Chart
const ctxCO = document.getElementById('chartCO').getContext('2d');
const chartCO = new Chart(ctxCO, {
    type: 'line',
    data: {
        labels: [],
        datasets: [{
            data: [],
            borderColor: 'rgba(249, 115, 22, 1)', // Orange
            backgroundColor: 'rgba(249, 115, 22, 0.1)',
            borderWidth: 2,
            tension: 0.3,
            fill: true,
            pointBackgroundColor: 'rgba(249, 115, 22, 1)',
            pointRadius: 2
        }]
    },
    options: chartOptions('CO', 'rgba(249, 115, 22, 0.1)', 'rgba(249, 115, 22, 1)')
});

// Fungsi untuk update data di ketiga chart sekaligus
function updateCharts(historyList) {
    const labels = [];
    const pm25Data = [];
    const pm10Data = [];
    const coData = [];

    historyList.forEach(entry => {
        let labelTime = "-";
        if (entry.timestamp) {
            const dateObj = new Date(entry.timestamp);
            if (!isNaN(dateObj.getTime())) {
                labelTime = dateObj.toLocaleTimeString('id-ID', { hour12: false, hour: '2-digit', minute: '2-digit' });
            } else {
                labelTime = String(entry.timestamp).substring(11, 16); // Ambil jam:menit dari string ISO
            }
        }
        labels.push(labelTime);
        pm25Data.push(entry.pm25 !== undefined ? entry.pm25 : 0);
        pm10Data.push(entry.pm10 !== undefined ? entry.pm10 : 0);
        coData.push(entry.co !== undefined ? entry.co : 0);
    });

    // PM2.5 Update
    chartPM25.data.labels = labels;
    chartPM25.data.datasets[0].data = pm25Data;
    chartPM25.update();

    // PM10 Update
    chartPM10.data.labels = labels;
    chartPM10.data.datasets[0].data = pm10Data;
    chartPM10.update();

    // CO Update
    chartCO.data.labels = labels;
    chartCO.data.datasets[0].data = coData;
    chartCO.update();
}

// =========================================================================
// 4. UI ELEMENT BINDINGS & REAL-TIME LISTENER
// =========================================================================
const elConnStatus      = document.getElementById('conn-status');
const elStatusBadge     = document.getElementById('status-level-badge');
const elSuhu            = document.getElementById('val-suhu');
const elKelembaban      = document.getElementById('val-kelembaban');
const elPm25            = document.getElementById('val-pm25');
const elPm10            = document.getElementById('val-pm10');
const elCo              = document.getElementById('val-co');
const elIspuPM25        = document.getElementById('val-ispu-pm25');
const elIspuPM10        = document.getElementById('val-ispu-pm10');
const elIspuCO          = document.getElementById('val-ispu-co');
const elLastUpdate      = document.getElementById('last-update');

// ISPU Elements
const elISPU            = document.getElementById('val-ispu');
const elISPUKategori    = document.getElementById('val-ispu-kategori');
const elISPURing        = document.getElementById('ispu-ring');
const elParamKritis     = document.getElementById('val-param-kritis');
const elExhaustFan      = document.getElementById('val-exhaust-fan');
const elFanIcon         = document.getElementById('fan-icon');
const elPurifier        = document.getElementById('val-purifier');
const elPurifierIcon    = document.getElementById('purifier-icon');

// History Elements
const elHistoryTbody    = document.getElementById('history-tbody');
const elHistoryCount    = document.getElementById('history-count');

// Listener status koneksi Firebase
onValue(connectedRef, (snap) => {
    if (snap.val() === true) {
        elConnStatus.className = "badge-conn connected";
        elConnStatus.innerHTML = '<i class="fas fa-wifi"></i> Connected';
    } else {
        elConnStatus.className = "badge-conn disconnected";
        elConnStatus.innerHTML = '<i class="fas fa-times-circle"></i> Disconnected';
    }
});

// Listener data sensor real-time
onValue(airQualityRef, (snapshot) => {
    const data = snapshot.val();
    if (data) {
        // Cek Preheating
        const preheatingOverlay = document.getElementById('preheating-overlay');
        const preheatingCountdown = document.getElementById('preheating-countdown');
        if (data.preheating === true) {
            if (preheatingOverlay) preheatingOverlay.style.display = 'flex';
            if (preheatingCountdown) preheatingCountdown.innerText = data.countdown || '60';
        } else {
            if (preheatingOverlay) preheatingOverlay.style.display = 'none';
        }

        // Iklim Ruangan
        elSuhu.innerText = data.suhu !== undefined ? data.suhu.toFixed(1) : '0.0';
        elKelembaban.innerText = data.kelembaban !== undefined ? data.kelembaban.toFixed(1) : '0.0';

        // Konsentrasi Polutan
        const pm25Val = data.pm25 !== undefined ? data.pm25 : 0;
        const pm10Val = data.pm10 !== undefined ? data.pm10 : 0;
        const coVal   = data.co !== undefined ? data.co : 0;

        elPm25.innerText = pm25Val.toFixed(1);
        elPm10.innerText = pm10Val.toFixed(1);
        elCo.innerText   = coVal.toFixed(2);

        // ISPU per Parameter
        const ispuPM25 = data.ispu?.pm25 !== undefined ? data.ispu.pm25 : '-';
        const ispuPM10 = data.ispu?.pm10 !== undefined ? data.ispu.pm10 : '-';
        const ispuCO   = data.ispu?.co !== undefined ? data.ispu.co : '-';

        elIspuPM25.innerText = ispuPM25;
        elIspuPM25.style.backgroundColor = ispuPM25 !== '-' ? dapatkanKategoriISPU(ispuPM25).warna + '22' : 'transparent';
        elIspuPM25.style.color = ispuPM25 !== '-' ? dapatkanKategoriISPU(ispuPM25).warna : 'var(--text-light)';

        elIspuPM10.innerText = ispuPM10;
        elIspuPM10.style.backgroundColor = ispuPM10 !== '-' ? dapatkanKategoriISPU(ispuPM10).warna + '22' : 'transparent';
        elIspuPM10.style.color = ispuPM10 !== '-' ? dapatkanKategoriISPU(ispuPM10).warna : 'var(--text-light)';

        elIspuCO.innerText   = ispuCO;
        elIspuCO.style.backgroundColor = ispuCO !== '-' ? dapatkanKategoriISPU(ispuCO).warna + '22' : 'transparent';
        elIspuCO.style.color = ispuCO !== '-' ? dapatkanKategoriISPU(ispuCO).warna : 'var(--text-light)';

        // ISPU Akhir (Ring Gauge)
        const ispuAkhir = data.ispu?.akhir !== undefined ? Number(data.ispu.akhir) : 0;
        const catInfo   = dapatkanKategoriISPU(ispuAkhir);

        elISPU.innerText = ispuAkhir;
        elISPU.style.color = catInfo.warna;
        elISPUKategori.innerText = data.ispu?.kategori || catInfo.kategori;
        elISPUKategori.style.color = catInfo.warna;

        // Visualisasi Ring Gauge
        const deg = Math.round(Math.min(ispuAkhir / 500, 1) * 360);
        elISPURing.style.background = `conic-gradient(${catInfo.warna} ${deg}deg, var(--border-color) ${deg}deg)`;

        // Parameter Kritis
        elParamKritis.innerText = data.ispu?.paramKritis || '-';

        // Exhaust Fan Status & Animasi
        const isFanOn = (data.exhaustFan === "ON");
        elExhaustFan.innerText = isFanOn ? "AKTIF" : "MATI";
        elExhaustFan.style.color = isFanOn ? "var(--accent-blue)" : "var(--text-muted)";
        
        if (isFanOn) {
            elFanIcon.classList.add("fan-spin");
            elFanIcon.style.color = "var(--accent-blue)";
        } else {
            elFanIcon.classList.remove("fan-spin");
            elFanIcon.style.color = "var(--text-muted)";
        }

        // Air Purifier Status & Animasi
        const isPurifierOn = (data.purifier === "ON");
        elPurifier.innerText = isPurifierOn ? "AKTIF" : "MATI";
        elPurifier.style.color = isPurifierOn ? "var(--accent-green)" : "var(--text-muted)";
        
        if (isPurifierOn) {
            elPurifierIcon.classList.add("purifier-pulse");
            elPurifierIcon.style.color = "var(--accent-green)";
        } else {
            elPurifierIcon.classList.remove("purifier-pulse");
            elPurifierIcon.style.color = "var(--text-muted)";
        }

        // Header Kategori ISPU Badge
        const headerCat = (data.ispu?.kategori || catInfo.kategori || '').toUpperCase();
        elStatusBadge.innerText = headerCat;
        elStatusBadge.className = ''; // reset class
        
        if (headerCat === 'BAIK' || headerCat === 'SEDANG') {
            elStatusBadge.classList.add('status-baik');
        } else if (headerCat !== '') {
            elStatusBadge.classList.add('status-buruk');
        }

        // Waktu Update Terakhir
        if (data.timestamp) {
            const dateObj = new Date(data.timestamp);
            if (!isNaN(dateObj.getTime())) {
                const dateStr = dateObj.toLocaleDateString('id-ID', { day: '2-digit', month: 'short', year: 'numeric' });
                const timeStr = dateObj.toLocaleTimeString('id-ID', { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
                elLastUpdate.innerText = `${dateStr} ${timeStr}`;
            } else {
                elLastUpdate.innerText = String(data.timestamp);
            }
        }

        // MQ-135 Ro Aktif
        const roVal = data.calibration?.ro_ohm !== undefined ? data.calibration.ro_ohm : null;
        if (roVal !== null) {
            document.getElementById('val-ro-active').innerText = `${roVal.toFixed(0)} Ω`;
        } else {
            document.getElementById('val-ro-active').innerText = '-';
        }
    }
});


// =========================================================================
// 5. RIWAYAT DATA & GRAFIK (REAL-TIME HISTORY QUERY)
// =========================================================================
onValue(historyQueryRef, (snapshot) => {
    const historyList = [];
    snapshot.forEach((childSnapshot) => {
        const key = childSnapshot.key;
        const entry = childSnapshot.val();
        historyList.push({
            id: key,
            ...entry
        });
    });

    // Balik urutan list agar data terbaru tampil di paling atas tabel
    const reversedHistory = [...historyList].reverse();

    // Update charts dengan urutan kronologis (historyList)
    updateCharts(historyList);

    // Update Counter
    elHistoryCount.innerText = `${historyList.length} entri`;

    // Render data di tabel
    if (reversedHistory.length === 0) {
        elHistoryTbody.innerHTML = `
            <tr>
                <td colspan="9" class="history-empty">Belum ada riwayat data tersimpan.</td>
            </tr>
        `;
    } else {
        elHistoryTbody.innerHTML = reversedHistory.map(entry => {
            let formattedTime = "-";
            if (entry.timestamp) {
                const dateObj = new Date(entry.timestamp);
                if (!isNaN(dateObj.getTime())) {
                    const d = dateObj.toLocaleDateString('id-ID', { day: '2-digit', month: '2-digit' });
                    const t = dateObj.toLocaleTimeString('id-ID', { hour: '2-digit', minute: '2-digit', hour12: false });
                    formattedTime = `${d} ${t}`;
                } else {
                    formattedTime = String(entry.timestamp).substring(5, 16).replace('T', ' ');
                }
            }

            const pm25 = entry.pm25 !== undefined ? entry.pm25.toFixed(1) : '0.0';
            const pm10 = entry.pm10 !== undefined ? entry.pm10.toFixed(1) : '0.0';
            const co   = entry.co !== undefined ? entry.co.toFixed(2) : '0.00';
            const ispu = entry.ispu_akhir !== undefined ? entry.ispu_akhir : '-';
            const kat  = entry.kategori || '-';
            const crit = entry.paramKritis || '-';
            const fan  = entry.exhaustFan || '-';
            const purif = entry.purifier || '-';

            // Klasifikasi badge untuk kategori di tabel
            let ispuBadgeClass = "history-badge";
            if (kat === 'Baik') ispuBadgeClass += " ispu-baik";
            else if (kat === 'Sedang') ispuBadgeClass += " ispu-sedang";
            else if (kat === 'Tidak Sehat') ispuBadgeClass += " ispu-tidak-sehat";
            else if (kat === 'Sangat Tidak Sehat') ispuBadgeClass += " ispu-sangat-tidak-sehat";
            else if (kat === 'Berbahaya') ispuBadgeClass += " ispu-berbahaya";

            // Class badge exhaust fan & purifier
            const fanBadgeClass = `history-badge ${fan === 'ON' ? 'fan-on' : 'fan-off'}`;
            const purifBadgeClass = `history-badge ${purif === 'ON' ? 'fan-on' : 'fan-off'}`;

            return `
                <tr>
                    <td>${formattedTime}</td>
                    <td>${pm25} <span class="card-unit">µg/m³</span></td>
                    <td>${pm10} <span class="card-unit">µg/m³</span></td>
                    <td>${co} <span class="card-unit">ppm</span></td>
                    <td><strong>${ispu}</strong></td>
                    <td><span class="${ispuBadgeClass}">${kat}</span></td>
                    <td><span class="ispu-inline-badge">${crit}</span></td>
                    <td><span class="${fanBadgeClass}">${fan}</span></td>
                    <td><span class="${purifBadgeClass}">${purif}</span></td>
                </tr>
            `;
        }).join('');
    }
});

// =========================================================================
// 6. PANEL KONTROL & KALIBRASI LOGIC
// =========================================================================
let isConfigLoaded = false;
const configRef = ref(db, "config");

// Sinkronisasi real-time nilai input konfigurasi dari Firebase
onValue(configRef, (snapshot) => {
    const config = snapshot.val();
    if (config) {
        // Hanya isi form otomatis pertama kali agar tidak mengganggu ketikan user
        if (!isConfigLoaded) {
            if (config.calibration?.pm25_multiplier !== undefined) {
                document.getElementById('input-pm25-mult').value = config.calibration.pm25_multiplier;
            }
            if (config.calibration?.pm10_multiplier !== undefined) {
                document.getElementById('input-pm10-mult').value = config.calibration.pm10_multiplier;
            }
            if (config.calibration?.use_humidity_corr !== undefined) {
                document.getElementById('input-use-humidity-corr').checked = config.calibration.use_humidity_corr;
            }
            if (config.thresholds?.asap_max !== undefined) {
                document.getElementById('input-thresh-co').value = config.thresholds.asap_max;
            }
            if (config.thresholds?.pm25_tidak_sehat !== undefined) {
                document.getElementById('input-thresh-pm25').value = config.thresholds.pm25_tidak_sehat;
            }
            if (config.thresholds?.pm10_tidak_sehat !== undefined) {
                document.getElementById('input-thresh-pm10').value = config.thresholds.pm10_tidak_sehat;
            }
            isConfigLoaded = true;
        }

        // Tampilkan status kalibrasi MQ-135 real-time
        const calib = config.calibration || {};
        const calibStatus = calib.status || "Belum Kalibrasi";
        const progress = calib.progress || 0;
        const isTriggered = calib.trigger || false;

        document.getElementById('val-calib-status').innerText = calibStatus;

        const progressWrap = document.getElementById('calib-progress-wrap');
        const progressBar = document.getElementById('calib-progress-bar');
        const btnTrigger = document.getElementById('btn-trigger-calib');

        if (isTriggered || progress > 0) {
            progressWrap.style.display = 'block';
            progressBar.style.width = `${progress}%`;
            btnTrigger.disabled = true;
            btnTrigger.innerHTML = `<i class="fas fa-spinner fa-spin"></i> Kalibrasi (${progress}%)`;
            document.getElementById('val-calib-status').style.color = 'var(--accent-blue)';
        } else {
            progressWrap.style.display = 'none';
            progressBar.style.width = `0%`;
            btnTrigger.disabled = false;
            btnTrigger.innerHTML = `<i class="fas fa-sync-alt"></i> Jalankan Kalibrasi MQ-135`;

            if (calibStatus === "Terkalibrasi") {
                document.getElementById('val-calib-status').style.color = 'var(--accent-green)';
            } else if (calibStatus.startsWith("Gagal")) {
                document.getElementById('val-calib-status').style.color = 'var(--accent-red)';
            } else {
                document.getElementById('val-calib-status').style.color = 'var(--text-light)';
            }
        }
    } else {
        // Jika data config kosong di Firebase, set default agar bisa langsung tersimpan saat user klik save
        document.getElementById('val-calib-status').innerText = "Belum Konfigurasi";
    }
});

// Event Handler Simpan Pengaturan
document.getElementById('btn-save-settings').addEventListener('click', () => {
    const pm25Mult = parseFloat(document.getElementById('input-pm25-mult').value);
    const pm10Mult = parseFloat(document.getElementById('input-pm10-mult').value);
    const useHumidCorr = document.getElementById('input-use-humidity-corr').checked;
    const threshCo = parseInt(document.getElementById('input-thresh-co').value);
    const threshPm25 = parseInt(document.getElementById('input-thresh-pm25').value);
    const threshPm10 = parseInt(document.getElementById('input-thresh-pm10').value);

    if (isNaN(pm25Mult) || isNaN(pm10Mult) || isNaN(threshCo) || isNaN(threshPm25) || isNaN(threshPm10)) {
        showToast("Harap masukkan nilai konfigurasi yang valid!", "error");
        return;
    }

    const updates = {};
    updates['config/calibration/pm25_multiplier'] = pm25Mult;
    updates['config/calibration/pm10_multiplier'] = pm10Mult;
    updates['config/calibration/use_humidity_corr'] = useHumidCorr;
    updates['config/thresholds/asap_max'] = threshCo;
    updates['config/thresholds/pm25_tidak_sehat'] = threshPm25;
    updates['config/thresholds/pm10_tidak_sehat'] = threshPm10;

    update(ref(db), updates)
        .then(() => {
            showToast("Pengaturan berhasil disimpan ke Firebase!", "success");
        })
        .catch((error) => {
            showToast(`Gagal menyimpan: ${error.message}`, "error");
        });
});

// Event Handler Trigger Kalibrasi MQ-135
document.getElementById('btn-trigger-calib').addEventListener('click', () => {
    const confirmMsg = "PASTIKAN sensor MQ-135 saat ini sedang berada di udara bersih.\n\nSistem akan mengambil sampel selama 120 detik untuk menghitung ulang nilai resistansi baseline (Ro).\n\nApakah Anda yakin ingin memulai kalibrasi?";
    if (confirm(confirmMsg)) {
        const calibTriggerRef = ref(db, "config/calibration");
        update(calibTriggerRef, {
            trigger: true,
            progress: 1,
            status: "Memulai..."
        })
        .then(() => {
            showToast("Kalibrasi MQ-135 telah dimulai!", "info");
        })
        .catch((error) => {
            showToast(`Gagal memulai kalibrasi: ${error.message}`, "error");
        });
    }
});

// Helper Toast Notification
function showToast(message, type = "success") {
    const container = document.getElementById('toast-container');
    if (!container) return;

    const toast = document.createElement('div');
    toast.className = `toast toast-${type}`;

    let icon = '<i class="fas fa-check-circle"></i>';
    if (type === "error") {
        icon = '<i class="fas fa-exclamation-circle"></i>';
    } else if (type === "info") {
        icon = '<i class="fas fa-info-circle"></i>';
    }

    toast.innerHTML = `${icon} <span>${message}</span>`;
    container.appendChild(toast);

    // Fade out & hapus toast
    setTimeout(() => {
        toast.style.animation = 'slideInRight 0.3s cubic-bezier(0.4, 0, 0.2, 1) reverse';
        toast.addEventListener('animationend', () => {
            toast.remove();
        });
    }, 4000);
}

