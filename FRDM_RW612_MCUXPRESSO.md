# FRDM-RW612 MCUXpresso 프로젝트 설정 완료

## ✅ 설정 완료 항목

### 1. 프로젝트 파일
- ✅ [.project](.project) - Eclipse/MCUXpresso 프로젝트 정의
- ✅ [.cproject](.cproject) - FRDM-RW612용 C/C++ 빌드 설정
- ✅ [.settings/](.settings/) - 프로젝트 설정 디렉토리

### 2. 빌드 구성
- ✅ **Debug**: FRDM-RW612 Debug (com.nxp.mcuxpresso.frdmrw612.zephyr.debug)
- ✅ **Release**: FRDM-RW612 Release (com.nxp.mcuxpresso.frdmrw612.zephyr.release)

### 3. 디버거 설정
- ✅ [frdm_rw612_Debug_LinkServer.launch](frdm_rw612_Debug_LinkServer.launch) - LinkServer (MCU-Link, 권장)
- ✅ [frdm_rw612_blank_Debug_JLink.launch](frdm_rw612_blank_Debug_JLink.launch) - J-Link (대체)

### 4. 보드 설정
- ✅ [boards/frdm_rw612.conf](boards/frdm_rw612.conf) - Kconfig 설정
- ✅ [boards/frdm_rw612.overlay](boards/frdm_rw612.overlay) - Device Tree
- ✅ [boards/FRDM_RW612_INFO.md](boards/FRDM_RW612_INFO.md) - 보드 정보

### 5. 빌드 스크립트
- ✅ [build.bat](build.bat) - Windows 빌드 스크립트
- ✅ [Makefile](Makefile) - Unix/Linux 빌드 스크립트

### 6. 문서
- ✅ [MCUXPRESSO_SETUP.md](MCUXPRESSO_SETUP.md) - MCUXpresso IDE 설정 가이드 (업데이트됨)

---

## 🎯 FRDM-RW612 보드 사양

| 항목 | 사양 |
|------|------|
| **MCU** | RW612 (ARM Cortex-M33) |
| **Clock** | 200 MHz |
| **Flash** | 64 MB External (W25Q512NW) |
| **SRAM** | 1.25 MB Internal |
| **Wireless** | Wi-Fi 6 (802.11ax) + BLE 5.3 |
| **Debug** | MCU-Link (LinkServer) onboard |
| **USB** | USB 2.0 High Speed |

---

## 🔧 MCUXpresso IDE 설정

### 컴파일러 설정
- **Toolchain**: MCU ARM GCC (arm-none-eabi-gcc)
- **Target**: Cortex-M33
- **FPU**: Enabled (hardware floating point)
- **Optimization**: -O0 (Debug), -O2 (Release)

### Include Paths
자동으로 설정된 인클루드 경로:
```
${workspace_loc:/${ProjName}/debug/zephyr/include/generated}
${workspace_loc:/${ProjName}/app}
${workspace_loc:/${ProjName}/app/ble}
${workspace_loc:/${ProjName}/app/wifi}
${workspace_loc:/${ProjName}/app/tcp}
${workspace_loc:/${ProjName}/app/ota}
${workspace_loc:/${ProjName}/app/ir}
${workspace_loc:/${ProjName}/app/leds}
${workspace_loc:/${ProjName}/app/sensors}
${workspace_loc:/${ProjName}/app/radar}
${workspace_loc:/${ProjName}/app/nvm}
```

### Preprocessor Defines
```c
BOARD_FRDM_RW612
CPU_RW612ETA2I
__ZEPHYR__=1
```

---

## 🚀 빠른 시작

### 1. MCUXpresso IDE에서 열기
```
File → Open Projects from File System...
→ 이 폴더 선택
→ Finish
```

### 2. 빌드
```
Project → Build Project (Ctrl+B)
```

### 3. 디버그
```
Run → Debug Configurations...
→ frdm_rw612_Debug_LinkServer 선택
→ Debug
```

### 4. 시리얼 콘솔 확인
```
포트: COM? (Device Manager에서 확인)
Baudrate: 115200
```

---

## 🔌 하드웨어 연결

### USB 연결
FRDM-RW612 보드에는 2개의 USB 포트가 있습니다:
1. **MCU-Link (Debug)** - J27 커넥터
   - 디버깅 및 플래싱
   - 가상 시리얼 포트 제공
2. **USB Device** - J28 커넥터
   - USB 기능 (선택 사항)

### 전원
- USB (MCU-Link 포트)로 전원 공급 가능
- 외부 5V 전원 (J29 핀헤더, 선택 사항)

---

## 📝 디버거 비교

| 항목 | LinkServer (권장) | J-Link |
|------|------------------|--------|
| **프로브** | MCU-Link (onboard) | J-Link (external) |
| **연결** | USB (보드 내장) | 10-pin SWD header |
| **속도** | 8 MHz | 4 MHz |
| **설정** | 추가 하드웨어 불필요 | 외부 프로브 필요 |
| **가격** | 무료 (보드 포함) | 별도 구매 필요 |
| **Launch** | frdm_rw612_Debug_LinkServer.launch | frdm_rw612_blank_Debug_JLink.launch |

---

## 🐛 문제 해결

### "프로젝트가 표시되지 않음"
```
1. MCUXpresso IDE 재시작
2. Project Explorer 새로고침 (F5)
3. Workspace 확인 (다른 위치에 설정)
```

### "빌드 실패"
```
1. Zephyr 환경 변수 확인 (ZEPHYR_BASE)
2. west 명령이 PATH에 있는지 확인
3. 터미널에서 직접 빌드 시도:
   build.bat build
```

### "디버거 연결 실패"
```
1. USB 케이블 확인 (MCU-Link 포트에 연결)
2. Device Manager에서 MCU-Link 인식 확인
3. 보드 리셋 버튼 누르기
4. LinkServer 펌웨어 업데이트 (Help → Update MCU-Link Firmware)
```

### "플래시 프로그래밍 실패"
```
1. External Flash 칩 확인 (W25Q512NW 장착됨?)
2. Flash 지우기: Target → Erase Flash
3. 전원 재연결
```

---

## 📚 추가 리소스

### 문서
- [MCUXPRESSO_SETUP.md](MCUXPRESSO_SETUP.md) - 상세 설정 가이드
- [MODULE_ARCHITECTURE.md](MODULE_ARCHITECTURE.md) - 모듈 구조
- [MULTITASKING_GUIDE.md](MULTITASKING_GUIDE.md) - Zephyr 멀티태스킹
- [boards/FRDM_RW612_INFO.md](boards/FRDM_RW612_INFO.md) - 보드 정보

### NXP 리소스
- [FRDM-RW612 페이지](https://www.nxp.com/frdm-rw612)
- [RW612 데이터시트](https://www.nxp.com/rw612)
- [MCUXpresso IDE 다운로드](https://www.nxp.com/mcuxpresso-ide)

### Zephyr 리소스
- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [Zephyr Getting Started](https://docs.zephyrproject.org/latest/getting_started/)

---

**설정 완료일**: 2026-05-24  
**프로젝트**: zCube FRDM-RW612 Zephyr  
**상태**: ✅ Ready for Development
