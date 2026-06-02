# zCube 프로젝트 모듈 구성 상세 분석

## 📋 개요

zCube는 **NXP RW612 기반 IoT 디바이스**로, 다음 기능을 제공합니다:
- **BLE**: 블루투스 저에너지 통신 (GATT 서버)
- **WiFi**: 무선 네트워크 연결 (STA 모드)
- **TCP/IP**: TCP 서버/클라이언트
- **OTA**: Over-The-Air 펌웨어 업데이트
- **IR**: 적외선 에어컨 제어 (삼성 등)
- **Sensors**: 조도/온도 센서
- **Radar**: 레이더 센서 통합
- **LED**: RGB LED 제어 (SK6812)
- **Matter**: Matter 스마트홈 통합

---

## 🏗️ 아키텍처 구조

### 현재 상태: FreeRTOS → Zephyr 전환 중

```
┌─────────────────────────────────────────────────────────┐
│                  app_main_task (메인)                    │
│            [src/main.c - K_THREAD_DEFINE]               │
└───────────────────────┬─────────────────────────────────┘
                        │
        ┌───────────────┼───────────────┐
        │               │               │
        ▼               ▼               ▼
    ┌───────┐      ┌───────┐      ┌───────┐
    │  BLE  │      │ WiFi  │      │  TCP  │
    └───────┘      └───────┘      └───────┘
        │               │               │
        ▼               ▼               ▼
    GATT Svcs    Network Stack    lwIP Sockets
```

---

## 📡 1. BLE (Bluetooth Low Energy)

### 위치
- `app/ble/gatt_servers/` - GATT 서비스들
- `app/ble/gatt_client/` - GATT 클라이언트
- `app/ble/zephyr_bt_rand_shim.c` - Zephyr BLE 난수 shim

### GATT 서비스 구성

프로젝트는 **8개의 GATT 서비스**를 제공합니다:

#### 1) `svc_status` - 상태 보고 서비스
- **UUID Base**: `5a637562-e100-4000-8000-000000000001`
- **기능**: WiFi, TCP, 센서 상태 및 시스템 uptime 브로드캐스트
- **특성**:
  - e100: service
  - e101: status (read + notify)

#### 2) `svc_ctrl` - 제어 서비스
- **UUID Base**: `5a637562-e200-4000-8000-000000000001`
- **기능**: 시스템 제어 명령 (재부팅, 초기화, WiFi/TCP 재연결)
- **특성**:
  - e200: service
  - e201: cmd (write) - 명령 전송
  - e202: resp (read + notify) - 응답 수신
- **지원 명령**:
  ```c
  APP_CTRL_OP_REBOOT             0x01
  APP_CTRL_OP_FACTORY_RESET      0x02
  APP_CTRL_OP_WIFI_DISCONNECT    0x03
  APP_CTRL_OP_WIFI_RECONNECT     0x04
  APP_CTRL_OP_TCP_DISCONNECT     0x05
  APP_CTRL_OP_TCP_RECONNECT      0x06
  ```

#### 3) `svc_prov` - 프로비저닝 서비스
- **UUID Base**: `5a637562-e300-4000-8000-000000000001`
- **기능**: WiFi/TCP 설정 저장 (SSID, 비밀번호, IP, 포트 등)

#### 4) `svc_ir` - 적외선 제어 서비스
- **UUID Base**: `5a637562-e400-4000-8000-000000000001`
- **기능**: 에어컨 IR 명령 전송
- **지원 브랜드**: 삼성, LG, 캐리어, 위니아, 파세코, 하이센스

#### 5) `svc_radar` - 레이더 서비스
- **UUID Base**: `5a637562-e500-4000-8000-000000000001`
- **기능**: 레이더 센서 데이터 스트리밍

#### 6) `svc_ota` - OTA 업데이트 서비스
- **UUID Base**: `5a637562-e600-4000-8000-000000000001`
- **기능**: BLE를 통한 펌웨어 업데이트
- **프로토콜**:
  ```
  begin(size) → chunk(data) → chunk(data) → commit() → reboot
  ```

