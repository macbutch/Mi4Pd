#include "m_pd.h"

//IMPROVE - inlets
//IMPROVE - variable large and small buffer
//TODO - help file
//TODO - types on params?


#include "clouds/dsp/granular_processor.h"

inline float constrain(float v, float vMin, float vMax) {
  return std::max<float>(vMin,std::min<float>(vMax, v));
}

static t_class *clds_tilde_class;

typedef struct _clds_tilde {
  t_object  x_obj;

  t_float  f_dummy;

  t_float f_freeze;
  t_float f_trig;
  t_float f_position;
  t_float f_size;
  t_float f_pitch;
  t_float f_density;
  t_float f_texture;
  t_float f_mix;
  t_float f_spread;
  t_float f_feedback;
  t_float f_reverb;
  t_float f_mode;
  t_float f_mono;
  t_float f_silence;
  t_float f_bypass;
  t_float f_lofi;

  // CLASS_MAINSIGNALIN  = in_left;
  t_inlet*  x_in_right;
  t_inlet*  x_in_amount;
  t_outlet* x_out_left;
  t_outlet* x_out_right;

  clouds::GranularProcessor processor;
  bool ltrig = false;
  clouds::ShortFrame* ibuf;
  clouds::ShortFrame* obuf;
  int iobufsz;

  static const int LARGE_BUF = 1048576;
  static const int SMALL_BUF = 262144;
  uint8_t large_buff [LARGE_BUF]; 
  uint8_t small_buff [SMALL_BUF];
} t_clds_tilde;


//define pure data methods
extern "C"  {
  t_int* clds_tilde_render(t_int *w);
  void clds_tilde_dsp(t_clds_tilde *x, t_signal **sp);
  void clds_tilde_free(t_clds_tilde *x);
  void* clds_tilde_new(t_floatarg f);
  void clds_tilde_setup(void);

  void clds_tilde_freeze(t_clds_tilde *x, t_floatarg);
  void clds_tilde_trig(t_clds_tilde *x, t_floatarg);
  void clds_tilde_position(t_clds_tilde *x, t_floatarg);
  void clds_tilde_size(t_clds_tilde *x, t_floatarg);
  void clds_tilde_pitch(t_clds_tilde *x, t_floatarg);
  void clds_tilde_density(t_clds_tilde *x, t_floatarg);
  void clds_tilde_texture(t_clds_tilde *x, t_floatarg);
  void clds_tilde_mix(t_clds_tilde *x, t_floatarg);
  void clds_tilde_spread(t_clds_tilde *x, t_floatarg);
  void clds_tilde_feedback(t_clds_tilde *x, t_floatarg);
  void clds_tilde_reverb(t_clds_tilde *x, t_floatarg);
  void clds_tilde_mode(t_clds_tilde *x, t_floatarg);
  void clds_tilde_mono(t_clds_tilde *x, t_floatarg);
  void clds_tilde_silence(t_clds_tilde *x, t_floatarg);
  void clds_tilde_bypass(t_clds_tilde *x, t_floatarg);
  void clds_tilde_lofi(t_clds_tilde *x, t_floatarg);
}

