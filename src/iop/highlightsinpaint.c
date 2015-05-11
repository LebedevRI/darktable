/*
  This file is part of darktable,
  copyright (c) 2015 Ulrich Pegelow. (colorreconstruction.c)
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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/colorspaces.h"
#include "gui/accelerators.h"
#include "common/gaussian.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include <gtk/gtk.h>
#include <inttypes.h>

DT_MODULE_INTROSPECTION(1, dt_iop_highlightsinpaint_params_t)

typedef struct dt_iop_highlightsinpaint_params_t
{
  float threshold;
  dt_gaussian_order_t order;
  float radius;
  float spatial;
  float range;
} dt_iop_highlightsinpaint_params_t;

typedef struct dt_iop_highlightsinpaint_gui_data_t
{
  GtkWidget *threshold;
  GtkWidget *radius;
  GtkWidget *spatial;
  GtkWidget *range;
} dt_iop_highlightsinpaint_gui_data_t;

typedef struct dt_iop_highlightsinpaint_data_t
{
  float threshold;
  dt_gaussian_order_t order;
  float radius;
  float spatial;
  float range;
} dt_iop_highlightsinpaint_data_t;

const char *name()
{
  return _("highlight inpainting");
}

int flags()
{
  // we do not allow tiling. reason: this module needs to see the full surrounding of highlights.
  // if we would split into tiles, each tile would result in different color corrections
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "threshold"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "spatial extent"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "range extent"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "threshold", GTK_WIDGET(g->threshold));
  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->radius));
  dt_accel_connect_slider_iop(self, "spatial extent", GTK_WIDGET(g->spatial));
  dt_accel_connect_slider_iop(self, "range extent", GTK_WIDGET(g->range));
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_highlightsinpaint_data_t *const d = (dt_iop_highlightsinpaint_data_t *)piece->data;

  const float threshold
      = d->threshold * fminf(piece->pipe->processed_maximum[0],
                             fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));

  const float radius = fmax(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;

  float Labmax[] = { 100.0f, 128.0f, 128.0f, 1.0f };
  float Labmin[] = { 0.0f, -128.0f, -128.0f, 0.0f };

  for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
  for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;

  if(d->radius > 0.0f)
  {
    dt_gaussian_t *g
        = dt_gaussian_init(roi_out->width, roi_out->height, piece->colors, Labmax, Labmin, sigma, d->order);
    if(!g) return;
    dt_gaussian_blur_4c(g, (float *)ivoid, ovoid);
    dt_gaussian_free(g);
  }
  else
  {
    memcpy(ovoid, ivoid, (size_t)piece->colors * roi_out->width * roi_out->height * sizeof(float));
  }

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic) default(none)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = (const float *)ivoid + (size_t)4 * roi_in->width * j;
    float *out = (float *)ovoid + (size_t)4 * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
    {
      const float R = in[0], G = in[1], B = in[2];
      if(!(R > threshold || G > threshold || B > threshold))
      {
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[2];
      }
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
}

static void threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->threshold = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void spatial_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->spatial = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void range_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;
  p->range = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)p1;
  dt_iop_highlightsinpaint_data_t *d = (dt_iop_highlightsinpaint_data_t *)piece->data;

  d->threshold = p->threshold;
  d->order = p->order;
  d->radius = p->radius;
  d->spatial = p->spatial;
  d->range = p->range;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_highlightsinpaint_data_t *d
      = (dt_iop_highlightsinpaint_data_t *)calloc(1, sizeof(dt_iop_highlightsinpaint_data_t));
  piece->data = (void *)d;
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)module->params;
  dt_bauhaus_slider_set(g->threshold, p->threshold);
  dt_bauhaus_slider_set(g->spatial, p->spatial);
  dt_bauhaus_slider_set(g->range, p->range);
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_highlightsinpaint_params_t));
  module->default_params = malloc(sizeof(dt_iop_highlightsinpaint_params_t));
  module->default_enabled = 0;
  module->priority = 134; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_highlightsinpaint_params_t);
  module->gui_data = NULL;
  dt_iop_highlightsinpaint_params_t tmp = (dt_iop_highlightsinpaint_params_t){
    .threshold = 1.0f, .order = 0, .radius = 4, .spatial = 400.0f, .range = 10.0f
  };
  memcpy(module->params, &tmp, sizeof(dt_iop_highlightsinpaint_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_highlightsinpaint_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_highlightsinpaint_gui_data_t));
  dt_iop_highlightsinpaint_gui_data_t *g = (dt_iop_highlightsinpaint_gui_data_t *)self->gui_data;
  dt_iop_highlightsinpaint_params_t *p = (dt_iop_highlightsinpaint_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->threshold = dt_bauhaus_slider_new_with_range(self, 0.0f, 2.0f, 0.01f, p->threshold, 2);
  g->radius = dt_bauhaus_slider_new_with_range(self, 0.0f, 200.0f, 0.1f, p->radius, 2);
  g->spatial = dt_bauhaus_slider_new_with_range(self, 0.0f, 1000.0f, 1.0f, p->spatial, 2);
  g->range = dt_bauhaus_slider_new_with_range(self, 0.0f, 50.0f, 0.1f, p->range, 2);

  dt_bauhaus_widget_set_label(g->threshold, NULL, _("clipping threshold"));
  dt_bauhaus_widget_set_label(g->radius, NULL, _("blur radius"));
  dt_bauhaus_widget_set_label(g->spatial, NULL, _("spatial extent"));
  dt_bauhaus_widget_set_label(g->range, NULL, _("range extent"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->threshold, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->spatial, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->range, TRUE, TRUE, 0);

  g_object_set(g->threshold, "tooltip-text", _("manually adjust the clipping threshold against magenta "
                                               "highlights (you shouldn't ever need to touch this)"),
               (char *)NULL);
  g_object_set(g->radius, "tooltip-text", _("radius of gaussian blur"), (char *)NULL);
  g_object_set(g->spatial, "tooltip-text", _("how far to look for replacement colors in spatial dimensions"),
               (char *)NULL);
  g_object_set(g->range, "tooltip-text",
               _("how far to look for replacement colors in the luminance dimension"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->threshold), "value-changed", G_CALLBACK(threshold_callback), self);
  g_signal_connect(G_OBJECT(g->radius), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->spatial), "value-changed", G_CALLBACK(spatial_callback), self);
  g_signal_connect(G_OBJECT(g->range), "value-changed", G_CALLBACK(range_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
