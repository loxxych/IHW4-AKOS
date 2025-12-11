#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

typedef enum {
    SPECIALIST_DENTIST = 0,
    SPECIALIST_SURGEON = 1,
    SPECIALIST_THERAPIST = 2
} SpecialistType;

const char *specialist_name(SpecialistType type) {
    switch (type) {
    case SPECIALIST_DENTIST:
        return "стоматолог";
    case SPECIALIST_SURGEON:
        return "хирург";
    case SPECIALIST_THERAPIST:
    default:
        return "терапевт";
    }
}

typedef struct {
    int id;
    sem_t finished;
} Patient;

typedef struct {
    Patient **items;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
    pthread_mutex_t mutex;
} PatientQueue;

typedef struct {
    int patient_count;
    int arrival_min_ms;
    int arrival_max_ms;
    int triage_min_ms;
    int triage_max_ms;
    int treatment_min_ms;
    int treatment_max_ms;
} SimulationConfig;

// Shared simulation state
static SimulationConfig config;
static PatientQueue triage_queue;
static PatientQueue dentist_queue;
static PatientQueue surgeon_queue;
static PatientQueue therapist_queue;
static sem_t triage_waiting;
static sem_t dentist_waiting;
static sem_t surgeon_waiting;
static sem_t therapist_waiting;
static pthread_mutex_t rng_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t stop_requested = 0;

static void handle_sigint(int sig) {
    (void)sig;
    stop_requested = 1;
    const char msg[] = "\nПолучен сигнал завершения, завершаем смену после текущих приёмов...\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
}

static int random_between(int min, int max) {
    if (min == max) {
        return min;
    }
    pthread_mutex_lock(&rng_mutex);
    int value = min + rand() % (max - min + 1);
    pthread_mutex_unlock(&rng_mutex);
    return value;
}

static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

static void queue_init(PatientQueue *queue, size_t capacity) {
    queue->items = calloc(capacity, sizeof(Patient *));
    queue->capacity = capacity;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    pthread_mutex_init(&queue->mutex, NULL);
}

static void queue_destroy(PatientQueue *queue) {
    free(queue->items);
    pthread_mutex_destroy(&queue->mutex);
}

static void queue_push(PatientQueue *queue, Patient *patient) {
    pthread_mutex_lock(&queue->mutex);
    queue->items[queue->tail] = patient;
    queue->tail = (queue->tail + 1) % queue->capacity;
    queue->size++;
    pthread_mutex_unlock(&queue->mutex);
}

static Patient *queue_pop(PatientQueue *queue) {
    pthread_mutex_lock(&queue->mutex);
    Patient *result = queue->items[queue->head];
    queue->head = (queue->head + 1) % queue->capacity;
    queue->size--;
    pthread_mutex_unlock(&queue->mutex);
    return result;
}

static void log_event(const char *format, ...) {
    va_list args;
    va_start(args, format);
    pthread_mutex_lock(&log_mutex);
    vprintf(format, args);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&log_mutex);
    va_end(args);
}

typedef struct {
    int id;
} PatientArgs;

typedef struct {
    int id;
} DutyDoctorArgs;

typedef struct {
    SpecialistType type;
} SpecialistArgs;

static void *patient_routine(void *arg) {
    PatientArgs *args = (PatientArgs *)arg;
    Patient *patient = malloc(sizeof(Patient));
    patient->id = args->id;
    sem_init(&patient->finished, 0, 0);

    int arrival_delay = random_between(config.arrival_min_ms, config.arrival_max_ms);
    sleep_ms(arrival_delay);
    if (stop_requested) {
        log_event("Пациент %d разворачивается домой из-за завершения работы клиники", patient->id);
        sem_destroy(&patient->finished);
        free(patient);
        free(args);
        return NULL;
    }

    log_event("Пациент %d пришёл в регистратуру через %d мс", patient->id, arrival_delay);

    queue_push(&triage_queue, patient);
    sem_post(&triage_waiting);

    sem_wait(&patient->finished);
    log_event("Пациент %d завершил лечение и уходит домой", patient->id);

    sem_destroy(&patient->finished);
    free(patient);
    free(args);
    return NULL;
}

static SpecialistType choose_specialist(void) {
    int pick = random_between(0, 2);
    switch (pick) {
    case 0:
        return SPECIALIST_DENTIST;
    case 1:
        return SPECIALIST_SURGEON;
    default:
        return SPECIALIST_THERAPIST;
    }
}

static void *duty_doctor_routine(void *arg) {
    DutyDoctorArgs *args = (DutyDoctorArgs *)arg;
    while (true) {
        sem_wait(&triage_waiting);
        Patient *patient = queue_pop(&triage_queue);
        if (patient == NULL) {
            break;
        }

        int talk_time = random_between(config.triage_min_ms, config.triage_max_ms);
        log_event("Дежурный врач %d беседует с пациентом %d (%d мс)", args->id, patient->id,
                  talk_time);
        sleep_ms(talk_time);

        SpecialistType target = choose_specialist();
        log_event("Дежурный врач %d отправляет пациента %d к врачу: %s", args->id, patient->id,
                  specialist_name(target));

        PatientQueue *queue = NULL;
        sem_t *waiting = NULL;
        switch (target) {
        case SPECIALIST_DENTIST:
            queue = &dentist_queue;
            waiting = &dentist_waiting;
            break;
        case SPECIALIST_SURGEON:
            queue = &surgeon_queue;
            waiting = &surgeon_waiting;
            break;
        case SPECIALIST_THERAPIST:
        default:
            queue = &therapist_queue;
            waiting = &therapist_waiting;
            break;
        }

        queue_push(queue, patient);
        sem_post(waiting);
    }

    log_event("Дежурный врач %d завершает смену", args->id);
    free(args);
    return NULL;
}

