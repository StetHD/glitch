#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "glitch.h"
#include "tr808.h"

#include "expr.h"

static int SAMPLE_RATE = 48000;

static glitch_loader_fn loader = NULL;

#define PI 3.1415926
#define MIN(a, b) ((a) < (b) ? (a) : (b))

static float denorm(float x) { return x * 127 + 128; }
static float arg(vec_expr_t args, int n, float defval) {
  if (vec_len(&args) < n + 1) {
    return defval;
  }
  return expr_eval(&vec_nth(&args, n));
}

struct osc_context {
  float freq;
  float w;
};

struct fm_context {
  float freq;
  int sync;
  float prev;

  float w0;
  float w1;
  float w2;
  float w3;
};

typedef vec(float) vec_float_t;

struct seq_context {
  int init;
  float mul;
  int beat;
  int t;

  float value;
  vec_float_t values;
};

struct env_context {
  int init;
  vec_float_t d;
  vec_float_t e;
  int t;
  int segment;
};

struct mix_context {
  int init;
  vec_float_t values;
};

struct iir_context {
  float value;
};

struct delay_context {
  float *buf;
  unsigned int pos;
  size_t size;
  float prev;
};

struct each_context {
  int init;
  vec_expr_t args;
};

#define PITCH_BUFSZ 0x10000
#define PITCH_WRAP(i) (fmodf((i)+PITCH_BUFSZ, PITCH_BUFSZ))
#define PITCH_AT(p, i) ((p)->buf[(int) PITCH_WRAP(i)])
#define PITCH_READ(p, fi, res) \
  do { \
    int i = (int) (fi); \
    float f = fi - i; \
    float a = PITCH_AT(p, i-1); \
    float b = PITCH_AT(p, i); \
    float c = PITCH_AT(p, i+1); \
    float d = PITCH_AT(p, i+2); \
    float cb = c - b; \
    res = b + f*(cb-0.16667*(1.-f)*((d-a-3*cb)*f+(d+2*a-3*b))); \
  } while(0)
#define PITCH_FIND(p, pf, pt1, pt2, ptmax) \
  do { \
    float cmax = 0.0f; \
    ptmax = pt1; \
    for (float pt = pt1; pt < pt2 - (p)->win; pt = pt + (p)->step) { \
      float c = 0.0f; \
      for (float i = 0.0; i < (p)->win - 1; i = i + (p)->step) { \
	c = c + PITCH_AT(p, pf+i) * PITCH_AT(p, pt+i); \
      } \
      if (c > cmax) { \
	cmax = c; \
	ptmax = pt;  \
      } \
    } \
  } while (0)

struct pitch_context {
  int init;
  float buf[PITCH_BUFSZ];
  float pr;
  float prn;
  float pw;
  float nmix;
  float mix;
  float dmax;
  float win;
  float step;
};

struct sample_context {
  int t;
};

static float lib_s(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  return denorm(sin(arg(args, 0, 0) * PI / 128));
}

static float lib_r(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  return rand() * arg(args, 0, 255) / RAND_MAX;
}

static float lib_l(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  if (arg(args, 0, 0)) {
    return log2f(arg(args, 0, 0));
  }
  return 0;
}

static float lib_a(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  float index = arg(args, 0, NAN);
  if (isnan(index)) {
    return NAN;
  }
  int len = vec_len(&args) - 1;
  if (len == 0) {
    return 0;
  }
  int i = (int) floor(index + len) % len;
  i = (i + len) % len;
  return arg(args, i+1, 0);
}

