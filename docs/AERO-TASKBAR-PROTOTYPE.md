# Prototipe glass untuk taskbar dengan alokasi terkendali

Permintaan pengguna: taskbar dahulu, memori sekecil mungkin dan diukur sebelum
menetapkan batas. Target visual adalah material Aero Glass Windows 7.

## Status

Prototipe sudah dikompilasi dan diuji untuk API efek, reuse graph dan lifecycle
resource. Uji sementara pertama memulihkan binary dan registry awal. Pengguna
kemudian memilih "Aktifkan dan biarkan aktif", lalu mengonfirmasi bahwa efek
terlihat bagus ketika slider digeser ke 100%. Tampilan identik Windows 7 dan
total biaya fitur dibandingkan DLL awal belum dibuktikan.

Pembaruan dengan kontrol Colors dimuat ke Explorer PID 30580 pada 14 Oktober
2026 sekitar 11:55 WIB, dengan AeroGlass=1 dan TaskbarTransparency=100.
Hash DLL aktif cocok dengan kandidat
`C78CF5F181CB9A1AC948CC6625DA5B062E23ED9EAE5420D16954096EEBB77820`.
Log mengonfirmasi `glass composition connected`. Konfirmasi visual pengguna
berlaku untuk renderer sebelum penambahan kontrol; renderer tidak diubah pada
pembaruan ini. Tampilan kontrol baru belum diverifikasi melalui UI karena
native pipe computer-use tidak tersedia.

