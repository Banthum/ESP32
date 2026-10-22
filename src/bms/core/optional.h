// "float | None" uit de Python-code: een waarde met eigen geldigheidsbit.
// Arrays hiervan houden de per-element-null-semantiek van docs/ipc_protocol.md
// (bv. expected_consumption_curve: elk kwartier onafhankelijk null).
#pragma once

#include <stdbool.h>

typedef struct {
    bool valid;
    double v;
} opt_d;

static inline opt_d opt_none(void)
{
    opt_d o = {false, 0.0};
    return o;
}

static inline opt_d opt_some(double v)
{
    opt_d o = {true, v};
    return o;
}

static inline void opt_fill_none(opt_d * arr, int n)
{
    for(int i = 0; i < n; i++) {
        arr[i] = opt_none();
    }
}
