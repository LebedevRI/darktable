/*
    This file is part of darktable,

    copyright (c) 2016 Roman Lebedev.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>  // for G_CALLBACK, G_OBJECT, g_signal_connect
#include <glib/gi18n.h>   // for _
#include <glib/gmacros.h> // for MIN, TRUE
#include <gtk/gtk.h>      // for gtk_box_new, gtk_box_pack_start
#include <math.h>         // for fminf, fmaxf
#include <stdlib.h>       // for size_t, free, NULL, calloc, malloc
#include <string.h>       // for memcpy

#if defined(__SSE__)
#include <xmmintrin.h> // for _mm_min_ps, _mm_set1_ps, _mm_sfence
#endif

#include "bauhaus/bauhaus.h"      // for dt_bauhaus_slider_get, dt_bauhaus_...
#include "common/darktable.h"     // for darktable_t, darktable, dt_codepath_t
#include "common/image.h"         // for dt_image_filter, dt_image_is_raw
#include "develop/develop.h"      // for dt_develop_t, dt_dev_add_history_item
#include "develop/imageop.h"      // for dt_iop_module_t, dt_iop_roi_t, dt_...
#include "develop/imageop_math.h" // for dt_iop_alpha_copy
#include "develop/pixelpipe_hb.h" // for dt_dev_pixelpipe_iop_t, dt_dev_pix...
#include "gui/gtk.h"              // for dt_gui_gtk_t
#include "iop/iop_api.h"          // for dt_iop_params_t

DT_MODULE_INTROSPECTION(1, dt_iop_highlightsinpaint_params_t)

typedef struct dt_iop_highlightsinpaint_params_t
{
  float clip;
  float midtones;
} dt_iop_highlightsinpaint_params_t;

typedef struct dt_iop_highlightsinpaint_gui_data_t
{
  GtkWidget *clip;
  GtkWidget *midtones;
} dt_iop_highlightsinpaint_gui_data_t;

typedef dt_iop_highlightsinpaint_params_t dt_iop_highlightsinpaint_data_t;

const char *name()
{
  return _("highlight inpainting");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_ONE_INSTANCE;
}

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  else
    return 4 * sizeof(float);
}

static void process_clip_plain(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                               void *const ovoid, const dt_iop_roi_t *const roi_in,
                               const dt_iop_roi_t *const roi_out, const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
  else
  {
    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(static)
#endif
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
}

#if defined(__SSE__)
static void process_clip_sse2(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                              void *const ovoid, const dt_iop_roi_t *const roi_in,
                              const dt_iop_roi_t *const roi_out, const float clip)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  { // raw mosaic
    const __m128 clipm = _mm_set1_ps(clip);
    const size_t n = (size_t)roi_out->height * roi_out->width;
    float *const out = (float *)ovoid;
    float *const in = (float *)ivoid;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) default(none)
#endif
    for(size_t j = 0; j < (n & ~3u); j += 4) _mm_stream_ps(out + j, _mm_min_ps(clipm, _mm_load_ps(in + j)));
    _mm_sfence();
    // lets see if there's a non-multiple of four rest to process:
    if(n & 3)
      for(size_t j = n & ~3u; j < n; j++) out[j] = MIN(clip, in[j]);
  }
  else
  {
    const __m128 clipm = _mm_set1_ps(clip);
    const int ch = piece->colors;

#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      float *out = (float *)ovoid + (size_t)ch * roi_out->width * j;
      float *in = (float *)ivoid + (size_t)ch * roi_in->width * j;
      for(int i = 0; i < roi_out->width; i++, in += ch, out += ch)
      {
        _mm_stream_ps(out, _mm_min_ps(clipm, _mm_set_ps(in[3], in[2], in[1], in[0])));
      }
    }
    _mm_sfence();
  }
}
#endif

static void process_clip(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                         void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                         const float clip)
{
  if(darktable.codepath.OPENMP_SIMD) process_clip_plain(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
#if defined(__SSE__)
  else if(darktable.codepath.SSE2)
    process_clip_sse2(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
#endif
  else
    dt_unreachable_codepath();
}

// downsample demosaic
static void inpaint_dd(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                       void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                       const float clip, const float midtones)
{
  const int filters = dt_image_filter(&piece->pipe->image);

// out is 1/4 of in

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    float *in = (float *)ivoid + (size_t)4 * roi_out->width * j;
    float *out = (float *)ovoid + (size_t)4 * roi_out->width * j;

    for(int i = 0; i < roi_out->width; i++, in += 2, out += 4)
    {
      // sample 1 bayer block. thus we will have 2 green values.
      float R = 0.0f, Gmin = FLT_MAX, Gmax = -FLT_MAX, B = 0.0f;
      for(int jj = 0; jj <= 1; jj++)
      {
        for(int ii = 0; ii <= 1; ii++)
        {
          const float val = in[(size_t)jj * 2 * roi_out->width + ii];

          if((val < clip) && (val > midtones))
          {
            const int c = FC(2 * j + jj + roi_in->y, 2 * i + ii + roi_in->x, filters);

            switch(c)
            {
              case 0:
                R = val;
                break;
              case 1:
                Gmin = MIN(Gmin, val);
                Gmax = MAX(Gmax, val);
                break;
              case 2:
                B = val;
                break;
            }

            out[c] = val;
          }
        }
      }

      out[0] = R;
      out[2] = B;

      if(Gmin != FLT_MAX && Gmax != -FLT_MAX)
        out[1] = (Gmin + Gmax) / 2.0f;
      else if(Gmin != FLT_MAX)
        out[1] = Gmin;
      else if(Gmax != -FLT_MAX)
        out[1] = Gmax;
    }
  }
}

static void inpaint_unroll(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                           void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                           const float clip)
{
  const int filters = dt_image_filter(&piece->pipe->image);

  // in is 1/4 of out

  memset(ovoid, 0, (size_t)sizeof(float) * roi_out->width * roi_out->height);

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none)
#endif
  for(int j = 0; j < roi_in->height; j++)
  {
    float *in = (float *)ivoid + (size_t)4 * roi_in->width * j;
    float *out = (float *)ovoid + (size_t)4 * roi_in->width * j;

    for(int i = 0; i < roi_in->width; i++, in += 4, out += 2)
    {
      // sample 1 bayer block. thus we will have 2 green values.
      for(int jj = 0; jj <= 1; jj++)
      {
        for(int ii = 0; ii <= 1; ii++)
        {
          const int c = FC(2 * j + jj + roi_out->y, 2 * i + ii + roi_out->x, filters);
          out[(size_t)jj * 2 * roi_in->width + ii] = in[c];
        }
      }
    }
  }
}

static void process_inpaint(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
                            void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                            const float clip, const float midtones)
{
  const dt_iop_roi_t roi_dd = (dt_iop_roi_t){
    .x = 0, .y = 0, .width = floor((double)roi_in->width / 2.0), .height = floor((double)roi_in->height / 2.0)
  };
  void *dd = calloc((size_t)roi_dd.width * roi_dd.height, (size_t)4 * sizeof(float));

  inpaint_dd(self, piece, ivoid, dd, roi_in, &roi_dd, clip, midtones);
  inpaint_unroll(self, piece, dd, ovoid, &roi_dd, roi_out, clip);

  free(dd);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const int filters = dt_image_filter(&piece->pipe->image);
  dt_iop_highlightsinpaint_data_t *data = (dt_iop_highlightsinpaint_data_t *)piece->data;

  const float clip
      = data->clip * fminf(piece->pipe->processed_maximum[0],
                           fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  const float midtones
      = data->midtones * fminf(piece->pipe->processed_maximum[0],
                               fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  if(dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) || !filters)
  {
    process_clip(self, piece, ivoid, ovoid, roi_in, roi_out, clip);
    for(int k = 0; k < 3; k++)
      piece->pipe->processed_maximum[k]
          = fminf(piece->pipe->processed_maximum[0],
                  fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));
    return;
  }

  process_inpaint(self, piece, ivoid, ovoid, roi_in, roi_out, clip, midtones);

  // update processed maximum
  const float m = fmaxf(fmaxf(piece->pipe->processed_maximum[0], piece->pipe->processed_maximum[1]),
                        piece->pipe->processed_maximum[2]);
  for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] = m;

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void threshold_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->clip = MAX(dt_bauhaus_slider_get(g->clip), dt_bauhaus_slider_get(g->midtones));
  p->midtones = MIN(dt_bauhaus_slider_get(g->clip), dt_bauhaus_slider_get(g->midtones));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)p1;
  dt_iop_highlightsinpaint_data_t *d = (dt_iop_highlightsinpaint_data_t *)piece->data;

  *d = *p;

  d->clip = MAX(p->clip, p->midtones);
  d->midtones = MIN(p->clip, p->midtones);

  // only raw.
  if(!dt_image_is_raw(&self->dev->image_storage)) piece->enabled = 0;

  // only plain bayer for now.
  if(pipe->image.filters == 9u || (self->dev->image_storage.flags & DT_IMAGE_4BAYER)) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_highlightsinpaint_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  dt_bauhaus_slider_set(g->clip, p->clip);
  dt_bauhaus_slider_set(g->midtones, p->midtones);
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_highlightsinpaint_params_t tmp = (dt_iop_highlightsinpaint_params_t){.clip = 1.0, .midtones = 0.5 };

  // we might be called from presets update infrastructure => there is no image
  if(!self->dev) goto end;

  // only on for raw images:
  if(dt_image_is_raw(&self->dev->image_storage))
    self->hide_enable_button = 0;
  else
    self->hide_enable_button = 1;

end:
  memcpy(self->params, &tmp, sizeof(dt_iop_highlightsinpaint_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_highlightsinpaint_params_t));
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_highlightsinpaint_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_highlightsinpaint_params_t));
  module->priority = 62; // module order created by iop_dependencies.py, do not edit!
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_highlightsinpaint_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlightsinpaint_gui_data_t));
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->clip = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->clip, 3);
  gtk_widget_set_tooltip_text(g->clip, _("manually adjust the clipping threshold against "
                                         "magenta highlights (you shouldn't ever need to touch this)"));
  dt_bauhaus_widget_set_label(g->clip, NULL, _("clipping threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->clip, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->clip), "value-changed", G_CALLBACK(threshold_callback), self);

  g->midtones = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->midtones, 3);
  gtk_widget_set_tooltip_text(g->midtones,
                              _("manually adjust the threshold between midtones and highlights. the values lying "
                                "above this threshold and below clipping threshold will be used for inpainting"));
  dt_bauhaus_widget_set_label(g->midtones, NULL, _("midtones threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->midtones, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->midtones), "value-changed", G_CALLBACK(threshold_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
