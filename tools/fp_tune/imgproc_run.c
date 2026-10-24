// Esegue la pipeline immagine REALE del driver (goodix_imgproc.c) su frame
// 16-bit dumpati (GOODIX_DUMP_IMGPROC: imgproc-<seq>-raw16/base16/pre16.pgm),
// cosi' il tuning offline usa esattamente il codice che gira in fprintd.
// I parametri si scelgono con le stesse variabili d'ambiente del driver
// (GOODIX_IMGPROC_MODE=ridge, GOODIX_IMGPROC_DEGHOST=1, ...).
//
// Uso: imgproc_run <raw16.pgm> <base16.pgm> <out8.pgm> [fresh16.pgm]
//   con fresh16 e GOODIX_IMGPROC_FRESH_BASE=1 la differenza usa il frame fresco.
// Stampa su stdout: "dc ridge" del frame (stesse statistiche loggate dal driver).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "goodix.h"

const char *gx_state_dir(void) { return "."; }
int gx_state_dir_ensure(void) { return 0; }

// PGM P5 16-bit (maxval > 255, big-endian) -> buffer 16-bit LE come nel driver.
static int load16(const char *path, uint8_t *dst, int *w, int *h)
{
    FILE *f = fopen(path, "rb");
    char magic[3] = { 0 };
    int maxval;
    if (!f)
        return -1;
    if (fscanf(f, "%2s %d %d %d", magic, w, h, &maxval) != 4 ||
        strcmp(magic, "P5") || maxval < 256 || (*w) * (*h) * 2 > GF_IMG_MAX) {
        fclose(f);
        return -1;
    }
    fgetc(f);
    for (int i = 0; i < (*w) * (*h); i++) {
        int hi = fgetc(f), lo = fgetc(f);
        if (hi < 0 || lo < 0) {
            fclose(f);
            return -1;
        }
        dst[2 * i] = (uint8_t)lo;
        dst[2 * i + 1] = (uint8_t)hi;
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    static struct goodix_dev d;
    static uint8_t raw[GF_IMG_MAX];
    uint8_t *out;
    struct gx_imgproc_params p = GX_IMGPROC_SIGFM_PARAMS;
    struct gx_imgproc_signal sig;
    int w, h, bw, bh;
    FILE *f;

    if (argc < 4) {
        fprintf(stderr, "uso: %s <raw16.pgm> <base16.pgm> <out8.pgm> [fresh16.pgm]\n", argv[0]);
        return 1;
    }
    if (load16(argv[1], raw, &w, &h) || load16(argv[2], d.img_base, &bw, &bh) ||
        w != bw || h != bh) {
        fprintf(stderr, "input non valido\n");
        return 1;
    }
    d.img_w = (uint16_t)w;
    d.img_h = (uint16_t)h;
    d.img_size = (uint16_t)(w * h * 2);
    d.img_base_valid = true;
    if (argc > 4) {
        int fw, fh;
        if (load16(argv[4], d.img_fresh, &fw, &fh) || fw != w || fh != h) {
            fprintf(stderr, "fresh non valido\n");
            return 1;
        }
        d.img_fresh_valid = true;
    }
    gx_imgproc_apply_env(&p);
    out = malloc((size_t)w * h);
    if (!out || gx_imgproc_to8bit(&d, &p, raw, out) != 0) {
        fprintf(stderr, "pipeline fallita\n");
        return 1;
    }
    if (gx_imgproc_signal(&d, &p, raw, &sig) == 0)
        printf("%.1f %.2f\n", sig.dc, sig.ridge);
    f = fopen(argv[3], "wb");
    if (!f)
        return 1;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(out, 1, (size_t)w * h, f);
    fclose(f);
    free(out);
    return 0;
}
