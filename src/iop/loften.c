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
#include <stdlib.h>
#include <math.h>
#include <gtk/gtk.h>

#include "bauhaus/bauhaus.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "develop/tiling.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "common/gaussian.h"
#include "common/bilateral.h"
#include "common/opencl.h"
#include "common/bilateralcl.h"

DT_MODULE_INTROSPECTION(1, dt_iop_loften_params_t)

typedef enum dt_iop_loften_soften_algo_t
{
  SOFTEN_ALGO_GAUSSIAN,
  SOFTEN_ALGO_BILATERAL
} dt_iop_loften_soften_algo_t;

typedef struct dt_iop_loften_params_t
{
  float radius;
  dt_iop_loften_soften_algo_t soften_algo;
  dt_gaussian_order_t order;
  float saturation;
  float brightness;
} dt_iop_loften_params_t;

typedef struct dt_iop_loften_gui_data_t
{
  GtkWidget *radius;
  GtkWidget *soften_algo;
  GtkWidget *saturation;
  GtkWidget *brightness;
} dt_iop_loften_gui_data_t;

typedef dt_iop_loften_params_t dt_iop_loften_data_t;

typedef struct dt_iop_loften_global_data_t
{
  int kernel_loften;
} dt_iop_loften_global_data_t;

const char *name()
{
  return _("soften (Lab)");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int groups()
{
  return IOP_GROUP_EFFECT;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "radius"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "soften with"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "saturation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "brightness"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_loften_gui_data_t *g = (dt_iop_loften_gui_data_t *)self->gui_data;

  dt_accel_connect_slider_iop(self, "radius", GTK_WIDGET(g->radius));
  dt_accel_connect_slider_iop(self, "soften with", GTK_WIDGET(g->soften_algo));
  dt_accel_connect_slider_iop(self, "saturation", GTK_WIDGET(g->saturation));
  dt_accel_connect_slider_iop(self, "brightness", GTK_WIDGET(g->brightness));
}

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_in,
                     const dt_iop_roi_t *const roi_out, dt_develop_tiling_t *tiling)
{
  const dt_iop_loften_data_t *const d = (dt_iop_loften_data_t *)piece->data;

  const float radius = fmax(0.1f, fabs(d->radius));
  const float sigma = radius * roi_in->scale / piece->iscale;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const size_t basebuffer = width * height * channels * sizeof(float);

  switch(d->soften_algo)
  {
    case SOFTEN_ALGO_GAUSSIAN:
    {
      tiling->factor = 2.0f + (float)dt_gaussian_memory_use(width, height, channels) / basebuffer;
      tiling->maxbuf = fmax(1.0f, (float)dt_gaussian_singlebuffer_size(width, height, channels) / basebuffer);
      break;
    }
    case SOFTEN_ALGO_BILATERAL:
    {
      const float sigma_r = 100.0f; // does not depend on scale
      const float sigma_s = sigma;

      tiling->factor = 2.0f + (float)dt_bilateral_memory_use(width, height, sigma_s, sigma_r) / basebuffer;
      tiling->maxbuf
          = fmax(1.0f, (float)dt_bilateral_singlebuffer_size(width, height, sigma_s, sigma_r) / basebuffer);
      break;
    }
  }

  tiling->overhead = 0;
  tiling->overlap = ceilf(4 * sigma);
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_loften_data_t *const d = (dt_iop_loften_data_t *)piece->data;

  const int ch = piece->colors;

  const __m128 scale = _mm_set_ps(1.0f, d->saturation, d->saturation, d->brightness);

// 1. create overexposed image
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;
    for(int i = 0; i < roi_out->width; i++)
    {
      _mm_stream_ps(out, _mm_mul_ps(scale, _mm_load_ps(in)));
      in += ch;
      out += ch;
    }
  }
  _mm_sfence();

  // 2. blur
  const float radius = fmaxf(0.1f, d->radius);
  const float sigma = radius * roi_in->scale / piece->iscale;

  switch(d->soften_algo)
  {
    case SOFTEN_ALGO_GAUSSIAN:
    {
      float Labmax[4];
      float Labmin[4];

      for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
      for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;

      dt_gaussian_t *g
          = dt_gaussian_init(roi_out->width, roi_out->height, ch, Labmax, Labmin, sigma, d->order);
      if(!g) return;

      // FIXME: tmp buffer?
      dt_gaussian_blur_4c(g, ovoid, ovoid);
      dt_gaussian_free(g);
      break;
    }
    case SOFTEN_ALGO_BILATERAL:
    {
      const float sigma_r = 100.0f; // does not depend on scale
      const float sigma_s = sigma;
      const float detail = -1.0f; // we want the bilateral base layer

      // FIXME: tmp buffer?
      dt_bilateral_t *b = dt_bilateral_init(roi_out->width, roi_out->height, sigma_s, sigma_r);
      if(!b) return;
      dt_bilateral_splat(b, ovoid);
      dt_bilateral_blur(b);
      dt_bilateral_slice(b, ovoid, ovoid, detail);
      dt_bilateral_free(b);
      break;
    }
  }

  if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);

  // 3. blend, suggested opts: uniformly, blend mode: normal, opacity: 50%
}

