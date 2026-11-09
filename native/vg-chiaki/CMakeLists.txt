# Được thêm vào cây mã nguồn chiaki-ng khi build (xem .github/workflows/build-vg-chiaki.yml)
add_executable(vg-chiaki main.c)
target_link_libraries(vg-chiaki chiaki-lib)
