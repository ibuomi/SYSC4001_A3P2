

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <signal.h>
#include <semaphore.h>
#include <errno.h>

#define NUM_QUESTIONS 5
#define RUBRIC_LINES 5
#define MAX_FILENAME 256

typedef struct {
    int total_exams;
    int exam_index;
    int student_num;
    int question_marked[NUM_QUESTIONS];

    char rubric[RUBRIC_LINES];

    // readers-writer control for rubric
    int readers;            // number of readers currently reading
    sem_t r_mutex;          // protects readers count
    sem_t rw_sem;           // writer/exclusive semaphore for rubric

    // mutex for claiming question index
    sem_t q_mutex;

    // mutex for loading next exam
    sem_t load_mutex;

    int loading;
} shared_data_t;

int shmid = -1;
shared_data_t *shm = NULL;

/* portable microsecond sleep using nanosleep */
static void sleep_us(long micros) {
    if (micros <= 0) return;
    struct timespec ts;
    ts.tv_sec = micros / 1000000;
    ts.tv_nsec = (micros % 1000000) * 1000L;
    nanosleep(&ts, NULL);
}

/* helper: read first integer token from file (student number) */
int read_student_number_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int sn = -1;
    while (fgets(line, sizeof(line), f)) {
        char *p = strtok(line, " \t\r\n,");
        if (p) {
            int v = atoi(p);
            if (v > 0) {
                sn = v;
                break;
            }
        }
    }
    fclose(f);
    return sn;
}

void save_rubric_from_shm(shared_data_t *s) {
    FILE *f = fopen("rubric.txt", "w");
    if (!f) {
        fprintf(stderr, "Failed to open rubric.txt for writing\n");
        return;
    }
    for (int i = 0; i < RUBRIC_LINES; ++i) {
        fprintf(f, "%d, %c\n", i+1, s->rubric[i]);
    }
    fflush(f);
    fclose(f);
}

void load_rubric_to_shm(shared_data_t *s) {
    FILE *f = fopen("rubric.txt", "r");
    if (!f) {
        fprintf(stderr, "Could not open rubric.txt for reading. Creating default.\n");
        for (int i = 0; i < RUBRIC_LINES; ++i) s->rubric[i] = 'A' + i;
        FILE *g = fopen("rubric.txt", "w");
        if (g) {
            for (int i = 0; i < RUBRIC_LINES; ++i) fprintf(g, "%d, %c\n", i+1, s->rubric[i]);
            fclose(g);
        }
        return;
    }
    char line[256];
    int i = 0;
    while (i < RUBRIC_LINES && fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        char c = 'A' + i;
        if (comma) {
            char *q = comma + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '\0' && *q != '\n' && *q != '\r') c = *q;
        } else {
            char *q = line;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '\0' && *q != '\n' && *q != '\r') c = *q;
        }
        s->rubric[i] = c;
        ++i;
    }
    for (; i < RUBRIC_LINES; ++i) s->rubric[i] = 'A' + i;
    fclose(f);
}

/* load exam index into shared memory: reads student number and resets question_marked */
void load_exam_to_shm(shared_data_t *s, int idx) {
    char fname[MAX_FILENAME];
    snprintf(fname, sizeof(fname), "exams/exam%xxd.txt", idx+1);
    int sn = read_student_number_from_file(fname);
    if (sn < 0) s->student_num = 9999;
    else s->student_num = sn;
    for (int i = 0; i < NUM_QUESTIONS; ++i) s->question_marked[i] = 0;
}

void cleanup_shm() {
    if (shm) {
        // destroy semaphores (parent only should call)
        sem_destroy(&shm->r_mutex);
        sem_destroy(&shm->rw_sem);
        sem_destroy(&shm->q_mutex);
        sem_destroy(&shm->load_mutex);

        shmdt((void*)shm);
        shm = NULL;
    }
    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
    }
}

void sigint_handler(int sig) {
    (void)sig;
    cleanup_shm();
    exit(1);
}

/* reader lock for rubric */
void rubric_reader_lock(shared_data_t *s) {
    sem_wait(&s->r_mutex);
    s->readers++;
    if (s->readers == 1) {
        sem_wait(&s->rw_sem); // first reader locks writers out
    }
    sem_post(&s->r_mutex);
}

/* reader unlock for rubric */
void rubric_reader_unlock(shared_data_t *s) {
    sem_wait(&s->r_mutex);
    s->readers--;
    if (s->readers == 0) {
        sem_post(&s->rw_sem); // last reader releases writers
    }
    sem_post(&s->r_mutex);
}

/* writer lock/unlock for rubric (exclusive) */
void rubric_writer_lock(shared_data_t *s) {
    sem_wait(&s->rw_sem);
}
void rubric_writer_unlock(shared_data_t *s) {
    sem_post(&s->rw_sem);
}

