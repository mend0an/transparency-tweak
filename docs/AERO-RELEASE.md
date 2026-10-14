# Transparency Tweak Aero 0.1.0-preview.1

Paket preview terpisah untuk taskbar kaca. Versi ini memakai renderer Windows
Composition yang sudah diuji pada Windows 11 x64 build 26300.9457. Ia tidak
memakai DLL Aero asli Windows 7. Pengaturan tambahan: Aero Glass, Blur strength,
dan Reflection strength. Pada transparansi 100%, blur dan pantulan tetap aktif.

## Paket

- [Installer Aero](../dist/TransparencyTweak-Aero-0.1.0-preview.1/Install%20Transparency%20Tweak%20Aero%200.1.0-preview.1.exe)
- [ZIP lengkap](../dist/TransparencyTweak-Aero-0.1.0-preview.1.zip)
- [Restore Previous Version](../dist/TransparencyTweak-Aero-0.1.0-preview.1/Restore%20Previous%20Version.exe)
- [Petunjuk paket](../dist/TransparencyTweak-Aero-0.1.0-preview.1/README.md)

Tidak perlu uninstall versi sebelumnya. Installer Aero menyimpan binary di
`%LOCALAPPDATA%\TransparencyTweak\versions\aero-0.1.0-preview.1`, tanpa mengganti
binary dalam folder `app` lama. Hanya satu versi aktif: scheduled task yang
sudah ada dialihkan ke versi Aero. Folder rilis `dist` lama tetap tersedia.

Installer menyimpan XML scheduled task dan nilai registry sebelum instalasi
di `previous-version.json` dalam folder versi tersebut. Restore Previous Version
mengembalikan konfigurasi dan versi yang disimpan, serta mempertahankan binary
Aero. Pada instalasi baru tanpa versi sebelumnya, restore kembali ke default
Windows. Jika versi yang aktif sebelumnya adalah prototipe Aero, restore juga
kembali ke prototipe itu; untuk kembali ke plain tint, jalankan installer stabil.

Instalasi/pergantian versi memerlukan restart Explorer dan dapat menutup
jendela File Explorer serta Settings yang memakai tweak. Jangan memakai
uninstaller lama hanya untuk berganti versi: ia menghapus seluruh direktori
TransparencyTweak, termasuk subfolder versi. Gunakan uninstaller lama hanya
jika ingin menghapus seluruh aplikasi.

## Build dan verifikasi

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\build-aero-package.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\experimental\Test-AeroPackage.ps1
```

Untuk rilis berikutnya, berikan `-Version 0.1.0-preview.2`. Builder menolak
folder versi yang sudah ada. Proses build tidak mengaktifkan atau menginstal
paket ke Windows. Hash binary dan installer stabil diperiksa tetap sama.

Pemeriksaan paket memverifikasi resource yang tertanam di EXE dan semua
checksum. Uji simulasi Windows API memeriksa pergantian versi, rollback saat
injector gagal, pemulihan task/preferensi, penjagaan binary lama, dan penolakan
jika versi aktif lain atau binary sebelumnya berubah. Probe graph Composition
lolos 20.000 perubahan parameter tanpa membangun ulang factory/brush.

Installer pergantian versi belum dijalankan pada sesi Windows nyata; pengujian
installer menggunakan simulasi task, registry, proses dan injector. Kontrol
Colors baru juga belum diverifikasi melalui UI. Pengujian rendering dan RAM
versi sebelumnya tercatat di [catatan prototipe](AERO-TASKBAR-PROTOTYPE.md).
