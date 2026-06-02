# MCUXpresso IDE 프로젝트 설정 가이드 (FRDM-RW612)

## 개요
이 프로젝트는 **NXP FRDM-RW612 보드**용 Zephyr RTOS 프로젝트입니다.
- **보드**: FRDM-RW612
- **MCU**: RW612 (Cortex-M33 @ 200MHz)
- **편집**: VS Code
- **빌드 및 디버깅**: MCUXpresso IDE

## 하드웨어 사양

### FRDM-RW612 Features
- **Core**: ARM Cortex-M33 @ 200MHz
- **Flash**: 64MB External (W25Q512NW via FlexSPI)
- **SRAM**: 1.25MB Internal
- **Wireless**:
  - Wi-Fi 6 (802.11ax)
  - Bluetooth 5.3 (BLE)
- **Debug**: Onboard MCU-Link (LinkServer)
- **USB**: USB 2.0 HS
- **Peripherals**: UART, I2C, SPI, PWM, ADC

## MCUXpresso IDE로 프로젝트 가져오기

### 1. 프로젝트 임포트
1. MCUXpresso IDE 실행
2. `File` → `Open Projects from File System...`
3. `Directory` 버튼 클릭하여 프로젝트 경로 선택: `c:\work\New_zCube\rw612_zephyr\frdm_rw612_blank`
4. `Finish` 클릭

### 2. 빌드 설정 확인
프로젝트가 두 개의 빌드 구성을 가지고 있습니다:
- **Debug**: 디버그 빌드 (debug 폴더에 출력)
- **Release**: 릴리스 빌드 (release 폴더에 출력)

빌드 구성 변경:
1. 프로젝트 우클릭 → `Build Configurations` → `Set Active`
2. `Debug` 또는 `Release` 선택

### 3. 프로젝트 빌드

#### 방법 1: MCUXpresso IDE에서 빌드
1. 프로젝트 선택
2. `Project` → `Build Project` (또는 Ctrl+B)
3. 빌드 스크립트(`build.bat`)가 자동으로 `west build` 명령을 실행합니다

#### 방법 2: 명령줄에서 빌드
```batch
# 프로젝트 디렉토리에서
build.bat build          # 빌드
build.bat clean          # 클린
build.bat configure      # CMake 설정만
build.bat flash          # 빌드 후 플래시
```

### 4. 디버깅

#### LinkServer 디버거 사용 (권장)
1. FRDM-RW612 보드를 PC에 USB로 연결 (MCU-Link 포트 사용)
2. `Run` → `Debug Configurations...`
3. `frdm_rw612_Debug_LinkServer` 선택
4. `Debug` 버튼 클릭

**디버거 설정:**
- **Probe**: MCU-Link (onboard)
- **Device**: RW612
- **Connection**: USB
- **Speed**: 8000 kHz
- **Interface**: SWD
- **ELF 파일**: `debug/zephyr/zephyr.elf`
#### J-Link 디버거 사용 (대체)
J-Link 프로브가 있### 6. 빌드 시스템gurations...`
3. `frdm_rw612_blank_Debug_JLink` 선택
4. `Debug` 버튼 클릭

**디버거 설정:**
- **Probe**: J-Link (external)
- **Device**: RW612
- **Speed**: 4000 kHz
- **Interface**: SWD

### 5. 시리얼 콘솔

보드와 통신하려면 시리얼 터미널을 설정하세요:

**설정:**
- **포트**: COM 포트 확인 (Device Manager에서)
- **Baudrate**: 115200
- **Data bits**: 8
- **Parity**: None
- **Stop bits**: 1
- **Flow control**: None

**터미널 프로그램:**
- PuTTY
- Tera Term
- MCUXpresso IDE 내장 터미널

**출력 예시:**
```
*** Booting Zephyr OS build v3.6.0 ***
APP: app_main_task started
APP: BLE stack ready
APP: advertising started
```

프로젝트는 다음과 같이 구성되어 있습니다:

