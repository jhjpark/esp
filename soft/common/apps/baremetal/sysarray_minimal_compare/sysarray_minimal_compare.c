/* Copyright (c) 2011-2021 Columbia University, System Level Design Group */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>
#ifndef __riscv
#include <stdlib.h>
#endif
#include <inttypes.h>

#include <esp_accelerator.h>
#include <esp_probe.h>
#include "sysarray_minimal.h"

#define MAX_PRINTED_ERRORS 10
#define HU_SYSARRAY  0x102
#define DEV_NAME "hu,hu_sysarray"

typedef char token_t;
typedef char native_t;

static unsigned in_len1;
static unsigned in_len2;
static unsigned in_len3;
static unsigned in_offset1;
static unsigned in_offset2;
static unsigned in_offset3;
static unsigned out_len;
static unsigned in_size1;
static unsigned in_size2;
static unsigned out_size;
static unsigned out_offset;
static unsigned mem_size;

// configuration parameters
const static int IsRelu = 1;
const static int BiasShift = 6;
const static int AccumShift = 10;
const static int AccumMul = 93;
// start signal (write only), fire interrupt upon completion
const static int MWR   = 1; // master weight read
const static int MDR   = 2; // master input read
const static int MDW   = 3; // master input write
const static int START = 4; // start systolic array

// configuration register offsets
#define Dummy_REG    0x00
#define SA_START     0x04
#define SA_CONFIC    0x08
// base address in DRAM for weight mem, data read mem and data write mem
#define SA_W_RD_BASE 0x0C
#define SA_D_RD_BASE 0x10
#define SA_D_WR_BASE 0x14

static unsigned DMA_WORD_PER_BEAT(unsigned _st)
{
    return (sizeof(void *) / _st);
}

static void iointerrupt()
{
    printf("wait\n");
}

// output validation
static int validate_buf(token_t *out, native_t *gold)
{
    int j;
    native_t val;
    unsigned errors = 0;

    for (j = 0; j < out_len; j++) {
        val = out[j];
        if (gold[j] != val) {
            errors++;
            if (errors <= MAX_PRINTED_ERRORS) {
                printf("%d : %d : %d\n", j, val, gold[j]);
            }
        }
    }

    return errors;
}

// input and expected output initialization
static void init_buf(token_t *B_mat, token_t *W_mat, token_t *I_mat, native_t *O_mat)
{
    #include "input_B.h"
    #include "input_W.h"
    #include "input_I.h"
    #include "gold.h"
}

// Used to calculate time (get current counter)
static inline uint64_t get_counter() {
    uint64_t counter;
    asm volatile (
        "li t0, 0;"
        "csrr t0, mcycle;"
        "mv %0, t0"
        : "=r" ( counter )
        :
        : "t0"
    );
    return counter;
}

// Multiply a N0 x M_mat matrix at a and M_mat x N1 matrix at b and store in d
void CPU_multiply(int *a, int *b, int N0, int M_mat, int N1, int *d) {
    for (int i = 0; i < N0; i++) {
        for (int j = 0; j < N1; j++) {
            // Get d[i][j] by getting dot product of row i and column j
            int sum = 0;
            for (int k = 0; k < M_mat; k++) {
                sum = sum + a[i * M_mat + k] * b[k * N1 + j];
            }
            d[i * N1 + j] = sum;
        }
    }
}

static void cpu_matmul() {
    // Matrix multiplication configurations
    int N0 = 32;
    int N1 = 32;
    int M_mat = 32;

    int *mat1 = aligned_malloc(N0 * M_mat * sizeof(int));
    int *mat2 = aligned_malloc(M_mat * N1 * sizeof(int));
    int *output = aligned_malloc(N0 * N1 * sizeof(int));

    // Fill with data
    for (int i = 0; i < N0 * M_mat; i++) {
        mat1[i] = 1;
    }
    for (int i = 0; i < M_mat * N1; i++) {
        mat2[i] = 1;
    }

    // Profile multiplication
    uint64_t count1;
    uint64_t count2;

    count1 = get_counter();
    // Query multiplication
    CPU_multiply(mat1, mat2, N0, M_mat, N1, output);
    count2 = get_counter();
    printf("...CPU Matmul takes %"PRIu64" clock cycles...\n", count2 - count1);

    aligned_free(mat1);
    aligned_free(mat2);
    aligned_free(output);
}

int main(int argc, char * argv[])
{
    int i;
    int n;
    int ndev;
    struct esp_device dev, coh_dev;
    unsigned done;
    token_t *mem;
    native_t *gold;
    unsigned errors = 0;
    unsigned coherence;
    unsigned data = 0, data_read = 0;

    // base address
    unsigned mask_rd_base;
    unsigned input_rd_base;
    unsigned aux_rd_base;
    unsigned mask_wr_base;
    unsigned input_wr_base;

    int num_interrupts = 0;

    // base address
    unsigned w_rd_base;
    unsigned d_rd_base;
    unsigned d_wr_base;

    // non const parameters
    data += (32 - 1);  // M_1
    data += IsRelu << 8;
    data += BiasShift << 16;
    data += AccumShift << 20;
    data += AccumMul << 24;

    // offsets in data buffer

    in_len1 = 32;    // bias
    in_len2 = 32*32; // weight
    in_len3 = 32*32; // activation
    out_len = 32*32;

    in_offset1 = 0;
    in_offset2 = in_offset1 + in_len1;
    in_offset3 = in_offset2 + in_len2;
    out_offset  = in_offset3 + in_len3;

    in_size1 = (in_len1 + in_len2) * sizeof(token_t);
    in_size2 = (in_len3) * sizeof(token_t);
    out_size = out_len * sizeof(token_t);

    mem_size = (out_offset * sizeof(token_t)) + out_size;

    dev.addr = ACC_ADDR;

    // TO MODIFY: Allocate memory
    // Allocation of the accelerator data array (mem) and of the expected output array (gold)
    mem = aligned_malloc(mem_size);
    gold = aligned_malloc(out_size);

    w_rd_base = ((unsigned) mem);
    d_rd_base = ((unsigned) mem) + in_size1;
    d_wr_base = ((unsigned) mem) + in_size1 + in_size2;

    printf("  Generate input...\n");

    init_buf(&mem[in_offset1], &mem[in_offset2], &mem[in_offset3], gold);

    // Flush (customize coherence model here)
    coherence = ACC_COH_RECALL;
    coh_dev.addr = CSR_TILE_ADDR;
    iowrite32(&coh_dev, CSR_REG_OFFSET*4, coherence);
    if (coherence != ACC_COH_RECALL)
	esp_flush(coherence);

    // Write the accelerator configuration registers
    iowrite32(&dev, SA_CONFIC, data);
    iowrite32(&dev, SA_W_RD_BASE, w_rd_base);
    iowrite32(&dev, SA_D_RD_BASE, d_rd_base);
    iowrite32(&dev, SA_D_WR_BASE, d_wr_base);

    iowrite32(&dev, SA_START, MDR);
    iointerrupt();

    iowrite32(&dev, SA_START, MWR);
    iointerrupt();

    iowrite32(&dev, SA_START, START);
    iointerrupt();

    iowrite32(&dev, SA_START, MDW);
    iointerrupt();

    /* Validation */
    errors = validate_buf(&mem[out_offset], gold);

    if (errors)
	printf("  ... FAIL\n");
    else
	printf("  ... PASS\n");

    aligned_free(mem);
    aligned_free(gold);

    // CPU profiling
    cpu_matmul();

    return 0;
}