#### 7) `svc_led` - LED 제어 서비스
- **UUID Base**: `5a637562-e700-4000-8000-000000000001`
- **기능**: RGB LED 패턴/색상/밝기 제어

#### 8) `svc_matter` - Matter 통합 서비스
- **UUID Base**: `5a637562-e800-4000-8000-000000000001`
- **기능**: Matter 스마트홈 프로토콜 브릿지

### BLE 디바이스 이름
```c
CONFIG_BT_DEVICE_NAME="zCube"
```

### 멀티태스킹
- **메인 스레드**: `app_main_task` (우선순위 5)
- **Zephyr BT 스택**: 내장 스레드들 (자동 관리)
- **이벤트 기반**: 콜백으로 다른 모듈과 통신

---

## 📶 2. WiFi 모듈

### 파일
- `app/wifi/app_wifi.c/h`

### 기능
- **모드**: STA (Station) - AP에 연결
- **드라이버**: NXP wlan SDK 래퍼
- **DHCP**: 자동 IP 할당

### API
```c
// 초기화 및 이벤트 콜백 등록
int app_wifi_init(app_wifi_evt_cb_t cb, void *user_arg);

// NVRAM 설정으로 AP 연결
int app_wifi_connect(const app_nvram_data_t *cfg);

// 연결 해제
int app_wifi_disconnect(void);

// 연결 상태 확인
bool app_wifi_is_connected(void);
```

### 이벤트
```c
typedef enum app_wifi_evt {
    APP_WIFI_EVT_READY = 0,        // wlan_start 완료
    APP_WIFI_EVT_CONNECTED,        // AP 연결 + DHCP 완료
    APP_WIFI_EVT_DISCONNECTED,     // 연결 끊김
    APP_WIFI_EVT_AUTH_FAILED,      // 인증 실패
    APP_WIFI_EVT_INIT_FAILED,      // 초기화 실패
} app_wifi_evt_t;
```

### 지원 보안
- OPEN (비밀번호 없음)
- WPA2 Personal
- WPA3 (SDK 지원 시)

### 멀티태스킹
- **스레드**: NXP wlan SDK 내부 스레드 사용
- **이벤트**: `wlan_event_cb` 콜백으로 비동기 처리
- **메인과 통신**: 이벤트 콜백 → `app_main_task`

---

## 🌐 3. TCP/IP 모듈

### 구성
1. **TCP Server** - `app/tcp/app_tcp_server.c/h`
2. **TCP Client** - `app/tcp/app_tcp_client.c/h`

### 3.1 TCP Server

#### 특징
- **단일 클라이언트**: 동시에 1개 클라이언트만 연결
- **전용 태스크**: FreeRTOS 태스크 사용
- **블로킹 I/O**: `accept()` → `recv()` 루프
- **포트**: 기본 5000 (설정 가능)

#### API
```c
// 서버 시작 (바인드 + listen)
int app_tcp_server_start(uint16_t port,
                        app_tcp_srv_rx_cb_t rx_cb, 
                        void *user_arg);

// 클라이언트에게 전송 (thread-safe)
int app_tcp_server_send(const void *data, size_t len);

// 서버 중지
void app_tcp_server_stop(void);

// 클라이언트 연결 상태
bool app_tcp_server_is_client_connected(void);
```

#### 태스크 설정
```c
#define APP_TCP_SRV_TASK_STACK    2048    // 스택 크기
#define APP_TCP_SRV_TASK_PRIO     3       // 우선순위
#define APP_TCP_SRV_RX_BUF_SIZE   512     // 수신 버퍼
```

#### 멀티태스킹
```c
xTaskCreate(srv_task,              // 태스크 함수
           "zcube_tcps",          // 이름
           APP_TCP_SRV_TASK_STACK,// 스택
           NULL,                  // 파라미터
           APP_TCP_SRV_TASK_PRIO, // 우선순위
           NULL);                 // 핸들
```

### 3.2 TCP Client

#### 특징
- **자동 재연결**: 연결 실패 시 재시도 (선택적)
- **전용 태스크**: FreeRTOS 태스크
- **NVRAM 설정**: 서버 IP/포트 저장

