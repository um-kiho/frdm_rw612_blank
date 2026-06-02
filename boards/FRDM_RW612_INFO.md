# FRDM-RW612 Board Configuration for MCUXpresso IDE

## Board Information
- **Board**: FRDM-RW612
- **MCU**: RW612 (NXP)
- **Core**: ARM Cortex-M33 @ 200MHz
- **Flash**: External (W25Q512NW - 64MB)
- **SRAM**: 1.25MB (Internal)
- **PSRAM**: 16MB (External, optional)

## Features
- Bluetooth 5.3 (BLE)
- IEEE 802.11a/b/g/n/ax Wi-Fi 6
- Ethernet (optional)
- USB 2.0 HS
- Flexcomm (UART, SPI, I2C, I2S)
- CTIMER, FlexPWM
- ADC, DAC

## Memory Map

### Internal SRAM (1.25MB)
```
0x2000_0000 - 0x2013_7FFF  SRAM (1248 KB)
```

### Flash (External W25Q512NW via FlexSPI)
```
0x0800_0000 - 0x0BFF_FFFF  FlexSPI Flash (64 MB)
```

### Peripheral Memory
```
0x4000_0000 - 0x4FFF_FFFF  Peripherals
```

## Debug Probes

### 1. LinkServer (Recommended)
- **Probe**: MCU-Link (onboard)
- **Connection**: USB
- **Speed**: 8 MHz (default)
- **Launch Config**: `frdm_rw612_Debug_LinkServer.launch`

### 2. J-Link (Alternative)
- **Probe**: J-Link (external)
- **Connection**: 10-pin SWD
- **Speed**: 4 MHz
- **Launch Config**: `frdm_rw612_Debug_JLink.launch`

## Pin Configuration

### Debug Interface (SWD)
- SWDIO: PIO0_21
- SWCLK: PIO0_20
- SWO: Not available
- RESET: PIO0_22

### UART (Console)
- TX: FC0_TXD (PIO0_0)
- RX: FC0_RXD (PIO0_1)
- Baudrate: 115200

### LED
- LED_RED: PIO0_14
- LED_GREEN: PIO0_15
- LED_BLUE: PIO0_16

### User Button
- SW2: PIO0_3 (active low)
- SW3: PIO0_4 (active low)

## Flash Programming

### Boot Sequence
1. ROM Bootloader (internal)
2. Primary Image (Slot 0) @ 0x0800_0000
3. Secondary Image (Slot 1) @ 0x0880_0000 (for OTA)

### Flash Layout (Zephyr)
```
0x0800_0000 - 0x0803_FFFF  MCUBoot (256 KB)
0x0804_0000 - 0x087F_FFFF  Slot 0 - Primary Image (8 MB)
0x0880_0000 - 0x08FF_FFFF  Slot 1 - Secondary Image (8 MB)
0x0900_0000 - 0x091F_FFFF  Storage (2 MB)
```

## Build Configuration

### Environment Variables
```
ZEPHYR_BASE=<path_to_zephyr>
GNUARMEMB_TOOLCHAIN_PATH=<path_to_arm_gcc>
```

### Build Commands
```bash
# Configure
west build -b frdm_rw612 -d debug

# Build
west build -d debug

# Flash
west flash -d debug

# Clean
west build -d debug -t clean
```

## Troubleshooting

### Debug Connection Failed
1. Check USB cable connection
2. Verify MCU-Link firmware is up-to-date
3. Try lower debug speed (4 MHz → 2 MHz)
4. Press RESET button on board

### Flash Programming Failed
1. Erase flash: LinkServer → "Erase Flash"
2. Check power supply (USB should be 5V)
3. Verify external flash is populated (W25Q512NW)

### UART Not Working
1. Check baud rate (115200)
2. Verify USB-to-UART driver installed
3. Check COM port number in Device Manager

## References
- [FRDM-RW612 User Guide](https://www.nxp.com/design/development-boards/freedom-development-boards/wireless-connectivity/frdm-rw612-evaluation-board:FRDM-RW612)
- [RW612 Datasheet](https://www.nxp.com/products/wireless-connectivity/wi-fi-plus-bluetooth-plus-802-15-4/wireless-mcu-with-integrated-tri-radiobr1x1-wi-fi-6-plus-bluetooth-low-energy-5-3-802-15-4:RW612)
- [MCUXpresso IDE User Guide](https://www.nxp.com/design/software/development-software/mcuxpresso-software-and-tools-/mcuxpresso-integrated-development-environment-ide:MCUXpresso-IDE)