#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_loften_data_t *const d = (dt_iop_loften_data_t *)piece->data;
  dt_iop_loften_global_data_t *gd = (dt_iop_loften_global_data_t *)self->data;

  cl_int err = -999;
  const int devid = piece->pipe->devid;

  const int width = roi_in->width;
  const int height = roi_in->height;
  const int channels = piece->colors;

  const float radius = fmax(0.1f, fabs(d->radius));
  const float sigma = radius * roi_in->scale / piece->iscale;
  const float brightness = d->brightness;
  const float saturation = d->saturation;
  const int order = d->order;

  size_t sizes[3];
  dt_gaussian_cl_t *g = NULL;
  dt_bilateral_cl_t *b = NULL;


  sizes[0] = ROUNDUPWD(width);
  sizes[1] = ROUNDUPWD(height);
  sizes[2] = 1;
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 4, sizeof(float), (void *)&brightness);
  dt_opencl_set_kernel_arg(devid, gd->kernel_loften, 5, sizeof(float), (void *)&saturation);

  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_loften, sizes);
  if(err != CL_SUCCESS) goto error;

  switch(d->soften_algo)
  {
    case SOFTEN_ALGO_GAUSSIAN:
    {
      float Labmax[4];
      float Labmin[4];

      for(int k = 0; k < 4; k++) Labmax[k] = INFINITY;
      for(int k = 0; k < 4; k++) Labmin[k] = -INFINITY;

      g = dt_gaussian_init_cl(devid, width, height, channels, Labmax, Labmin, sigma, order);
      if(!g) goto error;
      err = dt_gaussian_blur_cl(g, dev_out, dev_out);
      if(err != CL_SUCCESS) goto error;
      dt_gaussian_free_cl(g);
      g = NULL;
      break;
    }
    case SOFTEN_ALGO_BILATERAL:
    {
      const float sigma_r = 100.0f; // does not depend on scale
      const float sigma_s = sigma;
      const float detail = -1.0f; // we want the bilateral base layer

      b = dt_bilateral_init_cl(devid, width, height, sigma_s, sigma_r);
      if(!b) goto error;
      err = dt_bilateral_splat_cl(b, dev_out);
      if(err != CL_SUCCESS) goto error;
      err = dt_bilateral_blur_cl(b);
      if(err != CL_SUCCESS) goto error;
      err = dt_bilateral_slice_cl(b, dev_out, dev_out, detail);
      if(err != CL_SUCCESS) goto error;
      dt_bilateral_free_cl(b);
      b = NULL; // make sure we don't clean it up twice
      break;
    }
  }

  return TRUE;

