# FRDM-RW612 zCube 개발 환경 / 빌드 가이드

## 1. 개발 환경 요약

| 항목 | 값 |
|------|-----|
| 보드 | `frdm_rw612` (NXP RW612, Cortex-M33) |
| 워크스페이스 루트 | `C:\work\New_zCube\rw612_zephyr\` (west 워크스페이스) |
| 앱 프로젝트 | `frdm_rw612_blank\` |
| Zephyr | `zephyr\` (west.yml revision: main, v4.4.0 계열) |
| Zephyr SDK | `C:\Users\User\zephyr-sdk-1.0.1` |
| 툴체인 | `ZEPHYR_TOOLCHAIN_VARIANT=zephyr` (arm-zephyr-eabi gcc 14.3.0) |
| **CMake** | **3.28.4 (필수)** — venv에 설치. 시스템 4.1.0은 genex 깨짐 |
| Python | `.venv` (cryptography, cmake 3.28.4) + 빌드용 store Python에 cryptography |
| 빌드 시스템 | `west build` (non-sysbuild) + `build.bat` 래퍼 |
| 부트로더 | `bootloader\mcuboot` (overwrite-only 모드) |
| 디버거 | J-Link / LinkServer (`.launch` 파일) |

### 모듈 (west.yml name-allowlist)
`cmsis`, `cmsis_6`, `hal_nxp`, `mbedtls`, `tf-psa-crypto`, `libsbc`, `mcuboot`

### Flash 파티션 (frdm_rw612_common.dtsi)
| 파티션 | 주소 | 크기 |
|--------|------|------|
| boot (mcuboot) | 0x000000 | 128 KB |
| slot0 / image-0 | 0x020000 | 3 MB |
| slot1 / image-1 | 0x320000 | 3 MB |
| storage (NVS) | 0x620000 | 나머지 |

---

## 2. 최초 1회 환경 셋업

```powershell
# (1) venv 활성화
.\.venv\Scripts\Activate.ps1          # 프롬프트에 (.venv) 표시

# (2) CMake 3.28.4 — 시스템 4.1.0 대신 사용 (Zephyr/mcuboot genex 호환)
pip install "cmake==3.28.4"
cmake --version                       # 3.28.4 / Source가 .venv\Scripts\cmake.exe 인지 확인

# (3) imgtool 서명 의존성 — '빌드가 쓰는' Python에 설치
#     (venv가 아니라 store Python을 쓰는 경우가 있어 양쪽 다 안전하게)
pip install -r bootloader\mcuboot\scripts\requirements.txt

# (4) 모듈 fetch (build.bat이 자동 실행하기도 함)
west update
```

> ⚠️ **CMake 주의:** clean 빌드 시 시스템 CMake 4.1.0이 잡히면 링크에서
> `cannot find :-Os>` 에러가 납니다. 반드시 `(.venv)` 활성 상태(=3.28.4)에서 빌드하세요.

---

## 3. 부트로더 빌드/플래시 (최초 1회 또는 부트로더 변경 시)

```powershell
# 워크스페이스 루트(rw612_zephyr\)에서, (.venv) 활성 상태
Remove-Item -Recurse -Force build_mcuboot -ErrorAction SilentlyContinue
west build -b frdm_rw612 -d build_mcuboot bootloader/mcuboot/boot/zephyr -- -DCONFIG_BOOT_UPGRADE_ONLY=y
west flash -d build_mcuboot
```
- `-DCONFIG_BOOT_UPGRADE_ONLY=y` = **overwrite-only** (slot1 offset 0 기대, slot1→slot0 복사).
  기본 swap-using-offset은 OTA(offset 0)와 안 맞아 "wrong upload address"로 거부됨.
- 서명키: 기본 테스트키 `bootloader/mcuboot/root-rsa-2048.pem` (앱과 동일해야 함).

---

## 4. 앱 빌드/플래시 (build.bat)

`build.bat [build|clean|configure|flash] [debug|release]` — 워크스페이스 루트로 자동 이동.
**반드시 `(.venv)` 활성 상태에서 실행** (CMake 3.28.4 사용).

```powershell
# 워크스페이스 루트에서
.\frdm_rw612_blank\build.bat build   release      # 빌드만
.\frdm_rw612_blank\build.bat flash   release      # 빌드 + J-Link 플래시
.\frdm_rw612_blank\build.bat clean   release      # 빌드 디렉터리 삭제
.\frdm_rw612_blank\build.bat configure release     # CMake configure만

