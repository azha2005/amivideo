/* gcc -std=c11 -O2 -Wall -Wextra -pedantic tools/test_rle.c
 *     encoder/stream.c -o work/test_rle.exe -lm */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../encoder/stream.h"

static unsigned seed = 12345;
static unsigned random_byte(void)
{
    seed = seed * 1664525u + 1013904223u;
    return seed >> 24;
}

static void planarize(const uint8_t *indices, uint8_t *fb, int planes)
{
    uint8_t row[A5_MAX_PLANES * A5_ROWBYTES];
    int y, p;
    for (y = 0; y < A5_H; y++) {
        a5_planarize_row(indices + y * A5_W, planes, row);
        for (p = 0; p < planes; p++)
            memcpy(fb + (p * A5_H + y) * A5_ROWBYTES,
                   row + p * A5_ROWBYTES, A5_ROWBYTES);
    }
}

int main(void)
{
    uint8_t hidden[A5_W * A5_H], target[A5_W * A5_H];
    uint8_t fb[A5_MAX_PLANES * A5_H * A5_ROWBYTES], expected[sizeof fb];
    int planes, trial, selected = 0;
    for (planes = 1; planes <= 5; planes++) {
        for (trial = 0; trial < 100; trial++) {
            A5Buf raw, packed;
            A5DeltaStats before, after, decoded;
            const uint8_t *end;
            int y, x;
            for (y = 0; y < A5_H; y++) {
                unsigned color = random_byte() & ((1u << planes) - 1);
                for (x = 0; x < A5_W; x++) {
                    int i = y * A5_W + x;
                    hidden[i] = (uint8_t)(random_byte() & ((1u << planes) - 1));
                    target[i] = trial % 5 == 0 ? hidden[i]
                        : trial % 5 == 1 ? (uint8_t)color
                        : trial % 5 == 2 && x < 120 ? (uint8_t)color
                        : trial % 5 == 3 && x < 144 ? hidden[i]
                        : (uint8_t)(random_byte() & ((1u << planes) - 1));
                }
            }
            a5buf_init(&raw); a5buf_init(&packed);
            a5_delta_encode(&raw, hidden, target, planes, &before);
            a5_delta_encode_rle(&packed, hidden, target, planes, &after);
            assert(packed.len <= raw.len);
            assert(after.cycles <= before.cycles);
            planarize(hidden, fb, planes); planarize(target, expected, planes);
            end = a5_delta_apply(fb, packed.p, packed.p + packed.len, planes, &decoded);
            assert(end);
            if (after.rle_rows) {
                const uint8_t *truncated;
                uint8_t scratch[sizeof fb];
                memcpy(scratch, fb, sizeof fb);
                truncated = a5_rle_rows_apply(scratch, end, packed.p + packed.len - 1, planes, &decoded);
                assert(!truncated);
                /* Restore stats after the deliberately incomplete decode. */
                end = a5_delta_apply(fb, packed.p, packed.p + packed.len, planes, &decoded);
                end = a5_rle_rows_apply(fb, end, packed.p + packed.len, planes, &decoded);
                assert(end); selected++;
            }
            assert(end == packed.p + packed.len);
            assert(decoded.cycles == after.cycles);
            assert(decoded.rows == after.rows && decoded.cols == after.cols);
            assert(!memcmp(fb, expected, planes * A5_H * A5_ROWBYTES));
            a5buf_free(&raw); a5buf_free(&packed);
        }
    }
    {
        A5DeltaStats st = {0};
        const uint8_t invalid[][4] = {
            {129, 0, 0, 0}, /* too many rows */
            {1, 128, 145, 0}, /* invalid Y */
            {1, 0, 146, 0}, /* run expands to 21 bytes */
            {1, 0, 20, 0} /* literal expands to 21 bytes */
        };
        size_t i;
        for (i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
            assert(!a5_rle_rows_apply(fb, invalid[i], invalid[i] + 4, 1, &st));
        {
            const uint8_t duplicate[] = {2, 7, 145, 0, 7, 145, 1};
            const uint8_t reversed[] = {2, 7, 145, 0, 6, 145, 1};
            assert(!a5_rle_rows_apply(fb, duplicate, duplicate + sizeof duplicate, 1, &st));
            assert(!a5_rle_rows_apply(fb, reversed, reversed + sizeof reversed, 1, &st));
        }
    }
    assert(selected > 0);
    printf("500 round trips (1..5 planes), %d RLE deltas, malformed inputs rejected\n", selected);
    return 0;
}