/* child worker (TA) */
void ta_worker(int ta_id) {
    srand((unsigned int)(time(NULL) ^ getpid() ^ (ta_id*7919)));

    while (1) {
        int sn, cur_idx;
        /* read student num and index in a safe way */
        sem_wait(&shm->load_mutex);
        sn = shm->student_num;
        cur_idx = shm->exam_index;
        sem_post(&shm->load_mutex);

        if (sn == 9999) {
            printf("[TA %d] Detected sentinel student 9999. Exiting.\n", ta_id);
            fflush(stdout);
            break;
        }

        printf("[TA %d] Working on exam index %d (student %04d)\n", ta_id, cur_idx+1, sn);
        fflush(stdout);

        /* Iterate through rubric: readers-writer semantics */
        for (int q = 0; q < RUBRIC_LINES; ++q) {
            int micros = 500000 + (rand() % 500001); // 0.5 - 1.0 s
            sleep_us(micros);

            // Decide whether to modify rubric (random)
            int decide = rand() % 2;
            if (decide) {
                // want to write -> acquire writer lock (exclusive)
                rubric_writer_lock(shm);
                char before = shm->rubric[q];
                char after = before + 1;
                shm->rubric[q] = after;
                // persist to file while still holding writer lock
                save_rubric_from_shm(shm);
                printf("[TA %d] Modified rubric for question %d: %c -> %c (saved)\n", ta_id, q+1, before, after);
                fflush(stdout);
                rubric_writer_unlock(shm);
            } else {
                // read-only: acquire reader lock
                rubric_reader_lock(shm);
                char val = shm->rubric[q];
                printf("[TA %d] Checked rubric for question %d: %c (no change)\n", ta_id, q+1, val);
                fflush(stdout);
                rubric_reader_unlock(shm);
            }
        }

        /* Marking phase: claim questions atomically */
        while (1) {
            int picked = -1;
            sem_wait(&shm->q_mutex);
            for (int i = 0; i < NUM_QUESTIONS; ++i) {
                if (shm->question_marked[i] == 0) {
                    shm->question_marked[i] = 1;
                    picked = i;
                    break;
                }
            }
            sem_post(&shm->q_mutex);

            if (picked == -1) {
                // all marked -> attempt to load next exam (one loader at a time)
                sem_wait(&shm->load_mutex);
                int next_idx = shm->exam_index + 1;
                if (next_idx >= shm->total_exams) {
                    // load sentinel
                    printf("[TA %d] No more exams. Loading sentinel 9999.\n", ta_id);
                    shm->exam_index = next_idx;
                    shm->student_num = 9999;
                    sem_post(&shm->load_mutex);
                    break;
                } else {
                    // only this TA will load next exam due to load_mutex
                    printf("[TA %d] Loading next exam %d...\n", ta_id, next_idx+1);
                    shm->exam_index = next_idx;
                    load_exam_to_shm(shm, next_idx);
                    if (shm->student_num == 9999) {
                        printf("[TA %d] Loaded sentinel 9999. Exiting.\n", ta_id);
                        sem_post(&shm->load_mutex);
                        break;
                    } else {
                        printf("[TA %d] Loaded exam %d (student %04d).\n", ta_id, next_idx+1, shm->student_num);
                    }
                    sem_post(&shm->load_mutex);
                    // after loading, go back to top of outer while to process new exam
                    break;
                }
            } else {
                // we picked a question
                int micros = 1000000 + (rand() % 1000001); // 1.0 - 2.0 s
                printf("[TA %d] Marking student %04d question %d (will take %.3f s)\n",
                       ta_id, shm->student_num, picked+1, micros / 1000000.0);
                fflush(stdout);
                sleep_us(micros);
                printf("[TA %d] Finished marking student %04d question %d\n",
                       ta_id, shm->student_num, picked+1);
                fflush(stdout);
                // continue marking remaining questions
            }
        } 

        
    } 

    shmdt((void*)shm);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <num_TAs (>=2)> <num_exams>\n", argv[0]);
        return 1;
    }
    int numTAs = atoi(argv[1]);
    int numExams = atoi(argv[2]);
    if (numTAs < 2) {
        fprintf(stderr, "Please use at least 2 TAs.\n");
        return 1;
    }
    if (numExams < 1) {
        fprintf(stderr, "Please provide at least 1 exam.\n");
        return 1;
    }

    // create shared memory
    shmid = shmget(IPC_PRIVATE, sizeof(shared_data_t), IPC_CREAT | 0666);
    if (shmid < 0) {
        perror("shmget");
        return 1;
    }
    shm = (shared_data_t*)shmat(shmid, NULL, 0);
    if (shm == (void*)-1) {
        perror("shmat");
        shmctl(shmid, IPC_RMID, NULL);
        return 1;
    }

    // initialize shared memory fields
    shm->total_exams = numExams;
    shm->exam_index = 0;
    shm->loading = 0;
    shm->readers = 0;


    // Initialize semaphores in shared memory (pshared = 1)
    if (sem_init(&shm->r_mutex, 1, 1) == -1) { perror("sem_init r_mutex"); cleanup_shm(); return 1; }
    if (sem_init(&shm->rw_sem, 1, 1) == -1) { perror("sem_init rw_sem"); cleanup_shm(); return 1; }
    if (sem_init(&shm->q_mutex, 1, 1) == -1) { perror("sem_init q_mutex"); cleanup_shm(); return 1; }
    if (sem_init(&shm->load_mutex, 1, 1) == -1) { perror("sem_init load_mutex"); cleanup_shm(); return 1; }

    // parent cleanup handler
    signal(SIGINT, sigint_handler);

    // spawn TAs
    pid_t *kids = calloc(numTAs, sizeof(pid_t));
    for (int i = 0; i < numTAs; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            for (int j = 0; j < i; ++j) kill(kids[j], SIGTERM);
            cleanup_shm();
            return 1;
        } else if (pid == 0) {
            // child
            ta_worker(i+1);
            exit(0);
        } else {
            kids[i] = pid;
        }
    }

    // wait for children to finish
    for (int i = 0; i < numTAs; ++i) {
        int status;
        waitpid(kids[i], &status, 0);
    }

    // cleanup semaphores and shared memory (parent)
    cleanup_shm();
    free(kids);
    printf("All TAs finished. Parent exiting.\n");
    return 0;
}
