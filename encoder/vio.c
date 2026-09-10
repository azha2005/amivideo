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
#  define POPEN  _popen
#  define PCLOSE _pclose
#else
#  define POPEN  popen
#  define PCLOSE pclose
#endif

static FILE *open_pipe(const char *cmd, const char *mode)
{
    FILE *f;
#ifdef _WIN32
    size_t n = strlen(cmd);
    char *wrapped = malloc(n + 3);
    if (!wrapped) return NULL;
    wrapped[0] = '"';
    memcpy(wrapped + 1, cmd, n);
    wrapped[n + 1] = '"';
    wrapped[n + 2] = 0;
    f = POPEN(wrapped, mode);
    free(wrapped);
#else
    f = POPEN(cmd, mode);
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
                 "[1:a]asetrate=%d*%.9f,aresample=%d,"
                 "aformat=channel_layouts=mono[a]\" ",
                 w * scale, h * scale, audio_rate, audio_ratio, audio_rate);
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