#### API
```c
// 클라이언트 시작 (NVRAM 설정 사용)
int app_tcp_client_start(const app_nvram_data_t *cfg,
                        app_tcp_rx_cb_t rx_cb,
                        void *user_arg);

// 서버에 전송 (thread-safe)
int app_tcp_client_send(const void *data, size_t len);

// 클라이언트 중지
void app_tcp_client_stop(void);

// 연결 상태
bool app_tcp_client_is_connected(void);
```

#### 멀티태스킹
```c
xTaskCreate(tcp_task, "zcube_tcp", ...);
```

### lwIP 통합
- **스택**: lwIP (경량 TCP/IP 스택)
- **소켓 API**: BSD 소켓 호환 (`lwip_socket`, `lwip_send` 등)
- **의존성**: WiFi가 연결된 후에만 시작 가능

---

## 🔄 4. OTA (Over-The-Air Update)

### 파일
- `app/ota/app_ota.c/h` - OTA 상태 머신
- `app/ota/app_ota_http.c/h` - HTTP OTA 전송

### 아키텍처

```
┌──────────────┐
│ Transport    │ ← BLE / HTTP / TCP
│ (svc_ota)    │
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  app_ota.c   │ ← 전송 무관 상태 머신
│ State Machine│
└──────┬───────┘
       │
       ▼
┌──────────────┐
│ NXP SDK      │ ← OtaSupport (플래시 기록)
│ OtaSupport   │
└──────────────┘
```

### 상태 머신
```c
typedef enum app_ota_state {
    APP_OTA_STATE_IDLE       = 0,  // 대기
    APP_OTA_STATE_READY      = 1,  // begin() 완료, 청크 대기
    APP_OTA_STATE_WRITING    = 2,  // 청크 수신 중
    APP_OTA_STATE_COMMITTED  = 3,  // 커밋 완료, 재부팅 대기
    APP_OTA_STATE_FAILED     = 4,  // 실패
} app_ota_state_t;
```

### OTA 프로토콜 시퀀스
```
1. app_ota_begin(total_size)
   ↓
2. app_ota_chunk(offset, data, len)  ← 반복
   ↓
3. app_ota_chunk(offset, data, len)
   ↓
4. app_ota_commit(header, header_len)
   ↓
5. app_ota_reboot_after_ms(1000)
   ↓
6. NVIC_SystemReset()
```

### API
```c
// 초기화
int app_ota_init(app_ota_status_cb_t cb, void *user_arg);

// OTA 시작 (전체 크기 지정)
int app_ota_begin(uint32_t total_size);

// 청크 전송 (순차적 또는 랜덤 오프셋)
int app_ota_chunk(uint32_t offset, const void *data, size_t len);

// 커밋 (검증 후 부트로더가 스왑)
int app_ota_commit(const uint8_t *header, size_t header_len);

// 취소
void app_ota_abort(void);

// 지연 재부팅
void app_ota_reboot_after_ms(uint32_t delay_ms);
```

### HTTP OTA
- **파일**: `app_ota_http.c`
- **기능**: HTTP GET으로 펌웨어 다운로드
- **태스크**: 전용 FreeRTOS 태스크
```c
xTaskCreate(http_task, "ota_http", APP_OTA_HTTP_TASK_STACK, ...);
```

### 멀티태스킹
- **OTA 태스크**: HTTP 다운로드 시 생성
- **Reboot 태스크**: 지연 재부팅용
```c
xTaskCreate(reboot_task, "ota_reboot", ...);
```

### 플래시 레이아웃
```
┌─────────────┐ 0x00000000
│ Bootloader  │
├─────────────┤
│ Slot 0      │ ← 현재 실행 중인 펌웨어
│ (Primary)   │
├─────────────┤
│ Slot 1      │ ← OTA 대상 슬롯
│ (Secondary) │
└─────────────┘
```

---

## 🌡️ 5. IR (적외선) 모듈

