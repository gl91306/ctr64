/* Word matching for the VRU: see vru_rec.h.
 *
 * 1. Features, exactly as tools/vru/feats.py: pre-emphasis, 25 ms Hamming window every 10 ms,
 *    256-point FFT, 32 mel bands (80-3800 Hz), log, normalised over the utterance.
 * 2. A small convolutional phoneme model (tools/vru/train.py, ~180k weights built in from
 *    ctr64/data/vru_model.bin): every 20 ms, how likely each of 37 English phones (or a pause) is.
 * 3. Each word the game listens for becomes its phones (sound units through vru_units.h), and the
 *    best path through the model's output (CTC Viterbi) scores it; the deviance the VRU reports
 *    is how far that path falls below the best unconstrained one, per frame.
 * Runs once per utterance on the VRU's own low-priority thread (vru.c). */
#include "vru_rec.h"
#include "vru_units.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFFT 256
#define NMEL 32
#define FLO 80.0
#define FHI 3800.0

extern const unsigned char vru_model_data[], vru_model_end[]; /* vru_model.S */

/* ---- model ---- */

static struct {
    bool ok, tried;
    int C, NOUT;
    const float *c1w, *c1b, *c2w, *c2b, *dww[4], *dwb[4], *pww[4], *pwb[4], *ow, *ob;
} M;

static bool model_init(void)
{
    if (M.tried)
        return M.ok;
    M.tried = true;
    const unsigned char *d = vru_model_data;
    const size_t size = (size_t)(vru_model_end - vru_model_data);
    if (size < 16 || memcmp(d, "VRU1", 4))
        return false;
    unsigned u[3];
    memcpy(u, d + 4, 12);
    M.C = (int)u[0];
    M.NOUT = (int)u[1];
    if (u[2] != NMEL || M.NOUT != VRU_NPHONES + 1)
        return false;
    const float *p = (const float *)(d + 16), *end = (const float *)(d + size);
    const int C = M.C;
#define TAKE(dst, n) (dst = p, p += (n))
    TAKE(M.c1w, C * NMEL * 5);
    TAKE(M.c1b, C);
    TAKE(M.c2w, C * C * 5);
    TAKE(M.c2b, C);
    for (int b = 0; b < 4; b++) {
        TAKE(M.dww[b], C * 9);
        TAKE(M.dwb[b], C);
        TAKE(M.pww[b], C * C);
        TAKE(M.pwb[b], C);
    }
    TAKE(M.ow, M.NOUT * C);
    TAKE(M.ob, M.NOUT);
#undef TAKE
    M.ok = p == end;
    return M.ok;
}

/* ---- features ---- */

