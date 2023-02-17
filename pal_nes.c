/*****************************************************************************/
/*
 * PAL/CRT - integer-only PAL video signal encoding / decoding emulation
 *
 *   by EMMIR 2018-2023
 *
 *   GitHub : https://github.com/LMP88959/PAL-CRT
 *   YouTube: https://www.youtube.com/@EMMIR_KC/videos
 *   Discord: https://discord.com/invite/hdYctSmyQJ
 */
/*****************************************************************************/

#include "pal_core.h"

#if (PAL_SYSTEM == PAL_SYSTEM_NES)
#include <stdlib.h>
#include <string.h>

/* generate the square wave for a given 9-bit pixel and phase
 * NOTE: in this PAL version, the red and green emphasis bits swap meaning
 * when compared to the NTSC version
 */
static int
square_sample(int p, int phase)
{
    /* amplified IRE = ((mV / 7.143) - 312 / 7.143) * 1024 */
    /* https://www.nesdev.org/wiki/NTSC_video#Brightness_Levels */
    static int IRE[16] = {
     /* 0d     1d     2d      3d */
       -12042, 0,     34406,  81427,
     /* 0d     1d     2d      3d emphasized */
       -17203,-8028,  19497,  57342,
     /* 00     10     20      30 */
        43581, 75693, 112965, 112965,
     /* 00     10     20      30 emphasized */
        26951, 52181, 83721,  83721
    };
    static int active[6] = {
        0300, 0200,
        0600, 0400,
        0500, 0100
    };
    int hue;
    int e, l, v;

    hue = (p & 0x0f);

    /* last two columns are black */
    if (hue >= 0x0e) {
        return 0;
    }

    v = (((hue + phase) % 12) < 6);
    
    /* red 0200, green 0100, blue 0400 */
    e = (((p & 0700) & active[(phase >> 1) % 6]) > 0);
    switch (hue) {
        case 0x00: l = 1; break;
        case 0x0d: l = 0; break;
        default:   l = v; break;
    }
    return IRE[(l << 3) + (e << 2) + ((p >> 4) & 3)];
}

/* this function is an optimization
 * basically factoring out the field setup since as long as PAL_CRT->analog
 * does not get cleared, all of this should remain the same every update
 */
static void
setup_field(struct PAL_CRT *v)
{
    int n;
 
    for (n = 0; n < PAL_VRES; n++) {
        int t; /* time */
        signed char *line = &v->analog[n * PAL_HRES];
 
        t = LINE_BEG;

        if (n <= 3 || (n >= 7 && n <= 9)) {
            /* equalizing pulses - small blips of sync, mostly blank */
            while (t < (4   * PAL_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (50  * PAL_HRES / 100)) line[t++] = BLANK_LEVEL;
            while (t < (54  * PAL_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (100 * PAL_HRES / 100)) line[t++] = BLANK_LEVEL;
        } else if (n >= 4 && n <= 6) {
            int offs[4] = { 46, 50, 96, 100 };
            /* vertical sync pulse - small blips of blank, mostly sync */
            while (t < (offs[0] * PAL_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (offs[1] * PAL_HRES / 100)) line[t++] = BLANK_LEVEL;
            while (t < (offs[2] * PAL_HRES / 100)) line[t++] = SYNC_LEVEL;
            while (t < (offs[3] * PAL_HRES / 100)) line[t++] = BLANK_LEVEL;
        } else {
            /* prerender/postrender/video scanlines */
            while (t < SYNC_BEG) line[t++] = BLANK_LEVEL; /* FP */
            while (t < BW_BEG) line[t++] = SYNC_LEVEL; /* SYNC */
            while (t < PAL_HRES) line[t++] = BLANK_LEVEL;
        }
    }
}
 
extern void
pal_modulate(struct PAL_CRT *v, struct PAL_SETTINGS *s)
{
    int x, y, xo, yo;
    int destw = AV_LEN;
    int desth = PAL_LINES;
    int n, phase;
    int iccf[6][4];
    int ccburst[6][4]; /* color phase for burst */
    int bsign[6];
    int sn, cs;
        
    if (!s->field_initialized) {
        setup_field(v);
        s->field_initialized = 1;
    }
    for (y = 0; y < 6; y++) {
        int vert = y * (360 / 6);
        bsign[y] = ((y & 1) ? -1 : 1);
        for (x = 0; x < 4; x++) {
            n = vert + s->hue + x * 90 + 135;
            /* swinging burst */
            pal_sincos14(&sn, &cs, (n + bsign[y] * 60) * 8192 / 180);
            ccburst[y][x] = sn >> 10;
        }
    }

    xo = AV_BEG;
    yo = PAL_TOP;
         
    /* align signal */
    xo = (xo & ~3);
    /* no border on PAL according to https://www.nesdev.org/wiki/PAL_video */
    for (y = 0; y < desth; y++) {
        signed char *line;  
        int t, cb;
        int sy = (y * s->h) / desth;

        if (sy >= s->h) sy = s->h;
        if (sy < 0) sy = 0;
 
        n = (y + yo);
        line = &v->analog[n * PAL_HRES];
        for (t = BW_BEG; t < CB_BEG; t++) {
            line[t] = SYNC_LEVEL;
        }
        
        for (t = CB_BEG; t < CB_BEG + (CB_CYCLES * PAL_CB_FREQ); t++) {
            cb = ccburst[n % 6][t & 3];
            line[t] = (BLANK_LEVEL + (cb * BURST_LEVEL)) >> 5;
            iccf[n % 6][t & 3] = line[t];
        }
        sy *= s->w;

        phase = (n % 12) * 2;
        phase += bsign[n % 6] == 1 ? 6 : 0;
        for (x = 0; x < destw; x++) {
            int ire, p;
            
            p = s->data[((x * s->w) / destw) + sy];
            ire = BLACK_LEVEL + v->black_point;

            ire += square_sample(p, phase + 0);
            ire += square_sample(p, phase + 1);
            ire += square_sample(p, phase + 2);
            ire += square_sample(p, phase + 3);
            ire = ((ire * v->white_point / 110) >> 12);
            v->analog[(x + xo) + (y + yo) * PAL_HRES] = ire;
            phase += 3;
        }
    }
   
    for (x = 0; x < 4; x++) {
        for (n = 0; n < 6; n++) {
            v->ccf[n][x] = iccf[n][x] << 7;
        }
    }
    v->cc_period = 6;
}

#endif