# debug 빌드는 두번째 인자 생략 또는 debug
.\frdm_rw612_blank\build.bat flash                 # = debug
```

- 빌드 산출물:
  - `frdm_rw612_blank\release\zephyr\zephyr.signed.bin` ← **OTA 서버에 올릴 이미지**
  - `...\zephyr.signed.hex` ← 플래시용(서명본). `build.bat flash`가 자동으로 이걸 플래시.
- `CONFIG_BOOTLOADER_MCUBOOT=y` + `CONFIG_MCUBOOT_SIGNATURE_KEY_FILE`(prj.conf) 덕에 서명 자동.

### west 직접 호출 (build.bat 없이)
```powershell
west build -b frdm_rw612 -d frdm_rw612_blank\release frdm_rw612_blank -- -DCONFIG_SIZE_OPTIMIZATIONS=y
west flash -d frdm_rw612_blank\release
# 서명 hex를 명시하려면:
west flash -d frdm_rw612_blank\release --hex-file frdm_rw612_blank\release\zephyr\zephyr.signed.hex
```

---

## 5. OTA (HTTP pull) 사용

### 서버 준비 (PC)
```powershell
# zephyr.signed.bin 을 firmware/ 에 두고
python -m http.server 8080
```
> ⚠️ **반드시 서명본(`zephyr.signed.bin`)을 서빙.** unsigned `zephyr.bin`은
> MCUboot가 slot1 검증 실패로 거부함.

### 보드에서 OTA 실행 (shell)
```
ota start http://<PC_IP>:8080/firmware/zephyr.signed.bin --commit
ota status
ota stop
```
흐름: 다운로드(slot1) → commit(`boot_request_upgrade`) → reboot → MCUboot가 slot1→slot0 복사 → 새 이미지 부팅.

---

## 6. 디버깅 (J-Link)
- `frdm_rw612_blank_Debug_JLink.launch` / `..._LinkServer.launch` 사용 (MCUXpresso).
- 시스템 hang 추적: 재현 후 Halt → 콜스택/PC 확인.

---

## 7. 알려진 이슈 / 주의

1. **CMake 4.1.0 비호환** — genex `$<...:-Os>` 깨짐. → venv의 3.28.4 사용.
2. **OTA 중 시스템 hang** — OTA가 실행 중인 FlexSPI flash(slot1)에 기록 시 WiFi/peripheral과
   충돌해 전체 정지 가능. 회피책: **OTA 전에 radar/IR 등 동작 task 정지** (예정).
   로그 모드를 `CONFIG_LOG_MODE_IMMEDIATE=y`로 두면 타이밍이 바뀌어 우회되기도 함(현재 설정).
3. **서명키** — 현재 MCUboot 기본 테스트키. 운영용은 자체 키 생성 후 앱·부트로더 모두 적용.
4. **롤백 없음** — overwrite-only. 필요 시 부트로더를 `-DCONFIG_BOOT_SWAP_USING_MOVE=y`로 재빌드.

---

## 8. 파티션 변경/확장 방법

현재 파티션은 **보드 dtsi**(`zephyr/boards/nxp/frdm_rw612/frdm_rw612_common.dtsi`)에 정의됨.
플래시 전체는 약 **64 MB** (boot 128K + slot0 3M + slot1 3M + storage ~57.9M).
→ slot을 키울 여유는 충분.

### ⚠️ 반드시 지킬 규칙
1. **slot0 크기 == slot1 크기** (MCUboot가 동일 크기 요구).
2. 주소·크기 모두 **4 KB(0x1000) 정렬**.
3. 파티션은 **연속·비중첩**, 합계가 플래시 크기(64 MB) 이내.
4. boot_partition은 부트로더가 들어갈 크기 (현재 59 KB 사용 / 128 KB).
5. **앱 빌드와 부트로더 빌드가 같은 파티션 정의를 봐야 함** ← 가장 중요한 함정.
   앱의 `boards/frdm_rw612.overlay`는 **앱 빌드에만** 적용되고, 부트로더 빌드
   (`bootloader/mcuboot/boot/zephyr`)는 그걸 안 봄. 둘을 따로 맞춰야 함.

### 방법: 공용 파티션 오버레이를 양쪽 빌드에 전달 (권장)

**(1)** `frdm_rw612_blank/partitions.overlay` 새로 만들기 (label로 reg만 덮어씀):
```dts
/* slot0 == slot1, 4KB 정렬. 예: 3M -> 4M로 확장 */
&boot_partition    { reg = <0x00000000 DT_SIZE_K(128)>; };
&slot0_partition   { reg = <0x00020000 DT_SIZE_M(4)>; };       /* 0x020000 ~ */
&slot1_partition   { reg = <0x00420000 DT_SIZE_M(4)>; };       /* 0x020000+4M */
&storage_partition { reg = <0x00820000 (DT_SIZE_M(56) - DT_SIZE_K(128))>; };
```

**(2)** 부트로더 빌드에 전달:
```powershell
Remove-Item -Recurse -Force build_mcuboot
west build -b frdm_rw612 -d build_mcuboot bootloader/mcuboot/boot/zephyr -- `
    -DCONFIG_BOOT_UPGRADE_ONLY=y `
    -DEXTRA_DTC_OVERLAY_FILE="C:/work/New_zCube/rw612_zephyr/frdm_rw612_blank/partitions.overlay"
west flash -d build_mcuboot
```

**(3)** 앱 빌드에 전달 — `build.bat`의 west 빌드 라인에 같은 `-DEXTRA_DTC_OVERLAY_FILE=...`
추가하거나, 직접:
```powershell
west build -b frdm_rw612 -d frdm_rw612_blank\release frdm_rw612_blank -- `
    -DCONFIG_SIZE_OPTIMIZATIONS=y `
    -DEXTRA_DTC_OVERLAY_FILE="C:/work/New_zCube/rw612_zephyr/frdm_rw612_blank/partitions.overlay"
```
> (대안: 보드 dtsi를 직접 수정하면 양쪽이 자동으로 같이 보지만, Zephyr 트리를
>  건드려 west update/재클론 시 사라지므로 비권장. 오버레이가 프로젝트에 남아 안전.)

### 변경 후 필수 작업
1. **부트로더 + 앱 모두 재빌드** (주소/슬롯크기 변경 반영).
   - 앱의 slot0 오프셋·서명 `--slot-size`는 DT에서 자동 반영됨.
2. **플래시 전체 erase 후 재플래시** (옛 주소에 남은 데이터 제거):
   ```powershell
   west flash -d build_mcuboot          # 부트로더
   .\frdm_rw612_blank\build.bat flash release   # 앱(서명본)
   ```
3. **storage(NVS) 주소가 바뀌면 기존 설정 데이터 무효화** →
   `CONFIG_NVS_INIT_BAD_MEMORY_REGION=y`로 자동 재초기화(=기존 NVS 값 소실).
4. OTA 이미지가 커지면 slot 크기 안에 들어가는지 확인 (`app_ota_begin`이 초과 시 거부).