error:
  if(g) dt_gaussian_free_cl(g);
  if(b) dt_bilateral_free_cl(b);

  dt_print(DT_DEBUG_OPENCL, "[opencl_loften] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static void radius_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;
  p->radius = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void soften_algo_callback(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;
  switch(dt_bauhaus_combobox_get(widget))
  {
    case 0:
      p->soften_algo = SOFTEN_ALGO_GAUSSIAN;
      break;
    case 1:
      p->soften_algo = SOFTEN_ALGO_BILATERAL;
      break;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void saturation_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;
  p->saturation = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;
  p->brightness = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)p1;
  dt_iop_loften_data_t *d = (dt_iop_loften_data_t *)piece->data;

  d->radius = p->radius;
  d->soften_algo = p->soften_algo;
  d->saturation = p->saturation / 100.0f;
  d->brightness = 1.0f / exp2f(-p->brightness);

#ifdef HAVE_OPENCL
  if(d->soften_algo == SOFTEN_ALGO_BILATERAL)
    piece->process_cl_ready = (piece->process_cl_ready && !(darktable.opencl->avoid_atomics));
#endif
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_loften_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_loften_gui_data_t *g = (dt_iop_loften_gui_data_t *)self->gui_data;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;
  dt_bauhaus_slider_set(g->radius, p->radius);
  switch(p->soften_algo)
  {
    case SOFTEN_ALGO_GAUSSIAN:
      dt_bauhaus_combobox_set(g->soften_algo, 0);
      break;
    case SOFTEN_ALGO_BILATERAL:
      dt_bauhaus_combobox_set(g->soften_algo, 1);
      break;
  }
  dt_bauhaus_slider_set(g->saturation, p->saturation);
  dt_bauhaus_slider_set(g->brightness, p->brightness);
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_loften_params_t tmp = (dt_iop_loften_params_t){
    .radius = 75.0f,
    .soften_algo = SOFTEN_ALGO_GAUSSIAN,
    .order = DT_IOP_GAUSSIAN_ZERO,
    .saturation = 100.0f,
    .brightness = 0.33f,
  };

  memcpy(self->params, &tmp, sizeof(dt_iop_loften_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_loften_params_t));
}

void init(dt_iop_module_t *self)
{
  self->params = calloc(1, sizeof(dt_iop_loften_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_loften_params_t));
  self->default_enabled = 0;
  self->priority = 808; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_loften_params_t);
  self->gui_data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 6; // gaussian.cl, from programs.conf
  module->data = malloc(sizeof(dt_iop_loften_global_data_t));
  dt_iop_loften_global_data_t *gd = module->data;
  gd->kernel_loften = dt_opencl_create_kernel(program, "loften");
}

void cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
  free(self->params);
  self->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_loften_global_data_t *gd = module->data;
  dt_opencl_free_kernel(gd->kernel_loften);
  free(module->data);
  module->data = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_loften_gui_data_t));
  dt_iop_loften_gui_data_t *g = (dt_iop_loften_gui_data_t *)self->gui_data;
  dt_iop_loften_params_t *p = (dt_iop_loften_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->radius = dt_bauhaus_slider_new_with_range(self, 0.1, 200.0, 2, p->radius, 2);
  g->soften_algo = dt_bauhaus_combobox_new(self);
  g->saturation = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 2, p->saturation, 2);
  g->brightness = dt_bauhaus_slider_new_with_range(self, -2.0, 2.0, 0.01, p->brightness, 2);

  dt_bauhaus_slider_set_format(g->radius, "%.02f");
  dt_bauhaus_slider_set_format(g->saturation, "%.0f%%");
  dt_bauhaus_slider_set_format(g->brightness, "%.2fEV");

  dt_bauhaus_widget_set_label(g->radius, NULL, _("radius"));
  dt_bauhaus_widget_set_label(g->soften_algo, NULL, _("soften with"));
  dt_bauhaus_widget_set_label(g->saturation, NULL, _("saturation"));
  dt_bauhaus_widget_set_label(g->brightness, NULL, _("brightness"));

  dt_bauhaus_combobox_add(g->soften_algo, _("gaussian"));
  dt_bauhaus_combobox_add(g->soften_algo, _("bilateral filter"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->radius, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->soften_algo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->saturation, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->brightness, TRUE, TRUE, 0);

  g_object_set(g->radius, "tooltip-text", _("spatial extent of the blurring"), (char *)NULL);
  g_object_set(g->soften_algo, "tooltip-text", _("which filter to use for blurring"), (char *)NULL);
  g_object_set(g->saturation, "tooltip-text", _("color saturation adjustment"), (char *)NULL);
  g_object_set(g->brightness, "tooltip-text", _("the brightness of blur"), (char *)NULL);

  g_signal_connect(G_OBJECT(g->radius), "value-changed", G_CALLBACK(radius_callback), self);
  g_signal_connect(G_OBJECT(g->soften_algo), "value-changed", G_CALLBACK(soften_algo_callback), self);
  g_signal_connect(G_OBJECT(g->saturation), "value-changed", G_CALLBACK(saturation_callback), self);
  g_signal_connect(G_OBJECT(g->brightness), "value-changed", G_CALLBACK(brightness_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