static void fft256(float *re, float *im)
{
    for (int i = 1, j = 0; i < NFFT; i++) { /* bit reversal */
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        const double a = -2 * M_PI / len;
        const float wr = (float)cos(a), wi = (float)sin(a);
        for (int i = 0; i < NFFT; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                const int x = i + k, y = i + k + len / 2;
                const float tr = re[y] * cr - im[y] * ci, ti = re[y] * ci + im[y] * cr;
                re[y] = re[x] - tr;
                im[y] = im[x] - ti;
                re[x] += tr;
                im[x] += ti;
                const float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static double hz2mel(double f) { return 2595.0 * log10(1.0 + f / 700.0); }
static double mel2hz(double m) { return 700.0 * (pow(10.0, m / 2595.0) - 1.0); }

/* (frames x NMEL), normalised; returns the frame count */
static int features(const int16_t *pcm, int samples, int rate, float **out)
{
    const int win = (int)lround(0.025 * rate), hop = (int)lround(0.010 * rate);
    if (samples < win || win > NFFT)
        return 0;
    const int n = 1 + (samples - win) / hop;
    float fb[NMEL][NFFT / 2 + 1];
    double pts[NMEL + 2];
    for (int i = 0; i < NMEL + 2; i++)
        pts[i] = mel2hz(hz2mel(FLO) + (hz2mel(FHI) - hz2mel(FLO)) * i / (NMEL + 1)) / rate * NFFT;
    for (int m = 0; m < NMEL; m++)
        for (int k = 0; k <= NFFT / 2; k++) {
            const double l = pts[m], c = pts[m + 1], r = pts[m + 2];
            fb[m][k] = l < k && k < c ? (float)((k - l) / (c - l)) : c <= k && k < r ? (float)((r - k) / (r - c)) : 0.0f;
        }
    float *w = malloc(sizeof(float) * win), *e = malloc(sizeof(float) * n * NMEL);
    float *x = malloc(sizeof(float) * samples);
    if (!w || !e || !x) {
        free(w);
        free(e);
        free(x);
        return 0;
    }
    for (int i = 0; i < win; i++)
        w[i] = (float)(0.54 - 0.46 * cos(2 * M_PI * i / (win - 1)));
    x[0] = pcm[0] / 32768.0f;
    for (int i = 1; i < samples; i++)
        x[i] = pcm[i] / 32768.0f - 0.97f * (pcm[i - 1] / 32768.0f);
    float re[NFFT], im[NFFT];
    for (int f = 0; f < n; f++) {
        for (int i = 0; i < NFFT; i++) {
            re[i] = i < win ? x[f * hop + i] * w[i] : 0.0f;
            im[i] = 0;
        }
        fft256(re, im);
        float pw[NFFT / 2 + 1];
        for (int k = 0; k <= NFFT / 2; k++)
            pw[k] = re[k] * re[k] + im[k] * im[k];
        for (int m = 0; m < NMEL; m++) {
            float s = 0;
            for (int k = 0; k <= NFFT / 2; k++)
                s += pw[k] * fb[m][k];
            e[f * NMEL + m] = logf(s + 1e-6f);
        }
    }
    for (int m = 0; m < NMEL; m++) {
        double mean = 0;
        for (int f = 0; f < n; f++)
            mean += e[f * NMEL + m];
        mean /= n;
        for (int f = 0; f < n; f++)
            e[f * NMEL + m] -= (float)mean;
    }
    double var = 0;
    for (int i = 0; i < n * NMEL; i++)
        var += (double)e[i] * e[i];
    const float inv = (float)(1.0 / (sqrt(var / (n * NMEL)) + 1e-3));
    for (int i = 0; i < n * NMEL; i++)
        e[i] *= inv;
    free(w);
    free(x);
    *out = e;
    return n;
}

/* ---- network: activations stored [channel][time] ---- */

/* out[o][t] = b[o] + sum_i sum_k w[o][i][k] * in[i][t * stride + k - pad] */
static void conv(const float *in, int ci, int tin, float *out, int co, int tout, const float *w, const float *b, int k,
                 int stride, bool relu)
{
    const int pad = k / 2;
    for (int o = 0; o < co; o++) {
        float *y = out + o * tout;
        for (int t = 0; t < tout; t++)
            y[t] = b[o];
        for (int i = 0; i < ci; i++) {
            const float *x = in + i * tin, *wk = w + (o * ci + i) * k;
            for (int j = 0; j < k; j++) {
                const float wv = wk[j];
                const int off = j - pad;
                int t0 = 0, t1 = tout;
                while (t0 < t1 && t0 * stride + off < 0)
                    t0++;
                while (t1 > t0 && (t1 - 1) * stride + off >= tin)
                    t1--;
                if (stride == 1) {
                    const float *xs = x + off;
                    for (int t = t0; t < t1; t++)
                        y[t] += wv * xs[t];
                } else {
                    for (int t = t0; t < t1; t++)
                        y[t] += wv * x[t * stride + off];
                }
            }
        }
        if (relu)
            for (int t = 0; t < tout; t++)
                y[t] = y[t] > 0 ? y[t] : 0;
    }
}

/* log probabilities (t x NOUT); returns the 20 ms frame count */
static int run_net(const float *feat, int n, float **logp_out)
{
    const int C = M.C, T2 = (n + 1) / 2;
    float *x0 = malloc(sizeof(float) * NMEL * n), *a = malloc(sizeof(float) * C * n);
    float *b = malloc(sizeof(float) * C * T2), *c = malloc(sizeof(float) * C * T2);
    float *lp = malloc(sizeof(float) * M.NOUT * T2);
    if (!x0 || !a || !b || !c || !lp) {
        free(x0);
        free(a);
        free(b);
        free(c);
        free(lp);
        return 0;
    }
    for (int t = 0; t < n; t++)
        for (int m = 0; m < NMEL; m++)
            x0[m * n + t] = feat[t * NMEL + m];
    conv(x0, NMEL, n, a, C, n, M.c1w, M.c1b, 5, 1, true);
    conv(a, C, n, b, C, T2, M.c2w, M.c2b, 5, 2, true);
    for (int blk = 0; blk < 4; blk++) {
        /* depthwise kernel 9 into a (as C x T2), pointwise into c, relu, residual into b */
        for (int ch = 0; ch < C; ch++) {
            const float *xi = b + ch * T2, *wk = M.dww[blk] + ch * 9;
            float *yo = a + ch * T2;
            for (int t = 0; t < T2; t++) {
                float s = M.dwb[blk][ch];
                for (int j = 0; j < 9; j++) {
                    const int tt = t + j - 4;
                    if (tt >= 0 && tt < T2)
                        s += wk[j] * xi[tt];
                }
                yo[t] = s;
            }
        }
        conv(a, C, T2, c, C, T2, M.pww[blk], M.pwb[blk], 1, 1, true);
        for (int i = 0; i < C * T2; i++)
            b[i] += c[i];
    }
    conv(b, C, T2, a, M.NOUT, T2, M.ow, M.ob, 1, 1, false);
    for (int t = 0; t < T2; t++) { /* log softmax, transposed to [t][k] */
        float mx = -1e30f;
        for (int k = 0; k < M.NOUT; k++)
            mx = a[k * T2 + t] > mx ? a[k * T2 + t] : mx;
        float s = 0;
        for (int k = 0; k < M.NOUT; k++)
            s += expf(a[k * T2 + t] - mx);
        const float l = mx + logf(s);
        for (int k = 0; k < M.NOUT; k++)
            lp[t * M.NOUT + k] = a[k * T2 + t] - l;
    }
    free(x0);
    free(a);
    free(b);
    free(c);
    *logp_out = lp;
    return T2;
}

/* ---- words ---- */

/* Densha de GO! 64's words: Shift-JIS hiragana/katakana and the long mark (one character per
 * 16-bit unit), read as romaji and then as the model's (English) phones: vowels a i u e o =
 * AA IY UW EH OW, "r" = R, "ts" = T S, n = N. Small ya/yu/yo after an i-syllable make kya, sha;
 * small vowels replace the vowel before (fa, ti); the small tsu (a doubled consonant) and the long
 * mark add no phone. */
static const char *const k_hiragana[83] = { /* 0x829F-0x82F1 */
    "_a", "a", "_i", "i", "_u", "u", "_e", "e", "_o", "o",
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",
    "ta", "da", "chi", "ji", "_", "tsu", "zu", "te", "de", "to", "do",
    "na", "ni", "nu", "ne", "no",
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
    "ma", "mi", "mu", "me", "mo",
    "_ya", "ya", "_yu", "yu", "_yo", "yo",
    "ra", "ri", "ru", "re", "ro",
    "_wa", "wa", "i", "e", "o", "n"};

static unsigned char phone_id(const char *name)
{
    for (int i = 1; i <= VRU_NPHONES; i++)
        if (!strcmp(vru_phone_names[i], name))
            return (unsigned char)i;
    return 0;
}

static unsigned char vowel_phone(char v)
{
    static const char *const names[] = {"AA", "IY", "UW", "EH", "OW"};
    const char *p = strchr("aiueo", v);
    return p && v ? phone_id(names[p - "aiueo"]) : 0;
}

/* the consonant letters of a romaji syllable -> phones (up to 2) */
static int consonant_phones(const char *c, int len, unsigned char *out)
{
    static const struct {
        const char *r, *p1, *p2;
    } map[] = {{"k", "K", 0},   {"g", "G", 0},  {"s", "S", 0},  {"z", "Z", 0},  {"sh", "SH", 0}, {"j", "JH", 0},
               {"t", "T", 0},   {"ch", "CH", 0}, {"ts", "T", "S"}, {"d", "D", 0}, {"n", "N", 0},   {"h", "HH", 0},
               {"f", "F", 0},   {"b", "B", 0},  {"p", "P", 0},  {"m", "M", 0},  {"y", "Y", 0},   {"r", "R", 0},
               {"w", "W", 0},   {"v", "V", 0}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if ((int)strlen(map[i].r) == len && !strncmp(map[i].r, c, len)) {
            out[0] = phone_id(map[i].p1);
            if (!map[i].p2)
                return 1;
            out[1] = phone_id(map[i].p2);
            return 2;
        }
    return 0;
}

/* one 16-bit unit -> its romaji (NULL: not kana), "-" for the long mark */
static const char *kana_romaji(unsigned u)
{
    if ((u & 0xFF) >= 0x81 && (u & 0xFF) <= 0x83 && (u >> 8) >= 0x40) /* other byte order */
        u = (u >> 8) | (u & 0xFF) << 8;
    if (u == 0x815B)
        return "-";
    if (u >= 0x829F && u <= 0x82F1)
        return k_hiragana[u - 0x829F];
    if (u >= 0x8340 && u <= 0x8396 && u != 0x837F) {
        const unsigned i = u - 0x8340 - (u > 0x837F);
        return i < 83 ? k_hiragana[i] : i == 83 ? "vu" : i == 84 ? "ka" : "ke";
    }
    return NULL;
}

static int kana_phones(const VruWord *w, unsigned char *ph, int max)
{
    int n = 0;
    int vowel_at = -1;      /* where the last syllable's vowel is in ph, -1: none */
    unsigned char cons = 0; /* the last syllable's (last) consonant phone */
    const unsigned char IY = phone_id("IY"), SH = phone_id("SH"), CH = phone_id("CH"), JH = phone_id("JH");
    for (int i = 0; i < w->n && n + 3 < max; i++) {
        const char *r = kana_romaji(w->unit[i]);
        if (!r || !strcmp(r, "-") || !strcmp(r, "_"))
            continue;
        if (r[0] == '_') { /* small kana */
            r++;
            if (vowel_at < 0)
                continue;
            if (r[0] == 'y') { /* kya, sha: the i goes */
                const unsigned char v = vowel_phone(r[1]);
                if (ph[vowel_at] == IY) {
                    n = vowel_at;
                    if (cons != SH && cons != CH && cons != JH)
                        ph[n++] = phone_id("Y");
                    vowel_at = n;
                    ph[n++] = v;
                }
            } else if (r[0] != 'w') { /* fa, ti, we: the vowel is replaced */
                if (cons)
                    ph[vowel_at] = vowel_phone(r[0]);
                else { /* u + small i = wi */
                    ph[vowel_at] = phone_id("W");
                    vowel_at = ++n - 1;
                    ph[vowel_at] = vowel_phone(r[0]);
                }
            }
            continue;
        }
        if (!strcmp(r, "n")) {
            ph[n++] = phone_id("N");
            vowel_at = -1;
            cons = 0;
            continue;
        }
        const int len = (int)strlen(r) - 1;
        unsigned char c[2];
        const int nc = consonant_phones(r, len, c);
        for (int k = 0; k < nc; k++)
            ph[n++] = c[k];
        cons = nc ? c[nc - 1] : 0;
        vowel_at = n;
        ph[n++] = vowel_phone(r[len]);
    }
    /* no two same phones in a row (a CTC path cannot tell them apart without a blank) */
    int m = 0;
    for (int i = 0; i < n; i++)
        if (ph[i] && (!m || ph[m - 1] != ph[i]))
            ph[m++] = ph[i];
    return m;
}

static int word_phones(const VruWord *w, unsigned char *ph, int max)
{
    int n = 0;
    if (w->japanese)
        return kana_phones(w, ph, max);
    for (int i = 0; i < w->n; i++) {
        const unsigned u = w->unit[i];
        if (u % 3 || u / 3 >= VRU_NUNITS)
            continue;
        for (int j = 0; j < 2; j++) {
            const unsigned char p = vru_unit_phones[u / 3][j];
            if (p && (!n || ph[n - 1] != p) && n < max)
                ph[n++] = p;
        }
    }
    return n;
}

/* best CTC path log probability of the phones (blank between and around, no repeats) */
static float ctc_viterbi(const float *lp, int T, int nout, const unsigned char *ph, int L)
{
    const int S = 2 * L + 1;
    float a[2 * 64 + 1], nb[2 * 64 + 1];
    const float NEG = -1e30f;
    for (int s = 0; s < S; s++)
        a[s] = NEG;
    a[0] = lp[0];
    if (S > 1)
        a[1] = lp[ph[0]];
    for (int t = 1; t < T; t++) {
        const float *l = lp + t * nout;
        for (int s = 0; s < S; s++) {
            float m = a[s];
            if (s >= 1 && a[s - 1] > m)
                m = a[s - 1];
            if (s >= 2 && (s & 1) && ph[s / 2] != ph[s / 2 - 1] && a[s - 2] > m)
                m = a[s - 2];
            nb[s] = m + l[(s & 1) ? ph[s / 2] : 0];
        }
        memcpy(a, nb, sizeof(float) * S);
    }
    return S > 1 && a[S - 2] > a[S - 1] ? a[S - 2] : a[S - 1];
}

/* ---- levels ---- */

static uint16_t level_of(double rms)
{
    double v = rms * 0.6; /* ~0xBB8 for normal speech near full scale */
    return (uint16_t)(v > 0xFFF ? 0xFFF : v);
}

int vru_rec_debug; /* vru.c: phone strings and scores to vru.log */
void vru_rec_log(const char *fmt, ...); /* vru.c */

void vru_recognize(const int16_t *pcm, int samples, int rate, const VruWord *words, int nwords, VruResult *out)
{
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < 10; i += 2)
        out->match[i] = 0x7FFF;

    /* levels: 20 ms frames; the loudest tenth is the voice (a short word in a longer button press),
     * the quietest tenth the room */
    const int frame = rate / 50;
    int n = samples / frame;
    if (n > 400)
        n = 400;
    float e[400];
    for (int f = 0; f < n; f++) {
        double s = 0;
        for (int i = 0; i < frame; i++) {
            const double x = pcm[f * frame + i];
            s += x * x;
        }
        e[f] = (float)sqrt(s / frame);
    }
    float sorted[400];
    memcpy(sorted, e, sizeof(float) * n);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
            const float t = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = t;
        }
    const double room = n ? sorted[n / 10] : 0, voice = n ? sorted[n - 1 - n / 10] : 0;
    int voiced = 0;
    for (int i = 0; i < n; i++)
        voiced += e[i] > room * 3 + 200;
    out->mic_level = level_of(voice);
    out->voice_level = level_of(voice - room);
    out->voice_length = (uint16_t)(voiced * 20);
    if (!n || voice < 150) {
        out->error_flags = 0x0400; /* voice too low */
        return;
    }

    if (!model_init()) {
        out->error_flags = 0x4000;
        return;
    }
    float *feat = NULL, *lp = NULL;
    const int nf = features(pcm, samples, rate, &feat);
    const int T = nf ? run_net(feat, nf, &lp) : 0;
    free(feat);
    if (!T) {
        out->error_flags = 0x4000;
        return;
    }
    float best = 0; /* best unconstrained path */
    for (int t = 0; t < T; t++) {
        float m = -1e30f;
        for (int k = 0; k < M.NOUT; k++)
            m = lp[t * M.NOUT + k] > m ? lp[t * M.NOUT + k] : m;
        best += m;
    }
    if (vru_rec_debug) { /* what the model heard (greedy) */
        char s[512];
        int k = 0, prev = 0;
        for (int t = 0; t < T && k < 480; t++) {
            int arg = 0;
            for (int j = 1; j < M.NOUT; j++)
                if (lp[t * M.NOUT + j] > lp[t * M.NOUT + arg])
                    arg = j;
            if (arg && arg != prev)
                k += snprintf(s + k, sizeof(s) - k, "%s ", vru_phone_names[arg]);
            prev = arg;
        }
        s[k] = 0;
        vru_rec_log("vru: heard %s(%d frames)", s, T);
    }
    /* top 5 by deviance */
    int top[5];
    float topd[5];
    int ntop = 0;
    for (int i = 0; i < nwords; i++) {
        unsigned char ph[64];
        const int L = word_phones(&words[i], ph, 64);
        if (!L || 2 * L + 1 > T * 2 + 1)
            continue;
        const float dev = (best - ctc_viterbi(lp, T, M.NOUT, ph, L)) / T;
        int pos = ntop;
        while (pos > 0 && topd[pos - 1] > dev)
            pos--;
        if (pos >= 5)
            continue;
        for (int j = (ntop < 5 ? ntop : 4); j > pos; j--) {
            top[j] = top[j - 1];
            topd[j] = topd[j - 1];
        }
        top[pos] = i;
        topd[pos] = dev;
        if (ntop < 5)
            ntop++;
    }
    free(lp);
    for (int i = 0; i < ntop; i++) {
        /* games accept a word under 0x640 (1600). Right words score 0-0.18 here and unrelated speech
         * 0.23-0.53 (synthetic voices, tools/vru): x6000 puts the line near 0.27, so unrelated speech is
         * "not understood" instead of taken for the nearest word */
        float d = topd[i] * 6000.0f;
        out->match[i * 2] = (uint16_t)top[i];
        out->match[i * 2 + 1] = (uint16_t)(d > 0x7FFF ? 0x7FFF : d);
        if (vru_rec_debug)
            vru_rec_log("vru: hit %d: word %d deviance %04X", i, top[i], out->match[i * 2 + 1]);
    }
    out->num_results = (uint16_t)ntop;
    if (!ntop || out->match[1] > 0x640)
        out->error_flags |= 0x4000;
}
