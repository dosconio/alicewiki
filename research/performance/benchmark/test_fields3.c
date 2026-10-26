#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { NUM_THREADS = 4, N = 8 };

/* A bare run must already repeat every cross-object pair often enough for the
 * window sampler, so the default is not 10. */
long ITERATIONS = 2000;

/* Migration source: acc is hot, pad is cold, and acc is touched only next to
 * the worker's own PosSample element. */
struct LiveState {
    volatile long acc;
    volatile long pad[7];
};

/* Merge partner A: four components of one element, owned by one worker. */
struct PosSample {
    volatile long x;
    volatile long y;
    volatile long z;
    volatile long w;
};

/* Merge partner B: the homologous components, consumed in the same loop body. */
struct VelSample {
    volatile long dx;
    volatile long dy;
    volatile long dz;
    volatile long dw;
};

/* Split source: one instance, one hot field per worker, all on the same line. */
struct StageBuf {
    volatile long s0;
    volatile long s1;
    volatile long s2;
    volatile long s3;
    volatile long spare[4];
};

/* Control: never touched next to a hot field. */
struct ColdBox {
    volatile long a;
    volatile long b;
};

/* Singleton control: read by every worker next to element-indexed fields, so a
 * merge against it would violate instance-cardinality compatibility. */
volatile long g_bias;

struct PosSample pos_arr[N][NUM_THREADS];
struct VelSample vel_arr[N][NUM_THREADS];
struct LiveState live_arr[N][NUM_THREADS];
struct StageBuf stage;
struct ColdBox cold_arr[N][NUM_THREADS];

_Static_assert(N == 8 && NUM_THREADS == 4, "element grid changed");
_Static_assert(sizeof(long) == 8, "long must be 8 bytes");
_Static_assert(sizeof(struct PosSample) == 32 && sizeof(struct VelSample) == 32,
               "component struct size changed");
_Static_assert(sizeof(struct LiveState) == 64 && sizeof(struct StageBuf) == 64 &&
                   sizeof(struct ColdBox) == 16,
               "container size changed");

static void *worker(void *arg) {
    int tid = *(int *)arg;
    int i;
    long iter;
    int k;

    for (iter = 0; iter < ITERATIONS; ++iter) {
        for (i = 0; i < N; ++i) {
            /* Migration: acc is read next to the owning element only. */
            pos_arr[i][tid].x += live_arr[i][tid].acc + g_bias;
            live_arr[i][tid].acc += 1;

            pos_arr[i][tid].y += 2;
            pos_arr[i][tid].z += 3;
            pos_arr[i][tid].w += 4;

            /* Merge: homologous components consumed in the same order. */
            vel_arr[i][tid].dx += pos_arr[i][tid].x;
            vel_arr[i][tid].dy += pos_arr[i][tid].y;
            vel_arr[i][tid].dz += pos_arr[i][tid].z;
            vel_arr[i][tid].dw += pos_arr[i][tid].w;
        }

        /* Split: different workers write different fields of the same instance. */
        switch (tid) {
            case 0: stage.s0 += 1; break;
            case 1: stage.s1 += 2; break;
            case 2: stage.s2 += 3; break;
            default: stage.s3 += 4; break;
        }
    }

    /* Cold tail: reached once per worker run, so these fields must never be
     * treated as hot or dropped from a recommended layout. */
    if (tid == 0) {
        for (i = 0; i < N; ++i) {
            live_arr[i][0].pad[0] += 1;
            cold_arr[i][0].a += 1;
            for (k = 0; k < 4; ++k) stage.spare[k] += 1;
        }
    }
    return NULL;
}

static int emit(long value, int first) {
    if (!first) putchar(',');
    printf("%ld", value);
    return 0;
}

static void emit_state(void) {
    int first = 1;
    int i;
    int t;
    int k;

    for (i = 0; i < N; ++i) {
        for (t = 0; t < NUM_THREADS; ++t) {
            first = emit(pos_arr[i][t].x, first);
            first = emit(pos_arr[i][t].y, first);
            first = emit(pos_arr[i][t].z, first);
            first = emit(pos_arr[i][t].w, first);
            first = emit(vel_arr[i][t].dx, first);
            first = emit(vel_arr[i][t].dy, first);
            first = emit(vel_arr[i][t].dz, first);
            first = emit(vel_arr[i][t].dw, first);
            first = emit(live_arr[i][t].acc, first);
            for (k = 0; k < 7; ++k) first = emit(live_arr[i][t].pad[k], first);
            first = emit(cold_arr[i][t].a, first);
            first = emit(cold_arr[i][t].b, first);
        }
    }
    first = emit(stage.s0, first);
    first = emit(stage.s1, first);
    first = emit(stage.s2, first);
    first = emit(stage.s3, first);
    for (k = 0; k < 4; ++k) first = emit(stage.spare[k], first);
    emit(g_bias, first);
}