// puredata methods implementation -start
t_int *clds_tilde_render(t_int *w)
{
  t_clds_tilde *x   = (t_clds_tilde *)(w[1]);
  t_sample  *in_left   = (t_sample *)(w[2]);
  t_sample  *in_right  = (t_sample *)(w[3]);
  t_sample  *out_left  = (t_sample *)(w[4]);
  t_sample  *out_right = (t_sample *)(w[5]);
  int n =  (int)(w[6]);

  // for now restrict playback mode to working modes (granular and looping) 
  // clouds::PlaybackMode mode  = (clouds::PlaybackMode) ((param_playmode + inlet_mode) % 4) );
  clouds::PlaybackMode mode =  
    (( (int) (x->f_mode) % 2) == 0) 
    ? clouds::PLAYBACK_MODE_GRANULAR 
    : clouds::PLAYBACK_MODE_LOOPING_DELAY;
  x->processor.set_playback_mode(mode);
///
  x->processor.mutable_parameters()->position  = constrain(x->f_position,   0.0f,1.0f);
  x->processor.mutable_parameters()->size    = constrain(x->f_size,     0.0f,1.0f);
  x->processor.mutable_parameters()->texture   = constrain(x->f_texture, 0.0f,1.0f);
  x->processor.mutable_parameters()->dry_wet   = constrain(x->f_mix,       0.0f,1.0f);
  x->processor.mutable_parameters()->stereo_spread= constrain(x->f_spread,    0.0f,1.0f);
  x->processor.mutable_parameters()->feedback  = constrain(x->f_feedback,   0.0f,1.0f);
  x->processor.mutable_parameters()->reverb    = constrain(x->f_reverb,     0.0f,1.0f);

  x->processor.mutable_parameters()->pitch   = constrain(x->f_pitch * 64.0f,-64.0f,64.0f);

  // restrict density to .2 to .8 for granular mode, outside this breaks up
  float density = (mode == clouds::PLAYBACK_MODE_GRANULAR) ? (x->f_density*.6)+0.2 : density;
  x->processor.mutable_parameters()->density = constrain(density, 0.0f, 1.0f);

  x->processor.mutable_parameters()->freeze = x->f_freeze;

  //note the trig input is really a gate... which then feeds the trig
  x->processor.mutable_parameters()->gate = x->f_trig;

  bool trig = false;
  if((x->f_trig > 0.5)  && !x->ltrig) {
    x->ltrig = true;
    trig  = true;
  } else if (! (x->f_trig > 0.5))  {
    x->ltrig = false;
  }
  x->processor.mutable_parameters()->trigger = trig;

  x->processor.set_bypass(x->f_bypass > 0.5f);
  x->processor.set_silence(x->f_silence > 0.5f);
  x->processor.set_num_channels(x->f_mono  < 0.5f ? 1 : 2 );
  x->processor.set_low_fidelity(x->f_lofi > 0.5f);

  if (n > x->iobufsz) {
    delete [] x->ibuf;
    delete [] x->obuf;
    x->iobufsz = n;
    x->ibuf = new clouds::ShortFrame[x->iobufsz];
    x->obuf = new clouds::ShortFrame[x->iobufsz];
  }

  for (int i = 0; i < n; i++) {
    x->ibuf[i].l = in_left[i] * 1024;
    x->ibuf[i].r = in_right[i] * 1024;
  }


  x->processor.Prepare();
  x->processor.Process(x->ibuf, x->obuf,n);

  for (int i = 0; i < n; i++) {
    out_left[i] = x->obuf[i].l / 1024.0f;
    out_right[i] = x->obuf[i].r / 1024.0f;
  }

  return (w + 7); // # args + 1
}

void clds_tilde_dsp(t_clds_tilde *x, t_signal **sp)
{
  // add the perform method, with all signal i/o
  dsp_add(clds_tilde_render, 6,
          x,
          sp[0]->s_vec, sp[1]->s_vec, sp[2]->s_vec, sp[3]->s_vec, // signal i/o (clockwise)
          sp[0]->s_n);
}

void clds_tilde_free(t_clds_tilde *x)
{
  delete [] x->ibuf;
  delete [] x->obuf;

  inlet_free(x->x_in_right);
  inlet_free(x->x_in_amount);
  outlet_free(x->x_out_left);
  outlet_free(x->x_out_right);
}