```
프로젝트 루트
├── .project              # Eclipse/MCUXpresso 프로젝트 파일
├── .cproject             # C/C++ 프로젝트 설정
├── build.bat             # Windows 빌드 스크립트
├── Makefile              # Unix/Linux 빌드 스크립트
├── CMakeLists.txt        # CMake 설정
├── prj.conf              # Zephyr 프로젝트 설정
├── west.yml              # West manifest
├── debug/                # Debug 빌드 출력
│   └── zephyr/
│       └── zephyr.elf    # 디버그용 실행 파일
└── app/                  # 애플리케이션 소스 코드
```

## 주의사항

###FRDM-RW612 특정 설정

### 메모리 맵
```
Flash (External W25Q512NW):
  0x0800_0000 - 0x0803_FFFF  MCUBoot (256 KB)
  0x0804_0000 - 0x087F_FFFF  Slot 0 - Primary (8 MB)
  0x0880_0000 - 0x08FF_FFFF  Slot 1 - Secondary (8 MB)
  0x0900_0000 - 0x091F_FFFF  Storage (2 MB)

SRAM (Internal):
  0x2000_0000 - 0x2013_7FFF  SRAM (1248 KB)
```

### GPIO 핀 맵
- **LED**:
  - RED: PIO0_14
  - GREEN: PIO0_15
  - BLUE: PIO0_16
- **Button**:
  - SW2: PIO0_3 (active low)
  - SW3: PIO0_4 (active low)
- **UART (Console)**:
  - TX: PIO0_0 (FC0_TXD)
  - RX: PIO0_1 (FC0_RXD)
- **Debug (SWD)**:
  - SWDIO: PIO0_21
  - SWCLK: PIO0_20

### 보드 설정 파일
- `boards/frdm_rw612.conf` - Kconfig 설정
- `boards/frdm_rw612.overlay` - Device Tree 오버레이
- `boards/FRDM_RW612_INFO.md` - 상세 정보

##  Zephyr 환경 변수
빌드하기 전에 Zephyr 환경이 설정되어 있어야 합니다:
```batch
# Zephyr SDK 및 west가 설치되어 있어야 함
# ZEPHYR_BASE 환경 변수가 설정되어 있어야 함
```

### 경로 문제
- MCUXpresso IDE의 workspace는 이 프로젝트와 **다른** 위치에 설정하세요
- 프로젝트 파일은 `c:\work\New_zCube\rw612_zephyr\frdm_rw612_blank`에 있어야 합니다

### 빌드 오류 해결
1. **west 명령을 찾을 수 없음**:
   - Zephyr SDK가 제대로 설치되었는지 확인
   - west가 PATH에 있는지 확인
   
2. **ZEPHYR_BASE 오류**:
   - 환경 변수 `ZEPHYR_BASE`가 설정되어 있는지 확인
   - 예: `C:\zephyr-sdk\zephyr`

3. **CMake 설정 오류**:
   ```batch
   build.bat clean
   build.bat configure
   build.bat build
   ```

## 추가 구성

### 인클루드 경로
프로젝트의 인클루드 경로는 자동으로 설정됩니다:
- `debug/zephyr/include/generated`
- `app/`
- 기타 Zephyr 시스템 헤더

### 디버거 설정 변경
디버그 설정을 변경하려면:
1. `frdm_rw612_blank_Debug_JLink.launch` 파일 편집
2. 또는 MCUXpresso IDE의 Debug Configurations에서 수정

## 문제 해결

### 프로젝트가 표시되지 않음
- MCUXpresso IDE를 재시작하세요
- Project Explorer 새로고침 (F5)

### 빌드가 실행되지 않음
- Windows 명령 프롬프트에서 `build.bat build`를 직접 실행해보세요
- 오류 메시지를 확인하세요

### 디버거 연결 실패
- J-Link 드라이버가 설치되어 있는지 확인
- 보드가 제대로 연결되어 있는지 확인
- 다른 디버그 세션이 실행 중인지 확인

## 참고 자료
- Zephyr 문서: https://docs.zephyrproject.org/
- NXP RW612 문서: https://www.nxp.com/rw612
- MCUXpresso IDE 가이드: https://www.nxp.com/mcuxpresso
