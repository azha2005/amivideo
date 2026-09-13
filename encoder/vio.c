/* vio.c - hablar con ffmpeg/ffprobe por pipes.
 *
 * Dos trampas de Windows, las dos costaron tiempo en su momento:
 *
 *  1. El pipe se abre en modo binario ("rb"/"wb"). En modo texto se traducen
 *     los 0x0A a 0x0D 0x0A y los frames quedan corridos.
 *  2. _popen lanza `cmd.exe /c <comando>`, y cmd.exe se come las comillas.
 *     Si el comando lleva rutas entrecomilladas hay que envolver todo el
 *     comando en OTRO par de comillas para que cmd deje las de adentro.
 */
#include <stdlib.h>
#include <string.h>
#include "a500vp.h"

#ifdef _WIN32
#  include <io.h>
#  include <fcntl.h>
#  include <wchar.h>
#  include <windows.h>
#  define PCLOSE _pclose
#else
#  define PCLOSE pclose
#endif

/* --- nombres de archivo con Unicode ---------------------------------------
 * yt-dlp y compania ponen en los nombres caracteres como "｜" (U+FF5C). En
 * Windows, argv y _popen/fopen usan la pagina de codigos ANSI y esos
 * caracteres llegan como "?": ffprobe no encontraba el archivo. Por dentro
 * todo va en UTF-8, y en el borde con Windows se pasa a UTF-16. */
#ifdef _WIN32
static wchar_t *to_wide(const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}
#endif

char **a5_utf8_args(int *argc, char **argv)
{
#ifdef _WIN32
    int n, i;
    LPWSTR *w = CommandLineToArgvW(GetCommandLineW(), &n);
    char **out;
    if (!w) return argv;
    out = calloc((size_t)n + 1, sizeof *out);
    if (!out) return argv;
    for (i = 0; i < n; i++) {
        int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, NULL, 0, NULL,
                                      NULL);
        out[i] = malloc(len > 0 ? (size_t)len : 1);
        if (!out[i]) return argv;
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, out[i], len, NULL, NULL);
    }
    LocalFree(w);
    *argc = n;
    return out;
#else
    (void)argc;
    return argv;
#endif
}

FILE *a5_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t *wp = to_wide(path), *wm = to_wide(mode);
    FILE *f = wp && wm ? _wfopen(wp, wm) : NULL;
    free(wp); free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

static FILE *open_pipe(const char *cmd, const char *mode);

/* Lanza un comando (con su propia redireccion) y devuelve el pipe: cerrarlo
 * con a5_pclose espera a que termine. Para correr varios en paralelo. */
FILE *a5_spawn(const char *cmd)
{
    return open_pipe(cmd, "r");
}

/* Ruta del ejecutable propio, en UTF-8. */
void a5_self_path(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
    wchar_t w[1024];
    DWORD n = GetModuleFileNameW(NULL, w, 1024);
    if (n > 0 && n < 1024 &&
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)size, NULL, NULL) > 0)
        return;
#endif
    snprintf(buf, size, "%s", argv0);
}

static FILE *open_pipe(const char *cmd, const char *mode)
{
    FILE *f;
#ifdef _WIN32
    size_t n = strlen(cmd);
    char *wrapped = malloc(n + 3);
    wchar_t *wc, *wm;
    if (!wrapped) return NULL;
    wrapped[0] = '"';
    memcpy(wrapped + 1, cmd, n);
    wrapped[n + 1] = '"';
    wrapped[n + 2] = 0;
    wc = to_wide(wrapped);
    wm = to_wide(mode);
    f = wc && wm ? _wpopen(wc, wm) : NULL;
    free(wrapped); free(wc); free(wm);
#else
    f = popen(cmd, mode);
#endif
    return f;
}

int a5_pclose(FILE *f)
{
    return f ? PCLOSE(f) : -1;
}

/* ------------------------------------------------------------------ */

static double parse_ratio(const char *s)
{
    double num = 0, den = 1;
    const char *slash = strchr(s, '/');

    num = atof(s);
    if (slash) den = atof(slash + 1);
    return den != 0 ? num / den : 0;
}

