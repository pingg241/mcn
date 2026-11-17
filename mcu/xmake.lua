-- MCA MCU 终端
--
-- PC 宿主版（单元测试 + 跑同一套 app/ 代码的终端，笔记本代替 STM32 时用它，带窗口）：
--   Windows：xmake f -p mingw && xmake && xmake run tests
--   Linux / WSL：xmake f -p linux && xmake && xmake run tests（窗口要 libx11-dev，找不到就无窗口）
--   xmake run mca_host --fpga 192.168.1.10
-- STM32H743 固件（arm-none-eabi-gcc）：
--   xmake f -p cross -a cortex-m7 --toolchain=gnu-rm && xmake
--   产物 build/cross/cortex-m7/release/firmware.{elf,hex,bin}

set_project("mca_mcu")
set_version("1.0.0")
set_xmakever("2.8.0")
add_rules("mode.release", "mode.debug")
set_defaultmode("release")
set_languages("c11")
set_warnings("all", "extra")

if not is_plat("cross") then

option("x11")
    set_description("Linux 宿主版用 X11 开窗口（自动检测 libX11）")
    add_cincludes("X11/Xlib.h", "X11/XKBlib.h")
    add_links("X11")
    add_defines("MCA_HAVE_X11")
option_end()

target("mca_app")
    set_kind("static")
    add_files("app/*.c")
    add_includedirs("app", {public = true})
    if not is_plat("windows") then
        add_syslinks("m", {public = true})
    end

target("tests")
    set_kind("binary")
    add_deps("mca_app")
    add_files("tests/test_main.c")

target("mca_host")
    set_kind("binary")
    add_deps("mca_app")
    add_files("port/host/main_host.c")
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "gdi32", "user32")
    elseif is_plat("linux") then
        add_options("x11")
    end

else

includes("port/stm32h7/firmware.lua")

end
