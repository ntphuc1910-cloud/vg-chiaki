# vg-chiaki

Chương trình dòng lệnh nhỏ giúp ứng dụng **VietGame** kết nối Remote Play với PS5: ghép đôi bằng PIN, đánh thức máy,
và đẩy luồng video H.264/H.265 ra stdout. Dựa trên **libchiaki** của [chiaki-ng](https://github.com/streetpea/chiaki-ng).

A small command-line helper used by the VietGame app for PS5 Remote Play (registration, wake-up, video stream to stdout),
built on libchiaki from chiaki-ng.

## Giấy phép / License

**GNU Affero General Public License v3.0** — xem [LICENSE](LICENSE). Kế thừa ngoại lệ OpenSSL (AGPL §7) của chiaki-ng.
Copyright (C) 2026 Phuc Nguyen; libchiaki copyright (C) các tác giả chiaki / chiaki-ng.

Đây là "mã nguồn tương ứng" (Corresponding Source) của `vg-chiaki.exe` phát hành kèm bộ cài VietGame.
Mỗi bản build ghi rõ commit trong tệp `NGUON.txt`.

## Build

Toàn bộ quy trình nằm ở [.github/workflows/build-vg-chiaki.yml](.github/workflows/build-vg-chiaki.yml)
(Windows, MSYS2 MinGW-w64):

1. Lấy chiaki-ng tại commit cố định `43728af2e639fecb8a7af1cd68c29820a40de973`.
2. Chép `native/vg-chiaki/` vào cây mã nguồn, thêm `add_subdirectory(vg-chiaki)`.
3. `cmake -G Ninja` (tắt GUI/CLI/FFmpeg…), `cmake --build build --target vg-chiaki`.
4. Gom exe + DLL MSYS2 + giấy phép các thư viện (`licenses/`, `PACKAGES.txt`) vào `vg-chiaki-win64.zip`.

Chạy: tab **Actions → Build vg-chiaki (Windows x64) → Run workflow**, tải `vg-chiaki-win64.zip` ở mục Artifacts
hoặc nhánh `build-output`.

## Thư viện bên thứ ba trong bản build

OpenSSL (Apache-2.0), libevent, miniupnpc, libssh2 (BSD-3-Clause), json-c, nghttp2, libpsl (MIT), zlib (zlib),
winpthreads (MIT/BSD), libiconv, libintl (LGPL-2.1+), libidn2, libunistring (LGPL-3.0+/GPL-2.0+),
nanopb (zlib), Jerasure, GF-Complete (BSD-3-Clause). Các DLL LGPL được liên kết động.
Mã nguồn các gói MSYS2: https://github.com/msys2/MINGW-packages

PlayStation và PS5 là nhãn hiệu của Sony Interactive Entertainment. Dự án này không liên kết với Sony.