Untuk mengaktifkan atau memulihkan konfigurasi yang disimpan:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Use-AeroPrototype.ps1 -Mode Enable
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Use-AeroPrototype.ps1 -Mode Restore
```

Script Enable/Restore menyimpan preferensi awal di
`native/out/aero-prototype/activation-state.json`. Ia memuat satu build pada
Explorer yang bersih dan menutup Settings yang memakai tweak agar tidak
memuat dua versi sekaligus. Kegagalan switch mengembalikan build stabil.
`active-verification.json` mencatat konfigurasi saat aktivasi; PID dapat
berubah setelah restart. Installer stabil di dist belum menyertakan mode ini.

Windows computer-use tidak dapat terhubung:
`Computer Use native pipe is unavailable: failed to connect native pipe: The system cannot find the file specified. (os error 2)`.
Counter proses/GPU dapat dibaca melalui shell di sesi Windows sebenarnya,
tetapi itu tidak menggantikan verifikasi piksel.

### Pengukuran pembaruan aktif pada 100%

Sampel singkat glass/plain/glass, sekitar 11:55 WIB, tanpa restart di antara
mode, tercatat di `native/out/aero-prototype/current-performance.json`.

| Halaman image DLL dalam Explorer | Ukuran |
| --- | ---: |
| Rentang virtual yang dipetakan | 388 KiB |
| Halaman resident dalam RAM | 276 KiB |
| Halaman resident privat (bagian dari 276 KiB) | 20 KiB |

QueryWorkingSetEx mengukur halaman image saja. Angka ini tidak mencakup heap,
stack, texture, dan resource compositor, sehingga bukan total alokasi tweak.
Private commit seluruh Explorer tetap 174.129.152 byte pada tiga fase.
Working set Explorer berubah kurang dari 0,11 MiB; shared GPU counter Explorer
tetap sama. Counter DWM berfluktuasi saat sampling dan tidak mengisolasi biaya
efek. Sampel CPU yang dinormalisasi terhadap 20 logical processor: Explorer
0–0,077%, DWM 0–0,231%. Task Manager tidak berjalan pada sampel ini; waktu
startup Task Manager dan penyebab kelambatan belum diukur.

### Pengaturan Colors

Tutup dan buka kembali **Settings → Personalization → Colors**. Di bawah
**Transparency effects**, build eksperimen menambahkan:

- **Taskbar transparency**: slider lama; 100% menghilangkan tint, blur dan
  pantulan tetap aktif ketika Aero Glass aktif.
- **Aero Glass**: menghidupkan glass atau kembali ke plain tint.
- **Blur strength**: 0–100% dipetakan ke sigma 0–16; default sigma 2,5,
  ditampilkan sekitar 16%.
- **Reflection strength**: 0–100% dipetakan ke alpha pantulan 0–30%; default
  alpha 12%, ditampilkan 40%.

Kontrol mengikuti switch Transparency effects bawaan. Nilai disimpan melalui
registry yang sama dan diterapkan tanpa restart. Warna tint masih melalui
registry. Scanner menyimpan weak reference ke toggle Colors selama dimuat
untuk menghindari traversal penuh setiap detik pada halaman tersebut.
Build berhasil; attachment, layout, dan interaksi kontrol baru pada Settings
nyata masih memerlukan verifikasi UI. Tidak ada klaim pengujian visual otomatis.

### Hasil uji taskbar nyata

Taskbar 1920 × 48 piksel; TaskbarTransparency 70%; 10 sampel per mode,
14 Oktober 2026 sekitar 11:27–11:28 WIB. Dua mode memakai DLL eksperimen dan
PID Explorer yang sama (27808). Log mengonfirmasi XAML composition connected
dan satu glass brush dibuat saat transisi pengukuran.

| Counter per proses | Median plain | Median glass | Selisih glass − plain |
| --- | ---: | ---: | ---: |
| Explorer private commit | 144,02 MiB | 143,43 MiB | −0,59 MiB |
| Explorer working set | 236,78 MiB | 236,83 MiB | +0,05 MiB |
| Explorer shared GPU memory | 43,86 MiB | 43,48 MiB | −0,38 MiB |
| DWM private commit | 250,36 MiB | 247,60 MiB | −2,76 MiB |
| DWM working set | 153,45 MiB | 152,00 MiB | −1,45 MiB |
| DWM shared GPU memory | 728,48 MiB | 738,71 MiB | +10,23 MiB |

Dedicated GPU memory pada counter dua proses tersebut bernilai 0 selama
kedua mode. DWM shared GPU memory pada glass bervariasi 729,24–739,29 MiB.
Selisih negatif bukan bukti efek menghemat memori: ada warmup, aktivitas host,
allocator/cache dan noise desktop. Counter DWM mencakup resource dari window
lain; +10,23 MiB belum dapat diatribusikan seluruhnya pada glass taskbar.

Shared GPU memory memakai system memory, dan resource yang dibagi lintas
proses dapat muncul pada counter beberapa proses sekaligus. Jangan menjumlah
counter GPU Explorer dan DWM sebagai RAM unik yang dialokasikan fitur.
[Penjelasan counter GPU Microsoft](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/).

Uji ini menunjukkan transisi glass tidak menimbulkan kenaikan multi-MiB pada
median working set Explorer dalam sampel tersebut. Ia bukan perbandingan
clean-start DLL stabil versus DLL eksperimen, bukan batas atas biaya memori,
dan bukan bukti kesamaan visual dengan Aero Glass.

Raw results di `native/out/aero-prototype/`:

- `taskbar-memory.json`: seluruh sampel dan counter.
- `taskbar-memory-summary.json`, `gpu-memory-summary.json`: median/range.
- `candidate-explorer.log`: koneksi Composition dan perubahan Fill.
- `temporary-test-results.json`: sukses eksperimen dan restore Explorer.
- `restore-verification.json`: cleanup Settings dan hash DLL stabil aktif.

Uji pertama ditolak karena verifier mengharuskan log baru meskipun mode plain
sudah aktif dari initial replay. Verifier diperbaiki agar membaca state Fill
terakhir beserta nilai slider; uji kedua berhasil. Kedua percobaan memulihkan
binary/registry. Cleanup akhir juga menghentikan Settings yang sempat memuat
DLL eksperimen, sehingga tidak ada callback/timer kandidat yang masih hidup.
Log mencatat timeout percobaan bootstrap ulang Settings selama sampling;
navigasi dan watcher Settings belum lolos regression test kandidat.

## Implementasi

- `native/experimental/AeroGlassBrush.h`: graph Windows Composition untuk
  backdrop → Gaussian blur → tint dan pantulan gradient. Tidak memakai Win2D
  runtime, bitmap reflection, screenshot loop, swap chain atau engine grafis
  tambahan milik aplikasi.
- `native/experimental/AeroTaskbarBackground.inl`: satu state per handle elemen
  taskbar, deduplikasi, callback token, cleanup Remove, dan cache brush. Hanya
  BackgroundFill di dalam TaskbarFrame diberi glass; BackgroundStroke memakai
  solid brush kecil. Watcher registry menggunakan pola thread versi awal.
- `native/TweakTap.cpp`: hook compile-time `TWEAK_EXPERIMENTAL_AERO`. Tanpa
  define ini, renderer solid dan alur build stabil tetap dipakai.
- Injector tetap keluar setelah memuat DLL. Bootstrap Settings dan watcher
  Settings tetap menggunakan jalur yang sudah ada.

Gradient pantulan dan parameter default adalah rancangan awal. Ini bukan
renderer atau asset Aero Windows 7. Microsoft mendeskripsikan Aero sebagai
blur kaca ditambah highlight dinamis yang mensimulasikan pantulan lingkungan;
gradient ini belum membuktikan kecocokan dengan highlight asli.

Referensi:
[perilaku Aero Windows 7](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-rdpcr2/3f7d2f30-5aa8-46c5-8efe-25d531d001ff),
[Composition brush combinations](https://learn.microsoft.com/en-us/windows/uwp/composition/composition-brushes),
[CreateBackdropBrush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.composition.compositor.createbackdropbrush).

## Build dan probe

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\build-aero-prototype.ps1
.\native\out\aero-prototype\AeroGlassProbe.exe
```

Output hanya di `native/out/aero-prototype/`:

| Binary | Ukuran file |
| --- | ---: |
| DLL stabil saat ini / stable branch compile check | 295.424 byte (288,5 KiB) |
| DLL eksperimen dengan kontrol Colors | 377.344 byte (368,5 KiB) |
| Selisih ukuran file DLL | 81.920 byte (80 KiB) |