static void emit_address(const char *name, const volatile long *value, int *first) {
    if (!*first) putchar(',');
    printf("\"%s\":%llu", name, (unsigned long long)(uintptr_t)value);
    *first = 0;
}

static void emit_addresses(void) {
    int first = 1;

    emit_address("pos_x_0_0", &pos_arr[0][0].x, &first);
    emit_address("pos_x_1_0", &pos_arr[1][0].x, &first);
    emit_address("pos_x_0_1", &pos_arr[0][1].x, &first);
    emit_address("pos_y_0_0", &pos_arr[0][0].y, &first);
    emit_address("vel_dx_0_0", &vel_arr[0][0].dx, &first);
    emit_address("vel_dx_1_0", &vel_arr[1][0].dx, &first);
    emit_address("live_acc_0_0", &live_arr[0][0].acc, &first);
    emit_address("live_acc_1_0", &live_arr[1][0].acc, &first);
    emit_address("stage_s0", &stage.s0, &first);
    emit_address("stage_s1", &stage.s1, &first);
    emit_address("stage_s2", &stage.s2, &first);
    emit_address("stage_s3", &stage.s3, &first);
    emit_address("cold_a_0_0", &cold_arr[0][0].a, &first);
    emit_address("g_bias", &g_bias, &first);
}

int main(int argc, char **argv) {
    cpu_set_t allowed;
    int cpus[NUM_THREADS];
    int observed[NUM_THREADS];
    pthread_t threads[NUM_THREADS];
    int ids[NUM_THREADS];
    struct timespec start;
    struct timespec stop;
    double seconds;
    int t;
    int cpu;
    int failed = 0;

    if (argc != 1 && argc != 2 && argc != 6) {
        fprintf(stderr, "usage: test_fields3 [iterations [cpu0 cpu1 cpu2 cpu3]]\n");
        return 1;
    }
    if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
        fprintf(stderr, "cannot read CPU affinity\n");
        return 1;
    }
    if (argc >= 2) ITERATIONS = strtol(argv[1], NULL, 10);
    if (argc == 6) {
        for (t = 0; t < NUM_THREADS; ++t) cpus[t] = atoi(argv[t + 2]);
    } else {
        int found = 0;
        for (cpu = 0; cpu < CPU_SETSIZE && found < NUM_THREADS; ++cpu) {
            if (CPU_ISSET(cpu, &allowed)) cpus[found++] = cpu;
        }
        if (found != NUM_THREADS) {
            fprintf(stderr, "four CPUs required\n");
            return 1;
        }
    }
    if (ITERATIONS < 1 || ITERATIONS > 2000000) {
        fprintf(stderr, "iterations must be in [1,2000000]\n");
        return 1;
    }
    for (t = 0; t < NUM_THREADS; ++t) {
        int other;
        if (cpus[t] < 0 || cpus[t] >= CPU_SETSIZE || !CPU_ISSET(cpus[t], &allowed)) {
            fprintf(stderr, "invalid CPU\n");
            return 1;
        }
        for (other = 0; other < t; ++other) {
            if (cpus[t] == cpus[other]) {
                fprintf(stderr, "distinct CPUs required\n");
                return 1;
            }
        }
    }
    for (cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &allowed) && cpu != cpus[0] && cpu != cpus[1] &&
            cpu != cpus[2] && cpu != cpus[3]) {
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(cpu, &mask);
            if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
                fprintf(stderr, "cannot bind controller\n");
                return 1;
            }
            break;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &start);
    for (t = 0; t < NUM_THREADS; ++t) {
        cpu_set_t mask;
        ids[t] = t;
        CPU_ZERO(&mask);
        CPU_SET(cpus[t], &mask);
        if (pthread_create(&threads[t], NULL, worker, &ids[t]) != 0) {
            fprintf(stderr, "cannot create worker %d\n", t);
            return 1;
        }
        if (pthread_setaffinity_np(threads[t], sizeof(mask), &mask) != 0) failed = 1;
        observed[t] = cpus[t];
    }
    for (t = 0; t < NUM_THREADS; ++t) pthread_join(threads[t], NULL);
    clock_gettime(CLOCK_MONOTONIC, &stop);
    if (failed) {
        fprintf(stderr, "worker binding failed\n");
        return 1;
    }

    seconds = (double)(stop.tv_sec - start.tv_sec) +
              (double)(stop.tv_nsec - start.tv_nsec) / 1e9;

    printf("{\"seconds\":%.17g,\"iterations\":%ld,\"cpus\":[", seconds, ITERATIONS);
    for (t = 0; t < NUM_THREADS; ++t) printf("%s%d", t ? "," : "", observed[t]);
    printf("],\"state\":[");
    emit_state();
    printf("],\"addresses\":{");
    emit_addresses();
    printf("}}\n");
    return 0;
}
