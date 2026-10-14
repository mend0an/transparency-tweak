# Jalur glass blur dengan overhead kecil

Riset 14 Oktober 2026. Tujuan: taskbar kaca transparan yang tetap memperlihatkan
aplikasi di belakangnya, dengan overhead Transparency Tweak sekecil mungkin.
Dokumen ini menjelaskan pilihan implementasi; belum mengubah renderer atau
mengukur versi Acrylic baru.

## Kesimpulan

Kandidat utama adalah `Windows.UI.Xaml.Media.AcrylicBrush` bawaan Windows,
dipasang oleh TweakTap.dll yang sudah bekerja di Explorer. Simpan brush pada
state elemen taskbar dan gunakan kembali saat konfigurasi berubah. Injector
tetap keluar setelah pemasangan; fitur ini tidak memerlukan proses permanen,
DLL TranslucentTB, atau Windows App SDK baru.

Ini pilihan berdasarkan kesesuaian dengan arsitektur yang sudah ada dan bukti
implementasi TranslucentTB, bukan hasil benchmark yang membuktikan bahwa native
Acrylic selalu lebih hemat total memori daripada custom Gaussian blur.

Menggunakan komponen bawaan menghindari tambahan engine milik aplikasi,
tetapi resource XAML/Composition, intermediate surfaces dan cache tetap dapat
menambah commit, working set dan penggunaan memori GPU.

## Kandidat yang ditelusuri

| Jalur | Bukti dan batas | Keputusan untuk taskbar |
| --- | --- | --- |
| API Aero lama `DwmEnableBlurBehindWindow` | Microsoft menyatakan bahwa sejak Windows 8 pemanggilan API ini tidak menghasilkan efek blur lama. Keberadaan dwmapi.dll dan aero.msstyles bukan bukti adanya switch untuk memulihkan renderer Windows 7. | Tidak menyediakan solusi terdokumentasi untuk kebutuhan ini. Tidak ada kesimpulan bahwa semua kode internal historis telah dihapus. |
| DWM Desktop Acrylic dengan `DWMWA_SYSTEMBACKDROP_TYPE` dan `DWMSBT_TRANSIENTWINDOW` | API bawaan Windows 11 build 22621+. Cocok untuk backdrop HWND, tetapi tidak otomatis menghilangkan Fill XAML yang menutupinya. Enum ini tidak menawarkan slider radius blur/tint. Interaksi dengan kebijakan backdrop taskbar dan penulisan ulang oleh Explorer belum diuji. | Kandidat eksperimen terpisah; belum terbukti cocok atau paling hemat pada taskbar build ini. |
| UWP `Windows.UI.Xaml.Media.AcrylicBrush`, `Backdrop` | TranslucentTB 2026.2 menggunakan jalur ini di ExplorerTAP. REA mengonfirmasi factory AcrylicBrush dan pemasangan brush pada Fill. TweakTap sudah memakai Windows.UI.Xaml dan melacak Fill taskbar. | Kandidat implementasi pertama. |
| Custom `XamlCompositionBrushBase` + Gaussian blur | Graph efek dikonfirmasi pada rilis TranslucentTB. Memungkinkan radius blur terpisah dari tint. Tambahan objek/factory perlu dikelola dan hasil memori perlu dibandingkan. | Pilihan berikutnya jika native Acrylic terlalu buram atau tidak memenuhi hasil visual. |

## Cara memakai Acrylic pada taskbar transparan

Dokumentasi enum membedakan:

- `HostBackdrop`: konten di belakang window.
- `Backdrop`: konten aplikasi di belakang elemen XAML.

TranslucentTB secara khusus memilih `Backdrop` pada taskbar karena HWND taskbar
transparan sehingga konten di belakang window tersedia di belakang XAML.
Komentar source juga menjelaskan bahwa pilihan ini menghindari penonaktifan
HostBackdrop ketika window kehilangan fokus. Itu penjelasan source; hasil
pada Windows 26300.9457 masih perlu dibuktikan secara visual.

Sketsa konfigurasi awal, bukan kode yang sudah diterapkan:

```cpp
// Jalankan pada UI thread milik elemen taskbar.
// Gunakan Windows.UI.Xaml.Media, bukan Microsoft.UI.Xaml milik WinUI 3.
if (!state.acrylic)
{
    state.acrylic = wux::Media::AcrylicBrush();
    state.acrylic.BackgroundSource(wux::Media::AcrylicBackgroundSource::Backdrop);
    state.acrylic.Opacity(1.0);
}
state.acrylic.TintColor(themeTint);
state.acrylic.TintOpacity(tintOpacity);
shape.Fill(state.acrylic);
```

`TintOpacity` dan `TintLuminosityOpacity` perlu dituning untuk kaca tipis.
Mengurangi Opacity seluruh brush juga mengurangi kontribusi blur, sehingga
transparansi tint dan opacity keseluruhan harus dibedakan. Konfigurasi ini
memerlukan latar HWND taskbar yang benar-benar transparan. Native Acrylic
tidak menyediakan pengaturan radius Gaussian secara langsung.

## Perubahan yang menjaga alokasi tetap terkendali