void *clds_tilde_new(t_floatarg)
{
  t_clds_tilde *x = (t_clds_tilde *) pd_new(clds_tilde_class);
  x->iobufsz = 64;
  x->ibuf = new clouds::ShortFrame[x->iobufsz];
  x->obuf = new clouds::ShortFrame[x->iobufsz];


  x->x_in_right   = inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_signal, &s_signal);
  x->x_out_left   = outlet_new(&x->x_obj, &s_signal);
  x->x_out_right  = outlet_new(&x->x_obj, &s_signal);

  x->f_freeze = 0.0f;
  x->f_trig = 0.0f;
  x->f_position = 0.0f;
  x->f_size = 0.0f;
  x->f_pitch = 0.0f;
  x->f_density = 0.0f;
  x->f_texture = 0.0f;
  x->f_mix = 0.0f;
  x->f_spread = 0.0f;
  x->f_feedback = 0.0f;
  x->f_reverb = 0.0f;
  x->f_mode = 0.0f;
  x->f_mono = 0.0f;
  x->f_silence = 0.0f;
  x->f_bypass = 0.0f;
  x->f_lofi = 0.0f;

  x->processor.Init(
    x->large_buff,x->LARGE_BUF, 
    x->small_buff,x->SMALL_BUF);

  x->ltrig = false;
  return (void *)x;
}


void clds_tilde_setup(void) {
  clds_tilde_class = class_new(gensym("clds~"),
                                         (t_newmethod)clds_tilde_new,
                                         0, sizeof(t_clds_tilde),
                                         CLASS_DEFAULT,
                                         A_DEFFLOAT, A_NULL);

  class_addmethod(  clds_tilde_class,
                    (t_method)clds_tilde_dsp,
                    gensym("dsp"), A_NULL);

  CLASS_MAINSIGNALIN(clds_tilde_class, t_clds_tilde, f_dummy);


  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_freeze, gensym("freeze"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_trig, gensym("trig"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_position, gensym("position"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_size, gensym("size"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_pitch, gensym("pitch"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_density, gensym("density"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_texture, gensym("texture"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_mix, gensym("mix"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_spread, gensym("spread"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_feedback, gensym("feedback"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_reverb, gensym("reverb"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_mode, gensym("mode"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_mono, gensym("mono"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_silence, gensym("silence"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_bypass, gensym("bypass"),
    A_DEFFLOAT, A_NULL);
  class_addmethod(clds_tilde_class,
    (t_method) clds_tilde_lofi, gensym("lofi"),
    A_DEFFLOAT, A_NULL);
}



void clds_tilde_freeze(t_clds_tilde *x, t_floatarg f)
{
  x->f_freeze = f;
}
void clds_tilde_trig(t_clds_tilde *x, t_floatarg f)
{
  x->f_trig = f;
}
void clds_tilde_position(t_clds_tilde *x, t_floatarg f)
{
  x->f_position = f;
}
void clds_tilde_size(t_clds_tilde *x, t_floatarg f)
{
  x->f_size = f;
}
void clds_tilde_pitch(t_clds_tilde *x, t_floatarg f)
{
  x->f_pitch = f;
}
void clds_tilde_density(t_clds_tilde *x, t_floatarg f)
{
  x->f_density = f;
}

void clds_tilde_texture(t_clds_tilde *x, t_floatarg f)
{
  x->f_texture = f;
}

void clds_tilde_mix(t_clds_tilde *x, t_floatarg f)
{
  x->f_mix = f;
}

void clds_tilde_spread(t_clds_tilde *x, t_floatarg f)
{
  x->f_spread = f;
}

void clds_tilde_feedback(t_clds_tilde *x, t_floatarg f)
{
  x->f_feedback = f;
}

void clds_tilde_reverb(t_clds_tilde *x, t_floatarg f)
{
  x->f_reverb = f;
}

void clds_tilde_mode(t_clds_tilde *x, t_floatarg f)
{
  x->f_mode = f;
}

void clds_tilde_mono(t_clds_tilde *x, t_floatarg f)
{
  x->f_mono = f;
}

void clds_tilde_silence(t_clds_tilde *x, t_floatarg f)
{
  x->f_silence = f;
}

void clds_tilde_bypass(t_clds_tilde *x, t_floatarg f)
{
  x->f_bypass = f;
}

void clds_tilde_lofi(t_clds_tilde *x, t_floatarg f)
{
  x->f_lofi = f;
}

// puredata methods implementation - end
