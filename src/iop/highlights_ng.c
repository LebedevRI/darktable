/*
    This file is part of darktable,
    copyright (c) 2015 LebedevRI.

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

#include "develop/imageop.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"

#include <math.h>
#include <gtk/gtk.h>
#include <stdlib.h>

#include <xmmintrin.h>

DT_MODULE_INTROSPECTION(1, dt_iop_highlights_ng_params_t)

typedef enum dt_iop_highlights_ng_mode_t
{
  MODE_CLIP
} dt_iop_highlights_ng_mode_t;

typedef struct dt_iop_highlights_ng_params_t
{
  dt_iop_highlights_ng_mode_t mode;
  float threshold;
} dt_iop_highlights_ng_params_t;

typedef struct dt_iop_highlights_ng_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *threshold;
} dt_iop_highlights_ng_gui_data_t;

typedef struct dt_iop_highlights_ng_global_data_t
{
} dt_iop_highlights_ng_global_data_t;

const char *name()
{
  return _("highlight reconstruction (NG)");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING
         | IOP_FLAGS_ONE_INSTANCE;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_highlights_ng_params_t *const d = piece->data;
  const float threshold
      = d->threshold * fminf(piece->pipe->processed_maximum[0],
                             fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  switch(d->mode)
  {
    case MODE_CLIP:
    {
#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none)
#endif
      for(int j = 0; j < roi_out->height; j++)
      {
        const float *in = (const float *)ivoid + (size_t)4 * roi_in->width * j;
        float *out = (float *)ovoid + (size_t)4 * roi_out->width * j;
        for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
      const __m128 clip = _mm_set1_ps(threshold);
          _mm_stream_ps(out, _mm_min_ps(clip, _mm_load_ps(in)));
      }
      _mm_sfence();
      break;
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_highlights_ng_params_t tmp = (dt_iop_highlights_ng_params_t){.mode = MODE_CLIP, .threshold = 1.0f };

  memcpy(self->params, &tmp, sizeof(dt_iop_highlights_ng_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_highlights_ng_params_t));
}

void init(dt_iop_module_t *self)
{
  self->data = NULL;
  self->params = calloc(1, sizeof(dt_iop_highlights_ng_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_highlights_ng_params_t));
  self->default_enabled = 0;
  self->priority = 135; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_highlights_ng_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL; // just to be sure
  free(self->params);
  self->params = NULL; // just to be sure
}

static void mode_changed(GtkWidget *combo, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_highlights_ng_params_t *p = self->params;

  p->mode = dt_bauhaus_combobox_get(combo);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void slider_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_highlights_ng_params_t *p = self->params;

  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_highlights_ng_gui_data_t *g = self->gui_data;
  dt_iop_highlights_ng_params_t *p = self->params;

  dt_bauhaus_combobox_set(g->mode, p->mode);
  dt_bauhaus_slider_set(g->threshold, p->threshold);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlights_ng_gui_data_t));

  dt_iop_highlights_ng_gui_data_t *g = self->gui_data;
  dt_iop_highlights_ng_params_t *p = self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->mode = dt_bauhaus_combobox_new(self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_label(g->mode, NULL, _("method"));
  dt_bauhaus_combobox_add(g->mode, _("clip highlights"));
  g_object_set(G_OBJECT(g->mode), "tooltip-text", _("highlight reconstruction method"), (char *)NULL);
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(mode_changed), self);

  g->threshold = dt_bauhaus_slider_new_with_range(self, 0.0, 2.0, 0.01, p->threshold, 3);
  g_object_set(G_OBJECT(g->threshold), "tooltip-text",
               _("manually adjust the clipping threshold against"
                 " magenta highlights (you shouldn't ever need to touch this)"),
               (char *)NULL);
  dt_bauhaus_widget_set_label(g->threshold, NULL, _("clipping threshold"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->threshold), "value-changed", G_CALLBACK(slider_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
