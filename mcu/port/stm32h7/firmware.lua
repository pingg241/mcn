-- STM32H743 固件（由 mcu/xmake.lua 在 -p cross 时引入）
local root = os.projectdir()
local tp  = path.join(root, "third_party")
local hal = path.join(tp, "stm32h7xx_hal_driver")
local cpu = {"-mcpu=cortex-m7", "-mfpu=fpv5-d16", "-mfloat-abi=hard", "-mthumb"}

target("firmware")
    set_kind("binary")
    set_extension(".elf")
    set_optimize("faster")
    set_symbols("debug")                   -- ELF 保留调试信息（烧录用的 .bin/.hex 不受影响）
    set_strip("none")
    add_defines("STM32H743xx", "USE_HAL_DRIVER", "USE_PWR_LDO_SUPPLY", "HSE_VALUE=25000000",
                "GFX_BYTESWAP=1")          -- 帧缓冲按 SPI 发送顺序存放
    add_includedirs(path.join(root, "app"), path.join(root, "port/stm32h7"), path.join(root, "port/stm32h7/lwip"),
                    path.join(hal, "Inc"), path.join(tp, "cmsis_device_h7/Include"),
                    path.join(tp, "cmsis_core/Include"), path.join(tp, "lwip/src/include"))

    add_files(path.join(root, "app/*.c"), path.join(root, "port/stm32h7/*.c"))

    -- 第三方代码不看警告
    local quiet = {cflags = "-w"}
    add_files(path.join(tp, "cmsis_device_h7/Source/Templates/gcc/startup_stm32h743xx.s"))
    add_files(path.join(tp, "cmsis_device_h7/Source/Templates/system_stm32h7xx.c"), quiet)
    for _, m in ipairs({"", "_cortex", "_rcc", "_rcc_ex", "_pwr", "_pwr_ex", "_gpio", "_dma", "_dma_ex", "_mdma",
                        "_exti", "_spi", "_spi_ex", "_eth", "_eth_ex", "_flash", "_flash_ex", "_uart", "_uart_ex"}) do
        add_files(path.join(hal, "Src/stm32h7xx_hal" .. m .. ".c"), quiet)
    end
    add_files(path.join(tp, "lwip/src/core/*.c"), path.join(tp, "lwip/src/core/ipv4/*.c"),
              path.join(tp, "lwip/src/netif/ethernet.c"), quiet)

    add_cflags(cpu, "-ffunction-sections", "-fdata-sections", {force = true})
    add_asflags(cpu, {force = true})
    add_ldflags(cpu, "-T" .. path.join(root, "port/stm32h7/STM32H743VI_FLASH.ld"),
                "-Wl,--gc-sections", "-Wl,-Map=" .. path.join(root, "build/firmware.map"), "-Wl,--no-warn-rwx-segments", {force = true})
    add_syslinks("m")

    after_build(function (target)
        local elf = target:targetfile()
        local base = path.join(path.directory(elf), path.basename(elf))
        -- 与编译器同目录同前缀（find_tool 会找到主机的 objcopy）
        local cc = target:tool("cc")
        local objcopy = cc:gsub("gcc(%.?%w*)$", "objcopy%1")
        local size = cc:gsub("gcc(%.?%w*)$", "size%1")
        os.vrunv(objcopy, {"-O", "ihex", elf, base .. ".hex"})
        os.vrunv(objcopy, {"-O", "binary", elf, base .. ".bin"})
        os.execv(size, {elf})
    end)