### 파일
- `app/ir/ir_aircon.c/h` - 에어컨 제어 로직
- `app/ir/ir_tx_nxp_ctimer.c/h` - NXP CTIMER IR 송신 드라이버
- `app/ir/ir_codec_samsung_ac.c/h` - 삼성 AC 코덱

### 지원 브랜드
```c
typedef enum {
    IR_AIRCON_BRAND_SAMSUNG = 0,   // ✅ 구현됨
    IR_AIRCON_BRAND_LG,            // ⚠️ 미구현
    IR_AIRCON_BRAND_CARRIER_KR,    // ⚠️ 미구현
    IR_AIRCON_BRAND_WINIA,         // ⚠️ 미구현
    IR_AIRCON_BRAND_PASECO,        // ⚠️ 미구현
    IR_AIRCON_BRAND_HISENSE,       // ⚠️ 미구현
} ir_aircon_brand_t;
```

### 지원 명령
```c
typedef enum {
    IR_AIRCON_ACTION_POWER_ON = 0,
    IR_AIRCON_ACTION_POWER_OFF,
    IR_AIRCON_ACTION_TEMP_UP,
    IR_AIRCON_ACTION_TEMP_DOWN,
} ir_aircon_action_t;
```

### 온도 범위 (삼성)
```c
#define IR_AIRCON_SAMSUNG_TEMP_MIN_C    16  // 16°C
#define IR_AIRCON_SAMSUNG_TEMP_MAX_C    30  // 30°C
```

### API
```c
// 초기화
int ir_aircon_init(void);

// IR 명령 전송 (블로킹)
int ir_aircon_send(ir_aircon_brand_t brand, 
                   ir_aircon_action_t action);

// 절대 온도 설정
int ir_aircon_set_temp_c(ir_aircon_brand_t brand, uint8_t temp_c);

// 현재 설정 온도 조회
uint8_t ir_aircon_get_temp_c(ir_aircon_brand_t brand);
```

### 하드웨어
- **타이머**: NXP CTIMER (38kHz 캐리어 생성)
- **GPIO**: IR LED 구동
- **프로토콜**: 삼성 AC (IRremoteESP8266 호환)

### 멀티태스킹
- **블로킹 전송**: IR 전송 중 태스크가 블록됨
- **우선순위**: 일반적으로 낮은 우선순위 (사용자 요청 시에만)
- **비동기**: BLE 콜백에서 호출되므로 BLE 스택과 직렬화됨

---

## 📊 6. 센서 모듈

### 구성
1. **조도 센서** - BH1750 (I2C)
2. **열화상 센서** - AMG8833 (I2C)

### 6.1 BH1750 (조도 센서)

#### 파일
- `app/sensors/bh1750.c/h` - 드라이버
- `app/sensors/bh1750_io_nxp_i2c.c/h` - I2C HAL
- `app/sensors/lux_task.c/h` - 태스크

#### 기능
- **측정 범위**: 1 ~ 65535 lux
- **정밀도**: 1 lux
- **I2C 주소**: 0x23 (기본)

#### 태스크
```c
xTaskCreate(lux_task, "zcube_lux", LUX_TASK_STACK, ...);
```

### 6.2 AMG8833 (열화상 센서)

#### 파일
- `app/sensors/amg8833.c/h` - 드라이버
- `app/sensors/amg8833_task.c/h` - 태스크

#### 기능
- **해상도**: 8x8 픽셀
- **온도 범위**: 0°C ~ 80°C
- **정밀도**: 0.25°C
- **I2C 주소**: 0x69 (기본)

#### 태스크
```c
xTaskCreate(amg_task, "zcube_amg", AMG_TASK_STACK, ...);
```

### I2C 버스 공유
- **파일**: `app/i2c_bus.c/h`
- **뮤텍스**: 다중 태스크 간 I2C 버스 접근 동기화

---

## 📡 7. Radar 모듈

### 파일
- `app/radar/app_radar.c/h`
- `app/radar/app_radar_zephyr.c`

### 기능
- **UART 통신**: 레이더 센서와 시리얼 통신
- **데이터 스트리밍**: BLE로 실시간 전송

