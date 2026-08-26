/**
 * @file rknn_x86_stub.cpp - RKNN API stub (x86 desktop dev only)
 *
 * All functions return errors so the desktop can compile/link and show the UI.
 * Real NPU inference runs on the RK3568 device; this stub is desktop-only.
 */
#include "rknn_api.h"
#include <cstdio>

int rknn_init(rknn_context* ctx, void* model, uint32_t size, uint32_t flag, rknn_init_extend* extend) {
    (void)model; (void)size; (void)flag; (void)extend;
    fprintf(stderr, "[RKNN_STUB] rknn_init called (x86 stub)\n");
    *ctx = 0;
    return -1;
}

int rknn_destroy(rknn_context ctx) {
    (void)ctx;
    return -1;
}

int rknn_query(rknn_context ctx, rknn_query_cmd cmd, void* info, uint32_t size) {
    (void)ctx; (void)cmd; (void)info; (void)size;
    return -1;
}

int rknn_inputs_set(rknn_context ctx, uint32_t n_inputs, rknn_input inputs[]) {
    (void)ctx; (void)n_inputs; (void)inputs;
    return -1;
}

int rknn_run(rknn_context ctx, rknn_run_extend* extend) {
    (void)ctx; (void)extend;
    return -1;
}

int rknn_outputs_get(rknn_context ctx, uint32_t n_outputs, rknn_output outputs[], rknn_output_extend* extend) {
    (void)ctx; (void)n_outputs; (void)outputs; (void)extend;
    return -1;
}

int rknn_outputs_release(rknn_context ctx, uint32_t n_outputs, rknn_output outputs[]) {
    (void)ctx; (void)n_outputs; (void)outputs;
    return -1;
}

int rknn_set_core_mask(rknn_context ctx, rknn_core_mask mask) {
    (void)ctx; (void)mask;
    return -1;
}
