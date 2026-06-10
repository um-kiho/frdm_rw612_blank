# RW612 OTA 기능 정의

## 1. 목적
- 본 문서는 FRDM-RW612 기반 zCube 펌웨어의 OTA(Over-The-Air) 업데이트 기능을 구현 코드 기준으로 정의한다.
- OTA 전송 계층(BLE, HTTP)과 무관하게 공통 상태 머신(app_ota)을 중심으로 동작을 규정한다.

## 2. 구성 요소
- OTA 코어: app/ota/app_ota.c, app/ota/app_ota.h
- HTTP Pull OTA: app/ota/app_ota_http.c, app/ota/app_ota_http.h
- BLE OTA 서비스: app/ble/gatt_servers/svc_ota.c, app/ble/gatt_servers/svc_ota.h
- 하부 기록 엔진: NXP OtaSupport (OTA_StartImage, OTA_PushImageChunk, OTA_CommitImage, OTA_CancelImage)

## 3. OTA 코어 상태 머신 정의

### 3.1 상태
- APP_OTA_STATE_IDLE(0): 대기 상태
- APP_OTA_STATE_READY(1): begin(total_size) 성공, 청크 대기
- APP_OTA_STATE_WRITING(2): 청크 수신/기록 진행 중
- APP_OTA_STATE_COMMITTED(3): commit 성공, 재부팅 대기
- APP_OTA_STATE_FAILED(4): 실패 상태

### 3.2 에러 코드
- APP_OTA_OK(0)
- APP_OTA_ERR_PARAM(1)
- APP_OTA_ERR_INVALID_STATE(2)
- APP_OTA_ERR_INVALID_OFFSET(3)
- APP_OTA_ERR_WRITE_FAILED(4)
- APP_OTA_ERR_COMMIT_FAILED(5)
- APP_OTA_ERR_ABORTED(6)
- APP_OTA_ERR_INIT_FAILED(7)

### 3.3 핵심 정책
- 청크 오프셋은 strict in-order 정책을 따른다.
- app_ota_chunk(offset, ...) 호출 시 offset == bytes_received 이어야 한다.
- 중간 오프셋 점프(holes)는 허용하지 않는다.
- total_size를 초과하는 write는 실패 처리한다.

## 4. 공통 OTA API 정의
- app_ota_init(cb, arg): OTA 서브시스템 초기화 및 상태 콜백 등록
- app_ota_begin(total_size): Slot1 대상 이미지 기록 세션 시작
- app_ota_chunk(offset, data, len): 이미지 청크 기록
- app_ota_commit(header, header_len): 커밋(후보 이미지 마킹)
- app_ota_abort(): 세션 중단 및 상태 초기화
- app_ota_get_status(out): 상태 조회
- app_ota_reboot_after_ms(delay_ms): 지연 재부팅

## 5. BLE OTA 기능 정의 (GATT e600/e601/e602)

### 5.1 UUID
- Service: e600
- CTRL(Write): e601
- STATUS(Read/Notify): e602

### 5.2 CTRL opcode
- 0x01 BEGIN: total_size(u32)
- 0x02 CHUNK: offset(u32), data_len(u16), data[data_len]
- 0x03 COMMIT: optional header bytes
- 0x04 ABORT
- 0x05 REBOOT
- 0x06 HTTP_PULL

### 5.3 HTTP_PULL payload
- [flags:u8][iurl_len:u16][image_url][hurl_len:u16][header_url]
- flags bit0: AUTO_COMMIT
- hurl_len=0이면 header_url 없음(Unsigned 이미지)

### 5.4 STATUS 포맷
- state:u8, last_err:u8, bytes_received:u32, total_size:u32

## 6. HTTP OTA 기능 정의

### 6.1 시작 옵션
- image_url(필수): HTTP 이미지 URL
- header_url(선택): signed image header blob URL
- auto_commit: true면 다운로드 완료 후 commit + reboot 자동 수행

### 6.2 동작 시퀀스
1. app_ota 상태를 확인해 resume 가능 여부 판단
2. image_url GET 수행
3. 필요 시 Range: bytes=N- 로 재개 시도
4. 수신 body를 APP_OTA_HTTP_RX_CHUNK 단위로 app_ota_chunk()에 전달
5. header_url 존재 시 추가 다운로드
6. auto_commit=true면 app_ota_commit() 후 reboot 예약

### 6.3 재개/복구 규칙
- 상태가 WRITING이고 bytes_received>0이면 resume 요청
- 서버가 206 + 일치하는 Content-Range를 반환하면 이어받기 성공
- 서버가 Range를 무시하고 200을 반환하면 기존 세션 abort 후 새 세션으로 재시작

### 6.4 리다이렉트
- 301/302/303/307/308 지원
- 최대 APP_OTA_HTTP_MAX_REDIRECTS(기본 2회)

### 6.5 응답 처리
- 고정 길이(body length)와 chunked transfer 모두 파싱 가능
- 단, image 다운로드에서 chunked는 총 길이를 사전 결정할 수 없어 실패 처리
- header.bin 다운로드는 chunked 허용

### 6.6 제약사항
- HTTPS/TLS 미지원 (http://만 허용)
- 이미지 URL은 Content-Length 기반 서버 권장
- URL 길이 제한: APP_OTA_HTTP_URL_MAX

## 7. 플래시/부트 의미
- OTA 기록 대상은 Secondary Slot(Slot1)
- commit 성공 시 이미지가 candidate로 마킹됨
- 실제 검증/스왑은 다음 부팅 시 부트로더 단계에서 수행됨

## 8. 동시성 및 태스크
- OTA 코어는 mutex 기반 직렬화로 상태 일관성 보장
- HTTP OTA는 전용 스레드(ota_http)에서 실행
- 재부팅은 별도 스레드(ota_reboot)에서 지연 수행 가능

## 9. 현재 프로젝트 적용 상태(중요)
- 현재 CMakeLists.txt에서 아래 파일은 target_sources에 포함되어 있지 않다.
  - app/ota/app_ota.c
  - app/ota/app_ota_http.c
  - app/ble/gatt_servers/svc_ota.c
- 즉, 코드 구현은 존재하지만 현재 빌드 타깃에는 비활성 상태다.
- OTA 기능을 실제 활성화하려면 위 소스 추가 + app_main_zephyr.c 초기화 경로 연결이 필요하다.

## 10. 최소 성공 시나리오
1. app_ota_init() 성공
2. BEGIN(total_size)
3. CHUNK(0..N-1 순서)
4. COMMIT(optional header)
5. reboot
6. 부트로더 검증 후 신규 이미지 부팅
