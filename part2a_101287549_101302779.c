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

// Shared memory struct
#define NUM_QUESTIONS 5
#define RUBRIC_LINES 5
#define MAX_FILENAME 256

typedef struct {
    int total_exams;        
    int exam_index;         
    int student_num;        
    int question_marked[NUM_QUESTIONS]; 
    char rubric[RUBRIC_LINES];
    // small flag to indicate an exam is being (re)loaded - not used for protection here
    int loading;
} shared_data_t;

int shmid = -1;
shared_data_t *shm = NULL;

/* helper: read first integer token from file (student number) */
int read_student_number_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    int sn = -1;
    while (fgets(line, sizeof(line), f)) {
        // find first 4-digit token
        char *p = strtok(line, " \t\r\n,");
        if (p) {
            // try parse integer
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

/* load rubric.txt into shared memory (rubric array of 5 chars) */
void load_rubric_to_shm(shared_data_t *s) {
    FILE *f = fopen("rubric.txt", "r");
    if (!f) {
        fprintf(stderr, "Could not open rubric.txt for reading. Creating default.\n");
        // create default rubric
        for (int i = 0; i < RUBRIC_LINES; ++i) s->rubric[i] = 'A' + i;
        // save to file
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
        // find comma then first non-space char after it
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
    // if file too short, fill rest
    for (; i < RUBRIC_LINES; ++i) s->rubric[i] = 'A' + i;
    fclose(f);
}

/* save rubric from shared memory back to file */
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

/* load exam index into shared memory: reads student number and resets question_marked */
void load_exam_to_shm(shared_data_t *s, int idx) {
    char fname[MAX_FILENAME];
    snprintf(fname, sizeof(fname), "exams/exam%02d.txt", idx+1); 
    int sn = read_student_number_from_file(fname);
    if (sn < 0) {
        // If cannot read file, treat as sentinel 9999 to stop
        s->student_num = 9999;
    } else {
        s->student_num = sn;
    }
    for (int i = 0; i < NUM_QUESTIONS; ++i) s->question_marked[i] = 0;
}

/* cleanup shared memory (parent only) */
void cleanup_shm() {
    if (shm) {
        shmdt((void*)shm);
        shm = NULL;
    }
    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
    }
}

/* signal handler for parent to cleanup on ctrl-c */
void sigint_handler(int sig) {
    (void)sig;
    cleanup_shm();
    exit(1);
}

/* child worker (TA) */
void ta_worker(int ta_id) {
    // seed randomness
    srand((unsigned int)(time(NULL) ^ getpid() ^ (ta_id*7919)));

    while (1) {
        // read current student num from shared memory
        int sn = shm->student_num;
        int cur_idx = shm->exam_index;
        if (sn == 9999) {
            printf("[TA %d] Detected sentinel student 9999. Exiting.\n", ta_id);
            fflush(stdout);
            break;
        }

        printf("[TA %d] Working on exam index %d (student %04d)\n", ta_id, cur_idx+1, sn);
        fflush(stdout);

        // iterate through rubric (all TAs can read concurrently)
        for (int q = 0; q < RUBRIC_LINES; ++q) {
            // simulate decision time between 0.5-1.0 seconds
            int micros = 500000 + (rand() % 500001); // [500000,1000000]
            usleep(micros);

            // random decision whether rubric must be corrected
            int decide = rand() % 2; // 0 or 1 (rough 50% chance)
            if (decide) {
                char before = shm->rubric[q];
                char after = before + 1;
                shm->rubric[q] = after;
                // save to rubric file immediately (no synchronization here)
                save_rubric_from_shm(shm);
                printf("[TA %d] Modified rubric for question %d: %c -> %c (saved to file)\n", ta_id, q+1, before, after);
                fflush(stdout);
            } else {
                printf("[TA %d] Checked rubric for question %d: %c (no change)\n", ta_id, q+1, shm->rubric[q]);
                fflush(stdout);
            }
            
        }

        // After checking rubric, start marking questions on the exam
        int any_marked = 0;
        int local_done = 0;
        while (1) {
            int picked = -1;
            // pick a question not marked yet
            for (int i = 0; i < NUM_QUESTIONS; ++i) {
                if (shm->question_marked[i] == 0) {
                    // claim it by setting to 1 immediately (race possible)
                    shm->question_marked[i] = 1;
                    picked = i;
                    break;
                }
            }
            if (picked == -1) {
                // all questions appear marked
                local_done = 1;
                break;
            }
            // simulate marking time between 1.0-2.0 seconds
            int micros = 1000000 + (rand() % 1000001); // [1,2] seconds
            printf("[TA %d] Marking student %04d question %d (will take %.3f s)\n",
                   ta_id, shm->student_num, picked+1, micros / 1000000.0);
            fflush(stdout);
            usleep(micros);
            printf("[TA %d] Finished marking student %04d question %d\n", ta_id, shm->student_num, picked+1);
            fflush(stdout);
            any_marked = 1;
            
        }

        if (local_done) {
            // try to load next exam if this TA notices all questions finished
            // Check if there are more exams
            int next_idx = shm->exam_index + 1;
            if (next_idx >= shm->total_exams) {
                // if no more, load a sentinel exam with 9999 to finish
                printf("[TA %d] No more exams available. Loading sentinel to finish.\n", ta_id);
                shm->exam_index = next_idx;
                shm->student_num = 9999;
                fflush(stdout);
                break;
            } else {
                // Attempt to load the next exam into shared memory
                printf("[TA %d] All questions on exam %d appear done. Loading next exam (%d)...\n", ta_id, cur_idx+1, next_idx+1);
                fflush(stdout);
                shm->exam_index = next_idx;
                load_exam_to_shm(shm, next_idx);
                if (shm->student_num == 9999) {
                    printf("[TA %d] Loaded sentinel student 9999. Exiting.\n", ta_id);
                    fflush(stdout);
                    break;
                } else {
                    printf("[TA %d] Loaded exam %d (student %04d).\n", ta_id, next_idx+1, shm->student_num);
                    fflush(stdout);
                    // continue to work on this newly loaded exam
                }
            }
        } else {
            
        }
    } 

    // detach shared mem
    shmdt((void*)shm);
    exit(0);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <num_TAs (>=2)> <num_exams>\n", argv[0]);
        fprintf(stderr, "Expects exam files: exams/exam01.txt ... examNN.txt and rubric.txt in current directory\n");
        return 1;
    }

    int numTAs = atoi(argv[1]);
    int numExams = atoi(argv[2]);
    if (numTAs < 2) {
        fprintf(stderr, "Please provide at least 2 TAs.\n");
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

    // initialize shared memory
    shm->total_exams = numExams;
    shm->exam_index = 0;
    shm->loading = 0;
    load_rubric_to_shm(shm);
    load_exam_to_shm(shm, 0);
    if (shm->student_num == 9999) {
        printf("First exam is sentinel 9999 -> exiting immediately.\n");
        cleanup_shm();
        return 0;
    }

    // parent sets up handler to cleanup on ctrl-c
    signal(SIGINT, sigint_handler);

    // spawn TA processes
    pid_t *kids = calloc(numTAs, sizeof(pid_t));
    for (int i = 0; i < numTAs; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            // kill earlier children
            for (int j = 0; j < i; ++j) kill(kids[j], SIGTERM);
            cleanup_shm();
            return 1;
        } else if (pid == 0) {
            // child
            ta_worker(i+1);
            // child should never return here
            exit(0);
        } else {
            kids[i] = pid;
        }
    }

    // parent waits for children to exit
    for (int i = 0; i < numTAs; ++i) {
        int status;
        waitpid(kids[i], &status, 0);
    }

    // cleanup
    cleanup_shm();
    free(kids);
    printf("All TAs finished. Parent exiting.\n");
    return 0;
}