int a5_probe(const char *path, A5SourceInfo *out)
{
    char cmd[4096];
    char line[512];
    FILE *f;

    memset(out, 0, sizeof *out);
    out->progressive = 1;

    snprintf(cmd, sizeof cmd,
             "ffprobe -v error"
             " -show_entries stream=codec_type,width,height,r_frame_rate,"
             "field_order,sample_rate,channels"
             " -show_entries format=duration"
             " -of default=noprint_wrappers=1 \"%s\"", path);

    f = open_pipe(cmd, "r");
    if (!f) return -1;

    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        char *key = line, *val;
        if (!eq) continue;
        *eq = 0;
        val = eq + 1;
        val[strcspn(val, "\r\n")] = 0;

        if (!strcmp(key, "width") && !out->width)        out->width = atoi(val);
        else if (!strcmp(key, "height") && !out->height) out->height = atoi(val);
        else if (!strcmp(key, "r_frame_rate") && !out->fps)
            out->fps = parse_ratio(val);
        else if (!strcmp(key, "field_order"))
            out->progressive = (!strcmp(val, "progressive") ||
                                !strcmp(val, "unknown"));
        else if (!strcmp(key, "sample_rate") && !out->audio_rate)
            out->audio_rate = atoi(val);
        else if (!strcmp(key, "channels") && !out->audio_channels)
            out->audio_channels = atoi(val);
        else if (!strcmp(key, "duration") && out->duration == 0)
            out->duration = atof(val);
    }
    a5_pclose(f);

    return (out->width > 0 && out->height > 0 && out->fps > 0) ? 0 : -1;
}

FILE *a5_open_decoder(const char *path, double start, double duration,
                      const char *vfilter, int w, int h)
{
    char cmd[8192];
    char sspart[64] = "", tpart[64] = "";
    FILE *f;

    (void)w; (void)h;   /* el tamano lo fija el filtro */

    if (start > 0)    snprintf(sspart, sizeof sspart, "-ss %.6f ", start);
    if (duration > 0) snprintf(tpart, sizeof tpart, "-t %.6f ", duration);

    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -nostdin %s-i \"%s\" %s-vf \"%s\""
             " -f rawvideo -pix_fmt rgb24 -",
             sspart, path, tpart, vfilter);

    f = open_pipe(cmd, "rb");
    return f;
}

FILE *a5_open_audio(const char *path, double start, double duration,
                    const char *afilter, int rate, int channels)
{
    char cmd[8192];
    char sspart[64] = "", tpart[64] = "", afpart[1024] = "";

    if (start > 0)    snprintf(sspart, sizeof sspart, "-ss %.6f ", start);
    if (duration > 0) snprintf(tpart, sizeof tpart, "-t %.6f ", duration);
    if (afilter && *afilter)
        snprintf(afpart, sizeof afpart, "-af \"%s\" ", afilter);

    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -nostdin %s-i \"%s\" %s-vn -ac %d %s"
             "-f s16le -ar %d -", sspart, path, tpart, channels, afpart, rate);
    return open_pipe(cmd, "rb");
}

FILE *a5_open_preview(const char *out, int w, int h, double fps, int scale,
                      const char *audio_src, double audio_start,
                      double audio_dur, double audio_ratio, int audio_rate)
{
    char cmd[8192];
    char apart[1024] = "", filt[1024], amap[256] = "";
    FILE *f;

    if (audio_src && audio_rate > 0) {
        char ss[64] = "", t[64] = "";
        if (audio_start > 0) snprintf(ss, sizeof ss, "-ss %.6f ", audio_start);
        if (audio_dur > 0)   snprintf(t, sizeof t, "-t %.6f ", audio_dur);
        snprintf(apart, sizeof apart, "%s%s-i \"%s\" ", ss, t, audio_src);
        /* Aceleracion PAL de verdad: sube la velocidad Y el tono, como la
         * television PAL. asetrate cambia el reloj, aresample vuelve a la
         * frecuencia original. Mono, que es lo que va a sonar en la Amiga. */
        snprintf(filt, sizeof filt,
                 "-filter_complex \"[0:v]scale=%d:%d:flags=neighbor[v];"
                 "[1:a]asetrate=%d*%.9f,aresample=44100,"
                 "aformat=channel_layouts=mono[a]\" ",
                 w * scale, h * scale, audio_rate, audio_ratio);
        snprintf(amap, sizeof amap,
                 "-map \"[v]\" -map \"[a]\" -c:a aac -b:a 128k ");
    } else {
        snprintf(filt, sizeof filt, "-vf scale=%d:%d:flags=neighbor ",
                 w * scale, h * scale);
    }

    snprintf(cmd, sizeof cmd,
             "ffmpeg -v error -nostdin -y -f rawvideo -pix_fmt rgb24"
             " -s %dx%d -r %.9f -i - %s%s%s"
             "-c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p"
             " -shortest \"%s\"",
             w, h, fps, apart, filt, amap, out);

    f = open_pipe(cmd, "wb");
    return f;
}
