// conv_dec.c allocates its SSE Viterbi buffers with memalign(16, n) and frees
// them with free(). 64-bit Windows' malloc already returns 16-byte aligned
// memory, and _aligned_malloc's blocks could not be released with free().
#pragma once
#include <stdlib.h>
#define memalign(align, size) (((align) <= 16) ? malloc(size) : NULL)