1. Tambahkan cached brush pada `BrushState` di `native/TweakTap.cpp`; lakukan
   lazy creation saat efek dipakai. Ubah properti ketika slider berubah.
   Jangan buat effect factory atau brush baru setiap update.
2. Blur hanya `BackgroundFill`; gunakan brush solid kecil untuk
   `BackgroundStroke`. Jangan menerapkan blur ke teks, ikon, atau setiap item.
3. Batasi elemen ke descendant `TaskbarFrame`, deduplikasi handle, dan simpan
   state sesuai UI thread/dispatcher. Jangan berbagi objek XAML lintas UI
   thread tanpa memastikan aturan thread-nya.
4. Simpan token `RegisterPropertyChangedCallback` dan tangani mutation Remove.
   Lepaskan callback, cached brush, original brush dan entry ketika elemen
   taskbar dihapus. Gunakan cleanup pada UI thread elemen.
5. Pertahankan salinan native Fill untuk restore. Saat efek dinonaktifkan,
   ganti kembali Fill dan lepaskan cached brush setelah tidak dipakai.
   Cache compositor/driver tidak selalu langsung hilang setelah referensi
   aplikasi dilepas.
6. Perubahan efek memakai watcher registry yang sudah ada. Tidak perlu
   menambah thread, polling, timer render, screenshot loop, swap chain milik
   aplikasi, atau buffer layar penuh untuk fitur blur taskbar ini.
7. Untuk kebutuhan semua menu/isi Explorer, ukur sebagai fitur terpisah:
   jumlah dan luas permukaan bertambah sehingga anggaran taskbar tidak bisa
   dianggap berlaku untuk seluruh UI.

### Temuan di kode saat ini

`Apply()` membuat `SolidColorBrush` setiap update. `Track()` selalu menambah
entry ke `m_entries`, tidak menyimpan token callback. `OnVisualTreeChange()`
mengabaikan semua mutation selain Add. Tidak ada jalur erase entry taskbar
yang terlihat di kelas tersebut.

Ini menunjukkan kemungkinan retensi state/brush lama ketika elemen taskbar
dibuat ulang. Belum dilakukan pengukuran yang membuktikan besarnya retensi
atau memory leak runtime. Lifecycle perlu dirapikan sebelum cached Acrylic
ditambahkan agar penggunaan memori tidak tumbuh mengikuti jumlah rebuild UI.

Watcher taskbar sudah berbasis registry notification. Watcher Settings
terpisah masih memindai proses setiap 1,5 detik; usulan di atas tidak
mengklaim seluruh aplikasi bebas polling dan tidak menambah polling baru.

## Cara membuktikan bahwa tetap ringan

Baseline terdahulu hanya mengukur halaman image DLL: 188 KiB resident di
Explorer, bukan total heap, stack, XAML dan resource compositor.
Snapshot tersimpan di `research/translucenttb-2026.2/memory-snapshot.json`.

Perbandingan yang diperlukan:

- Bandingkan renderer plain tint saat ini dengan cached Acrylic pada ukuran
  taskbar, nilai tint, tema, monitor dan aplikasi yang sama.
- Ukur Explorer private commit, private/total working set, serta penggunaan
  memori GPU dedicated/shared dan perubahan pada DWM. Memindahkan alokasi ke
  DWM/GPU tidak boleh dianggap menghilangkan biaya memori.
- Catat startup peak dan keadaan stabil setelah resource dipanaskan. Angka
  working set sesaat bisa turun karena trimming atau proses disuspend.
- Uji perubahan slider berulang dan rebuild taskbar. Penggunaan memori yang
  terus bertambah perlu ditelusuri; cache yang mencapai plateau bukan otomatis
  memory leak.
- Verifikasi visual dengan pola/aplikasi di belakang taskbar, aplikasi lain
  aktif, light/dark mode, Windows Transparency effects off, dan restore.
- Untuk perbandingan cold-start yang adil, gunakan sesi host bersih terpisah;
  DLL sebelumnya tidak dapat dianggap hilang hanya karena efek dimatikan.

Belum ada benchmark yang membenarkan janji angka total seperti 1 MB atau 5 MB.
Anggaran harus ditentukan terhadap baseline total yang diukur, bukan ukuran
file DLL atau image resident saja.

## Referensi

- [API Aero lama dan batas sejak Windows 8](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmenableblurbehindwindow)
- [DWM Desktop Acrylic](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwm_systembackdrop_type)
- [Windows.UI.Xaml.Media.AcrylicBrush](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.media.acrylicbrush)
- [Backdrop versus HostBackdrop](https://learn.microsoft.com/en-us/uwp/api/windows.ui.xaml.media.acrylicbackgroundsource)
- [TranslucentTB: alasan memilih Backdrop pada taskbar](https://github.com/TranslucentTB/TranslucentTB/blob/d4636e439865df0a1a1419db408e055740ce5c74/ExplorerTAP/taskbarappearanceservice.cpp)
- [Acrylic: adaptasi dan penggunaan GPU](https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic)
- [Inspeksi binary REA lokal](TRANSLUCENTTB-REA-INSPECTION.md)
