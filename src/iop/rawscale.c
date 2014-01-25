/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI

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

#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

DT_MODULE(1)

typedef struct dt_iop_rawscale_params_t
{
  uint16_t raw_black_level;
  uint16_t raw_black_level_separate[4];
  uint16_t raw_white_point;
}
dt_iop_rawscale_params_t;

typedef struct dt_iop_rawscale_gui_data_t
{
  GtkWidget *raw_black_level, *raw_black_level_separate[4], *raw_white_point;
}
dt_iop_rawscale_gui_data_t;

const char *name()
{
  return _("raw scale");
}

int
flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int
groups()
{
  return IOP_GROUP_BASIC;
}

int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW)) return sizeof(float);
  else return 4*sizeof(float);
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  dt_iop_rawscale_params_t *d = (dt_iop_rawscale_params_t *)piece->data;
  d = d;
  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp != 4)
  {
    const float coeffsi = 1/65535.0f;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      int i=0;
      const uint16_t *in = ((uint16_t *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;

      // process unaligned pixels
      for ( ; i < ((4-(j*roi_out->width & 3)) & 3) ; i++,out++,in++)
        *out = *in / coeffsi;

      const __m128 coeffs = _mm_set_ps(coeffsi,
                                       coeffsi,
                                       coeffsi,
                                       coeffsi);

      // process aligned pixels with SSE
      for( ; i < roi_out->width - 3 ; i+=4,out+=4,in+=4)
      {
        _mm_stream_ps(out,_mm_div_ps(_mm_set_ps(in[3],in[2],in[1],in[0]),coeffs));
      }

      // process the rest
      for( ; i<roi_out->width; i++,out++,in++)
        *out = *in / coeffsi;
    }
    _mm_sfence();
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp == 4)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
        *out = *in / 1;
    }
  }
  else
  {
    const int ch = piece->colors;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const float *in = ((float*)ivoid) + ch*k*roi_out->width;
      float *out = ((float*)ovoid) + ch*k*roi_out->width;
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
        for(int c=0; c<3; c++) out[c] = in[c] / 1;
    }
  }
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = piece->pipe->processed_maximum[k] / 1;
}

void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_rawscale_params_t tmp = (dt_iop_rawscale_params_t)
  {
    0, {0, 0, 0, 0}, 16384
  };

  tmp.raw_black_level = module->dev->image_storage.raw_black_level;
  for(int i=0; i<4; i++) tmp.raw_black_level_separate[i] = module->dev->image_storage.raw_black_level_separate[i];
  tmp.raw_white_point = module->dev->image_storage.raw_white_point;

  memcpy(module->params, &tmp, sizeof(dt_iop_rawscale_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_rawscale_params_t));
}

void init(dt_iop_module_t *module)
{
  // we don't need global data:
  module->params = malloc(sizeof(dt_iop_rawscale_params_t));
  module->default_params = malloc(sizeof(dt_iop_rawscale_params_t));
  module->default_enabled = 0;
  module->priority = 16; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_rawscale_params_t);
  module->gui_data = NULL;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawscale_gui_data_t *g = (dt_iop_rawscale_gui_data_t *)self->gui_data;
  dt_iop_rawscale_params_t *p = (dt_iop_rawscale_params_t *)self->params;

  dt_bauhaus_slider_set(g->raw_black_level, p->raw_black_level);
  for(int i=0; i<4; i++) dt_bauhaus_slider_set(g->raw_black_level_separate[i], p->raw_black_level_separate[i]);
  dt_bauhaus_slider_set(g->raw_white_point, p->raw_white_point);
}

static void
callback (GtkWidget *widget, gpointer *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_rawscale_gui_data_t *g = (dt_iop_rawscale_gui_data_t *)self->gui_data;
  dt_iop_rawscale_params_t *p = (dt_iop_rawscale_params_t *)self->params;

  p->raw_black_level = dt_bauhaus_slider_get(g->raw_black_level);
  for(int i=0; i<4; i++) p->raw_black_level_separate[i] = dt_bauhaus_slider_get(g->raw_black_level_separate[i]);
  p->raw_white_point = dt_bauhaus_slider_get(g->raw_white_point);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawscale_gui_data_t));
  dt_iop_rawscale_gui_data_t *g = (dt_iop_rawscale_gui_data_t *)self->gui_data;

  self->widget = gtk_vbox_new(FALSE, 5);

  g->raw_black_level =  dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, 0, 0);
  dt_bauhaus_widget_set_label(g->raw_black_level, NULL, _("Black level"));
  g_object_set(G_OBJECT(g->raw_black_level), "tooltip-text", _("Black level"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->raw_black_level, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->raw_black_level), "value-changed", G_CALLBACK(callback), self);

  for(int i=0; i<4; i++)
  {
    g->raw_black_level_separate[i] =  dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, 0, 0);
    dt_bauhaus_widget_set_label(g->raw_black_level_separate[i], NULL, g_strdup_printf(_("Separate black level %i"), i));
    g_object_set(G_OBJECT(g->raw_black_level_separate[i]), "tooltip-text", g_strdup_printf(_("Separate black level %i"), i), (char *)NULL);
    gtk_box_pack_start(GTK_BOX(self->widget), g->raw_black_level_separate[i], FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(g->raw_black_level_separate[i]), "value-changed", G_CALLBACK(callback), self);
  }

  g->raw_white_point =  dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, 16384, 0);
  dt_bauhaus_widget_set_label(g->raw_white_point, NULL, _("White point"));
  g_object_set(G_OBJECT(g->raw_white_point), "tooltip-text", _("White point"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(self->widget), g->raw_white_point, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(g->raw_white_point), "value-changed", G_CALLBACK(callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