### 태스크
```c
xTaskCreate(radar_task, "zcube_radar", APP_RADAR_TASK_STACK, ...);
```

### 멀티태스킹
- **전용 태스크**: UART RX 처리
- **콜백**: BLE 서비스로 데이터 전달
- **우선순위**: 실시간 데이터 → 높은 우선순위

---

## 💡 8. LED 모듈

### 파일
- `app/leds/sk6812.c/h` - SK6812 드라이버
- `app/leds/sk6812_io_nxp_spi.c/h` - SPI 기반 LED 제어
- `app/leds/led_task.c/h` - LED 태스크

### 기능
- **LED 타입**: SK6812 RGBW (WS2812B 호환)
- **개수**: 설정 가능 (`APP_LED_COUNT`)
- **인터페이스**: SPI (비트뱅잉 대신)

### 패턴
```c
typedef enum led_pattern {
    LED_PAT_OFF = 0,
    LED_PAT_SOLID,       // 고정 색상
    LED_PAT_BLINK,       // 깜빡임
    LED_PAT_BREATHE,     // 호흡
    LED_PAT_RAINBOW,     // 무지개
    LED_PAT_SLEEP_LIGHT, // 수면등
} led_pattern_t;
```

### API
```c
int led_task_start(void);
void led_task_set_pattern(led_pattern_t p, sk6812_color_t base);
void led_task_set_color(sk6812_color_t base);
void led_task_set_brightness(uint8_t br);
int led_task_set_pixel(uint16_t idx, sk6812_color_t c);
```

### 멀티태스킹
- **현재**: Zephyr shim (스텁)
- **원래**: FreeRTOS 태스크로 패턴 애니메이션 처리

---

## 💾 9. NVRAM (비휘발성 저장)

### 파일
- `app/nvm/app_nvram.c/h`

### 저장 데이터
```c
typedef struct app_nvram_data {
    // WiFi 설정
    char ssid[32];
    char password[64];
    char security[8];  // "OPEN", "WPA2"
    
    // TCP 설정
    char tcp_server_ip[16];
    uint16_t tcp_server_port;
    
    // 기타 설정
    // ...
} app_nvram_data_t;
```

### API
```c
int app_nvram_init(void);
int app_nvram_load(app_nvram_data_t *out);
int app_nvram_save(const app_nvram_data_t *cfg);
int app_nvram_factory_reset(void);
```

---

## 🎯 10. Matter 통합

### 파일
- `app/.matter/matter_hub_core.c/h`
- `app/.matter/matter_prefs.c/h`

### 기능
- **Matter 프로토콜**: 스마트홈 표준
- **브릿지 모드**: BLE/WiFi를 Matter로 브릿징
- **디바이스 타입**: 조명, 센서, 온도조절기 등

---

## 🔄 멀티태스킹 구조 요약

### FreeRTOS 태스크 (현재)

| 태스크 이름 | 우선순위 | 스택 크기 | 역할 |
|------------|---------|----------|------|
| `app_main` | 5 | 2048 | 메인 로직, 이벤트 처리 |
| `zcube_tcps` | 3 | 2048 | TCP 서버 |
| `zcube_tcp` | 3 | 2048 | TCP 클라이언트 |
| `zcube_lux` | ? | 1024 | 조도 센서 |
| `zcube_amg` | ? | 1024 | 열화상 센서 |
| `zcube_radar` | ? | 2048 | 레이더 |
| `ota_http` | ? | 2048 | HTTP OTA |
| `ota_reboot` | ? | 512 | 지연 재부팅 |

### Zephyr 스레드 (포팅 중)

```c
// main.c
K_THREAD_DEFINE(app_main_tid, 
                APP_MAIN_STACK_SIZE,
                app_main_task,
                NULL, NULL, NULL,
                APP_MAIN_PRIORITY, 0, 0);
```

### 동기화 메커니즘

1. **Semaphore**: TCP 클라이언트 소켓 보호
2. **Mutex**: I2C 버스 접근 직렬화
3. **Message Queue**: 센서 데이터 전달 (필요 시)
4. **Callbacks**: 비동기 이벤트 전달

