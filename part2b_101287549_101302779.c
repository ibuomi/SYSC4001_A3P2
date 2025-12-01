#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <signal.h>
#include <semaphore.h>
#include <fcntl.h>

#define NUM_QUESTIONS 5
#define RUBRIC_LINES 5
#define MAX_FILENAME 256

typedef struct {
    int total_exams;
    int exam_index;
    int student_num;
    int question_marked[NUM_QUESTIONS];
    char rubric[RUBRIC_LINES];
    int loading; 
} shared_data_t;

int shmid = -1;
shared_data_t *shm = NULL;

sem_t *sem_rubric;   // semaphore for exclusive rubric modification
sem_t *sem_exam;     // semaphore for loading exam

void cleanup_shm() {
    if(shm) {
        shmdt(shm);
        shm = NULL;
    }
    if(shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
    }
    if(sem_rubric) sem_close(sem_rubric);
    if(sem_exam) sem_close(sem_exam);
    sem_unlink("/sem_rubric");
    sem_unlink("/sem_exam");
}

void sigint_handler(int sig) {
    (void)sig;
    cleanup_shm();
    exit(1);
}

int read_student_number_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if(!f) return -1;
    char line[256];
    int sn = -1;
    if(fgets(line, sizeof(line), f)) {
        sn = atoi(line);
    }
    fclose(f);
    return sn;
}

void load_rubric_to_shm(shared_data_t *s) {
    FILE *f = fopen("rubric.txt", "r");
    if(!f) {
        for(int i = 0; i < RUBRIC_LINES; i++) s->rubric[i] = 'A' + i;
        FILE *g = fopen("rubric.txt", "w");
        if(g) {
            for(int i = 0; i < RUBRIC_LINES; i++) fprintf(g, "%d, %c\n", i+1, s->rubric[i]);
            fclose(g);
        }
        return;
    }
    char line[256];
    int i = 0;
    while(i < RUBRIC_LINES && fgets(line, sizeof(line), f)) {
        char *comma = strchr(line, ',');
        char c = 'A' + i;
        if(comma) {
            char *q = comma+1;
            while(*q==' '||*q=='\t') q++;
            if(*q!='\0'&&*q!='\n'&&*q!='\r') c = *q;
        }
        s->rubric[i] = c;
        i++;
    }
    for(;i<RUBRIC_LINES;i++) s->rubric[i] = 'A'+i;
    fclose(f);
}

void save_rubric_from_shm(shared_data_t *s) {
    FILE *f = fopen("rubric.txt", "w");
    if(!f) return;
    for(int i = 0; i<RUBRIC_LINES; i++) fprintf(f,"%d, %c\n", i+1, s->rubric[i]);
    fclose(f);
}

void load_exam_to_shm(shared_data_t *s, int idx) {
    char fname[MAX_FILENAME];
    snprintf(fname, sizeof(fname), "exams/exam%02d.txt", idx+1);
    int sn = read_student_number_from_file(fname);
    if(sn<0) s->student_num = 9999;
    else s->student_num = sn;
    for(int i=0;i<NUM_QUESTIONS;i++) s->question_marked[i]=0;
}

void ta_worker(int ta_id) {
    srand(time(NULL) ^ getpid() ^ (ta_id*7919));
    while(1) {
        int sn = shm->student_num;
        int cur_idx = shm->exam_index;
        if(sn==9999) {
            printf("[TA %d] Detected sentinel student 9999. Exiting.\n", ta_id);
            fflush(stdout);
            break;
        }
        printf("[TA %d] Working on exam index %d (student %04d)\n", ta_id, cur_idx+1, sn);
        fflush(stdout);

        // check rubric
        for(int q=0;q<RUBRIC_LINES;q++) {
            usleep(500000 + rand()%500001); // 0.5-1s
            int decide = rand()%2;
            if(decide) {
                sem_wait(sem_rubric);
                char before = shm->rubric[q];
                shm->rubric[q] = before+1;
                save_rubric_from_shm(shm);
                printf("[TA %d] Modified rubric for question %d: %c -> %c (saved)\n", ta_id, q+1, before, before+1);
                fflush(stdout);
                sem_post(sem_rubric);
            } else {
                printf("[TA %d] Checked rubric for question %d: %c (no change)\n", ta_id, q+1, shm->rubric[q]);
                fflush(stdout);
            }
        }

        // mark exam questions
        while(1) {
            int picked = -1;
            for(int i=0;i<NUM_QUESTIONS;i++) {
                if(shm->question_marked[i]==0) {
                    shm->question_marked[i]=1;
                    picked=i;
                    break;
                }
            }
            if(picked==-1) break;
            int micros = 1000000 + rand()%1000001;
            printf("[TA %d] Marking student %04d question %d (will take %.3f s)\n", ta_id, shm->student_num, picked+1, micros/1000000.0);
            fflush(stdout);
            usleep(micros);
            printf("[TA %d] Finished marking student %04d question %d\n", ta_id, shm->student_num, picked+1);
            fflush(stdout);
        }

        // load next exam
        sem_wait(sem_exam);
        if(shm->exam_index+1 >= shm->total_exams) {
            shm->student_num=9999;
            shm->exam_index++;
            sem_post(sem_exam);
            break;
        } else {
            shm->exam_index++;
            load_exam_to_shm(shm, shm->exam_index);
            printf("[TA %d] Loaded exam %d (student %04d).\n", ta_id, shm->exam_index+1, shm->student_num);
            fflush(stdout);
            sem_post(sem_exam);
        }
    }
    shmdt(shm);
    exit(0);
}

int main(int argc,char **argv) {
    if(argc<3) {
        fprintf(stderr,"Usage: %s <num_TAs (>=2)> <num_exams>\n", argv[0]);
        return 1;
    }
    int numTAs = atoi(argv[1]);
    int numExams = atoi(argv[2]);
    if(numTAs<2) { fprintf(stderr,"At least 2 TAs required\n"); return 1; }

    shmid = shmget(IPC_PRIVATE,sizeof(shared_data_t),IPC_CREAT|0666);
    if(shmid<0) { perror("shmget"); return 1; }
    shm = shmat(shmid,NULL,0);
    if(shm==(void*)-1) { perror("shmat"); shmctl(shmid, IPC_RMID,NULL); return 1; }

    shm->total_exams=numExams;
    shm->exam_index=0;
    shm->loading=0;
    load_rubric_to_shm(shm);
    load_exam_to_shm(shm,0);

    signal(SIGINT, sigint_handler);

    // semaphores
    sem_unlink("/sem_rubric");
    sem_unlink("/sem_exam");
    sem_rubric = sem_open("/sem_rubric", O_CREAT, 0644, 1);
    sem_exam = sem_open("/sem_exam", O_CREAT, 0644, 1);

    pid_t *kids = calloc(numTAs,sizeof(pid_t));
    for(int i=0;i<numTAs;i++) {
        pid_t pid = fork();
        if(pid<0) { perror("fork"); cleanup_shm(); return 1; }
        if(pid==0) ta_worker(i+1);
        kids[i]=pid;
    }

    for(int i=0;i<numTAs;i++) {
        int status;
        waitpid(kids[i],&status,0);
    }

    cleanup_shm();
    free(kids);
    printf("All TAs finished. Parent exiting.\n");
    return 0;
}
