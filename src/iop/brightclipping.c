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

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#include "bauhaus/bauhaus.h"
#include "common/histogram.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "develop/pixelpipe.h"
#include "dtgtk/paint.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

DT_MODULE_INTROSPECTION(1, dt_iop_brightclipping_params_t)

typedef struct dt_iop_brightclipping_params_t
{
  float threshold_detect;
  float threshold_target;
} dt_iop_brightclipping_params_t;

typedef struct dt_iop_brightclipping_gui_data_t
{
  GtkWidget *threshold_detect;
  GtkWidget *threshold_target;
} dt_iop_brightclipping_gui_data_t;

typedef dt_iop_brightclipping_params_t dt_iop_brightclipping_data_t;

const char *name()
{
  return _("bright clipping");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return 4 * sizeof(float);
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_brightclipping_data_t *const d = piece->data;

  const int ch = piece->colors;

  memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_in->width * roi_in->height);

  for(int k = 0; k < 3; k++)
    printf("piece->pipe->processed_maximum[%i] = %f\n", k, piece->pipe->processed_maximum[k]);

#if 0
  printf("bins_count = %i; ch = %i; pixels = %i\n", piece->histogram_stats.bins_count, piece->histogram_stats.ch,
         piece->histogram_stats.pixels);

  const double mul = (double)(piece->histogram_params.bins_count - 1);

  for(int k = piece->histogram_stats.bins_count - 1; k >= 0; k--)
  {
    const uint32_t *bin = piece->histogram + (size_t)piece->histogram_stats.ch * k;
    if((bin[0] == bin[1] && bin[0] == bin[2]) && bin[0] == 0) continue;

    printf("histogram[%i] (%f) = %i %i %i %s\n", k, (double)k / mul, bin[0], bin[1], bin[2],
           (bin[0] == bin[1] && bin[0] == bin[2]) ? "EQUAL" : "non-eq");
    // return;
  }
#endif

  const float thr = fminf(piece->pipe->processed_maximum[0],
                          fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  const float tf = d->threshold_detect * thr;
  const float td = 0.95 * tf;
  const float t2 = d->threshold_target / tf;

  printf("thr = %f\n", thr);
  printf("tf = %f\n", tf);
  printf("td = %f\n", td);
  printf("t2 = %f\n\n", t2);

#ifdef _OPENMP
#pragma omp parallel for SIMD() default(none) schedule(dynamic)
#endif
  for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
  {
    const float *const in = (const float *const)ivoid + (size_t)k * ch;
    float *const out = (float *const)ovoid + (size_t)k * ch;

    if(in[0] < td && in[1] < td && in[2] < td) continue;

    for(int c = 0; c < 4; c++)
    {
      const float blend = CLAMPS(1.0f / tf * in[c] - 0.95f, 0.0f, 1.0f);
      out[c] = in[c] * (1.0f - blend) + in[c] * t2 * blend;
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_brightclipping_params_t *p = p1;
  dt_iop_brightclipping_data_t *d = piece->data;

  // if(pipe->type != DT_DEV_PIXELPIPE_FULL) piece->enabled = 0;

  d->threshold_detect = p->threshold_detect;
  d->threshold_target = p->threshold_target;

#if 0
  piece->request_histogram |= (DT_REQUEST_ON);
  piece->request_histogram |= (DT_REQUEST_ONLY_IN_GUI);
  piece->histogram_params.bins_count = (UINT16_MAX + 1);

  if(!self->dev->gui_attached) piece->request_histogram &= ~(DT_REQUEST_ONLY_IN_GUI);
#endif
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_brightclipping_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_brightclipping_gui_data_t *g = self->gui_data;
  dt_iop_brightclipping_params_t *p = self->params;

  dt_bauhaus_slider_set(g->threshold_detect, p->threshold_detect);
  dt_bauhaus_slider_set(g->threshold_target, p->threshold_target);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_brightclipping_params_t));
  module->default_params = calloc(1, sizeof(dt_iop_brightclipping_params_t));
  module->default_enabled = 0;
  module->priority = 170; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_brightclipping_params_t);
  module->gui_data = NULL;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_brightclipping_params_t tmp
      = (dt_iop_brightclipping_params_t){.threshold_detect = 0.5, .threshold_target = 1.0f };

  memcpy(module->params, &tmp, sizeof(dt_iop_brightclipping_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_brightclipping_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

static void callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_brightclipping_gui_data_t *g = self->gui_data;
  dt_iop_brightclipping_params_t *p = self->params;
  p->threshold_detect = dt_bauhaus_slider_get(g->threshold_detect);
  p->threshold_target = dt_bauhaus_slider_get(g->threshold_target);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_brightclipping_gui_data_t));
  dt_iop_brightclipping_gui_data_t *g = self->gui_data;
  dt_iop_brightclipping_params_t *p = self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold_detect = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->threshold_detect, 3);
  gtk_widget_set_tooltip_text(g->threshold_detect, _("thr 1 (you shouldn't ever need to touch this)"));
  dt_bauhaus_widget_set_label(g->threshold_detect, NULL, _("thr 1"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold_detect, TRUE, TRUE, 0);
  g_signal_connect(g->threshold_detect, "value-changed", G_CALLBACK(callback), self);

  g->threshold_target = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->threshold_target, 3);
  gtk_widget_set_tooltip_text(g->threshold_target, _("thr 2 (you shouldn't ever need to touch this)"));
  dt_bauhaus_widget_set_label(g->threshold_target, NULL, _("thr 2"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold_target, TRUE, TRUE, 0);
  g_signal_connect(g->threshold_target, "value-changed", G_CALLBACK(callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