---

## 🔧 빌드 구성

### CMakeLists.txt 포함 파일

```cmake
target_sources(app PRIVATE
    src/main.c
    app/app_main_zephyr.c
    app/ble/zephyr_bt_rand_shim.c
    app/ble/gatt_servers/svc_status.c
    app/ble/gatt_servers/svc_ctrl.c
    app/ble/gatt_servers/svc_prov.c
    app/ble/gatt_servers/svc_ir.c
    app/ble/gatt_servers/svc_radar.c
    app/ble/gatt_servers/svc_led.c
    app/ble/gatt_servers/svc_matter.c
    app/nvm/app_nvram.c
    app/ir/ir_aircon.c
    app/ir/ir_tx_nxp_ctimer.c
    app/ir/ir_codec_samsung_ac.c
    app/radar/app_radar_zephyr.c
    app/leds/led_task_zephyr.c
    app/.matter/matter_hub_core.c
    app/.matter/matter_prefs.c
)
```

### prj.conf 주요 설정

```conf
CONFIG_GPIO=y
CONFIG_BT=y
CONFIG_BT_PERIPHERAL=y
CONFIG_BT_DEVICE_NAME="zCube"
CONFIG_MULTITHREADING=y
CONFIG_TIMESLICING=y
```

---

## 🔄 포팅 상태

### ✅ Zephyr로 포팅 완료
- [x] main.c (K_THREAD_DEFINE)
- [x] BLE (Zephyr Bluetooth API)
- [x] app_main_zephyr.c

### ⚠️ FreeRTOS 의존 (포팅 필요)
- [ ] WiFi (NXP wlan SDK - FreeRTOS 기반)
- [ ] TCP Server/Client (xTaskCreate)
- [ ] Sensors (xTaskCreate)
- [ ] Radar (xTaskCreate)
- [ ] OTA (xTaskCreate)
- [ ] LED Task (FreeRTOS 큐/타이머)

### 💡 포팅 전략

1. **WiFi/TCP**: NXP SDK가 FreeRTOS에 의존
   - **옵션 A**: Zephyr의 네이티브 WiFi/lwIP 사용
   - **옵션 B**: FreeRTOS 호환 레이어 유지

2. **Sensors/Radar**: 
   - `xTaskCreate` → `K_THREAD_DEFINE` 또는 `k_thread_create`
   - `vTaskDelay` → `k_msleep`

3. **동기화**:
   - `xSemaphore` → `k_sem`
   - `xMutex` → `k_mutex`
   - `xQueue` → `k_msgq`

---

## 📚 참고 자료

### 관련 문서
- [MULTITASKING_GUIDE.md](MULTITASKING_GUIDE.md) - Zephyr 멀티태스킹 가이드
- [MCUXPRESSO_SETUP.md](MCUXPRESSO_SETUP.md) - MCUXpresso IDE 설정
- [README.rst](README.rst) - 프로젝트 개요

### 외부 링크
- [Zephyr Documentation](https://docs.zephyrproject.org/)
- [NXP RW612 SDK](https://www.nxp.com/rw612)
- [Matter](https://buildwithmatter.com/)

---

## 🎯 다음 단계

### Zephyr 완전 포팅을 위한 작업

1. **WiFi 포팅**
   - Zephyr WiFi 관리 API 사용
   - 또는 NXP SDK FreeRTOS 호환 레이어 유지

2. **TCP 포팅**
   - Zephyr 소켓 API로 전환
   - `k_thread_create`로 서버/클라이언트 스레드 생성

3. **센서 포팅**
   - Zephyr 센서 API 사용 (선택적)
   - FreeRTOS 태스크 → Zephyr 스레드

4. **동기화 포팅**
   - 모든 FreeRTOS 프리미티브 → Zephyr 등가물

5. **테스트**
   - 각 모듈별 단위 테스트
   - 통합 테스트

---

**작성일**: 2026-05-24  
**프로젝트**: zCube RW612 Zephyr Port  
**상태**: FreeRTOS → Zephyr 전환 진행 중
