/*
 * Zephyr Multi-Threading Example
 * 
 * 이 파일은 Zephyr에서 여러 태스크를 동시에 실행하는 방법을 보여줍니다.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(multitask_example, LOG_LEVEL_INF);

/* 스레드 스택 크기 및 우선순위 정의 */
#define TASK1_STACK_SIZE 1024
#define TASK2_STACK_SIZE 1024
#define TASK3_STACK_SIZE 1024

#define TASK1_PRIORITY 7
#define TASK2_PRIORITY 7
#define TASK3_PRIORITY 7

/* ========================================
 * 방법 1: K_THREAD_DEFINE 사용 (컴파일 타임 정의)
 * ======================================== */

/* Task 1: LED 제어 태스크 */
void task1_led_control(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Task 1 (LED Control) started");

    while (1) {
        LOG_INF("Task 1: LED Toggle");
        
        /* LED 제어 코드 */
        // gpio_pin_toggle_dt(&led);
        
        k_msleep(1000);  /* 1초 대기 */
    }
}

/* Task 2: 센서 읽기 태스크 */
void task2_sensor_read(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Task 2 (Sensor Read) started");

    while (1) {
        LOG_INF("Task 2: Reading Sensors");
        
        /* 센서 읽기 코드 */
        // sensor_sample_fetch(dev);
        // sensor_channel_get(dev, SENSOR_CHAN_ALL, &val);
        
        k_msleep(500);  /* 500ms 대기 */
    }
}

/* Task 3: 통신 태스크 */
void task3_communication(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);

    LOG_INF("Task 3 (Communication) started");

    while (1) {
        LOG_INF("Task 3: Handling Communication");
        
        /* 통신 처리 코드 */
        // send_data();
        // receive_data();
        
        k_msleep(2000);  /* 2초 대기 */
    }
}

/* 컴파일 타임에 스레드 정의 (자동으로 시작됨) */
K_THREAD_DEFINE(task1_tid, TASK1_STACK_SIZE,
                task1_led_control, NULL, NULL, NULL,
                TASK1_PRIORITY, 0, 0);

K_THREAD_DEFINE(task2_tid, TASK2_STACK_SIZE,
                task2_sensor_read, NULL, NULL, NULL,
                TASK2_PRIORITY, 0, 0);

K_THREAD_DEFINE(task3_tid, TASK3_STACK_SIZE,
                task3_communication, NULL, NULL, NULL,
                TASK3_PRIORITY, 0, 0);


/* ========================================
 * 방법 2: k_thread_create 사용 (런타임 생성)
 * ======================================== */

/* 런타임 스레드를 위한 스택 및 스레드 구조체 */
K_THREAD_STACK_DEFINE(dynamic_task_stack, 1024);
static struct k_thread dynamic_task_data;

void dynamic_task_function(void *p1, void *p2, void *p3)
{
    int counter = 0;
    
    LOG_INF("Dynamic Task started");

    while (1) {
        LOG_INF("Dynamic Task: counter=%d", counter++);
        k_msleep(3000);  /* 3초 대기 */
    }
}

/* 런타임에 스레드 생성하는 함수 */
void create_dynamic_task(void)
{
    k_tid_t tid;

    tid = k_thread_create(&dynamic_task_data, dynamic_task_stack,
                         K_THREAD_STACK_SIZEOF(dynamic_task_stack),
                         dynamic_task_function,
                         NULL, NULL, NULL,
                         5,  /* 우선순위 */
                         0,  /* 옵션 */
                         K_NO_WAIT);  /* 즉시 시작 */

    k_thread_name_set(tid, "dynamic_task");
    
    LOG_INF("Dynamic task created with TID: %p", tid);
}


/* ========================================
 * 방법 3: Work Queue 사용 (지연된 작업)
 * ======================================== */

/* Work item 정의 */
static struct k_work_delayable delayed_work;

void delayed_work_handler(struct k_work *work)
{
    LOG_INF("Delayed work executed!");
    
    /* 작업 수행 */
    
    /* 다시 스케줄링 (5초 후) */
    k_work_schedule(&delayed_work, K_SECONDS(5));
}

void init_delayed_work(void)
{
    k_work_init_delayable(&delayed_work, delayed_work_handler);
    k_work_schedule(&delayed_work, K_SECONDS(5));
    
    LOG_INF("Delayed work initialized");
}


/* ========================================
 * 방법 4: 스레드 간 통신 (Message Queue)
 * ======================================== */

/* 메시지 구조체 */
struct data_msg {
    uint32_t timestamp;
    int16_t value;
    uint8_t sensor_id;
};

/* 메시지 큐 정의 (최대 10개 메시지) */
K_MSGQ_DEFINE(sensor_msgq, sizeof(struct data_msg), 10, 4);

/* Producer 태스크 */
void producer_task(void *p1, void *p2, void *p3)
{
    struct data_msg msg;
    int counter = 0;

    LOG_INF("Producer task started");

    while (1) {
        msg.timestamp = k_uptime_get_32();
        msg.value = counter++;
        msg.sensor_id = 1;

        /* 메시지 큐에 데이터 전송 */
        if (k_msgq_put(&sensor_msgq, &msg, K_NO_WAIT) != 0) {
            LOG_WRN("Message queue full!");
        } else {
            LOG_DBG("Sent: value=%d", msg.value);
        }

        k_msleep(1500);
    }
}

/* Consumer 태스크 */
void consumer_task(void *p1, void *p2, void *p3)
{
    struct data_msg msg;

    LOG_INF("Consumer task started");

    while (1) {
        /* 메시지 큐에서 데이터 수신 */
        if (k_msgq_get(&sensor_msgq, &msg, K_FOREVER) == 0) {
            LOG_INF("Received: ts=%u, value=%d, sensor=%d",
                    msg.timestamp, msg.value, msg.sensor_id);
        }
    }
}

K_THREAD_DEFINE(producer_tid, 1024,
                producer_task, NULL, NULL, NULL,
                6, 0, 0);

K_THREAD_DEFINE(consumer_tid, 1024,
                consumer_task, NULL, NULL, NULL,
                6, 0, 0);


/* ========================================
 * 방법 5: 세마포어를 사용한 동기화
 * ======================================== */

K_SEM_DEFINE(sync_sem, 0, 1);

void synchronized_task1(void *p1, void *p2, void *p3)
{
    LOG_INF("Synchronized Task 1 started");

    while (1) {
        /* 작업 수행 */
        LOG_INF("Task 1: Working...");
        k_msleep(1000);
        
        /* Task 2에게 신호 전송 */
        k_sem_give(&sync_sem);
        
        k_msleep(2000);
    }
}

void synchronized_task2(void *p1, void *p2, void *p3)
{
    LOG_INF("Synchronized Task 2 started");

    while (1) {
        /* Task 1의 신호 대기 */
        k_sem_take(&sync_sem, K_FOREVER);
        
        LOG_INF("Task 2: Received signal from Task 1");
        /* 작업 수행 */
    }
}


/* ========================================
 * 초기화 함수 (필요시 사용)
 * ======================================== */

int multitask_example_init(void)
{
    LOG_INF("Multi-task example initialized");
    
    /* 런타임 스레드 생성 (필요시) */
    // create_dynamic_task();
    
    /* Delayed work 초기화 (필요시) */
    // init_delayed_work();
    
    return 0;
}

/* 부팅 시 자동 초기화 (선택 사항) */
// SYS_INIT(multitask_example_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
