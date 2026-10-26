// Offline test bench of the SIGFM matcher (the same as libfprint goodixgf).
// Loads PGM images dumped by the driver (GOODIX_DUMP_IMGPROC, stage enh8/final8),
// computes for every test frame the MAX score against the template (= what
// fpi_print_sigfm_match does: a match if one frame exceeds the threshold), and
// sweeps the threshold printing FRR (genuine rejected) and FAR (impostors accepted).
//
// Build (see build.sh):
//   g++ -O2 sigfm_test.cpp -o sigfm_test -I<sigfm_dir> <libsigfm.a> $(pkg-config ... opencv)
// Usage:
//   ./sigfm_test <template_dir> <genuine_dir> <impostor_dir>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "sigfm.h"

// Loads an 8-bit binary P5 PGM.
static SigfmPix *load_pgm(const std::string &path, int *w, int *h) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    char magic[3] = {0};
    int maxval = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P5") != 0 ||
        fscanf(f, "%d %d %d", w, h, &maxval) != 3) {
        fclose(f);
        return nullptr;
    }
    fgetc(f); // single whitespace after maxval
    int n = (*w) * (*h);
    SigfmPix *px = (SigfmPix *)malloc(n);
    if (!px || fread(px, 1, n, f) != (size_t)n) {
        free(px);
        fclose(f);
        return nullptr;
    }
    fclose(f);
    return px;
}

static std::vector<std::string> list_stage(const char *dir, const char *stage) {
    std::vector<std::string> out;
    DIR *d = opendir(dir);
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d))) {
        std::string nm = e->d_name;
        if (nm.find(stage) != std::string::npos && nm.size() > 4 &&
            nm.substr(nm.size() - 4) == ".pgm")
            out.push_back(std::string(dir) + "/" + nm);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

static std::vector<SigfmImgInfo *> load_dir(const char *dir) {
    auto files = list_stage(dir, "-enh8");   // the real input of sigfm (enhance on)
    if (files.empty()) files = list_stage(dir, "-final8");
    std::vector<SigfmImgInfo *> infos;
    for (auto &p : files) {
        int w, h;
        SigfmPix *px = load_pgm(p, &w, &h);
        if (!px) continue;
        SigfmImgInfo *info = sigfm_extract(px, w, h);
        free(px);
        if (info) infos.push_back(info);
    }
    return infos;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <template_dir> <genuine_dir> <impostor_dir>\n", argv[0]);
        return 1;
    }
    auto tmpl = load_dir(argv[1]);
    auto gen = load_dir(argv[2]);
    auto imp = load_dir(argv[3]);
    fprintf(stderr, "template=%zu  genuine=%zu  impostor=%zu frame\n",
            tmpl.size(), gen.size(), imp.size());
    if (tmpl.empty() || (gen.empty() && imp.empty())) {
        fprintf(stderr, "not enough data\n");
        return 1;
    }

    auto max_score = [&](SigfmImgInfo *t) {
        int mx = 0;
        for (auto *f : tmpl) {
            int s = sigfm_match_score(f, t);
            if (s > mx) mx = s;
        }
        return mx;
    };

    std::vector<int> gs, is;
    for (auto *f : gen) gs.push_back(max_score(f));
    for (auto *f : imp) is.push_back(max_score(f));
    std::sort(gs.begin(), gs.end());
    std::sort(is.begin(), is.end());

    printf("GENUINE  max-score: ");
    for (int s : gs) printf("%d ", s);
    printf("\nIMPOSTOR max-score: ");
    for (int s : is) printf("%d ", s);
    printf("\n\nthresh  FRR (genuine rejected)     FAR (impostors accepted)\n");

    int imax = is.empty() ? 0 : is.back();
    int hi = std::max(imax + 25, 120);
    int best = -1;
    for (int thr = 5; thr <= hi; thr += 5) {
        int frr = 0, far = 0;
        for (int s : gs) if (s < thr) frr++;
        for (int s : is) if (s >= thr) far++;
        bool far0 = (far == 0);
        if (far0 && best < 0) best = thr;
        printf("%4d    %2d/%zu (%3.0f%%)            %2d/%zu (%3.0f%%)%s\n", thr,
               frr, gs.size(), gs.empty() ? 0.0 : 100.0 * frr / gs.size(),
               far, is.size(), is.empty() ? 0.0 : 100.0 * far / is.size(),
               far0 ? "   <= FAR 0" : "");
    }
    if (best >= 0)
        printf("\nLowest threshold with FAR=0: %d  (FRR there: %.0f%%)\n", best,
               [&] { int r = 0; for (int s : gs) if (s < best) r++;
                     return gs.empty() ? 0.0 : 100.0 * r / gs.size(); }());
    else
        printf("\nNo threshold brings FAR to 0: genuine and impostor overlap.\n");
    return 0;
}