static void *specialist_routine(void *arg) {
    SpecialistArgs *args = (SpecialistArgs *)arg;
    SpecialistType type = args->type;
    PatientQueue *queue = NULL;
    sem_t *waiting = NULL;

    switch (type) {
    case SPECIALIST_DENTIST:
        queue = &dentist_queue;
        waiting = &dentist_waiting;
        break;
    case SPECIALIST_SURGEON:
        queue = &surgeon_queue;
        waiting = &surgeon_waiting;
        break;
    case SPECIALIST_THERAPIST:
    default:
        queue = &therapist_queue;
        waiting = &therapist_waiting;
        break;
    }

    while (true) {
        sem_wait(waiting);
        Patient *patient = queue_pop(queue);
        if (patient == NULL) {
            break;
        }

        int treat_time = random_between(config.treatment_min_ms, config.treatment_max_ms);
        log_event("%s начинает лечение пациента %d (%d мс)", specialist_name(type), patient->id,
                  treat_time);
        sleep_ms(treat_time);
        log_event("%s завершил лечение пациента %d", specialist_name(type), patient->id);
        sem_post(&patient->finished);
    }

    log_event("%s уходит домой", specialist_name(type));
    free(args);
    return NULL;
}

static void usage(const char *program) {
    fprintf(stderr,
            "Использование: %s [--random-config] [--seed N] "
            "<patients> <arrival_min> <arrival_max> <triage_min> <triage_max> <treat_min> <treat_max>\n",
            program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Позиционные параметры используются, если не указан --random-config.\\n\n");
    fprintf(stderr, "--random-config  Сгенерировать набор входных данных в допустимых диапазонах\n");
    fprintf(stderr, "--seed N        Использовать заданное зерно генератора случайных чисел\n");
}

static bool parse_int(const char *text, int *out) {
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    if (value > INT_MAX || value < INT_MIN) {
        return false;
    }
    *out = (int)value;
    return true;
}

static void clamp_ranges(void) {
    if (config.patient_count < 1) {
        config.patient_count = 1;
    }
    if (config.arrival_min_ms < 0) {
        config.arrival_min_ms = 0;
    }
    if (config.arrival_max_ms < config.arrival_min_ms) {
        config.arrival_max_ms = config.arrival_min_ms;
    }
    if (config.triage_min_ms < 0) {
        config.triage_min_ms = 0;
    }
    if (config.triage_max_ms < config.triage_min_ms) {
        config.triage_max_ms = config.triage_min_ms;
    }
    if (config.treatment_min_ms < 0) {
        config.treatment_min_ms = 0;
    }
    if (config.treatment_max_ms < config.treatment_min_ms) {
        config.treatment_max_ms = config.treatment_min_ms;
    }
}

static void randomize_config(void) {
    // Базовые допустимые диапазоны генерации
    const int patient_min = 3;
    const int patient_max = 15;
    const int arrival_min_low = 0;
    const int arrival_min_high = 150;
    const int arrival_spread_min = 40;
    const int arrival_spread_max = 220;
    const int triage_min_low = 20;
    const int triage_min_high = 150;
    const int triage_spread_min = 40;
    const int triage_spread_max = 160;
    const int treat_min_low = 60;
    const int treat_min_high = 220;
    const int treat_spread_min = 50;
    const int treat_spread_max = 250;

    config.patient_count = random_between(patient_min, patient_max);
    config.arrival_min_ms = random_between(arrival_min_low, arrival_min_high);
    config.arrival_max_ms = config.arrival_min_ms + random_between(arrival_spread_min, arrival_spread_max);
    config.triage_min_ms = random_between(triage_min_low, triage_min_high);
    config.triage_max_ms = config.triage_min_ms + random_between(triage_spread_min, triage_spread_max);
    config.treatment_min_ms = random_between(treat_min_low, treat_min_high);
    config.treatment_max_ms = config.treatment_min_ms + random_between(treat_spread_min, treat_spread_max);

    clamp_ranges();
}

static bool parse_args(int argc, char *argv[], unsigned *seed_out, bool *random_out) {
    bool random_config = false;
    int idx = 1;
    bool seed_set = false;

    while (idx < argc && strncmp(argv[idx], "--", 2) == 0) {
        if (strcmp(argv[idx], "--random-config") == 0) {
            random_config = true;
            idx++;
        } else if (strcmp(argv[idx], "--seed") == 0) {
            if (idx + 1 >= argc) {
                usage(argv[0]);
                return false;
            }
            int seed_value = 0;
            if (!parse_int(argv[idx + 1], &seed_value)) {
                usage(argv[0]);
                return false;
            }
            *seed_out = (unsigned)seed_value;
            seed_set = true;
            idx += 2;
        } else if (strcmp(argv[idx], "--help") == 0) {
            usage(argv[0]);
            exit(0);
        } else {
            usage(argv[0]);
            return false;
        }
    }

    if (!seed_set) {
        *seed_out = (unsigned)time(NULL);
    }

    if (random_config) {
        if (idx != argc) {
            usage(argv[0]);
            return false;
        }
        *random_out = true;
        return true;
    }

    if (argc - idx != 7) {
        usage(argv[0]);
        return false;
    }

    if (!parse_int(argv[idx], &config.patient_count) ||
        !parse_int(argv[idx + 1], &config.arrival_min_ms) ||
        !parse_int(argv[idx + 2], &config.arrival_max_ms) ||
        !parse_int(argv[idx + 3], &config.triage_min_ms) ||
        !parse_int(argv[idx + 4], &config.triage_max_ms) ||
        !parse_int(argv[idx + 5], &config.treatment_min_ms) ||
        !parse_int(argv[idx + 6], &config.treatment_max_ms)) {
        usage(argv[0]);
        return false;
    }

    clamp_ranges();
    *random_out = false;
    return true;
}

static void init_simulation(void) {
    size_t capacity = (size_t)config.patient_count + 4;
    queue_init(&triage_queue, capacity);
    queue_init(&dentist_queue, capacity);
    queue_init(&surgeon_queue, capacity);
    queue_init(&therapist_queue, capacity);
    sem_init(&triage_waiting, 0, 0);
    sem_init(&dentist_waiting, 0, 0);
    sem_init(&surgeon_waiting, 0, 0);
    sem_init(&therapist_waiting, 0, 0);
}

static void destroy_simulation(void) {
    queue_destroy(&triage_queue);
    queue_destroy(&dentist_queue);
    queue_destroy(&surgeon_queue);
    queue_destroy(&therapist_queue);
    sem_destroy(&triage_waiting);
    sem_destroy(&dentist_waiting);
    sem_destroy(&surgeon_waiting);
    sem_destroy(&therapist_waiting);
}

int main(int argc, char *argv[]) {
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);

    unsigned seed = 0;
    bool random_config = false;
    if (!parse_args(argc, argv, &seed, &random_config)) {
        return 1;
    }

    srand(seed);
    if (random_config) {
        randomize_config();
        log_event("Случайная конфигурация: пациентов %d, приход [%d;%d] мс, беседа [%d;%d] мс, лечение [%d;%d] мс",
                  config.patient_count, config.arrival_min_ms, config.arrival_max_ms, config.triage_min_ms,
                  config.triage_max_ms, config.treatment_min_ms, config.treatment_max_ms);
    } else {
        log_event("Конфигурация из командной строки: пациентов %d, приход [%d;%d] мс, беседа [%d;%d] мс, лечение [%d;%d] мс",
                  config.patient_count, config.arrival_min_ms, config.arrival_max_ms, config.triage_min_ms,
                  config.triage_max_ms, config.treatment_min_ms, config.treatment_max_ms);
    }
    init_simulation();

    pthread_t duty_doctors[2];
    pthread_t specialists[3];
    pthread_t *patients = calloc((size_t)config.patient_count, sizeof(pthread_t));

    for (int i = 0; i < 2; ++i) {
        DutyDoctorArgs *args = malloc(sizeof(DutyDoctorArgs));
        args->id = i + 1;
        pthread_create(&duty_doctors[i], NULL, duty_doctor_routine, args);
    }

    for (int i = 0; i < 3; ++i) {
        SpecialistArgs *args = malloc(sizeof(SpecialistArgs));
        args->type = (SpecialistType)i;
        pthread_create(&specialists[i], NULL, specialist_routine, args);
    }

    for (int i = 0; i < config.patient_count; ++i) {
        PatientArgs *args = malloc(sizeof(PatientArgs));
        args->id = i + 1;
        pthread_create(&patients[i], NULL, patient_routine, args);
    }

    for (int i = 0; i < config.patient_count; ++i) {
        pthread_join(patients[i], NULL);
    }

    // Все пациенты завершили лечение, разбудим дежурных врачей контрольными посылками
    for (int i = 0; i < 2; ++i) {
        queue_push(&triage_queue, NULL);
        sem_post(&triage_waiting);
    }

    for (int i = 0; i < 2; ++i) {
        pthread_join(duty_doctors[i], NULL);
    }

    // Разбудим специалистов, чтобы они завершили работу
    queue_push(&dentist_queue, NULL);
    queue_push(&surgeon_queue, NULL);
    queue_push(&therapist_queue, NULL);
    sem_post(&dentist_waiting);
    sem_post(&surgeon_waiting);
    sem_post(&therapist_waiting);

    for (int i = 0; i < 3; ++i) {
        pthread_join(specialists[i], NULL);
    }

    free(patients);
    destroy_simulation();
    log_event("Рабочий день клиники завершён");
    return 0;
}