static int scales[][13] = {
  {7, 0, 2, 4, 5, 7, 9, 11, 0, 0, 0, 0}, // ionian, major scale
  {7, 0, 2, 3, 5, 7, 9, 10, 0, 0, 0, 0}, // dorian
  {7, 0, 1, 3, 5, 7, 8, 10, 0, 0, 0, 0}, // phrygian
  {7, 0, 2, 4, 6, 7, 9, 11, 0, 0, 0, 0}, // lydian
  {7, 0, 2, 4, 5, 7, 9, 10, 0, 0, 0, 0}, // mixolydian
  {7, 0, 2, 3, 5, 7, 8, 10, 0, 0, 0, 0}, // aeolian, natural minor scale
  {7, 0, 1, 3, 5, 6, 8, 10, 0, 0, 0, 0}, // locrian
  {7, 0, 2, 3, 5, 7, 8, 11, 0, 0, 0, 0}, // harmonic minor scale
  {7, 0, 2, 3, 5, 7, 9, 11, 0, 0, 0, 0}, // melodic minor scale
  {5, 0, 2, 4, 7, 9, 0, 0, 0, 0, 0, 0}, // major pentatonic
  {5, 0, 3, 5, 7, 10, 0, 0, 0, 0, 0, 0}, // minor pentatonic
  {6, 0, 3, 5, 6, 7, 10, 0, 0, 0, 0}, // blues
  {6, 0, 2, 4, 6, 8, 10, 0, 0, 0, 0}, // whole tone scale
  {8, 0, 1, 3, 4, 6, 7, 9, 10, 0, 0, 0}, // octatonic
  {12, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, // chromatic is a fallback scale
};

static float lib_scale(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  float note = arg(args, 0, 0);
  float scale = arg(args, 1, 0);
  if (isnan(scale) || isnan(note)) {
    return NAN;
  }
  if (scale < 0 || scale > 14) {
    scale = 14;
  }
  int len = scales[(int) scale][0];
  int n = (int) note;
  int transpose = n / len * 12;
  n = (n + len) % len;
  n = (n + len) % len;
  return scales[(int) scale][n+1] + transpose;
}

static float lib_hz(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f; (void) context;
  return pow(2, arg(args, 0, 0) / 12) * 440;
}

static float lib_each(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct each_context *each = (struct each_context *) context;
  float r = NAN;

  if (vec_len(&args) < 3) {
    return NAN;
  }

  if (!each->init) {
    each->init = 1;
    for (int i = 0; i < vec_len(&args) - 2; i++) {
      struct expr tmp = {0};
      expr_copy(&tmp, &vec_nth(&args, 1));
      vec_push(&each->args, tmp);
    }
  }

  // List of variables
  struct expr *init = &vec_nth(&args, 0);
  float mix = 0.0f;
  for (int i = 0; i < vec_len(&each->args); i++) {
    struct expr *ilist = init;
    struct expr *alist = &vec_nth(&args, i + 2);
    struct expr *acar = NULL;
    struct expr *icar = NULL;
    while (ilist->type == OP_COMMA) {
      icar = &vec_nth(&ilist->param.op.args, 0);
      ilist = &vec_nth(&ilist->param.op.args, 1);
      acar = alist;
      if (alist->type == OP_COMMA) {
	acar = &vec_nth(&alist->param.op.args, 0);
	alist = &vec_nth(&alist->param.op.args, 1);
      }
      if (icar->type == OP_VAR) {
	*icar->param.var.value = expr_eval(acar);
      }
    }
    if (ilist->type == OP_VAR) {
      *ilist->param.var.value = expr_eval(alist);
    }
    r = expr_eval(&vec_nth(&each->args, i));
    if (!isnan(r)) {
      r = (r - 127.0) / 128.0;
      mix = mix + r;
    }
  }
  return denorm(mix / sqrt(vec_len(&each->args)));
}

static float lib_osc(struct expr_func *f, vec_expr_t args, void *context) {
  struct osc_context *osc = (struct osc_context *) context;
  float freq = arg(args, 0, NAN);
  osc->w += osc->freq / SAMPLE_RATE;
  if (isnan(freq)) {
    return NAN;
  }
  osc->freq = freq;
  if (osc->w > 1) {
    osc->w = osc->w - 1;
  }
  float w = osc->w;
  if (strncmp(f->name, "sin", 4) == 0) {
    return denorm(sin(w * 2 * PI));
  } else if (strncmp(f->name, "tri", 4) == 0) {
    w = w + 0.25f;
    return denorm(-1 + 4 * fabs(w - roundf(w)));
  } else if (strncmp(f->name, "saw", 4) == 0) {
    return denorm(2 * (w - roundf(w)));
  } else if (strncmp(f->name, "sqr", 4) == 0) {
    w = w - floor(w);
    return denorm(w < arg(args, 1, 0.5) ? 1 : -1);
  }
  return 0;
}

static float lib_fm(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct fm_context *fm = (struct fm_context *) context;
  float freq = arg(args, 0, NAN);
  float mf1 = arg(args, 1, 0);
  float mi1 = arg(args, 2, 0);
  float mf2 = arg(args, 3, 0);
  float mi2 = arg(args, 4, 0);
  float mf3 = arg(args, 5, 0);
  float mi3 = arg(args, 6, 0);

  fm->w3 += mf3 * fm->freq / SAMPLE_RATE;
  fm->w2 += mf2 * fm->freq / SAMPLE_RATE;
  fm->w1 += mf1 * fm->freq / SAMPLE_RATE;
  fm->w0 += fm->freq / SAMPLE_RATE;
  if (fm->w3 > 1) fm->w3 -= 1;
  if (fm->w2 > 1) fm->w2 -= 1;
  if (fm->w1 > 1) fm->w1 -= 1;
  if (fm->w0 > 1) fm->w0 -= 1;

  float v3 = mi3 * sin(2 * PI * fm->w3);
  float v2 = mi2 * sin(2 * PI * (fm->w2 + v3));
  float v1 = mi1 * sin(2 * PI * (fm->w1 + v3));
  float v0 = sin(2 * PI * (fm->w0 + v1 + v2));

  if (isnan(freq)) {
    fm->sync = 1;
    return NAN;
  } else {
    fm->freq = freq;
  }

  if (fm->sync && v0 >= 0 && fm->prev <= 0) {
    fm->sync = 0;
    fm->w0 = fm->w1 = fm->w2 = fm->w3 = 0;
  }

  fm->prev = v0;

  return denorm(v0);
}

static float lib_seq(struct expr_func *f, vec_expr_t args, void *context) {
  struct seq_context *seq = (struct seq_context *) context;
  if (!seq->init) {
    seq->mul = 1;
    seq->init = 1;
    if (vec_len(&args) > 0) {
      struct expr *bpm = &vec_nth(&args, 0);
      if (bpm->type == OP_COMMA) {
	float beat = expr_eval(&vec_nth(&bpm->param.op.args, 0));
	float tempo = expr_eval(bpm);
	seq->beat = to_int(beat);
	seq->t = (int) ((beat - floorf(beat)) * SAMPLE_RATE / (tempo / 60.0));
      }
    }
  }
  float beatlen = SAMPLE_RATE / (arg(args, 0, NAN) / 60) * seq->mul;
  if (isnan(beatlen)) {
    return NAN;
  }
  int t = seq->t;
  seq->t = seq->t + 1;
  int len = vec_len(&args) - 1;
  if (len <= 0) {
    return (t == 0 ? NAN : 0);
  }
  int i = seq->beat % len;
  if (seq->t >= beatlen) {
    seq->t = 0;
    seq->beat++;
  }

  struct expr *e = &vec_nth(&args, i+1);
  if (strncmp(f->name, "seq", 4) == 0) {
    if (t == 0) {
      vec_free(&seq->values);
      if (e->type == OP_COMMA) {
	seq->mul = expr_eval(&vec_nth(&e->param.op.args, 0));
	e = &vec_nth(&e->param.op.args, 1);
	if (e->type == OP_COMMA) {
          while (e->type == OP_COMMA) {
            vec_push(&seq->values, expr_eval(&vec_nth(&e->param.op.args, 0)));
	    e = &vec_nth(&e->param.op.args, 1);
          }
	  vec_push(&seq->values, expr_eval(e));
          return NAN;
	}
      } else {
	seq->mul = 1;
      }
      seq->value = expr_eval(e);
      return NAN;
    }

    len = vec_len(&seq->values);
    if (len == 0) {
      return seq->value;
    } else {
      int n = len - 1;
      int i = (int) (len * t / beatlen);
      float from = vec_nth(&seq->values, i);
      float to = vec_nth(&seq->values, i + 1);
      float k = (t / beatlen - i / n) * n;
      return from + (to - from) * k;
    }
  } else if (strncmp(f->name, "loop", 5) == 0) {
    if (e->type == OP_COMMA) {
      seq->mul = expr_eval(&vec_nth(&e->param.op.args, 0));
      e = &vec_nth(&e->param.op.args, 1);
    } else {
      seq->mul = 1;
    }
    float value = expr_eval(e);
    return t == 0 ? NAN : value;
  }
  return 0;
}

static float lib_env(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct env_context *env = (struct env_context *) context;

  // First argument is signal value
  float v = arg(args, 0, NAN);

  // Default time interval between envelope sections is 0.05
  float dt = 0.05;

  if (!env->init) {
    for (int i = 0; i < vec_len(&args); i++) {
      vec_push(&env->d, 0);
      vec_push(&env->e, 0);
    }
    env->init = 1;
  }

  int i, j;
  float level = 0;
  for (i = 1, j = 0; i < vec_len(&args); i++, j++) {
    struct expr *e = &vec_nth(&args, i);
    if (e->type == OP_COMMA) {
      dt = expr_eval(&vec_nth(&e->param.op.args, 0));
      e = &vec_nth(&e->param.op.args, 1);
    }
    level = expr_eval(e);
    if (j == 0 && level < 1) {
      vec_nth(&env->d, 0) = 0;
      vec_nth(&env->e, 0) = 1;
      j++;
    }
    vec_nth(&env->d, j) = dt * SAMPLE_RATE;
    vec_nth(&env->e, j) = level;
  }
  if (level > 0) {
    vec_nth(&env->d, j) = dt * SAMPLE_RATE;
    vec_nth(&env->e, j) = 0;
  }

  if (isnan(v)) {
    env->segment = 0;
    env->t = 0;
    return NAN;
  }
  env->t++;
  if (env->segment < vec_len(&env->d)) {
    if (env->t > vec_nth(&env->d, env->segment)) {
      env->t = 0;
      env->segment++;
    }
  } else {
    return 128; // end of envelope
  }
  float prevAmp = (env->segment == 0 ? 0 : vec_nth(&env->e, env->segment - 1));
  float amp = vec_nth(&env->e, env->segment);
  return (v - 128) * (prevAmp + (amp - prevAmp) *
      (env->t / vec_nth(&env->d, env->segment))) + 128;
}

static float lib_mix(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct mix_context *mix = (struct mix_context *) context;
  if (!mix->init) {
    for (int i = 0; i < vec_len(&args); i++) {
      vec_push(&mix->values, 0);
    }
    mix->init = 1;
  }
  float v = 0;
  for (int i = 0; i < vec_len(&args); i++) {
    struct expr *e = &vec_nth(&args, i);
    float vol = 1;
    if (e->type == OP_COMMA) {
      vol = expr_eval(&vec_nth(&e->param.op.args, 0));
      e = &vec_nth(&e->param.op.args, 1);
    }
    float sample = expr_eval(e);
    if (isnan(sample)) {
      sample = vec_nth(&mix->values, i);
    }
    vec_nth(&mix->values, i) = sample;
    v = v + vol * (sample - 128) / 127;
  }
  if (vec_len(&args) > 0) {
    v = v / sqrtf(vec_len(&args));
    v = ((v < -1) ? -1 : v);
    v = ((v > 1) ? 1 : v);
    return denorm(v);
  }
  return 128;
}

static float lib_iir(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct iir_context *iir = (struct iir_context *) context;
  float cutoff = arg(args, 1, 200);
  float signal = arg(args, 0, NAN);
  if (isnan(signal) || isnan(cutoff)) {
    return NAN;
  }
  float wa = tan(PI * cutoff / SAMPLE_RATE);
  float a = wa / (1.0 + wa);
  iir->value = iir->value + (signal - iir->value) * a;
  if (strncmp(f->name, "lpf", 4) == 0) {
    return iir->value;
  } else if (strncmp(f->name, "hpf", 4) == 0) {
    return signal - iir->value;
  } else {
    return signal;
  }
}

static float lib_delay(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct delay_context *delay = (struct delay_context *) context;

  float signal = arg(args, 0, NAN);
  float time = arg(args, 1, 0);
  float level = arg(args, 2, 0);
  float feedback = arg(args, 3, 0);
  if (feedback > 1.0f) feedback = 1.0f;
  if (feedback < 0.0f) feedback = 0.0f;

  size_t bufsz = time * SAMPLE_RATE;

  if (bufsz != delay->size) {
    delay->buf = (float *) realloc(delay->buf, bufsz * sizeof(float));
    memset(delay->buf, 0, bufsz * sizeof(float));
    delay->size = bufsz;
  }
  if (delay->buf == NULL) {
    return signal;
  }
  signal = (signal - 127.0f)/128.0f;
  float out = 0;
  int pos = delay->pos;
  if (isnan(signal)) {
    signal = 0;
  }
  out = delay->buf[pos] * level;
  delay->buf[pos] = delay->buf[pos] * feedback + signal;
  delay->pos = (delay->pos + 1) % delay->size;
  return denorm(signal + out);
}

static float lib_pitch(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct pitch_context *p = (struct pitch_context *) context;

  if (!p->init) {
      p->nmix = 50;
      p->dmax = 1200;
      p->win = 1000;
      p->step = 10;
      p->init = 1;
  }
  float vo;
  float x = arg(args, 0, NAN);
  float shift = arg(args, 1, 0);

  if (isnan(shift) || isnan(x)) {
    return NAN;
  }

  p->buf[(int) p->pw] = x;
  PITCH_READ(p, p->pr, vo);
  if (p->mix > 0) {
    float f = p->mix / p->nmix;
    float r;
    PITCH_READ(p, p->prn, r);
    vo = vo*f + r *(1-f);
    p->mix = p->mix - 1;
    if (p->mix == 0) {
      p->pr = p->prn;
    }
  } else if (p->mix == 0) {
    float d = fmodf(PITCH_BUFSZ + p->pw - p->pr, PITCH_BUFSZ);
    if (shift < 1) {
      if (d > p->dmax) {
	p->mix = p->nmix;
	PITCH_FIND(p, p->pr, p->pr+p->dmax/2, p->pr+p->dmax, p->prn);
      }
    } else if (d < p->win || d > p->dmax*2) {
      p->mix = p->nmix;
      PITCH_FIND(p, p->pr, p->pr-p->dmax, p->pr-p->win, p->prn);
    }
  }
  p->pw = PITCH_WRAP(p->pw + 1);
  p->pr = PITCH_WRAP(p->pr + shift);
  p->prn = PITCH_WRAP(p->prn + shift);
  return vo;
}

static float lib_tr808(struct expr_func *f, vec_expr_t args, void *context) {
  (void) f;
  struct osc_context *osc = (struct osc_context *) context;

  float drum = arg(args, 0, NAN);
  float vol = arg(args, 1, 1);
  /*let len = TR808Samples.length;*/
  if (isnan(drum) || isnan(vol)) {
    osc->w = 0;
    return NAN;
  }

  static unsigned char *samples[] = {
    tr808_bd_wav,
    tr808_sn_wav,
    tr808_mt_wav,
    tr808_mc_wav,
    tr808_rs_wav,
    tr808_cl_wav,
    tr808_cb_wav,
    tr808_oh_wav,
    tr808_hh_wav,
  };
  static unsigned int len[] = {
    tr808_bd_wav_len,
    tr808_sn_wav_len,
    tr808_mt_wav_len,
    tr808_mc_wav_len,
    tr808_rs_wav_len,
    tr808_cl_wav_len,
    tr808_cb_wav_len,
    tr808_oh_wav_len,
    tr808_hh_wav_len,
  };
  static int N = sizeof(samples)/sizeof(*samples);

  int index = (((int) drum % N) + N) % N;
  unsigned char *sample = samples[index];
  if (osc->w * 2 + 0x80 + 1 < len[index]) {
    unsigned char hi = sample[0x80 + (int) osc->w * 2+1];
    unsigned char lo = sample[0x80 + (int) osc->w * 2];
    int sign = hi & (1 << 7);
    int v = (hi << 8) | lo;
    if (sign) {
      v = -v + 0x10000;
    }
    float x =  v * 1.0 / 0x7fff;
    osc->w = osc->w + 1;
    return x * vol * 127 + 128;
  }
  return 128;
}

static float lib_sample(struct expr_func *f, vec_expr_t args, void *context) {
  if (loader == NULL) {
    return NAN;
  }
  struct sample_context *sample = (struct sample_context *) context;
  float variant = arg(args, 0, NAN);
  float vol = arg(args, 1, 1);
  if (isnan(variant) || isnan(vol)) {
    sample->t = 0;
    return NAN;
  }
  return denorm(loader(f->name, (int) variant, sample->t++) * vol);
}

#define MAX_FUNCS 1024
static struct expr_func glitch_funcs[MAX_FUNCS+1] = {
    {"s", lib_s, 0},
    {"r", lib_r, 0},
    {"l", lib_l, 0},
    {"a", lib_a, 0},
    {"scale", lib_scale, 0},
    {"hz", lib_hz, 0},

    {"each", lib_each, sizeof(struct each_context)},

    {"sin", lib_osc, sizeof(struct osc_context)},
    {"tri", lib_osc, sizeof(struct osc_context)},
    {"saw", lib_osc, sizeof(struct osc_context)},
    {"sqr", lib_osc, sizeof(struct osc_context)},
    {"fm", lib_fm, sizeof(struct fm_context)},
    {"tr808", lib_tr808, sizeof(struct osc_context)},

    {"loop", lib_seq, sizeof(struct seq_context)},
    {"seq", lib_seq, sizeof(struct seq_context)},

    {"env", lib_env, sizeof(struct env_context)},

    {"mix", lib_mix, sizeof(struct mix_context)},

    {"lpf", lib_iir, sizeof(struct iir_context)},
    {"hpf", lib_iir, sizeof(struct iir_context)},
    {"delay", lib_delay, sizeof(struct delay_context)},
    {"pitch", lib_pitch, sizeof(struct pitch_context)},
    {NULL, NULL, 0},
};

struct glitch *glitch_create() {
  struct glitch *g = calloc(1, sizeof(struct glitch));
  return g;
}

void glitch_sample_rate(int rate) {
  SAMPLE_RATE = rate;
}

void glitch_set_loader(glitch_loader_fn fn) {
  loader = fn;
}

int glitch_add_sample_func(const char *name) {
  for (int i = 0; i < MAX_FUNCS; i++) {
    if (glitch_funcs[i].name == NULL) {
      glitch_funcs[i].name = name;
      glitch_funcs[i].f = lib_sample;
      glitch_funcs[i].ctxsz = sizeof(struct sample_context);
      glitch_funcs[i+1].name = NULL;
      return 0;
    }
  }
  return -1;
}

void glitch_destroy(struct glitch *g) {
  expr_destroy(g->e, &g->vars);
  free(g);
}

void glitch_xy(struct glitch *g, float x, float y) {
  g->x->value = x;
  g->y->value = y;
}

void glitch_midi(struct glitch *g, unsigned char cmd, unsigned char a, unsigned char b) {
  if (cmd == 144 && b > 0) {
    // Note pressed: insert to the head of the "list"
    for (int i = 0; i < MAX_POLYPHONY; i++) {
      if (isnan(g->k[i]->value)) {
	g->k[i]->value = a - 69;
	g->v[i]->value = b / 128.0;
	break;
      }
    }
  } else if ((cmd == 144 && b == 0) || cmd == 128) {
    // Note released: remove from the "list" and shift the rest
    float key = a - 69;
    for (int i = 0; i < MAX_POLYPHONY; i++) {
      if (g->k[i]->value == key) {
	g->k[i]->value = g->v[i]->value = NAN;
      }
    }
  } else {
    fprintf(stderr, "MIDI command %d %d %d\n", cmd, a, b);
  }
}

int glitch_compile(struct glitch *g, const char *s, size_t len) {
  if (!g->init) {
    g->t = expr_var(&g->vars, "t", 1);
    g->x = expr_var(&g->vars, "x", 1);
    g->y = expr_var(&g->vars, "y", 1);
    g->bpm = expr_var(&g->vars, "bpm", 3);

    for (int i = 0; i < MAX_POLYPHONY; i++) {
      char name[4];
      snprintf(name, sizeof(name), "k%d", i);
      g->k[i] = expr_var(&g->vars, name, strlen(name));
      snprintf(name, sizeof(name), "v%d", i);
      g->v[i] = expr_var(&g->vars, name, strlen(name));
      g->k[i]->value = g->v[i]->value = NAN;
    }

    /* Note constants */
    static char buf[4] = {0, 0, 0, 0};
    int note = -4 * 12;
    for (int octave = -4; octave < 4; octave++) {
      for (int n = 0; n < 7; n++) {
	buf[0] = 'A' + n;
	buf[1] = '0' + octave + 4;
	buf[2] = '\0';
	expr_var(&g->vars, buf, 2)->value = note;
	if (n != 1 && n != 4) {
	  buf[2] = buf[1];
	  buf[1] = '#';
	  expr_var(&g->vars, buf, 3)->value = note + 1;
	  buf[0] = 'A' + ((n+1)%7);
	  buf[1] = 'b';
	  expr_var(&g->vars, buf, 3)->value = note + 1;
	  note = note + 2;
	} else {
	  buf[2] = buf[1];
	  buf[1] = '#';
	  expr_var(&g->vars, buf, 3)->value = note + 1;
	  buf[0] = 'A' + ((n+1)%7);
	  buf[1] = 'b';
	  expr_var(&g->vars, buf, 3)->value = note + 1;
	  note = note + 1;
	}
      }
    }

    /* TR808 drum constants */
    expr_var(&g->vars, "BD", 3)->value = 0;
    expr_var(&g->vars, "SD", 3)->value = 1;
    expr_var(&g->vars, "MT", 3)->value = 2;
    expr_var(&g->vars, "MA", 3)->value = 3;
    expr_var(&g->vars, "RS", 3)->value = 4;
    expr_var(&g->vars, "CP", 3)->value = 5;
    expr_var(&g->vars, "CB", 7)->value = 6;
    expr_var(&g->vars, "OH", 3)->value = 7;
    expr_var(&g->vars, "HH", 3)->value = 8;

    g->init = 1;
  }
  struct expr *e = expr_create(s, len, &g->vars, glitch_funcs);
  if (e == NULL) {
    return -1;
  }
  expr_destroy(g->next_expr, NULL);
  g->next_expr = e;
  return 0;
}

float glitch_beat(struct glitch *g) {
  return (g->frame - g->bpm_start) * g->bpm->value / 60.0 / SAMPLE_RATE;
}

float glitch_eval(struct glitch *g) {
  int apply_next = 1;
  /* If BPM is given - apply changes on the next beat */
  if (g->bpm->value > 0) {
    float beat = glitch_beat(g);
    if (beat - floorf(beat) > g->bpm->value / 60.0 / SAMPLE_RATE) {
      apply_next = 0;
    }
  }
  if (apply_next && g->next_expr != NULL) {
    if (g->bpm->value != g->last_bpm) {
      g->last_bpm = g->bpm->value;
      g->bpm_start = g->frame;
    }
    expr_destroy(g->e, NULL);
    g->e = g->next_expr;
    g->next_expr = NULL;
  }
  float v = expr_eval(g->e);
  if (!isnan(v)) {
    g->last_sample = fmod(fmod(v, 256)+256, 256)/128 - 1;
  }
  g->t->value = roundf(g->frame * 8000.0f / SAMPLE_RATE);
  g->frame++;
  return g->last_sample;
}