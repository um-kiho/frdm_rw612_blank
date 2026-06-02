# Zephyr 멀티태스킹 가이드

## 개요
Zephyr RTOS는 완벽한 멀티태스킹을 지원합니다. 여러 작업을 동시에 실행할 수 있습니다.

## 현재 프로젝트 상태 ✅
프로젝트는 이미 멀티태스킹 모드로 실행 중입니다!

## 설정 완료 사항

### [prj.conf](prj.conf) 설정
```conf
CONFIG_MULTITHREADING=y              # 멀티스레딩 활성화
CONFIG_NUM_PREEMPT_PRIORITIES=15     # 우선순위 레벨 (0-14)
CONFIG_TIMESLICING=y                 # 타임슬라이싱 (같은 우선순위 스레드 간)
CONFIG_TIMESLICE_SIZE=10             # 타임슬라이스 크기 (ms)
CONFIG_THREAD_NAME=y                 # 스레드 이름 지정 가능
CONFIG_THREAD_STACK_INFO=y           # 스택 정보 모니터링
CONFIG_THREAD_MONITOR=y              # 스레드 모니터링
```

## 멀티태스킹 사용 방법

### 1. 컴파일 타임 스레드 생성 (K_THREAD_DEFINE)

가장 간단하고 권장되는 방법입니다.

```c
#include <zephyr/kernel.h>

#define TASK_STACK_SIZE 1024
#define TASK_PRIORITY 7

void my_task_function(void *p1, void *p2, void *p3)
{
    while (1) {
        printk("Task running\n");
        k_msleep(1000);  // 1초 대기
    }
}

// 스레드 정의 (자동으로 시작됨)
K_THREAD_DEFINE(my_task_tid,           // 스레드 ID
                TASK_STACK_SIZE,        // 스택 크기
                my_task_function,       // 태스크 함수
                NULL, NULL, NULL,       // 파라미터 3개
                TASK_PRIORITY,          // 우선순위 (낮을수록 높음)
                0,                      // 옵션
                0);                     // 시작 지연 (ms)
```

### 2. 런타임 스레드 생성 (k_thread_create)

동적으로 스레드를 생성할 때 사용합니다.

```c
#include <zephyr/kernel.h>

K_THREAD_STACK_DEFINE(my_stack, 1024);
static struct k_thread my_thread_data;

void my_task(void *p1, void *p2, void *p3)
{
    while (1) {
        printk("Dynamic task\n");
        k_msleep(1000);
    }
}

void create_my_task(void)
{
    k_tid_t tid;
    
    tid = k_thread_create(&my_thread_data,      // 스레드 구조체
                         my_stack,               // 스택
                         K_THREAD_STACK_SIZEOF(my_stack),
                         my_task,                // 태스크 함수
                         NULL, NULL, NULL,       // 파라미터
                         5,                      // 우선순위
                         0,                      // 옵션
                         K_NO_WAIT);             // 즉시 시작
    
    k_thread_name_set(tid, "my_task");
}
```

### 3. Work Queue 사용 (지연/주기적 작업)

시스템 워크큐에 작업을 스케줄링합니다.

```c
#include <zephyr/kernel.h>

static struct k_work_delayable my_work;

void work_handler(struct k_work *work)
{
    printk("Work executed!\n");
    
    // 다시 스케줄 (5초 후)
    k_work_schedule(&my_work, K_SECONDS(5));
}

void init_work(void)
{
    k_work_init_delayable(&my_work, work_handler);
    k_work_schedule(&my_work, K_SECONDS(5));
}
```

### 4. 스레드 간 통신

#### Message Queue (메시지 큐)

```c
struct sensor_data {
    uint32_t timestamp;
    int16_t value;
};

// 메시지 큐 정의 (최대 10개)
K_MSGQ_DEFINE(sensor_q, sizeof(struct sensor_data), 10, 4);

// 송신 스레드
void sender_task(void)
{
    struct sensor_data data = {
        .timestamp = k_uptime_get_32(),
        .value = 100
    };
    
    k_msgq_put(&sensor_q, &data, K_NO_WAIT);
}

// 수신 스레드
void receiver_task(void)
{
    struct sensor_data data;
    
    while (1) {
        k_msgq_get(&sensor_q, &data, K_FOREVER);
        printk("Received: %d\n", data.value);
    }
}
```

#### Semaphore (세마포어)

```c
K_SEM_DEFINE(my_sem, 0, 1);

// Task 1: 신호 전송
void task1(void)
{
    printk("Task 1 working\n");
    k_sem_give(&my_sem);  // 신호 전송
}

// Task 2: 신호 대기
void task2(void)
{
    k_sem_take(&my_sem, K_FOREVER);  // 신호 대기
    printk("Task 2 received signal\n");
}
```

#### Mutex (뮤텍스) - 공유 자원 보호