Ukuran file bukan total pemakaian RAM. Installer di dist dan DLL stabil di
native/out tidak diganti oleh build ini.

Probe memakai compositor dalam prosesnya sendiri, tanpa window/target render.
Ia menguji batas ABI effect description, konfigurasi NaN, nilai blur akhir,
identity brush selama update, jumlah factory build, dan Close/reconnect.

Hasil run terakhir:

| Fase | Private commit | Working set |
| --- | ---: | ---: |
| Compositor siap | 1.675.264 byte | 10.293.248 byte |
| Graph siap | 1.732.608 byte | 11.100.160 byte |
| Setelah 10.000 update | 1.732.608 byte | 11.100.160 byte |
| Setelah 20.000 update | 1.732.608 byte | 11.100.160 byte |
| Setelah close/reconnect/close | 1.732.608 byte | 11.145.216 byte |

Graph menambah 57.344 byte (56 KiB) private commit dan 806.912 byte (788 KiB)
working set pada probe ini. Selama dua batch update, private commit dan working
set tidak bertambah. Factory dan effect brush yang sama dipakai selama seluruh
20.000 update. Close/reconnect membentuk tepat satu graph baru.

**Angka ini bukan tambahan RAM taskbar yang sudah dirender dan bukan biaya
GPU.** Alokasi texture/shader/render dapat terjadi belakangan. Tidak ada klaim
bahwa compositor/driver segera mengembalikan seluruh cache setelah Close.
Raw output: `native/out/aero-prototype/probe-results.txt`.

Build eksperimen, stable compile check dan injector berhasil tanpa warning
setelah koreksi. Script PowerShell lolos parser. Guard benchmark dijalankan
terhadap instalasi stabil dan berhasil menolak sebelum write registry; nilai
TaskbarTransparency/AeroGlass terverifikasi tidak berubah.

## Parameter eksperimen

Di `HKCU\Software\TransparencyTweak` (hanya berlaku pada DLL eksperimen):

| Nilai DWORD | Default / perilaku |
| --- | --- |
| AeroGlass | 1. Set 0 untuk plain tint sebagai mode pembanding. |
| GlassBlurSigmaMilli | 2500 = sigma 2,5; dibatasi 0–16. |
| GlassReflectionPercent | 12; dibatasi 0–30%. |
| GlassTintArgb | 0xFF74A6CD. Channel RGB dipakai; alpha dihitung dari slider. |
| TaskbarTransparency | Slider yang sudah ada. 0 memulihkan native Fill. Pada mode glass, 100 menghilangkan tint tetapi mempertahankan blur/pantulan. |

Windows Transparency effects off memulihkan native brush. Semantik 100 pada
mode glass berbeda dari mode plain tint yang benar-benar clear; karena itu
test stabil yang mengharuskan Fill opacity 0 pada 100 bukan test visual mode
glass. Mode plain tetap memiliki semantik lama.

## Pengukuran langsung berikutnya

Setelah DLL eksperimen dimuat ke Explorer yang bersih:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Measure-AeroTaskbar.ps1
```

Script memverifikasi hash DLL yang aktif dan menolak bila masih ada DLL lain.
Ia memakai nilai slider yang sama untuk plain tint dan glass, mengambil sampel
Explorer/DWM/Settings serta GPU counters, lalu mengembalikan kedua nilai registry
yang diubah. Counter GPU yang tidak tersedia dicatat sebagai unknown.
Pengukuran ini warm A/B, bukan cold-start measurement atau verifikasi visual.

Untuk uji penolakan terhadap instalasi stabil:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Check-BenchmarkGuard.ps1
```

DLL lama tetap terkunci di Explorer sampai host keluar. Menjalankan injector
eksperimen saat DLL stabil masih aktif tidak menggantikannya. Uji taskbar
langsung memerlukan pemuatan kandidat setelah restart Explorer; jendela File
Explorer dapat tertutup. Jangan menjalankan kedua renderer bersamaan.

Backup exact installed DLL/injector disiapkan di
`backups/before-aero-experiment-20261014/`, dengan checksum pada `hashes.json`.
Backup ini untuk binary instalasi, bukan rollback seluruh perubahan source.

## Yang masih harus dibuktikan

1. Perilaku blur backdrop di belakang taskbar ketika fokus berpindah. Pengguna
   telah mengonfirmasi efek terlihat bagus pada 100%, tetapi kasus fokus belum
   diverifikasi secara terpisah.
2. Material kaca, tint dan pantulan cocok dengan acuan Aero Glass Windows 7.
3. Biaya total DLL eksperimen terhadap DLL awal dalam clean-start comparison,
   atribusi alokasi GPU, startup peak dan kondisi stabil lebih lama. Warm
   comparison plain/glass sudah dilakukan, tetapi tidak menuntaskan pertanyaan
   tersebut.
4. Lifecycle XAML sesungguhnya, multi-monitor/rebuild, perubahan tema, mode off,
   serta navigasi Settings tetap sehat. Probe tidak menguji callback XAML dalam
   Explorer.