```c
K_MUTEX_DEFINE(my_mutex);
int shared_resource = 0;

void task_a(void)
{
    k_mutex_lock(&my_mutex, K_FOREVER);
    shared_resource++;
    k_mutex_unlock(&my_mutex);
}

void task_b(void)
{
    k_mutex_lock(&my_mutex, K_FOREVER);
    shared_resource--;
    k_mutex_unlock(&my_mutex);
}
```

## 스레드 우선순위

Zephyr는 **숫자가 낮을수록 우선순위가 높습니다**.

- `0`: 최고 우선순위 (실시간 critical)
- `1-7`: 높은 우선순위 (중요한 태스크)
- `8-14`: 일반 우선순위 (일반 태스크)
- 같은 우선순위면 타임슬라이싱으로 순환 실행

### 권장 우선순위 배정
```
0-2:  인터럽트 수준 작업, 실시간 통신
3-5:  센서 읽기, 모터 제어 등 시간 민감 작업
6-8:  BLE, WiFi, 네트워크 통신
9-11: LED, UI, 디스플레이 업데이트
12-14: 로깅, 백그라운드 작업
```

## 타이밍 함수

```c
k_sleep(K_MSEC(100));      // 100ms 대기 (절전 모드)
k_msleep(100);             // 100ms 대기 (동일)
k_usleep(1000);            // 1000us = 1ms 대기

k_busy_wait(1000);         // 1000us busy wait (CPU 사용)

uint32_t uptime = k_uptime_get_32();  // 부팅 후 시간 (ms)
```

## 실전 예제

### 예제 1: LED + 센서 + 통신

```c
// LED 제어 태스크 (우선순위 10)
K_THREAD_DEFINE(led_task_tid, 1024, led_task, 
                NULL, NULL, NULL, 10, 0, 0);

void led_task(void *p1, void *p2, void *p3)
{
    while (1) {
        gpio_pin_toggle_dt(&led);
        k_msleep(500);
    }
}

// 센서 읽기 태스크 (우선순위 5 - 더 높음)
K_THREAD_DEFINE(sensor_task_tid, 1024, sensor_task,
                NULL, NULL, NULL, 5, 0, 0);

void sensor_task(void *p1, void *p2, void *p3)
{
    while (1) {
        read_temperature();
        read_humidity();
        k_msleep(1000);
    }
}

// BLE 통신 태스크 (우선순위 7)
K_THREAD_DEFINE(ble_task_tid, 2048, ble_task,
                NULL, NULL, NULL, 7, 0, 0);

void ble_task(void *p1, void *p2, void *p3)
{
    while (1) {
        ble_process();
        k_msleep(100);
    }
}
```

## CMakeLists.txt에 추가

[CMakeLists.txt](CMakeLists.txt)에 새 소스 파일을 추가하세요:

```cmake
target_sources(app PRIVATE
    # ... 기존 파일들
    app/multitask_example.c  # 추가
)
```

## 디버깅 팁

### 스레드 상태 확인
```c
// 스레드 이름 설정
k_thread_name_set(tid, "my_task");

// 스레드 우선순위 변경
k_thread_priority_set(tid, 5);

// 스레드 일시 중지/재개
k_thread_suspend(tid);
k_thread_resume(tid);
```

### 로깅
```c
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(my_module, LOG_LEVEL_INF);

LOG_INF("Info message");
LOG_WRN("Warning message");
LOG_ERR("Error message");
LOG_DBG("Debug message");
```

### Shell에서 스레드 확인 (CONFIG_SHELL=y 필요)
```bash
uart:~$ kernel threads
```

## 주의사항

1. **스택 크기**: 너무 작으면 스택 오버플로우 발생
   - 최소 512 바이트
   - 일반적으로 1024-2048 바이트
   - `CONFIG_THREAD_STACK_INFO=y`로 모니터링

2. **우선순위 역전**: 낮은 우선순위가 높은 우선순위를 블록
   - Mutex 사용 시 주의
   - `CONFIG_PRIORITY_INHERITANCE=y` 권장

3. **데드락**: 두 스레드가 서로를 기다림
   - 항상 같은 순서로 락 획득
   - 타임아웃 사용 (`K_MSEC(100)`)

4. **공유 메모리**: 반드시 동기화 필요
   - Mutex, Semaphore, Atomic 연산 사용

## 참고 자료

- [Zephyr Kernel Services](https://docs.zephyrproject.org/latest/kernel/services/index.html)
- [Zephyr Threading](https://docs.zephyrproject.org/latest/kernel/services/threads/index.html)
- [Zephyr Synchronization](https://docs.zephyrproject.org/latest/kernel/services/synchronization/index.html)

## 예제 파일

- [app/multitask_example.c](app/multitask_example.c) - 5가지 멀티태스킹 방법 예제
- [app/multitask_example.h](app/multitask_example.h) - 헤더 파일
